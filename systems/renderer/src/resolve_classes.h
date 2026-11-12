#pragma once

// **The resolve by pixel class** (roadmap R151; docs/subsystems/renderer.md, "The resolve by pixel
// class"; the measurements, docs/experiments/resolve-by-class-2026-11-12.md). What a frame's
// resolve pass draws under `renderer.resolve.classes`: the plan — which classes, and which of them
// by the module without water — and the per-class draws of one view. The classify pass and the
// pipelines are `SceneRenderer`'s (`create_resolve_class_pipelines`, resolve_classes.cpp); the
// classes and their depths are gfx's (domain/gfx/resolve_classes.h, resolve_classify.slang).

#include <core/base/types.h>
#include <domain/gfx/commands.h>
#include <domain/gfx/resolve_classes.h>
#include <domain/gfx/rhi.h>
#include <systems/renderer/settings.h>

namespace engine::renderer {

// The resolve module of a combination (scene_renderer.cpp's `resolve_module`), for the split's
// pipelines: the scene's own, and the same combination with no water.
const char* resolve_module_name(bool shadows, bool water, bool terrain, bool stochastic) noexcept;

struct ResolveClassPlan {
  u32 classes = 0;  // gfx::ResolveClass bits the resolve draws; 0: the one fullscreen draw
  u32 plain = 0;    // of them, the ones drawn by `plain_module`
  gfx::PipelineHandle classify;
  gfx::PipelineHandle module;        // the scene's own resolve module, depth-equal
  gfx::PipelineHandle plain_module;  // the same combination without water, depth-equal
};

// What a frame knows that decides its plan.
struct ResolveClassFrame {
  ResolveClassMode mode = ResolveClassMode::Off;  // the tunable, this frame
  bool water = false;      // the scene has a water material: the two water classes exist
  bool open_sea = false;   // an open body: any pixel may be the sea's, so every class needs water
  bool resample = false;   // a Panini view: the resolve reads a source pixel the map names
  bool measuring = false;  // a stop word (R115): the frame is a measurement's, drawn by one module
  // The split's own measurement switches (settings.h, `ResolveClassMeasure`).
  u32 mask = gfx::k_resolve_class_all;
  bool wet_plain = false;
};

// The plan of one frame from the pipelines that exist: nothing without the classify pass and the
// module's depth-equal pipeline (a renderer made under `off`), every class the scene can hold
// otherwise, and under `modules` the classes that cannot touch water by the plain module — unless
// the scene has an open sea (an empty pixel and a pixel below the level are the sea's), the view
// resamples (the classify pass and the resolve would each have to map a pixel to the same source
// pixel in two shaders' arithmetic), or the frame is a measurement's.
ResolveClassPlan plan_resolve_classes(const ResolveClassFrame& frame, gfx::PipelineHandle classify,
                                      gfx::PipelineHandle module,
                                      gfx::PipelineHandle plain_module) noexcept;

// One view's resolve as a draw per class of `plan`: the fullscreen triangle at each class's depth
// (the viewport's depth range; the rectangle with the renderer's y flip, as every pass sets it),
// the plain module's classes first. The bindless set and the push block are bound by the caller.
void record_resolve_class_draws(gfx::CommandList commands, const ResolveClassPlan& plan, u32 x,
                                u32 y, u32 width, u32 height);

}  // namespace engine::renderer
