/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "device/memory.h"

#include "scene/colorspace.h"

#include "util/string.h"
#include "util/thread.h"
#include "util/transform.h"
#include "util/unique_ptr.h"
#include "util/vector.h"

CCL_NAMESPACE_BEGIN

class Device;
class DeviceInfo;
class ImageHandle;
class ImageKey;
class ImageMetaData;
class ImageManager;
class Progress;
class RenderStats;
class Scene;
class ColorSpaceProcessor;
class VDBImageLoader;

/* Image Parameters */
class ImageParams {
 public:
  bool animated = false;
  InterpolationType interpolation = INTERPOLATION_LINEAR;
  ExtensionType extension = EXTENSION_CLIP;
  ImageAlphaType alpha_type = IMAGE_ALPHA_AUTO;
  ustring colorspace;
  float frame = 0.0f;

  ImageParams() : colorspace(u_colorspace_raw) {}

  bool operator==(const ImageParams &other) const
  {
    return (animated == other.animated && interpolation == other.interpolation &&
            extension == other.extension && alpha_type == other.alpha_type &&
            colorspace == other.colorspace && frame == other.frame);
  }
};

/* Image MetaData
 *
 * Information about the image that is available before the image pixels are loaded. */
class ImageMetaData {
 public:
  /* Set by ImageLoader.load_metadata(). */
  int channels;
  size_t width, height;
  size_t byte_size;
  ImageDataType type;

  /* Optional color space, defaults to raw. */
  ustring colorspace;
  string colorspace_file_hint;
  const char *colorspace_file_format;

  /* Optional transform for 3D images. */
  bool use_transform_3d;
  Transform transform_3d;

  /* Automatically set. */
  bool compress_as_srgb;

  ImageMetaData();
  bool operator==(const ImageMetaData &other) const;
  bool is_float() const;
  void detect_colorspace();
};

/* Information about supported features that Image loaders can use. */
class ImageDeviceFeatures {
 public:
  bool has_nanovdb = true;
};

/* Image loader base class, that can be subclassed to load image data
 * from custom sources (file, memory, procedurally generated, etc). */
class ImageLoader {
 public:
  ImageLoader();
  virtual ~ImageLoader() = default;

  /* Load metadata without actual image yet, should be fast. */
  virtual bool load_metadata(const ImageDeviceFeatures &features, ImageMetaData &metadata) = 0;

  /* Load actual image contents. */
  virtual bool load_pixels(const ImageMetaData &metadata,
                           void *pixels,
                           const size_t pixels_size,
                           const bool associate_alpha) = 0;

  /* Name for logs and stats. */
  virtual string name() const = 0;

  /* Optional for OSL texture cache. */
  virtual ustring osl_filepath() const;

  /* Optional for tiled textures loaded externally. */
  virtual int get_tile_number() const;

  /* Free any memory used for loading metadata and pixels. */
  virtual void cleanup(){};

  /* Compare avoid loading the same image multiple times. */
  virtual bool equals(const ImageLoader &other) const = 0;
  static bool equals(const ImageLoader *a, const ImageLoader *b);

  virtual bool is_vdb_loader() const;

  /* Work around for no RTTI. */
};

/* Image Handle
 *
 * Access handle for image in the image manager. Multiple shader nodes may
 * share the same image, and this class handles reference counting for that. */
class ImageHandle {
 public:
  ImageHandle();
  ImageHandle(const ImageHandle &other);
  ImageHandle &operator=(const ImageHandle &other);
  ~ImageHandle();

  bool operator==(const ImageHandle &other) const;

  void clear();

  bool empty() const;
  int num_tiles() const;
  int num_svm_slots() const;

  ImageMetaData metadata();
  int svm_slot(const int slot_index = 0) const;
  vector<int4> get_svm_slots() const;
  device_texture *image_memory() const;

  VDBImageLoader *vdb_loader() const;

  ImageManager *get_manager() const;

 protected:
  vector<size_t> slots;
  bool is_tiled = false;
  ImageManager *manager;

  friend class ImageManager;
};

/* Image Manager
 *
 * Handles loading and storage of all images in the scene. This includes 2D
 * texture images and 3D volume images. */
class ImageManager {
 public:
  explicit ImageManager(const DeviceInfo &info);
  ~ImageManager();

  ImageHandle add_image(const string &filename, const ImageParams &params);
  ImageHandle add_image(const string &filename,
                        const ImageParams &params,
                        const array<int> &tiles);
  ImageHandle add_image(unique_ptr<ImageLoader> &&loader,
                        const ImageParams &params,
                        const bool builtin = true);
  ImageHandle add_image(vector<unique_ptr<ImageLoader>> &&loaders, const ImageParams &params);

  void device_update(Device *device, Scene *scene, Progress &progress);
  void device_update_slot(Device *device, Scene *scene, const size_t slot, Progress &progress);
  void device_free(Device *device);

  void device_load_builtin(Device *device, Scene *scene, Progress &progress);
  void device_free_builtin(Device *device);

  void set_osl_texture_system(void *texture_system);
  bool set_animation_frame_update(const int frame);

  void collect_statistics(RenderStats *stats);

  void tag_update();

  bool need_update() const;

  struct Image {
    ImageParams params;
    ImageMetaData metadata;
    unique_ptr<ImageLoader> loader;

    bool need_metadata;
    bool need_load;
    bool builtin;

    string mem_name;
    unique_ptr<device_texture> mem;

    int users;
    thread_mutex mutex;
  };

 private:
  bool need_update_;

  ImageDeviceFeatures features;

  thread_mutex device_mutex;
  thread_mutex images_mutex;
  int animation_frame;

  vector<unique_ptr<Image>> images;
  void *osl_texture_system;

  size_t add_image_slot(unique_ptr<ImageLoader> &&loader,
                        const ImageParams &params,
                        const bool builtin);
  void add_image_user(const size_t slot);
  void remove_image_user(const size_t slot);
  Image *get_image_slot(const size_t slot);

  void load_image_metadata(Image *img);

  template<TypeDesc::BASETYPE FileFormat, typename StorageType>
  bool file_load_image(Image *img, const int texture_limit);

  void device_load_image(Device *device, Scene *scene, const size_t slot, Progress &progress);
  void device_free_image(Device *device, const size_t slot);

  friend class ImageHandle;
};

CCL_NAMESPACE_END
