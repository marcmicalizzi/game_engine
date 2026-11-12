#include "resolve_classes.h"

#include "renderer_log.h"

#include <domain/gfx/capture.h>
#include <domain/gfx/pipeline.h>
#include <domain/gfx/pipeline_batch.h>
#include <systems/renderer/scene_renderer.h>

#include <cstring>

namespace engine::renderer {

ResolveClassPlan plan_resolve_classes(const ResolveClassFrame& frame, gfx::PipelineHandle classify,
                                      gfx::PipelineHandle module,
                                      gfx::PipelineHandle plain_module) noexcept {
  ResolveClassPlan plan;
  if (frame.mode == ResolveClassMode::Off || !classify.valid() || !module.valid()) return plan;
  plan.classify = classify;
  plan.module = module;
  // Every class the classify pass can write is drawn: a class no pixel holds costs its triangle's
  // rejection and nothing else, and a class left out would leave its pixels the clear.
  plan.classes = frame.water ? gfx::k_resolve_class_all
                             : gfx::k_resolve_class_all & ~gfx::k_resolve_class_water_mask;
  if (frame.mode == ResolveClassMode::Modules && frame.water && plain_module.valid() &&
      !frame.open_sea && !frame.resample && !frame.measuring) {
    plan.plain = plan.classes & ~gfx::k_resolve_class_water_mask;
    if (frame.wet_plain) plan.plain |= 1u << static_cast<u32>(gfx::ResolveClass::Wet);
    plan.plain_module = plain_module;
  }
  plan.classes &= frame.mask;
  plan.plain &= frame.mask;
  if (plan.classes == 0) plan = ResolveClassPlan{};
  return plan;
}

void record_resolve_class_draws(gfx::CommandList commands, const ResolveClassPlan& plan, u32 x,
                                u32 y, u32 width, u32 height) {
  gfx::Viewport viewport{};
  viewport.x = static_cast<f32>(x);
  viewport.y = static_cast<f32>(y + height);
  viewport.width = static_cast<f32>(width);
  viewport.height = -static_cast<f32>(height);
  const u32 masks[2] = {plan.plain, plan.classes & ~plan.plain};
  const gfx::PipelineHandle pipelines[2] = {plan.plain_module, plan.module};
  for (u32 pass = 0; pass < 2; ++pass) {
    if (masks[pass] == 0) continue;
    commands.bind_pipeline(gfx::BindPoint::Graphics, pipelines[pass]);
    for (u32 c = 0; c < gfx::k_resolve_classes; ++c) {
      if ((masks[pass] & (1u << c)) == 0) continue;
      // The triangle's z is 0 and the range is one value, so every fragment's depth is the
      // class's own, exactly: what the classify pass wrote for the class's pixels.
      viewport.min_depth = viewport.max_depth = gfx::resolve_class_depth(c);
      commands.set_viewport(viewport);
      commands.draw(3, 1, 0, 0);
    }
  }
}

// The classify pass, and the resolve's depth-equal pipelines for each colour format the resolve
// draws into. Inside `create_pipelines`' batch they compile beside everything else; from a frame
// pass's first registration, in a batch of their own.
bool SceneRenderer::create_resolve_class_pipelines(bool linear, std::string* error) {
  const ResolveClassMode mode = resolve_classes_tunable();
  if (mode == ResolveClassMode::Off || resolved_.direct) return true;
  const gfx::Device& device = *device_;
  gfx::BindlessSet& bindless = scene_->bindless();
  const bool water = scene_->has_water();
  const bool terrain = scene_->has_terrain_materials();
  const bool stochastic = scene_->has_stochastic_materials();
  const gfx::Shader* module =
      shaders_.get(resolve_module_name(resolved_.shadows, water, terrain, stochastic), error);
  if (module == nullptr) return false;
  // The module without water, for a scene with water (`modules`): the pixels that cannot touch
  // water stop carrying its registers and its tests.
  const gfx::Shader* plain = nullptr;
  if (water && mode == ResolveClassMode::Modules) {
    plain = shaders_.get(resolve_module_name(resolved_.shadows, false, terrain, stochastic), error);
    if (plain == nullptr) return false;
  }
  gfx::PipelineBatch batch(device);
  if (!pipelines_.classify.valid()) {
    const gfx::Shader* classify =
        shaders_.get(water ? "resolve_classify_water" : "resolve_classify", error);
    if (classify == nullptr) return false;
    gfx::GraphicsPipelineDesc desc;
    desc.vertex = classify->module;
    desc.vertex_entry = "vs_classify";
    desc.fragment = classify->module;
    desc.fragment_entry = "fs_classify";
    desc.layout = bindless.pipeline_layout();
    desc.depth_format = gfx::Format::D32Sfloat;
    desc.depth_test = true;
    desc.depth_write = true;
    desc.depth_compare = gfx::CompareOp::Always;
    if (!gfx::create_graphics_pipeline(device, desc, pipelines_.classify, error)) return false;
  }
  const gfx::Format formats[2] = {desc_.color_format, k_frame_pass_format};
  for (u32 f = 0; f < (linear ? 2u : 1u); ++f) {
    gfx::GraphicsPipelineDesc desc;
    desc.vertex = module->module;
    desc.vertex_entry = "vs_fullscreen";
    desc.fragment = module->module;
    desc.fragment_entry = "fs_resolve";
    desc.layout = bindless.pipeline_layout();
    desc.color_format = formats[f];
    desc.depth_format = gfx::Format::D32Sfloat;
    desc.depth_test = true;
    desc.depth_write = false;
    desc.depth_compare = gfx::CompareOp::Equal;
    if (!pipelines_.resolve_class[f].valid() &&
        !gfx::create_graphics_pipeline(device, desc, pipelines_.resolve_class[f], error)) {
      return false;
    }
    if (plain != nullptr && !pipelines_.resolve_class_plain[f].valid()) {
      desc.vertex = desc.fragment = plain->module;
      if (!gfx::create_graphics_pipeline(device, desc, pipelines_.resolve_class_plain[f], error)) {
        return false;
      }
    }
  }
  ENGINE_LOG_INFO(log_renderer, "resolve classes",
                  log::field("mode", mode == ResolveClassMode::Modules ? "modules" : "draws"),
                  log::field("water", water), log::field("plain_module", plain != nullptr),
                  log::field("linear", linear));
  return batch.end(error);
}

bool SceneRenderer::resolve_class_census(ResolveClassCensus& out, std::string* error) {
  out = ResolveClassCensus{};
  if (resolve_classes_drawn_ == 0) {
    if (error != nullptr) *error = "the last frame's resolve drew no classes";
    return false;
  }
  frames_.wait_idle();
  gfx::Capture shot;
  if (!gfx::capture_image(*device_, targets_.depth, gfx::ImageLayout::DepthAttachment, shot,
                          error)) {
    return false;
  }
  const u8* bytes = shot.bytes.data();
  const auto class_at = [&](u32 x, u32 y) noexcept {
    f32 depth = 0.0f;
    std::memcpy(&depth, bytes + (u64{y} * shot.width + x) * sizeof(f32), sizeof(f32));
    // The class's depth to the bit: a sixteenth's multiple is exact, so nothing else is a class.
    const u32 steps = depth > 0.0f && depth < 1.0f
                          ? static_cast<u32>(depth / gfx::k_resolve_class_depth_step + 0.5f)
                          : 0u;
    const u32 c = steps > 0 ? steps - 1 : gfx::k_resolve_classes;
    const f32 expected = gfx::resolve_class_depth(c);
    return c < gfx::k_resolve_classes && std::memcmp(&expected, &depth, sizeof(f32)) == 0
               ? c
               : gfx::k_resolve_classes;
  };
  // A fragment shader runs in 2x2 quads on the target's even pixels: a quad is run once by the
  // one draw, and once by every class's draw that has a pixel in it.
  for (u32 y = 0; y < shot.height; y += 2) {
    for (u32 x = 0; x < shot.width; x += 2) {
      u32 held = 0;
      for (u32 q = 0; q < 4; ++q) {
        const u32 px = x + (q & 1u);
        const u32 py = y + (q >> 1);
        if (px >= shot.width || py >= shot.height) continue;
        const u32 c = class_at(px, py);
        if (c < gfx::k_resolve_classes) {
          ++out.pixels[c];
          held |= 1u << c;
        } else {
          ++out.unclassed;
        }
      }
      ++out.quads_total;
      u32 classes = 0;
      for (u32 c = 0; c < gfx::k_resolve_classes; ++c) {
        if ((held & (1u << c)) == 0) continue;
        ++out.quads[c];
        ++classes;
      }
      if (classes > 1) ++out.quads_mixed;
    }
  }
  return true;
}

void SceneRenderer::log_resolve_class_census() {
  ResolveClassCensus census;
  if (!resolve_class_census(census, nullptr)) return;
  u64 quad_draws = 0;
  for (u32 c = 0; c < gfx::k_resolve_classes; ++c)
    quad_draws += census.quads[c];
  ENGINE_LOG_WARN(
      log_renderer, "resolve class census", log::field("sky", census.pixels[0]),
      log::field("rigid", census.pixels[1]), log::field("ground", census.pixels[2]),
      log::field("masked", census.pixels[3]), log::field("deformed", census.pixels[4]),
      log::field("water", census.pixels[5]), log::field("wet", census.pixels[6]),
      log::field("unclassed", census.unclassed), log::field("quads", census.quads_total),
      log::field("quads_mixed", census.quads_mixed), log::field("quad_draws", quad_draws),
      log::field("quads_sky", census.quads[0]), log::field("quads_rigid", census.quads[1]),
      log::field("quads_ground", census.quads[2]), log::field("quads_masked", census.quads[3]),
      log::field("quads_deformed", census.quads[4]), log::field("quads_water", census.quads[5]),
      log::field("quads_wet", census.quads[6]), log::field("drawn", resolve_classes_drawn_),
      log::field("plain", resolve_classes_plain_));
}

}  // namespace engine::renderer
