#pragma once

// **What the drawn ground is at a point** (G201 stage 2; renderer.md, "A tree seated by its
// footprint"; apps.md, "engine-content ground sample"): the scene reader's own ground after its
// planning pass — the terrain with the scene's edits and every edit its placement entries plan,
// the ground a `ground: true` instance stands on — asked at a point, and over a disc round it, by
// the functions the load seats a tree with (`scene_gen::ground_disc`, `seated_y_mm`). What an
// author placing by hand asks before they write a translation, so the cuts, the benches and the
// planned edits are seen without a copy of their profiles.

#include <core/json/json_value.h>
#include <core/math/world.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/terrain.h>

#include <memory>
#include <string>

namespace engine::renderer {

// One question: a point in x and z, metres, and a disc's radius round it (0: the point alone).
struct GroundQuestion {
  f64 x_m = 0.0;
  f64 z_m = 0.0;
  f64 radius_m = 0.0;
};

class SceneGround {
 public:
  SceneGround() = default;
  SceneGround(const SceneGround&) = delete;
  SceneGround& operator=(const SceneGround&) = delete;
  ~SceneGround();

  // Reads the scene as a load does, through its ground plan (`SceneFileOptions::ground_only`), or
  // through its placements too with `placements` (their shapes then join the exclusion, so
  // `excluded` names `footprints` where an entry's shapes stand). `ddc` is the host's root, as
  // `--ddc` is. `uncut` asks the same terrain less every edit, the scene's and the plans'. False,
  // with the reader's sentence, for a scene that does not read or has no terrain.
  bool open(const std::string& scene_path, const std::string& ddc, bool uncut, bool placements,
            std::string& error);

  // The head line: the scene, the provider, whether it is the uncut ground, the terrain's own grid
  // spacing (the bound on how far this bilinear answer stands from the drawn triangles, for a
  // ground drawn from that grid; a tile set's own spacing is the caller's to add), and what a disc
  // samples.
  JsonValue head() const;

  // One answer: `x_m`, `z_m`, `y_m` (the surface, shortest round-trip), `hole`, `excluded` (the
  // kinds that cover the point: holes, channels, water, footprints), and the gradient `gx`, `gz`
  // (the plane over a disc of `k_gradient_radius_m`). With a radius, `disc`: its `min_m`, `max_m`,
  // `drop_m` (the centre less the lowest), `samples` (the rim's that are ground), the fitted
  // plane's `gx`, `gz`, `residual_m`, the four-byte `slope` words; and with `skirt_m` positive,
  // `seat_m`: the highest an origin there may stand so that a skirt that deep is the seat's margin
  // under the lowest ground the rim stands over (`scene_gen::seated_y_mm`).
  JsonValue sample(const GroundQuestion& q, f64 skirt_m) const;

  static constexpr f64 k_gradient_radius_m = 0.25;

  // The placement generators the scene names that this executable does not carry, read through
  // the plan alone: not refused, since nothing of them expands, but whatever they would have
  // planned for the ground is missing from it. The head line's `not_carried`.
  const Vector<std::string>& not_carried() const noexcept { return not_carried_; }

 private:
  Vector<std::string> not_carried_;
  SceneDesc desc_;
  std::unique_ptr<TerrainSampler> sampler_;
  std::string path_;
  bool uncut_ = false;
};

}  // namespace engine::renderer
