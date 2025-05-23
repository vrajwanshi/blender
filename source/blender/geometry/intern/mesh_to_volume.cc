/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_math_matrix.hh"
#include "BLI_task.hh"

#include "BKE_volume.hh"
#include "BKE_volume_grid.hh"
#include "BKE_volume_openvdb.hh"

#include "GEO_mesh_to_volume.hh"

#ifdef WITH_OPENVDB
#  include <algorithm>
#  include <openvdb/openvdb.h>
#  include <openvdb/tools/GridTransformer.h>
#  include <openvdb/tools/LevelSetUtil.h>
#  include <openvdb/tools/VolumeToMesh.h>

namespace blender::geometry {

/* This class follows the MeshDataAdapter interface from openvdb. */
class OpenVDBMeshAdapter {
 private:
  Span<float3> positions_;
  Span<int> corner_verts_;
  Span<int3> corner_tris_;
  float4x4 transform_;

 public:
  OpenVDBMeshAdapter(const Span<float3> positions,
                     const Span<int> corner_verts,
                     const Span<int3> corner_tris,
                     const float4x4 &transform);
  size_t polygonCount() const;
  size_t pointCount() const;
  size_t vertexCount(size_t /*polygon_index*/) const;
  void getIndexSpacePoint(size_t polygon_index, size_t vertex_index, openvdb::Vec3d &pos) const;
};

OpenVDBMeshAdapter::OpenVDBMeshAdapter(const Span<float3> positions,
                                       const Span<int> corner_verts,
                                       const Span<int3> corner_tris,
                                       const float4x4 &transform)
    : positions_(positions),
      corner_verts_(corner_verts),
      corner_tris_(corner_tris),
      transform_(transform)
{
}

size_t OpenVDBMeshAdapter::polygonCount() const
{
  return size_t(corner_tris_.size());
}

size_t OpenVDBMeshAdapter::pointCount() const
{
  return size_t(positions_.size());
}

size_t OpenVDBMeshAdapter::vertexCount(size_t /*polygon_index*/) const
{
  /* All polygons are triangles. */
  return 3;
}

void OpenVDBMeshAdapter::getIndexSpacePoint(size_t polygon_index,
                                            size_t vertex_index,
                                            openvdb::Vec3d &pos) const
{
  const int3 &tri = corner_tris_[polygon_index];
  const float3 transformed_co = math::transform_point(
      transform_, positions_[corner_verts_[tri[vertex_index]]]);
  pos = &transformed_co.x;
}

float volume_compute_voxel_size(const Depsgraph *depsgraph,
                                const FunctionRef<Bounds<float3>()> bounds_fn,
                                const MeshToVolumeResolution res,
                                const float exterior_band_width,
                                const float4x4 &transform)
{
  const float volume_simplify = BKE_volume_simplify_factor(depsgraph);
  if (volume_simplify == 0.0f) {
    return 0.0f;
  }

  if (res.mode == MESH_TO_VOLUME_RESOLUTION_MODE_VOXEL_SIZE) {
    return res.settings.voxel_size / volume_simplify;
  }
  if (res.settings.voxel_amount <= 0) {
    return 0;
  }

  const Bounds<float3> bounds = bounds_fn();

  /* Compute the diagonal of the bounding box. This is used because
   * it will always be bigger than the widest side of the mesh. */
  const float diagonal = math::distance(math::transform_point(transform, bounds.min),
                                        math::transform_point(transform, bounds.max));

  /* To get the approximate size per voxel, first subtract the exterior band from the requested
   * voxel amount, then divide the diagonal with this value if it's bigger than 1. */
  const float voxel_size =
      (diagonal / std::max(1.0f, float(res.settings.voxel_amount) - 2.0f * exterior_band_width));

  /* Return the simplified voxel size. */
  return voxel_size / volume_simplify;
}

static openvdb::FloatGrid::Ptr mesh_to_density_grid_impl(
    const Span<float3> positions,
    const Span<int> corner_verts,
    const Span<int3> corner_tris,
    const float4x4 &mesh_to_volume_space_transform,
    const float voxel_size,
    const float interior_band_width,
    const float density)
{
  if (voxel_size < 1e-5f) {
    return nullptr;
  }

  float4x4 mesh_to_index_space_transform = math::from_scale<float4x4>(float3(1.0f / voxel_size));
  mesh_to_index_space_transform *= mesh_to_volume_space_transform;
  /* Better align generated grid with the source mesh. */
  mesh_to_index_space_transform.location() -= 0.5f;

  OpenVDBMeshAdapter mesh_adapter{
      positions, corner_verts, corner_tris, mesh_to_index_space_transform};
  const float interior = std::max(1.0f, interior_band_width / voxel_size);

  openvdb::math::Transform::Ptr transform = openvdb::math::Transform::createLinearTransform(
      voxel_size);
  openvdb::FloatGrid::Ptr new_grid = openvdb::tools::meshToVolume<openvdb::FloatGrid>(
      mesh_adapter, *transform, 1.0f, interior);

  openvdb::tools::sdfToFogVolume(*new_grid);

  if (density != 1.0f) {
    openvdb::tools::foreach(new_grid->beginValueOn(),
                            [&](const openvdb::FloatGrid::ValueOnIter &iter) {
                              iter.modifyValue([&](float &value) { value *= density; });
                            });
  }
  return new_grid;
}

bke::VolumeGrid<float> mesh_to_density_grid(const Span<float3> positions,
                                            const Span<int> corner_verts,
                                            const Span<int3> corner_tris,
                                            const float voxel_size,
                                            const float interior_band_width,
                                            const float density)
{
  openvdb::FloatGrid::Ptr grid = mesh_to_density_grid_impl(positions,
                                                           corner_verts,
                                                           corner_tris,
                                                           float4x4::identity(),
                                                           voxel_size,
                                                           interior_band_width,
                                                           density);
  if (!grid) {
    return {};
  }
  return bke::VolumeGrid<float>(std::move(grid));
}

bke::VolumeGrid<float> mesh_to_sdf_grid(const Span<float3> positions,
                                        const Span<int> corner_verts,
                                        const Span<int3> corner_tris,
                                        const float voxel_size,
                                        const float half_band_width)
{
  if (voxel_size <= 0.0f || half_band_width <= 0.0f) {
    return {};
  }

  std::vector<openvdb::Vec3s> points(positions.size());
  std::vector<openvdb::Vec3I> triangles(corner_tris.size());

  threading::parallel_for(positions.index_range(), 2048, [&](const IndexRange range) {
    for (const int i : range) {
      const float3 &co = positions[i];
      points[i] = openvdb::Vec3s(co.x, co.y, co.z) - 0.5f * voxel_size;
    }
  });

  threading::parallel_for(corner_tris.index_range(), 2048, [&](const IndexRange range) {
    for (const int i : range) {
      const int3 &tri = corner_tris[i];
      triangles[i] = openvdb::Vec3I(
          corner_verts[tri[0]], corner_verts[tri[1]], corner_verts[tri[2]]);
    }
  });

  openvdb::math::Transform::Ptr transform = openvdb::math::Transform::createLinearTransform(
      voxel_size);
  openvdb::FloatGrid::Ptr new_grid = openvdb::tools::meshToLevelSet<openvdb::FloatGrid>(
      *transform, points, triangles, half_band_width);

  return bke::VolumeGrid<float>(std::move(new_grid));
}

bke::VolumeGridData *fog_volume_grid_add_from_mesh(Volume *volume,
                                                   const StringRefNull name,
                                                   const Span<float3> positions,
                                                   const Span<int> corner_verts,
                                                   const Span<int3> corner_tris,
                                                   const float4x4 &mesh_to_volume_space_transform,
                                                   const float voxel_size,
                                                   const float interior_band_width,
                                                   const float density)
{
  openvdb::FloatGrid::Ptr mesh_grid = mesh_to_density_grid_impl(positions,
                                                                corner_verts,
                                                                corner_tris,
                                                                mesh_to_volume_space_transform,
                                                                voxel_size,
                                                                interior_band_width,
                                                                density);
  return mesh_grid ? BKE_volume_grid_add_vdb(*volume, name, std::move(mesh_grid)) : nullptr;
}

}  // namespace blender::geometry
#endif
