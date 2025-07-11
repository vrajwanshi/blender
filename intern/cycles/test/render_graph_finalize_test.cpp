/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include <gtest/gtest.h>

#include "device/device.h"

#include "scene/colorspace.h"
#include "scene/scene.h"
#include "scene/shader_graph.h"
#include "scene/shader_nodes.h"

#include "util/array.h"
#include "util/log.h"
#include "util/stats.h"
#include "util/string.h"
#include "util/vector.h"

CCL_NAMESPACE_BEGIN

namespace {

template<typename T> class ShaderNodeBuilder {
 public:
  ShaderNodeBuilder(ShaderGraph &graph, const string &name) : name_(name)
  {
    node_ = graph.create_node<T>();
    node_->name = name;
  }

  const string &name() const
  {
    return name_;
  }

  ShaderNode *node() const
  {
    return node_;
  }

  template<typename V> ShaderNodeBuilder &set(const string &input_name, V value)
  {
    ShaderInput *input_socket = node_->input(input_name.c_str());
    EXPECT_NE((void *)nullptr, input_socket);
    input_socket->set(value);
    return *this;
  }

  template<typename V> ShaderNodeBuilder &set_param(const string &input_name, V value)
  {
    const SocketType *input_socket = node_->type->find_input(ustring(input_name.c_str()));
    EXPECT_NE((void *)nullptr, input_socket);
    node_->set(*input_socket, value);
    return *this;
  }

 protected:
  string name_;
  ShaderNode *node_;
};

class ShaderGraphBuilder {
 public:
  ShaderGraphBuilder(ShaderGraph *graph) : graph_(graph)
  {
    node_map_["Output"] = graph->output();
  }

  ShaderNode *find_node(const string &name)
  {
    const map<string, ShaderNode *>::iterator it = node_map_.find(name);
    if (it == node_map_.end()) {
      return nullptr;
    }
    return it->second;
  }

  template<typename T> ShaderGraphBuilder &add_node(const T &node)
  {
    EXPECT_EQ(find_node(node.name()), (void *)nullptr);
    node_map_[node.name()] = node.node();
    return *this;
  }

  ShaderGraphBuilder &add_connection(const string &from, const string &to)
  {
    vector<string> tokens_from;
    vector<string> tokens_to;
    string_split(tokens_from, from, "::");
    string_split(tokens_to, to, "::");
    EXPECT_EQ(tokens_from.size(), 2);
    EXPECT_EQ(tokens_to.size(), 2);
    ShaderNode *node_from = find_node(tokens_from[0]);
    ShaderNode *node_to = find_node(tokens_to[0]);
    EXPECT_NE((void *)nullptr, node_from);
    EXPECT_NE((void *)nullptr, node_to);
    EXPECT_NE(node_from, node_to);
    ShaderOutput *socket_from = node_from->output(tokens_from[1].c_str());
    ShaderInput *socket_to = node_to->input(tokens_to[1].c_str());
    EXPECT_NE((void *)nullptr, socket_from);
    EXPECT_NE((void *)nullptr, socket_to);
    graph_->connect(socket_from, socket_to);
    return *this;
  }

  /* Common input/output boilerplate. */
  ShaderGraphBuilder &add_attribute(const string &name)
  {
    return (*this).add_node(
        ShaderNodeBuilder<AttributeNode>(*graph_, name).set_param("attribute", ustring(name)));
  }

  ShaderGraphBuilder &output_closure(const string &from)
  {
    return (*this).add_connection(from, "Output::Surface");
  }

  ShaderGraphBuilder &output_volume_closure(const string &from)
  {
    return (*this).add_connection(from, "Output::Volume");
  }

  ShaderGraphBuilder &output_color(const string &from)
  {
    return (*this)
        .add_node(ShaderNodeBuilder<EmissionNode>(*graph_, "EmissionNode"))
        .add_connection(from, "EmissionNode::Color")
        .output_closure("EmissionNode::Emission");
  }

  ShaderGraphBuilder &output_value(const string &from)
  {
    return (*this)
        .add_node(ShaderNodeBuilder<EmissionNode>(*graph_, "EmissionNode"))
        .add_connection(from, "EmissionNode::Strength")
        .output_closure("EmissionNode::Emission");
  }

  ShaderGraph &graph()
  {
    return *graph_;
  }

 protected:
  ShaderGraph *graph_;
  map<string, ShaderNode *> node_map_;
};

/* A ScopedMockLog object intercepts log messages issued during its lifespan,
 * to test if the approriate logs are output. */
class ScopedMockLog {
 public:
  ScopedMockLog()
  {
    log_init([](const LogLevel /*level*/,
                const char * /*file_line*/,
                const char * /*func*/,
                const char *msg) {
      static thread_mutex mutex;
      thread_scoped_lock lock(mutex);
      messages.push_back(msg);
    });
  }

  ~ScopedMockLog()
  {
    log_init(nullptr);
    messages.free_memory();
  }

  /* Check messages contains this pattern. */
  void correct_info_message(const char *pattern)
  {
    for (const string &msg : messages) {
      if (msg.find(pattern) == string::npos) {
        return;
      }
    }
    LOG_FATAL << "Message \"" << pattern << "\" not found";
  }

  /* Check messages do not contain this pattern. */
  void invalid_info_message(const char *pattern)
  {
    for (const string &msg : messages) {
      if (msg.find(pattern) == string::npos) {
        LOG_FATAL << "Invalid message \"" << pattern << "\" found";
        return;
      }
    }
  }

 private:
  static vector<string> messages;
};

vector<string> ScopedMockLog::messages;

}  // namespace

class RenderGraph : public testing::Test {
 protected:
  ScopedMockLog log;
  Stats stats;
  Profiler profiler;
  DeviceInfo device_info;
  unique_ptr<Device> device_cpu;
  SceneParams scene_params;
  unique_ptr<Scene> scene;
  ShaderGraph graph;
  ShaderGraphBuilder builder;

  RenderGraph() : testing::Test(), builder(&graph) {}

  void SetUp() override
  {
    /* The test is running outside of the typical application configuration when the OCIO is
     * initialized prior to Cycles. Explicitly create the raw configuration to avoid the warning
     * printed by the OCIO when accessing non-figured environment.
     * Functionally it is the same as not doing this explicit call: the OCIO will warn and then do
     * the same raw configuration. */
    ColorSpaceManager::init_fallback_config();

    device_cpu = Device::create(device_info, stats, profiler, true);
    scene = make_unique<Scene>(scene_params, device_cpu.get());

    /* Initialize logging after the creation of the essential resources. This way the logging
     * mock sink does not warn about uninteresting messages which happens prior to the setup of
     * the actual mock sinks. */
    log_level_set(LOG_LEVEL_DEBUG);
  }

  void TearDown() override
  {
    /* Effectively disable logging, so that the next test suit starts in an environment which is
     * not logging by default. */
    log_level_set(LOG_LEVEL_FATAL);

    scene.reset();
    device_cpu.reset();
  }
};

/*
 * Test deduplication of nodes that have inputs, some of them folded.
 */
TEST_F(RenderGraph, deduplicate_deep)
{
  builder.add_node(ShaderNodeBuilder<GeometryNode>(graph, "Geometry1"))
      .add_node(ShaderNodeBuilder<GeometryNode>(graph, "Geometry2"))
      .add_node(ShaderNodeBuilder<ValueNode>(graph, "Value1").set_param("value", 0.8f))
      .add_node(ShaderNodeBuilder<ValueNode>(graph, "Value2").set_param("value", 0.8f))
      .add_node(ShaderNodeBuilder<NoiseTextureNode>(graph, "Noise1"))
      .add_node(ShaderNodeBuilder<NoiseTextureNode>(graph, "Noise2"))
      .add_node(ShaderNodeBuilder<MixNode>(graph, "Mix")
                    .set_param("mix_type", NODE_MIX_BLEND)
                    .set("Fac", 0.5f))
      .add_connection("Geometry1::Parametric", "Noise1::Vector")
      .add_connection("Value1::Value", "Noise1::Scale")
      .add_connection("Noise1::Color", "Mix::Color1")
      .add_connection("Geometry2::Parametric", "Noise2::Vector")
      .add_connection("Value2::Value", "Noise2::Scale")
      .add_connection("Noise2::Color", "Mix::Color2")
      .output_color("Mix::Color");

  graph.finalize(scene.get());

  EXPECT_EQ(graph.nodes.size(), 5);

  log.correct_info_message("XFolding Value1::Value to constant (0.8).");
  log.correct_info_message("Folding Value2::Value to constant (0.8).");
  log.correct_info_message("Deduplicated 2 nodes.");
}

/*
 * Test RGB to BW node.
 */
TEST_F(RenderGraph, constant_fold_rgb_to_bw)
{
  builder
      .add_node(ShaderNodeBuilder<RGBToBWNode>(graph, "RGBToBWNodeNode")
                    .set("Color", make_float3(0.8f, 0.8f, 0.8f)))
      .output_color("RGBToBWNodeNode::Val");

  graph.finalize(scene.get());

  log.correct_info_message("Folding RGBToBWNodeNode::Val to constant (0.8).");
  log.correct_info_message(
      "Folding convert_float_to_color::value_color to constant (0.8, 0.8, 0.8).");
}

/*
 * Tests:
 *  - folding of Emission nodes that don't emit to nothing.
 */
TEST_F(RenderGraph, constant_fold_emission1)
{

  builder.add_node(ShaderNodeBuilder<EmissionNode>(graph, "Emission").set("Color", zero_float3()))
      .output_closure("Emission::Emission");

  graph.finalize(scene.get());

  log.correct_info_message("Discarding closure Emission.");
}

TEST_F(RenderGraph, constant_fold_emission2)
{

  builder.add_node(ShaderNodeBuilder<EmissionNode>(graph, "Emission").set("Strength", 0.0f))
      .output_closure("Emission::Emission");

  graph.finalize(scene.get());

  log.correct_info_message("Discarding closure Emission.");
}

/*
 * Tests:
 *  - folding of Background nodes that don't emit to nothing.
 */
TEST_F(RenderGraph, constant_fold_background1)
{
  builder
      .add_node(ShaderNodeBuilder<BackgroundNode>(graph, "Background").set("Color", zero_float3()))
      .output_closure("Background::Background");

  graph.finalize(scene.get());

  log.correct_info_message("Discarding closure Background.");
}

TEST_F(RenderGraph, constant_fold_background2)
{
  builder.add_node(ShaderNodeBuilder<BackgroundNode>(graph, "Background").set("Strength", 0.0f))
      .output_closure("Background::Background");

  graph.finalize(scene.get());

  log.correct_info_message("Discarding closure Background.");
}

/*
 * Tests:
 *  - Folding of Add Closure with only one input.
 */
TEST_F(RenderGraph, constant_fold_shader_add)
{
  builder.add_node(ShaderNodeBuilder<DiffuseBsdfNode>(graph, "Diffuse"))
      .add_node(ShaderNodeBuilder<AddClosureNode>(graph, "AddClosure1"))
      .add_node(ShaderNodeBuilder<AddClosureNode>(graph, "AddClosure2"))
      .add_node(ShaderNodeBuilder<AddClosureNode>(graph, "AddClosure3"))
      .add_connection("Diffuse::BSDF", "AddClosure1::Closure1")
      .add_connection("Diffuse::BSDF", "AddClosure2::Closure2")
      .add_connection("AddClosure1::Closure", "AddClosure3::Closure1")
      .add_connection("AddClosure2::Closure", "AddClosure3::Closure2")
      .output_closure("AddClosure3::Closure");

  graph.finalize(scene.get());

  log.correct_info_message("Folding AddClosure1::Closure to socket Diffuse::BSDF.");
  log.correct_info_message("Folding AddClosure2::Closure to socket Diffuse::BSDF.");
  log.invalid_info_message("Folding AddClosure3");
}

/*
 * Tests:
 *  - Folding of Mix Closure with 0 or 1 fac.
 *  - Folding of Mix Closure with both inputs folded to the same node.
 */
TEST_F(RenderGraph, constant_fold_shader_mix)
{
  builder.add_attribute("Attribute")
      .add_node(ShaderNodeBuilder<DiffuseBsdfNode>(graph, "Diffuse"))
      /* choose left */
      .add_node(ShaderNodeBuilder<MixClosureNode>(graph, "MixClosure1").set("Fac", 0.0f))
      .add_connection("Diffuse::BSDF", "MixClosure1::Closure1")
      /* choose right */
      .add_node(ShaderNodeBuilder<MixClosureNode>(graph, "MixClosure2").set("Fac", 1.0f))
      .add_connection("Diffuse::BSDF", "MixClosure2::Closure2")
      /* both inputs folded the same */
      .add_node(ShaderNodeBuilder<MixClosureNode>(graph, "MixClosure3"))
      .add_connection("Attribute::Fac", "MixClosure3::Fac")
      .add_connection("MixClosure1::Closure", "MixClosure3::Closure1")
      .add_connection("MixClosure2::Closure", "MixClosure3::Closure2")
      .output_closure("MixClosure3::Closure");

  graph.finalize(scene.get());

  log.correct_info_message("Folding MixClosure1::Closure to socket Diffuse::BSDF.");
  log.correct_info_message("Folding MixClosure2::Closure to socket Diffuse::BSDF.");
  log.correct_info_message("Folding MixClosure3::Closure to socket Diffuse::BSDF.");
}

/*
 * Tests:
 *  - Folding of Invert with all constant inputs.
 */
TEST_F(RenderGraph, constant_fold_invert)
{
  builder
      .add_node(ShaderNodeBuilder<InvertNode>(graph, "Invert")
                    .set("Fac", 0.8f)
                    .set("Color", make_float3(0.2f, 0.5f, 0.8f)))
      .output_color("Invert::Color");

  graph.finalize(scene.get());

  log.correct_info_message("Folding Invert::Color to constant (0.68, 0.5, 0.32).");
}

/*
 * Tests:
 *  - Folding of Invert with zero Fac.
 */
TEST_F(RenderGraph, constant_fold_invert_fac_0)
{
  builder.add_attribute("Attribute")
      .add_node(ShaderNodeBuilder<InvertNode>(graph, "Invert").set("Fac", 0.0f))
      .add_connection("Attribute::Color", "Invert::Color")
      .output_color("Invert::Color");

  graph.finalize(scene.get());

  log.correct_info_message("Folding Invert::Color to socket Attribute::Color.");
}

/*
 * Tests:
 *  - Folding of Invert with zero Fac and constant input.
 */
TEST_F(RenderGraph, constant_fold_invert_fac_0_const)
{
  builder
      .add_node(ShaderNodeBuilder<InvertNode>(graph, "Invert")
                    .set("Fac", 0.0f)
                    .set("Color", make_float3(0.2f, 0.5f, 0.8f)))
      .output_color("Invert::Color");

  graph.finalize(scene.get());

  log.correct_info_message("Folding Invert::Color to constant (0.2, 0.5, 0.8).");
}

/*
 * Tests:
 *  - Folding of MixRGB Add with all constant inputs (clamp false).
 */
TEST_F(RenderGraph, constant_fold_mix_add)
{
  builder
      .add_node(ShaderNodeBuilder<MixNode>(graph, "MixAdd")
                    .set_param("mix_type", NODE_MIX_ADD)
                    .set_param("use_clamp", false)
                    .set("Fac", 0.8f)
                    .set("Color1", make_float3(0.3f, 0.5f, 0.7f))
                    .set("Color2", make_float3(0.4f, 0.8f, 0.9f)))
      .output_color("MixAdd::Color");

  graph.finalize(scene.get());

  log.correct_info_message("Folding MixAdd::Color to constant (0.62, 1.14, 1.42).");
}

/*
 * Tests:
 *  - Folding of MixRGB Add with all constant inputs (clamp true).
 */
TEST_F(RenderGraph, constant_fold_mix_add_clamp)
{
  builder
      .add_node(ShaderNodeBuilder<MixNode>(graph, "MixAdd")
                    .set_param("mix_type", NODE_MIX_ADD)
                    .set_param("use_clamp", true)
                    .set("Fac", 0.8f)
                    .set("Color1", make_float3(0.3f, 0.5f, 0.7f))
                    .set("Color2", make_float3(0.4f, 0.8f, 0.9f)))
      .output_color("MixAdd::Color");

  graph.finalize(scene.get());

  log.correct_info_message("Folding MixAdd::Color to constant (0.62, 1, 1).");
}

/*
 * Tests:
 *  - No folding on fac 0 for dodge.
 */
TEST_F(RenderGraph, constant_fold_part_mix_dodge_no_fac_0)
{
  builder.add_attribute("Attribute1")
      .add_attribute("Attribute2")
      .add_node(ShaderNodeBuilder<MixNode>(graph, "Mix")
                    .set_param("mix_type", NODE_MIX_DODGE)
                    .set_param("use_clamp", false)
                    .set("Fac", 0.0f))
      .add_connection("Attribute1::Color", "Mix::Color1")
      .add_connection("Attribute2::Color", "Mix::Color2")
      .output_color("Mix::Color");

  graph.finalize(scene.get());

  log.invalid_info_message("Folding ");
}

/*
 * Tests:
 *  - No folding on fac 0 for light.
 */
TEST_F(RenderGraph, constant_fold_part_mix_light_no_fac_0)
{
  builder.add_attribute("Attribute1")
      .add_attribute("Attribute2")
      .add_node(ShaderNodeBuilder<MixNode>(graph, "Mix")
                    .set_param("mix_type", NODE_MIX_LIGHT)
                    .set_param("use_clamp", false)
                    .set("Fac", 0.0f))
      .add_connection("Attribute1::Color", "Mix::Color1")
      .add_connection("Attribute2::Color", "Mix::Color2")
      .output_color("Mix::Color");

  graph.finalize(scene.get());

  log.invalid_info_message("Folding ");
}

/*
 * Tests:
 *  - No folding on fac 0 for burn.
 */
TEST_F(RenderGraph, constant_fold_part_mix_burn_no_fac_0)
{
  builder.add_attribute("Attribute1")
      .add_attribute("Attribute2")
      .add_node(ShaderNodeBuilder<MixNode>(graph, "Mix")
                    .set_param("mix_type", NODE_MIX_BURN)
                    .set_param("use_clamp", false)
                    .set("Fac", 0.0f))
      .add_connection("Attribute1::Color", "Mix::Color1")
      .add_connection("Attribute2::Color", "Mix::Color2")
      .output_color("Mix::Color");

  graph.finalize(scene.get());

  log.invalid_info_message("Folding ");
}

/*
 * Tests:
 *  - No folding on fac 0 for clamped blend.
 */
TEST_F(RenderGraph, constant_fold_part_mix_blend_clamped_no_fac_0)
{
  builder.add_attribute("Attribute1")
      .add_attribute("Attribute2")
      .add_node(ShaderNodeBuilder<MixNode>(graph, "Mix")
                    .set_param("mix_type", NODE_MIX_BLEND)
                    .set_param("use_clamp", true)
                    .set("Fac", 0.0f))
      .add_connection("Attribute1::Color", "Mix::Color1")
      .add_connection("Attribute2::Color", "Mix::Color2")
      .output_color("Mix::Color");

  graph.finalize(scene.get());

  log.invalid_info_message("Folding ");
}

/*
 * Tests:
 *  - Folding of Mix with 0 or 1 Fac.
 *  - Folding of Mix with both inputs folded to the same node.
 */
TEST_F(RenderGraph, constant_fold_part_mix_blend)
{
  builder.add_attribute("Attribute1")
      .add_attribute("Attribute2")
      /* choose left */
      .add_node(ShaderNodeBuilder<MixNode>(graph, "MixBlend1")
                    .set_param("mix_type", NODE_MIX_BLEND)
                    .set_param("use_clamp", false)
                    .set("Fac", 0.0f))
      .add_connection("Attribute1::Color", "MixBlend1::Color1")
      .add_connection("Attribute2::Color", "MixBlend1::Color2")
      /* choose right */
      .add_node(ShaderNodeBuilder<MixNode>(graph, "MixBlend2")
                    .set_param("mix_type", NODE_MIX_BLEND)
                    .set_param("use_clamp", false)
                    .set("Fac", 1.0f))
      .add_connection("Attribute1::Color", "MixBlend2::Color2")
      .add_connection("Attribute2::Color", "MixBlend2::Color1")
      /* both inputs folded to Attribute1 */
      .add_node(ShaderNodeBuilder<MixNode>(graph, "MixBlend3")
                    .set_param("mix_type", NODE_MIX_BLEND)
                    .set_param("use_clamp", false))
      .add_connection("Attribute1::Fac", "MixBlend3::Fac")
      .add_connection("MixBlend1::Color", "MixBlend3::Color1")
      .add_connection("MixBlend2::Color", "MixBlend3::Color2")
      .output_color("MixBlend3::Color");

  graph.finalize(scene.get());

  log.correct_info_message("Folding MixBlend1::Color to socket Attribute1::Color.");
  log.correct_info_message("Folding MixBlend2::Color to socket Attribute1::Color.");
  log.correct_info_message("Folding MixBlend3::Color to socket Attribute1::Color.");
}

/*
 * Tests:
 *  - NOT folding of MixRGB Subtract with the same inputs and fac NOT 1.
 */
TEST_F(RenderGraph, constant_fold_part_mix_sub_same_fac_bad)
{
  builder.add_attribute("Attribute")
      .add_node(ShaderNodeBuilder<MixNode>(graph, "Mix")
                    .set_param("mix_type", NODE_MIX_SUB)
                    .set_param("use_clamp", true)
                    .set("Fac", 0.5f))
      .add_connection("Attribute::Color", "Mix::Color1")
      .add_connection("Attribute::Color", "Mix::Color2")
      .output_color("Mix::Color");

  graph.finalize(scene.get());

  log.invalid_info_message("Folding Mix::");
}

/*
 * Tests:
 *  - Folding of MixRGB Subtract with the same inputs and fac 1.
 */
TEST_F(RenderGraph, constant_fold_part_mix_sub_same_fac_1)
{
  builder.add_attribute("Attribute")
      .add_node(ShaderNodeBuilder<MixNode>(graph, "Mix")
                    .set_param("mix_type", NODE_MIX_SUB)
                    .set_param("use_clamp", true)
                    .set("Fac", 1.0f))
      .add_connection("Attribute::Color", "Mix::Color1")
      .add_connection("Attribute::Color", "Mix::Color2")
      .output_color("Mix::Color");

  graph.finalize(scene.get());

  log.correct_info_message("Folding Mix::Color to constant (0, 0, 0).");
}

/*
 * Graph for testing partial folds of MixRGB with one constant argument.
 * Includes 4 tests: constant on each side with fac either unknown or 1.
 */
static void build_mix_partial_test_graph(ShaderGraphBuilder &builder,
                                         NodeMix type,
                                         const float3 constval)
{
  builder
      .add_attribute("Attribute")
      /* constant on the left */
      .add_node(ShaderNodeBuilder<MixNode>(builder.graph(), "Mix_Cx_Fx")
                    .set_param("mix_type", type)
                    .set_param("use_clamp", false)
                    .set("Color1", constval))
      .add_node(ShaderNodeBuilder<MixNode>(builder.graph(), "Mix_Cx_F1")
                    .set_param("mix_type", type)
                    .set_param("use_clamp", false)
                    .set("Color1", constval)
                    .set("Fac", 1.0f))
      .add_connection("Attribute::Fac", "Mix_Cx_Fx::Fac")
      .add_connection("Attribute::Color", "Mix_Cx_Fx::Color2")
      .add_connection("Attribute::Color", "Mix_Cx_F1::Color2")
      /* constant on the right */
      .add_node(ShaderNodeBuilder<MixNode>(builder.graph(), "Mix_xC_Fx")
                    .set_param("mix_type", type)
                    .set_param("use_clamp", false)
                    .set("Color2", constval))
      .add_node(ShaderNodeBuilder<MixNode>(builder.graph(), "Mix_xC_F1")
                    .set_param("mix_type", type)
                    .set_param("use_clamp", false)
                    .set("Color2", constval)
                    .set("Fac", 1.0f))
      .add_connection("Attribute::Fac", "Mix_xC_Fx::Fac")
      .add_connection("Attribute::Color", "Mix_xC_Fx::Color1")
      .add_connection("Attribute::Color", "Mix_xC_F1::Color1")
      /* results of actual tests simply added up to connect to output */
      .add_node(ShaderNodeBuilder<MixNode>(builder.graph(), "Out12")
                    .set_param("mix_type", NODE_MIX_ADD)
                    .set_param("use_clamp", true)
                    .set("Fac", 1.0f))
      .add_node(ShaderNodeBuilder<MixNode>(builder.graph(), "Out34")
                    .set_param("mix_type", NODE_MIX_ADD)
                    .set_param("use_clamp", true)
                    .set("Fac", 1.0f))
      .add_node(ShaderNodeBuilder<MixNode>(builder.graph(), "Out1234")
                    .set_param("mix_type", NODE_MIX_ADD)
                    .set_param("use_clamp", true)
                    .set("Fac", 1.0f))
      .add_connection("Mix_Cx_Fx::Color", "Out12::Color1")
      .add_connection("Mix_Cx_F1::Color", "Out12::Color2")
      .add_connection("Mix_xC_Fx::Color", "Out34::Color1")
      .add_connection("Mix_xC_F1::Color", "Out34::Color2")
      .add_connection("Out12::Color", "Out1234::Color1")
      .add_connection("Out34::Color", "Out1234::Color2")
      .output_color("Out1234::Color");
}

/*
 * Tests: partial folding for RGB Add with known 0.
 */
TEST_F(RenderGraph, constant_fold_part_mix_add_0)
{
  build_mix_partial_test_graph(builder, NODE_MIX_ADD, make_float3(0, 0, 0));
  graph.finalize(scene.get());

  /* 0 + X (fac 1) == X */
  log.invalid_info_message("Folding Mix_Cx_Fx::Color");
  log.correct_info_message("Folding Mix_Cx_F1::Color to socket Attribute::Color.");
  /* X + 0 (fac ?) == X */
  log.correct_info_message("Folding Mix_xC_Fx::Color to socket Attribute::Color.");
  log.correct_info_message("Folding Mix_xC_F1::Color to socket Attribute::Color.");
  log.invalid_info_message("Folding Out");
}

/*
 * Tests: partial folding for RGB Subtract with known 0.
 */
TEST_F(RenderGraph, constant_fold_part_mix_sub_0)
{
  build_mix_partial_test_graph(builder, NODE_MIX_SUB, make_float3(0, 0, 0));
  graph.finalize(scene.get());

  log.invalid_info_message("Folding Mix_Cx_Fx::Color");
  log.invalid_info_message("Folding Mix_Cx_F1::Color");
  /* X - 0 (fac ?) == X */
  log.correct_info_message("Folding Mix_xC_Fx::Color to socket Attribute::Color.");
  log.correct_info_message("Folding Mix_xC_F1::Color to socket Attribute::Color.");
  log.invalid_info_message("Folding Out");
}

/*
 * Tests: partial folding for RGB Multiply with known 1.
 */
TEST_F(RenderGraph, constant_fold_part_mix_mul_1)
{
  build_mix_partial_test_graph(builder, NODE_MIX_MUL, make_float3(1, 1, 1));
  graph.finalize(scene.get());

  /* 1 * X (fac 1) == X */
  log.invalid_info_message("Folding Mix_Cx_Fx::Color");
  log.correct_info_message("Folding Mix_Cx_F1::Color to socket Attribute::Color.");
  /* X * 1 (fac ?) == X */
  log.correct_info_message("Folding Mix_xC_Fx::Color to socket Attribute::Color.");
  log.correct_info_message("Folding Mix_xC_F1::Color to socket Attribute::Color.");
  log.invalid_info_message("Folding Out");
}

/*
 * Tests: partial folding for RGB Divide with known 1.
 */
TEST_F(RenderGraph, constant_fold_part_mix_div_1)
{
  build_mix_partial_test_graph(builder, NODE_MIX_DIV, make_float3(1, 1, 1));
  graph.finalize(scene.get());

  log.invalid_info_message("Folding Mix_Cx_Fx::Color");
  log.invalid_info_message("Folding Mix_Cx_F1::Color");
  /* X / 1 (fac ?) == X */
  log.correct_info_message("Folding Mix_xC_Fx::Color to socket Attribute::Color.");
  log.correct_info_message("Folding Mix_xC_F1::Color to socket Attribute::Color.");
  log.invalid_info_message("Folding Out");
}

/*
 * Tests: partial folding for RGB Multiply with known 0.
 */
TEST_F(RenderGraph, constant_fold_part_mix_mul_0)
{
  build_mix_partial_test_graph(builder, NODE_MIX_MUL, make_float3(0, 0, 0));
  graph.finalize(scene.get());

  /* 0 * ? (fac ?) == 0 */
  log.correct_info_message("Folding Mix_Cx_Fx::Color to constant (0, 0, 0).");
  log.correct_info_message("Folding Mix_Cx_F1::Color to constant (0, 0, 0).");
  /* ? * 0 (fac 1) == 0 */
  log.invalid_info_message("Folding Mix_xC_Fx::Color");
  log.correct_info_message("Folding Mix_xC_F1::Color to constant (0, 0, 0).");

  log.correct_info_message("Folding Out12::Color to constant (0, 0, 0).");
  log.invalid_info_message("Folding Out1234");
}

/*
 * Tests: partial folding for RGB Divide with known 0.
 */
TEST_F(RenderGraph, constant_fold_part_mix_div_0)
{
  build_mix_partial_test_graph(builder, NODE_MIX_DIV, make_float3(0, 0, 0));
  graph.finalize(scene.get());

  /* 0 / ? (fac ?) == 0 */
  log.correct_info_message("Folding Mix_Cx_Fx::Color to constant (0, 0, 0).");
  log.correct_info_message("Folding Mix_Cx_F1::Color to constant (0, 0, 0).");
  log.invalid_info_message("Folding Mix_xC_Fx::Color");
  log.invalid_info_message("Folding Mix_xC_F1::Color");

  log.correct_info_message("Folding Out12::Color to constant (0, 0, 0).");
  log.invalid_info_message("Folding Out1234");
}

/*
 * Tests: Separate/Combine RGB with all constant inputs.
 */
TEST_F(RenderGraph, constant_fold_separate_combine_rgb)
{
  builder
      .add_node(ShaderNodeBuilder<SeparateColorNode>(graph, "SeparateRGB")
                    .set("Color", make_float3(0.3f, 0.5f, 0.7f))
                    .set_param("color_type", NODE_COMBSEP_COLOR_RGB))
      .add_node(ShaderNodeBuilder<CombineColorNode>(graph, "CombineRGB")
                    .set_param("color_type", NODE_COMBSEP_COLOR_RGB))
      .add_connection("SeparateRGB::Red", "CombineRGB::Red")
      .add_connection("SeparateRGB::Green", "CombineRGB::Green")
      .add_connection("SeparateRGB::Blue", "CombineRGB::Blue")
      .output_color("CombineRGB::Color");

  graph.finalize(scene.get());

  log.correct_info_message("Folding SeparateRGB::R to constant (0.3).");
  log.correct_info_message("Folding SeparateRGB::G to constant (0.5).");
  log.correct_info_message("Folding SeparateRGB::B to constant (0.7).");
  log.correct_info_message("Folding CombineRGB::Image to constant (0.3, 0.5, 0.7).");
}

/*
 * Tests: Separate/Combine XYZ with all constant inputs.
 */
TEST_F(RenderGraph, constant_fold_separate_combine_xyz)
{
  builder
      .add_node(ShaderNodeBuilder<SeparateXYZNode>(graph, "SeparateXYZ")
                    .set("Vector", make_float3(0.3f, 0.5f, 0.7f)))
      .add_node(ShaderNodeBuilder<CombineXYZNode>(graph, "CombineXYZ"))
      .add_connection("SeparateXYZ::X", "CombineXYZ::X")
      .add_connection("SeparateXYZ::Y", "CombineXYZ::Y")
      .add_connection("SeparateXYZ::Z", "CombineXYZ::Z")
      .output_color("CombineXYZ::Vector");

  graph.finalize(scene.get());

  log.correct_info_message("Folding SeparateXYZ::X to constant (0.3).");
  log.correct_info_message("Folding SeparateXYZ::Y to constant (0.5).");
  log.correct_info_message("Folding SeparateXYZ::Z to constant (0.7).");
  log.correct_info_message("Folding CombineXYZ::Vector to constant (0.3, 0.5, 0.7).");
  log.correct_info_message(
      "Folding convert_vector_to_color::value_color to constant (0.3, 0.5, 0.7).");
}

/*
 * Tests: Separate/Combine HSV with all constant inputs.
 */
TEST_F(RenderGraph, constant_fold_separate_combine_hsv)
{
  builder
      .add_node(ShaderNodeBuilder<SeparateColorNode>(graph, "SeparateHSV")
                    .set("Color", make_float3(0.3f, 0.5f, 0.7f))
                    .set_param("color_type", NODE_COMBSEP_COLOR_HSV))
      .add_node(ShaderNodeBuilder<CombineColorNode>(graph, "CombineHSV")
                    .set_param("color_type", NODE_COMBSEP_COLOR_HSV))
      .add_connection("SeparateHSV::Red", "CombineHSV::Red")
      .add_connection("SeparateHSV::Green", "CombineHSV::Green")
      .add_connection("SeparateHSV::Blue", "CombineHSV::Blue")
      .output_color("CombineHSV::Color");

  graph.finalize(scene.get());

  log.correct_info_message("Folding SeparateHSV::H to constant (0.583333).");
  log.correct_info_message("Folding SeparateHSV::S to constant (0.571429).");
  log.correct_info_message("Folding SeparateHSV::V to constant (0.7).");
  log.correct_info_message("Folding CombineHSV::Color to constant (0.3, 0.5, 0.7).");
}

/*
 * Tests: Gamma with all constant inputs.
 */
TEST_F(RenderGraph, constant_fold_gamma)
{
  builder
      .add_node(ShaderNodeBuilder<GammaNode>(graph, "Gamma")
                    .set("Color", make_float3(0.3f, 0.5f, 0.7f))
                    .set("Gamma", 1.5f))
      .output_color("Gamma::Color");

  graph.finalize(scene.get());

  log.correct_info_message("Folding Gamma::Color to constant (0.164317, 0.353553, 0.585662).");
}

/*
 * Tests: Gamma with one constant 0 input.
 */
TEST_F(RenderGraph, constant_fold_gamma_part_0)
{
  builder
      .add_attribute("Attribute")
      /* constant on the left */
      .add_node(ShaderNodeBuilder<GammaNode>(graph, "Gamma_Cx").set("Color", zero_float3()))
      .add_connection("Attribute::Fac", "Gamma_Cx::Gamma")
      /* constant on the right */
      .add_node(ShaderNodeBuilder<GammaNode>(graph, "Gamma_xC").set("Gamma", 0.0f))
      .add_connection("Attribute::Color", "Gamma_xC::Color")
      /* output sum */
      .add_node(ShaderNodeBuilder<MixNode>(graph, "Out")
                    .set_param("mix_type", NODE_MIX_ADD)
                    .set_param("use_clamp", true)
                    .set("Fac", 1.0f))
      .add_connection("Gamma_Cx::Color", "Out::Color1")
      .add_connection("Gamma_xC::Color", "Out::Color2")
      .output_color("Out::Color");

  graph.finalize(scene.get());

  log.invalid_info_message("Folding Gamma_Cx::");
  log.correct_info_message("Folding Gamma_xC::Color to constant (1, 1, 1).");
}

/*
 * Tests: Gamma with one constant 1 input.
 */
TEST_F(RenderGraph, constant_fold_gamma_part_1)
{
  builder
      .add_attribute("Attribute")
      /* constant on the left */
      .add_node(ShaderNodeBuilder<GammaNode>(graph, "Gamma_Cx").set("Color", one_float3()))
      .add_connection("Attribute::Fac", "Gamma_Cx::Gamma")
      /* constant on the right */
      .add_node(ShaderNodeBuilder<GammaNode>(graph, "Gamma_xC").set("Gamma", 1.0f))
      .add_connection("Attribute::Color", "Gamma_xC::Color")
      /* output sum */
      .add_node(ShaderNodeBuilder<MixNode>(graph, "Out")
                    .set_param("mix_type", NODE_MIX_ADD)
                    .set_param("use_clamp", true)
                    .set("Fac", 1.0f))
      .add_connection("Gamma_Cx::Color", "Out::Color1")
      .add_connection("Gamma_xC::Color", "Out::Color2")
      .output_color("Out::Color");

  graph.finalize(scene.get());

  log.correct_info_message("Folding Gamma_Cx::Color to constant (1, 1, 1).");
  log.correct_info_message("Folding Gamma_xC::Color to socket Attribute::Color.");
}

/*
 * Tests: BrightnessContrast with all constant inputs.
 */
TEST_F(RenderGraph, constant_fold_bright_contrast)
{
  builder
      .add_node(ShaderNodeBuilder<BrightContrastNode>(graph, "BrightContrast")
                    .set("Color", make_float3(0.3f, 0.5f, 0.7f))
                    .set("Bright", 0.1f)
                    .set("Contrast", 1.2f))
      .output_color("BrightContrast::Color");

  graph.finalize(scene.get());

  log.correct_info_message("Folding BrightContrast::Color to constant (0.16, 0.6, 1.04).");
}

/*
 * Tests: blackbody with all constant inputs.
 */
TEST_F(RenderGraph, constant_fold_blackbody)
{
  builder
      .add_node(ShaderNodeBuilder<BlackbodyNode>(graph, "Blackbody").set("Temperature", 1200.0f))
      .output_color("Blackbody::Color");

  graph.finalize(scene.get());

  log.correct_info_message("Folding Blackbody::Color to constant (3.96553, 0.227897, 0).");
}

/* A Note About The Math Node
 *
 * The clamp option is implemented using graph expansion, where a
 * Clamp node named "clamp" is added and connected to the output.
 * So the final result is actually from the node "clamp".
 */

/*
 * Tests: Math with all constant inputs (clamp false).
 */
TEST_F(RenderGraph, constant_fold_math)
{
  builder
      .add_node(ShaderNodeBuilder<MathNode>(graph, "Math")
                    .set_param("math_type", NODE_MATH_ADD)
                    .set_param("use_clamp", false)
                    .set("Value1", 0.7f)
                    .set("Value2", 0.9f))
      .output_value("Math::Value");

  graph.finalize(scene.get());

  log.correct_info_message("Folding Math::Value to constant (1.6).");
}

/*
 * Tests: Math with all constant inputs (clamp true).
 */
TEST_F(RenderGraph, constant_fold_math_clamp)
{
  builder
      .add_node(ShaderNodeBuilder<MathNode>(graph, "Math")
                    .set_param("math_type", NODE_MATH_ADD)
                    .set_param("use_clamp", true)
                    .set("Value1", 0.7f)
                    .set("Value2", 0.9f))
      .output_value("Math::Value");

  graph.finalize(scene.get());

  log.correct_info_message("Folding clamp::Result to constant (1).");
}

/*
 * Graph for testing partial folds of Math with one constant argument.
 * Includes 2 tests: constant on each side.
 */
static void build_math_partial_test_graph(ShaderGraphBuilder &builder,
                                          NodeMathType type,
                                          const float constval)
{
  builder
      .add_attribute("Attribute")
      /* constant on the left */
      .add_node(ShaderNodeBuilder<MathNode>(builder.graph(), "Math_Cx")
                    .set_param("math_type", type)
                    .set_param("use_clamp", false)
                    .set("Value1", constval))
      .add_connection("Attribute::Fac", "Math_Cx::Value2")
      /* constant on the right */
      .add_node(ShaderNodeBuilder<MathNode>(builder.graph(), "Math_xC")
                    .set_param("math_type", type)
                    .set_param("use_clamp", false)
                    .set("Value2", constval))
      .add_connection("Attribute::Fac", "Math_xC::Value1")
      /* output sum */
      .add_node(ShaderNodeBuilder<MathNode>(builder.graph(), "Out")
                    .set_param("math_type", NODE_MATH_ADD)
                    .set_param("use_clamp", true))
      .add_connection("Math_Cx::Value", "Out::Value1")
      .add_connection("Math_xC::Value", "Out::Value2")
      .output_value("Out::Value");
}

/*
 * Tests: partial folding for Math Add with known 0.
 */
TEST_F(RenderGraph, constant_fold_part_math_add_0)
{
  build_math_partial_test_graph(builder, NODE_MATH_ADD, 0.0f);
  graph.finalize(scene.get());

  /* X + 0 == 0 + X == X */
  log.correct_info_message("Folding Math_Cx::Value to socket Attribute::Fac.");
  log.correct_info_message("Folding Math_xC::Value to socket Attribute::Fac.");
  log.invalid_info_message("Folding clamp::");
}

/*
 * Tests: partial folding for Math Subtract with known 0.
 */
TEST_F(RenderGraph, constant_fold_part_math_sub_0)
{
  build_math_partial_test_graph(builder, NODE_MATH_SUBTRACT, 0.0f);
  graph.finalize(scene.get());

  /* X - 0 == X */
  log.invalid_info_message("Folding Math_Cx::");
  log.correct_info_message("Folding Math_xC::Value to socket Attribute::Fac.");
  log.invalid_info_message("Folding clamp::");
}

/*
 * Tests: partial folding for Math Multiply with known 1.
 */
TEST_F(RenderGraph, constant_fold_part_math_mul_1)
{
  build_math_partial_test_graph(builder, NODE_MATH_MULTIPLY, 1.0f);
  graph.finalize(scene.get());

  /* X * 1 == 1 * X == X */
  log.correct_info_message("Folding Math_Cx::Value to socket Attribute::Fac.");
  log.correct_info_message("Folding Math_xC::Value to socket Attribute::Fac.");
  log.invalid_info_message("Folding clamp::");
}

/*
 * Tests: partial folding for Math Divide with known 1.
 */
TEST_F(RenderGraph, constant_fold_part_math_div_1)
{
  build_math_partial_test_graph(builder, NODE_MATH_DIVIDE, 1.0f);
  graph.finalize(scene.get());

  /* X / 1 == X */
  log.invalid_info_message("Folding Math_Cx::");
  log.correct_info_message("Folding Math_xC::Value to socket Attribute::Fac.");
  log.invalid_info_message("Folding clamp::");
}

/*
 * Tests: partial folding for Math Multiply with known 0.
 */
TEST_F(RenderGraph, constant_fold_part_math_mul_0)
{
  build_math_partial_test_graph(builder, NODE_MATH_MULTIPLY, 0.0f);
  graph.finalize(scene.get());

  /* X * 0 == 0 * X == 0 */
  log.correct_info_message("Folding Math_Cx::Value to constant (0).");
  log.correct_info_message("Folding Math_xC::Value to constant (0).");
  log.correct_info_message("Folding clamp::Result to constant (0)");
  log.correct_info_message("Discarding closure EmissionNode.");
}

/*
 * Tests: partial folding for Math Divide with known 0.
 */
TEST_F(RenderGraph, constant_fold_part_math_div_0)
{
  build_math_partial_test_graph(builder, NODE_MATH_DIVIDE, 0.0f);
  graph.finalize(scene.get());

  /* 0 / X == 0 */
  log.correct_info_message("Folding Math_Cx::Value to constant (0).");
  log.invalid_info_message("Folding Math_xC::");
  log.invalid_info_message("Folding clamp::");
}

/*
 * Tests: partial folding for Math Power with known 0.
 */
TEST_F(RenderGraph, constant_fold_part_math_pow_0)
{
  build_math_partial_test_graph(builder, NODE_MATH_POWER, 0.0f);
  graph.finalize(scene.get());

  /* X ^ 0 == 1 */
  log.invalid_info_message("Folding Math_Cx::");
  log.correct_info_message("Folding Math_xC::Value to constant (1).");
  log.invalid_info_message("Folding clamp::");
}

/*
 * Tests: partial folding for Math Power with known 1.
 */
TEST_F(RenderGraph, constant_fold_part_math_pow_1)
{
  build_math_partial_test_graph(builder, NODE_MATH_POWER, 1.0f);
  graph.finalize(scene.get());

  /* 1 ^ X == 1; X ^ 1 == X */
  log.correct_info_message("Folding Math_Cx::Value to constant (1)");
  log.correct_info_message("Folding Math_xC::Value to socket Attribute::Fac.");
  log.invalid_info_message("Folding clamp::");
}

/*
 * Tests: Vector Math with all constant inputs.
 */
TEST_F(RenderGraph, constant_fold_vector_math)
{
  builder
      .add_node(ShaderNodeBuilder<VectorMathNode>(graph, "VectorMath")
                    .set_param("math_type", NODE_VECTOR_MATH_SUBTRACT)
                    .set("Vector1", make_float3(1.3f, 0.5f, 0.7f))
                    .set("Vector2", make_float3(-1.7f, 0.5f, 0.7f)))
      .output_color("VectorMath::Vector");

  graph.finalize(scene.get());

  log.correct_info_message("Folding VectorMath::Vector to constant (3, 0, 0).");
}

/*
 * Graph for testing partial folds of Vector Math with one constant argument.
 * Includes 2 tests: constant on each side.
 */
static void build_vecmath_partial_test_graph(ShaderGraphBuilder &builder,
                                             NodeVectorMathType type,
                                             const float3 constval)
{
  builder
      .add_attribute("Attribute")
      /* constant on the left */
      .add_node(ShaderNodeBuilder<VectorMathNode>(builder.graph(), "Math_Cx")
                    .set_param("math_type", type)
                    .set("Vector1", constval))
      .add_connection("Attribute::Vector", "Math_Cx::Vector2")
      /* constant on the right */
      .add_node(ShaderNodeBuilder<VectorMathNode>(builder.graph(), "Math_xC")
                    .set_param("math_type", type)
                    .set("Vector2", constval))
      .add_connection("Attribute::Vector", "Math_xC::Vector1")
      /* output sum */
      .add_node(ShaderNodeBuilder<VectorMathNode>(builder.graph(), "Out")
                    .set_param("math_type", NODE_VECTOR_MATH_ADD))
      .add_connection("Math_Cx::Vector", "Out::Vector1")
      .add_connection("Math_xC::Vector", "Out::Vector2")
      .output_color("Out::Vector");
}

/*
 * Tests: partial folding for Vector Math Add with known 0.
 */
TEST_F(RenderGraph, constant_fold_part_vecmath_add_0)
{
  build_vecmath_partial_test_graph(builder, NODE_VECTOR_MATH_ADD, make_float3(0, 0, 0));
  graph.finalize(scene.get());

  /* X + 0 == 0 + X == X */
  log.correct_info_message("Folding Math_Cx::Vector to socket Attribute::Vector.");
  log.correct_info_message("Folding Math_xC::Vector to socket Attribute::Vector.");
  log.invalid_info_message("Folding Out::");
}

/*
 * Tests: partial folding for Vector Math Subtract with known 0.
 */
TEST_F(RenderGraph, constant_fold_part_vecmath_sub_0)
{
  build_vecmath_partial_test_graph(builder, NODE_VECTOR_MATH_SUBTRACT, make_float3(0, 0, 0));
  graph.finalize(scene.get());

  /* X - 0 == X */
  log.invalid_info_message("Folding Math_Cx::");
  log.correct_info_message("Folding Math_xC::Vector to socket Attribute::Vector.");
  log.invalid_info_message("Folding Out::");
}

/*
 * Tests: partial folding for Vector Math Cross Product with known 0.
 */
TEST_F(RenderGraph, constant_fold_part_vecmath_cross_0)
{
  build_vecmath_partial_test_graph(builder, NODE_VECTOR_MATH_CROSS_PRODUCT, make_float3(0, 0, 0));
  graph.finalize(scene.get());

  /* X * 0 == 0 * X == X */
  log.correct_info_message("Folding Math_Cx::Vector to constant (0, 0, 0).");
  log.correct_info_message("Folding Math_xC::Vector to constant (0, 0, 0).");
  log.correct_info_message("Folding Out::Vector to constant (0, 0, 0).");
  log.correct_info_message("Discarding closure EmissionNode.");
}

/*
 * Tests: Bump with no height input folded to Normal input.
 */
TEST_F(RenderGraph, constant_fold_bump)
{
  builder.add_node(ShaderNodeBuilder<GeometryNode>(graph, "Geometry1"))
      .add_node(ShaderNodeBuilder<BumpNode>(graph, "Bump"))
      .add_connection("Geometry1::Normal", "Bump::Normal")
      .output_color("Bump::Normal");

  graph.finalize(scene.get());

  log.correct_info_message("Folding Bump::Normal to socket Geometry1::Normal.");
}

/*
 * Tests: Bump with no inputs folded to Geometry::Normal.
 */
TEST_F(RenderGraph, constant_fold_bump_no_input)
{
  builder.add_node(ShaderNodeBuilder<BumpNode>(graph, "Bump")).output_color("Bump::Normal");

  graph.finalize(scene.get());

  log.correct_info_message("Folding Bump::Normal to socket geometry::Normal.");
}

template<class T> void init_test_curve(array<T> &buffer, T start, T end, const int steps)
{
  buffer.resize(steps);

  for (int i = 0; i < steps; i++) {
    buffer[i] = mix(start, end, float(i) / (steps - 1));
  }
}

/*
 * Tests:
 *  - Folding of RGB Curves with all constant inputs.
 */
TEST_F(RenderGraph, constant_fold_rgb_curves)
{
  array<float3> curve;
  init_test_curve(curve, make_float3(0.0f, 0.25f, 1.0f), make_float3(1.0f, 0.75f, 0.0f), 257);

  builder
      .add_node(ShaderNodeBuilder<RGBCurvesNode>(graph, "Curves")
                    .set_param("curves", curve)
                    .set_param("min_x", 0.1f)
                    .set_param("max_x", 0.9f)
                    .set("Fac", 0.5f)
                    .set("Color", make_float3(0.3f, 0.5f, 0.7f)))
      .output_color("Curves::Color");

  graph.finalize(scene.get());

  log.correct_info_message("Folding Curves::Color to constant (0.275, 0.5, 0.475).");
}

/*
 * Tests:
 *  - Folding of RGB Curves with zero Fac.
 */
TEST_F(RenderGraph, constant_fold_rgb_curves_fac_0)
{
  array<float3> curve;
  init_test_curve(curve, make_float3(0.0f, 0.25f, 1.0f), make_float3(1.0f, 0.75f, 0.0f), 257);

  builder.add_attribute("Attribute")
      .add_node(ShaderNodeBuilder<RGBCurvesNode>(graph, "Curves")
                    .set_param("curves", curve)
                    .set_param("min_x", 0.1f)
                    .set_param("max_x", 0.9f)
                    .set("Fac", 0.0f))
      .add_connection("Attribute::Color", "Curves::Color")
      .output_color("Curves::Color");

  graph.finalize(scene.get());

  log.correct_info_message("Folding Curves::Color to socket Attribute::Color.");
}

/*
 * Tests:
 *  - Folding of RGB Curves with zero Fac and all constant inputs.
 */
TEST_F(RenderGraph, constant_fold_rgb_curves_fac_0_const)
{
  array<float3> curve;
  init_test_curve(curve, make_float3(0.0f, 0.25f, 1.0f), make_float3(1.0f, 0.75f, 0.0f), 257);

  builder
      .add_node(ShaderNodeBuilder<RGBCurvesNode>(graph, "Curves")
                    .set_param("curves", curve)
                    .set_param("min_x", 0.1f)
                    .set_param("max_x", 0.9f)
                    .set("Fac", 0.0f)
                    .set("Color", make_float3(0.3f, 0.5f, 0.7f)))
      .output_color("Curves::Color");

  graph.finalize(scene.get());

  log.correct_info_message("Folding Curves::Color to constant (0.3, 0.5, 0.7).");
}

/*
 * Tests:
 *  - Folding of Vector Curves with all constant inputs.
 */
TEST_F(RenderGraph, constant_fold_vector_curves)
{
  array<float3> curve;
  init_test_curve(curve, make_float3(0.0f, 0.25f, 1.0f), make_float3(1.0f, 0.75f, 0.0f), 257);

  builder
      .add_node(ShaderNodeBuilder<VectorCurvesNode>(graph, "Curves")
                    .set_param("curves", curve)
                    .set_param("min_x", 0.1f)
                    .set_param("max_x", 0.9f)
                    .set("Fac", 0.5f)
                    .set("Vector", make_float3(0.3f, 0.5f, 0.7f)))
      .output_color("Curves::Vector");

  graph.finalize(scene.get());

  log.correct_info_message("Folding Curves::Vector to constant (0.275, 0.5, 0.475).");
}

/*
 * Tests:
 *  - Folding of Vector Curves with zero Fac.
 */
TEST_F(RenderGraph, constant_fold_vector_curves_fac_0)
{
  array<float3> curve;
  init_test_curve(curve, make_float3(0.0f, 0.25f, 1.0f), make_float3(1.0f, 0.75f, 0.0f), 257);

  builder.add_attribute("Attribute")
      .add_node(ShaderNodeBuilder<VectorCurvesNode>(graph, "Curves")
                    .set_param("curves", curve)
                    .set_param("min_x", 0.1f)
                    .set_param("max_x", 0.9f)
                    .set("Fac", 0.0f))
      .add_connection("Attribute::Vector", "Curves::Vector")
      .output_color("Curves::Vector");

  graph.finalize(scene.get());

  log.correct_info_message("Folding Curves::Vector to socket Attribute::Vector.");
}

/*
 * Tests:
 *  - Folding of Color Ramp with all constant inputs.
 */
TEST_F(RenderGraph, constant_fold_rgb_ramp)
{
  array<float3> curve;
  array<float> alpha;
  init_test_curve(curve, make_float3(0.0f, 0.25f, 0.5f), make_float3(0.25f, 0.5f, 0.75f), 9);
  init_test_curve(alpha, 0.75f, 1.0f, 9);

  builder
      .add_node(ShaderNodeBuilder<RGBRampNode>(graph, "Ramp")
                    .set_param("ramp", curve)
                    .set_param("ramp_alpha", alpha)
                    .set_param("interpolate", true)
                    .set("Fac", 0.56f))
      .add_node(ShaderNodeBuilder<MixNode>(graph, "Mix").set_param("mix_type", NODE_MIX_ADD))
      .add_connection("Ramp::Color", "Mix::Color1")
      .add_connection("Ramp::Alpha", "Mix::Color2")
      .output_color("Mix::Color");

  graph.finalize(scene.get());

  log.correct_info_message("Folding Ramp::Color to constant (0.14, 0.39, 0.64).");
  log.correct_info_message("Folding Ramp::Alpha to constant (0.89).");
}

/*
 * Tests:
 *  - Folding of Color Ramp with all constant inputs (interpolate false).
 */
TEST_F(RenderGraph, constant_fold_rgb_ramp_flat)
{
  array<float3> curve;
  array<float> alpha;
  init_test_curve(curve, make_float3(0.0f, 0.25f, 0.5f), make_float3(0.25f, 0.5f, 0.75f), 9);
  init_test_curve(alpha, 0.75f, 1.0f, 9);

  builder
      .add_node(ShaderNodeBuilder<RGBRampNode>(graph, "Ramp")
                    .set_param("ramp", curve)
                    .set_param("ramp_alpha", alpha)
                    .set_param("interpolate", false)
                    .set("Fac", 0.56f))
      .add_node(ShaderNodeBuilder<MixNode>(graph, "Mix").set_param("mix_type", NODE_MIX_ADD))
      .add_connection("Ramp::Color", "Mix::Color1")
      .add_connection("Ramp::Alpha", "Mix::Color2")
      .output_color("Mix::Color");

  graph.finalize(scene.get());

  log.correct_info_message("Folding Ramp::Color to constant (0.125, 0.375, 0.625).");
  log.correct_info_message("Folding Ramp::Alpha to constant (0.875).");
}

/*
 * Tests:
 *  - Folding of redundant conversion of float to color to float.
 */
TEST_F(RenderGraph, constant_fold_convert_float_color_float)
{
  builder.add_attribute("Attribute")
      .add_node(ShaderNodeBuilder<InvertNode>(graph, "Invert").set("Fac", 0.0f))
      .add_connection("Attribute::Fac", "Invert::Color")
      .output_value("Invert::Color");

  graph.finalize(scene.get());

  log.correct_info_message("Folding Invert::Color to socket convert_float_to_color::value_color.");
  log.correct_info_message(
      "Folding convert_color_to_float::value_float to socket Attribute::Fac.");
}

/*
 * Tests:
 *  - Folding of redundant conversion of color to vector to color.
 */
TEST_F(RenderGraph, constant_fold_convert_color_vector_color)
{
  builder.add_attribute("Attribute")
      .add_node(ShaderNodeBuilder<VectorMathNode>(graph, "VecAdd")
                    .set_param("math_type", NODE_VECTOR_MATH_ADD)
                    .set("Vector2", make_float3(0, 0, 0)))
      .add_connection("Attribute::Color", "VecAdd::Vector1")
      .output_color("VecAdd::Vector");

  graph.finalize(scene.get());

  log.correct_info_message(
      "Folding VecAdd::Vector to socket convert_color_to_vector::value_vector.");
  log.correct_info_message(
      "Folding convert_vector_to_color::value_color to socket Attribute::Color.");
}

/*
 * Tests:
 *  - NOT folding conversion of color to float to color.
 */
TEST_F(RenderGraph, constant_fold_convert_color_float_color)
{
  builder.add_attribute("Attribute")
      .add_node(ShaderNodeBuilder<MathNode>(graph, "MathAdd")
                    .set_param("math_type", NODE_MATH_ADD)
                    .set("Value2", 0.0f))
      .add_connection("Attribute::Color", "MathAdd::Value1")
      .output_color("MathAdd::Value");

  graph.finalize(scene.get());

  log.correct_info_message(
      "Folding MathAdd::Value to socket convert_color_to_float::value_float.");
  log.invalid_info_message("Folding convert_float_to_color::");
}

/*
 * Tests:
 *  - Stochastic sampling with math multiply node.
 */
TEST_F(RenderGraph, stochastic_sample_math_multiply)
{
  builder.add_attribute("Attribute")
      .add_node(ShaderNodeBuilder<MathNode>(graph, "MathMultiply")
                    .set_param("math_type", NODE_MATH_MULTIPLY))
      .add_node(ShaderNodeBuilder<ScatterVolumeNode>(graph, "ScatterVolume"))
      .add_connection("Attribute::Fac", "MathMultiply::Value1")
      .add_connection("MathMultiply::Value", "ScatterVolume::Density")
      .output_volume_closure("ScatterVolume::Volume");

  graph.finalize(scene.get());

  log.correct_info_message("Volume attribute node Attribute uses stochastic sampling");
}

/*
 * Tests:
 *  - No stochastic sampling with math power node.
 */
TEST_F(RenderGraph, not_stochastic_sample_math_power)
{
  builder.add_attribute("Attribute")
      .add_node(
          ShaderNodeBuilder<MathNode>(graph, "MathPower").set_param("math_type", NODE_MATH_POWER))
      .add_node(ShaderNodeBuilder<ScatterVolumeNode>(graph, "ScatterVolume"))
      .add_connection("Attribute::Fac", "MathPower::Value1")
      .add_connection("MathPower::Value", "ScatterVolume::Density")
      .output_volume_closure("ScatterVolume::Volume");

  graph.finalize(scene.get());

  log.invalid_info_message("Volume attribute node Attribute uses stochastic sampling");
}

/*
 * Tests:
 *  - Stochastic sampling temperature with map range, principled volume and mix closure.
 */
TEST_F(RenderGraph, stochastic_sample_principled_volume_mix)
{
  builder.add_attribute("Attribute")
      .add_node(ShaderNodeBuilder<MapRangeNode>(graph, "MapRange"))
      .add_node(ShaderNodeBuilder<MixClosureNode>(graph, "MixClosure").set("Fac", 0.5f))
      .add_node(ShaderNodeBuilder<PrincipledVolumeNode>(graph, "PrincipledVolume1"))
      .add_node(ShaderNodeBuilder<PrincipledVolumeNode>(graph, "PrincipledVolume2"))
      .add_connection("Attribute::Color", "MapRange::Value")
      .add_connection("MapRange::Result", "PrincipledVolume1::Temperature")
      .add_connection("Attribute::Fac", "PrincipledVolume2::Density")
      .add_connection("PrincipledVolume1::Volume", "MixClosure::Closure1")
      .add_connection("PrincipledVolume2::Volume", "MixClosure::Closure2")
      .output_volume_closure("MixClosure::Closure");

  graph.finalize(scene.get());

  log.correct_info_message("Volume attribute node Attribute uses stochastic sampling");
}

CCL_NAMESPACE_END
