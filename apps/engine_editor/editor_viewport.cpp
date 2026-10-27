#include "editor_viewport.h"

#include <systems/renderer/capture.h>
#include <systems/renderer/request.h>

#include <algorithm>
#include <span>

namespace engine::editor {

bool Viewport::build(const gfx::Device& device, const doc::Document& document,
                     const std::string& base_dir, const Options& options, std::string& error) {
  destroy();
  device_ = &device;
  options_ = options;
  base_dir_ = base_dir;
  if (!doc_scene::read_document_scene(document, base_dir, scene_, error)) return false;
  return load(error);
}

bool Viewport::load(std::string& error) {
  renderer::SceneDesc desc = scene_.desc;
  desc.ddc = options_.ddc;
  desc.terrain_tiles = desc.terrain.enabled && !renderer::terrain_moves(desc.terrain) &&
                       renderer::terrain_has_millimetres(desc.terrain);
  data_ = renderer::SceneData{};
  if (!renderer::load_scene(desc, data_, error)) return false;
  // Each mesh's box from its vertices, read now while the merged float positions exist: what the
  // selection is outlined by.
  mesh_lo_.assign(data_.parts.size(), Vec3{});
  mesh_hi_.assign(data_.parts.size(), Vec3{});
  for (u32 m = 0; m < data_.parts.size(); ++m) {
    const u32 first = data_.parts[m].first_vertex;
    const u32 end = m + 1 < data_.parts.size() ? data_.parts[m + 1].first_vertex
                                               : static_cast<u32>(data_.lod.mesh.vertices.size());
    Vec3 lo{1e30f, 1e30f, 1e30f};
    Vec3 hi{-1e30f, -1e30f, -1e30f};
    for (u32 v = first; v < end && v < data_.lod.mesh.vertices.size(); ++v) {
      const Vec3 p = data_.lod.mesh.vertices[v];
      lo = Vec3{std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
      hi = Vec3{std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
    }
    if (lo.x > hi.x) lo = hi = Vec3{};
    mesh_lo_[m] = lo;
    mesh_hi_[m] = hi;
  }
  renderer::RenderSettings settings = options_.settings;
  settings.terrain_tiles = settings.terrain_tiles || desc.terrain_tiles;
  renderer::resolve_settings(settings, device_->features(), &data_, resolved_);
  const renderer::RenderAvailability availability =
      renderer::check_availability(resolved_, device_->features());
  if (availability != renderer::RenderAvailability::Ok) {
    error = renderer::unavailable_reason(availability, *device_);
    return false;
  }
  gpu_ = std::make_unique<renderer::GpuScene>();
  if (resolved_.terrain_levels) {
    ground_ = std::make_unique<renderer::MovingGround>();
    if (!ground_->prepare(data_, resolved_, framing_camera(), &error, !options_.offscreen))
      return false;
  }
  const renderer::ResolvedSettings requested = resolved_;
  if (!gpu_->create(*device_, data_, resolved_, &error,
                    ground_ != nullptr ? ground_->level_set() : nullptr))
    return false;
  resolved_ = gpu_->resolved();
  if (ground_ != nullptr && !ground_->start(*gpu_, resolved_, options_.offscreen, &error))
    return false;
  renderer::SceneRenderer::Desc rd;
  rd.width = options_.width;
  rd.height = options_.height;
  rd.color_format = options_.color_format;
  rd.display = gfx::DisplayEncoding::Sdr;  // the overlay composites onto SDR only (ADR-0054)
  rd.offscreen = options_.offscreen;
  rd.views = renderer::view_set_desc(resolved_.settings);
  renderer_ = std::make_unique<renderer::SceneRenderer>();
  return renderer_->create(*device_, *gpu_, requested, rd, &error);
}

void Viewport::destroy() noexcept {
  preview_instance_ = ~0u;
  if (ground_ != nullptr) ground_->finish();
  if (renderer_ != nullptr) renderer_->destroy();
  renderer_.reset();
  if (gpu_ != nullptr) gpu_->destroy();
  gpu_.reset();
  ground_.reset();
}

void Viewport::advance_ground(const renderer::Camera& camera) {
  if (ground_ == nullptr) return;
  ground_->follow_tiles(camera.position);
  ground_->frame(camera);
}

bool Viewport::sync(const doc::Document& document, bool& rebuilt, std::string& error) {
  rebuilt = false;
  doc_scene::SceneChanges changes;
  if (!doc_scene::changes_since(document, scene_, changes, error)) return false;
  if (changes.rebuild) {
    preview_instance_ = ~0u;  // the instances are numbered again; a preview of one is gone
    const u32 width = renderer_ != nullptr ? renderer_->width() : options_.width;
    const u32 height = renderer_ != nullptr ? renderer_->height() : options_.height;
    options_.width = width;
    options_.height = height;
    destroy();
    if (!doc_scene::read_document_scene(document, base_dir_, scene_, error)) return false;
    rebuilt = true;
    return load(error);
  }
  if (changes.moved.empty()) return true;
  return renderer_->move_instances(changes.moved, changes.placements, &error);
}

renderer::Camera Viewport::framing_camera() const noexcept {
  // 2.6 radii out: `orbit_camera` measures its distance in tenths of the radius, so 26. (Stage 1
  // passed 2.6 radii as the distance, which is 0.26 r² metres: inside any scene smaller than
  // ten metres across — the camera stood inside the nearer of two cubes.)
  return renderer::orbit_camera(data_.center, data_.radius, 26.0f, 0);
}

bool Viewport::pick(u32 x, u32 y, Id128& out, std::string& error) {
  out = Id128{};
  renderer::CaptureChannels channels;
  channels.color = false;
  channels.ids = true;
  renderer::CapturedFrame shot;
  if (!renderer_->read_last_frame(channels, shot, &error)) return false;
  if (x >= shot.width || y >= shot.height) return true;
  const u32 instance = shot.ids[(y * shot.width + x) * renderer::k_id_words];
  if (instance == renderer::k_no_id || instance >= scene_.records.size()) return true;
  out = scene_.records[instance];
  return true;
}

bool Viewport::preview(u32 instance, const renderer::SceneInstance& placed, std::string& error) {
  if (instance >= scene_.desc.instances.size()) {
    error = "no such instance to preview";
    return false;
  }
  if (preview_instance_ != ~0u && preview_instance_ != instance && !cancel_preview(error))
    return false;
  if (!renderer_->move_instances(std::span<const u32>(&instance, 1),
                                 std::span<const renderer::SceneInstance>(&placed, 1), &error))
    return false;
  preview_instance_ = instance;
  preview_placement_ = placed;
  return true;
}

bool Viewport::cancel_preview(std::string& error) {
  if (preview_instance_ == ~0u) return true;
  const u32 instance = preview_instance_;
  preview_instance_ = ~0u;
  const renderer::SceneInstance& document_placement = scene_.desc.instances[instance];
  return renderer_->move_instances(std::span<const u32>(&instance, 1),
                                   std::span<const renderer::SceneInstance>(&document_placement, 1),
                                   &error);
}

const renderer::SceneInstance* Viewport::placement(u32 instance) const noexcept {
  if (instance >= scene_.desc.instances.size()) return nullptr;
  return instance == preview_instance_ ? &preview_placement_ : &scene_.desc.instances[instance];
}

bool Viewport::instance_box(u32 instance, WorldPos corners[8]) const noexcept {
  const renderer::SceneInstance* placed = placement(instance);
  if (placed == nullptr) return false;
  const renderer::SceneInstance& source = *placed;
  if (source.mesh >= mesh_lo_.size() || source.mesh >= data_.mesh_fit.size()) return false;
  const Mat4& fit = data_.mesh_fit[source.mesh];
  // Placed as `load_scene` places it: the linear part of transform-then-fit about the instance's
  // translation, which is the origin plus the fit's own offset carried through (scene.h,
  // `instance_translation`).
  const Mat4 linear = mat4_from_transform(source.transform) * fit;
  const WorldPos at = renderer::instance_translation(source, fit);
  const Vec3 lo = mesh_lo_[source.mesh];
  const Vec3 hi = mesh_hi_[source.mesh];
  for (u32 c = 0; c < 8; ++c) {
    const Vec3 p{(c & 1) ? hi.x : lo.x, (c & 2) ? hi.y : lo.y, (c & 4) ? hi.z : lo.z};
    const Vec3 d = transform_direction(linear, p);
    corners[c] = at + DVec3{static_cast<f64>(d.x), static_cast<f64>(d.y), static_cast<f64>(d.z)};
  }
  return true;
}

}  // namespace engine::editor
