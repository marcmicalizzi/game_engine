#include <core/json/json.h>
#include <domain/scene_gen/exclusion.h>
#include <domain/scene_gen/scene_gen.h>
#include <domain/scene_gen/seat.h>
#include <foundation/io/vfs.h>
#include <systems/renderer/ground_sample.h>

namespace engine::renderer {

namespace {

// The placement generators the file names (its `ruins` entries are the generator "ruins") that
// this executable does not carry, in the file's order, each once.
Vector<std::string> generators_not_carried(const std::string& scene_path) {
  Vector<std::string> out;
  std::string text;
  JsonValue root;
  if (io::read_file(scene_path, text) != io::Status::Ok || !parse_json(text, root).ok) return out;
  const scene_gen::GeneratorRegistry& registry = scene_gen::GeneratorRegistry::global();
  const auto note = [&](std::string_view name) {
    if (registry.find_placement(name) != nullptr) return;
    for (const std::string& seen : out)
      if (seen == name) return;
    out.push_back(std::string(name));
  };
  const JsonValue* ruins = root.find("ruins");
  if (ruins != nullptr && ruins->is_array() && ruins->size() > 0) note("ruins");
  const JsonValue* placements = root.find("placements");
  if (placements != nullptr && placements->is_array()) {
    for (usize i = 0; i < placements->size(); ++i) {
      const JsonValue* generator = (*placements)[i].find("generator");
      std::string_view name;
      if (generator != nullptr && generator->get_string(name)) note(name);
    }
  }
  return out;
}

}  // namespace

SceneGround::~SceneGround() = default;

bool SceneGround::open(const std::string& scene_path, const std::string& ddc, bool uncut,
                       bool placements, std::string& error) {
  sampler_.reset();
  desc_ = SceneDesc{};
  desc_.ddc = ddc;
  path_ = scene_path;
  uncut_ = uncut;
  SceneFileOptions options;
  options.ground_only = !placements;
  if (!read_scene_file(scene_path, options, desc_, error)) return false;
  // Read through the plan alone, an entry this build does not carry is not refused (nothing of it
  // expands); what it would have planned is missing, so it is named.
  not_carried_ = placements ? Vector<std::string>{} : generators_not_carried(scene_path);
  if (!desc_.terrain.enabled) {
    error = scene_path + ": the scene names no terrain, so there is no ground to ask";
    return false;
  }
  // The ground before every edit: the scene's own and every plan's, as `Context::uncut()` is.
  if (uncut && desc_.terrain.tiles.has_value()) desc_.terrain.tiles->edits.clear();
  sampler_ = std::make_unique<TerrainSampler>(desc_.terrain);
  if (!sampler_->ok()) {
    error = scene_path + ": " + sampler_->error();
    sampler_.reset();
    return false;
  }
  return true;
}

JsonValue SceneGround::head() const {
  JsonValue out = JsonValue::object();
  out.set("scene", JsonValue(path_));
  out.set("provider", JsonValue(terrain_provider(desc_.terrain)));
  out.set("uncut", JsonValue(uncut_));
  out.set("edits", JsonValue(desc_.terrain.tiles.has_value()
                                 ? static_cast<u32>(desc_.terrain.tiles->edits.size())
                                 : 0u));
  const f64 spacing = desc_.terrain.size > 1 ? 2.0 * static_cast<f64>(desc_.terrain.extent) /
                                                   static_cast<f64>(desc_.terrain.size - 1)
                                             : 0.0;
  // A ground drawn from the scene's grid; a tile set's finest spacing is its own (engine-content
  // adds it from the set).
  out.set("grid_spacing_m", JsonValue(spacing));
  out.set("rim_samples", JsonValue(scene_gen::k_seat_rim_samples));
  out.set("seat_margin_m", JsonValue(static_cast<f64>(scene_gen::k_seat_margin_mm) / 1000.0));
  out.set("time_s", JsonValue(desc_.terrain.time_s));
  JsonValue missing = JsonValue::array();
  for (const std::string& name : not_carried_)
    missing.push_back(JsonValue(name));
  out.set("not_carried", std::move(missing));
  return out;
}

JsonValue SceneGround::sample(const GroundQuestion& q, f64 skirt_m) const {
  JsonValue out = JsonValue::object();
  out.set("x_m", JsonValue(q.x_m));
  out.set("z_m", JsonValue(q.z_m));
  if (sampler_ == nullptr) return out;
  const scene_gen::Ground ground = sampler_->provider().view();
  const scene_gen::Exclusion* exclusion =
      desc_.terrain.exclusion.empty() ? nullptr : &desc_.terrain.exclusion;
  const i64 x = scene_gen::nearest_mm(q.x_m);
  const i64 z = scene_gen::nearest_mm(q.z_m);
  out.set("y_m", JsonValue(ground.surface_mm(x, z)));
  JsonValue kinds = JsonValue::array();
  bool hole = false;
  if (exclusion != nullptr) {
    struct Kind {
      u32 bit;
      const char* name;
    };
    constexpr Kind k_kinds[] = {{scene_gen::k_exclude_holes, "holes"},
                                {scene_gen::k_exclude_channels, "channels"},
                                {scene_gen::k_exclude_water, "water"},
                                {scene_gen::k_exclude_footprints, "footprints"}};
    for (const Kind& kind : k_kinds) {
      if (!exclusion->excluded_mm(ground, x, z, kind.bit)) continue;
      kinds.push_back(JsonValue(kind.name));
      hole = hole || kind.bit == scene_gen::k_exclude_holes;
    }
  }
  out.set("hole", JsonValue(hole));
  out.set("excluded", std::move(kinds));
  const scene_gen::GroundDisc around =
      scene_gen::ground_disc(ground, exclusion, x, z, scene_gen::nearest_mm(k_gradient_radius_m));
  out.set("gx", JsonValue(around.gx));
  out.set("gz", JsonValue(around.gz));
  if (q.radius_m > 0.0) {
    const scene_gen::GroundDisc d =
        scene_gen::ground_disc(ground, exclusion, x, z, scene_gen::nearest_mm(q.radius_m));
    JsonValue disc = JsonValue::object();
    disc.set("radius_m", JsonValue(q.radius_m));
    if (!d.centre_hole) {
      disc.set("centre_m", JsonValue(static_cast<f64>(d.centre_mm) / 1000.0));
      disc.set("min_m", JsonValue(static_cast<f64>(d.min_mm) / 1000.0));
      disc.set("max_m", JsonValue(static_cast<f64>(d.max_mm) / 1000.0));
      disc.set("drop_m", JsonValue(static_cast<f64>(d.drop_mm) / 1000.0));
      disc.set("samples", JsonValue(d.rim_samples));
      disc.set("gx", JsonValue(d.gx));
      disc.set("gz", JsonValue(d.gz));
      disc.set("residual_m", JsonValue(d.residual_mm / 1000.0));
      const scene_gen::GroundSlope slope = d.slope();
      JsonValue words = JsonValue::array();
      words.push_back(JsonValue(static_cast<i32>(slope.gx)));
      words.push_back(JsonValue(static_cast<i32>(slope.gz)));
      disc.set("slope", std::move(words));
      if (skirt_m > 0.0) {
        const i64 seated = scene_gen::seated_y_mm(d, d.centre_mm, scene_gen::nearest_mm(skirt_m));
        disc.set("skirt_m", JsonValue(skirt_m));
        disc.set("seat_m", JsonValue(static_cast<f64>(seated) / 1000.0));
        disc.set("lowered_m", JsonValue(static_cast<f64>(d.centre_mm - seated) / 1000.0));
      }
    } else {
      disc.set("centre_hole", JsonValue(true));
    }
    out.set("disc", std::move(disc));
  }
  return out;
}

}  // namespace engine::renderer
