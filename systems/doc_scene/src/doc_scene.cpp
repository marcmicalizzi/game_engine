#include <core/json/json.h>
#include <core/json/json_value.h>
#include <core/math/math.h>
#include <core/schema/json_reflect.h>
#include <domain/scene_gen/scene_gen.h>
#include <foundation/io/vfs.h>
#include <systems/doc_scene/doc_scene.h>
#include <systems/renderer/terrain_materials.h>

#include <algorithm>
#include <cmath>
#include <schemas/scene.h>
#include <string>

namespace engine::doc_scene {

namespace {

std::string hex_of(Id128 id) {
  char text[33];
  id.to_hex(text);
  return std::string(text);
}

bool is_absolute(const std::string& path) noexcept {
  return (!path.empty() && (path[0] == '/' || path[0] == '\\')) ||
         (path.size() > 1 && path[1] == ':');
}

std::string resolve_path(const std::string& base_dir, const std::string& path) {
  if (base_dir.empty() || is_absolute(path)) return path;
  const char last = base_dir.back();
  return last == '/' || last == '\\' ? base_dir + path : base_dir + "/" + path;
}

// A path a record holds, made the one to open (doc_scene.h, `read_document_scene`): with a host's
// roots it is admitted (`io::admit_stored_path`) and the native path answered; false, and nothing
// opened, when it is refused. Without roots it is the operator's own reading, resolved as written.
bool stored(const io::Roots* roots, const std::string& base_dir, const std::string& path,
            std::string& out) {
  if (roots == nullptr) {
    out = resolve_path(base_dir, path);
    return true;
  }
  io::Admitted admitted;
  io::Refusal why = io::Refusal::none;
  if (!io::admit_stored_path(*roots, path, base_dir, admitted, why)) return false;
  out = std::move(admitted.native);
  return true;
}

// A file a header names beside itself (a heightfield's `data`): relative, not absolute, no `..`
// component, no `<root>://`, by the text rules the roots use (`io::check_stored_path_form`). Held
// lexically, before anything opens it, because nothing a host's roots admitted says where a file
// the record's file names may lead (T174).
bool names_a_file_beside(std::string_view name) {
  if (name.empty() || name.find("://") != std::string_view::npos) return false;
  if (io::check_stored_path_form(name) != io::Refusal::none) return false;
  usize start = 0;
  while (start <= name.size()) {
    usize end = name.find_first_of("/\\", start);
    if (end == std::string_view::npos) end = name.size();
    if (name.substr(start, end - start) == "..") return false;
    start = end + 1;
  }
  return true;
}

// The record's composed properties as the schema type. Unknown properties are left alone: the
// document validates its records against their types (`doc::Document::validate`), and a viewport
// that refused to draw a record over a property it does not read would hide the record, not the
// problem.
bool read_placement(const doc::Document& document, Id128 id, scene::MeshPlacement& out,
                    std::string& error) {
  doc::ResolvedObject object;
  if (!document.resolve(id, object) || object.deleted || object.type != k_mesh_placement) {
    error = "record " + hex_of(id) + " is not a live " + std::string(k_mesh_placement);
    return false;
  }
  JsonValue properties = JsonValue::object();
  for (const auto& entry : object.properties) {
    if (entry.second != nullptr) properties[entry.first] = *entry.second;
  }
  schema::ReadContext ctx;
  ctx.options.ignore_unknown_fields = true;
  out = scene::MeshPlacement{};
  if (!schema::from_json(out, properties, ctx) || !ctx.ok()) {
    error = "record " + hex_of(id) + ": ";
    for (const schema::Diagnostic& d : ctx.diagnostics)
      error += d.path + ": " + d.message + "; ";
    return false;
  }
  if (out.mesh.empty()) {
    error = "record " + hex_of(id) + " names no mesh";
    return false;
  }
  return true;
}

// A placement as a scene file's `Instance` is placed (renderer/src/scene.cpp): the world position
// whole in `origin`, the yaw after the rotation, the scale when given.
renderer::SceneInstance instance_of_placement(const scene::MeshPlacement& p, u32 mesh) {
  renderer::SceneInstance instance{mesh, Transform3::identity()};
  instance.origin = p.translation;
  instance.transform.rotation = normalize(
      quat_from_axis_angle(Vec3{0.0f, 1.0f, 0.0f}, radians(p.yaw_deg)) * normalize(p.rotation));
  if (p.scale.has_value()) instance.transform.scale = *p.scale;
  return instance;
}

u32 mesh_index(const DocumentScene& scene, const std::string& path) noexcept {
  for (u32 m = 0; m < scene.mesh_paths.size(); ++m) {
    if (scene.mesh_paths[m] == path) return m;
  }
  return ~0u;
}

// ---- the authored ground ------------------------------------------------------------------------

bool read_heightfield_record(const doc::Document& document, Id128 id, terrain::Heightfield& out,
                             std::string& error) {
  doc::ResolvedObject object;
  if (!document.resolve(id, object) || object.deleted || object.type != k_heightfield) {
    error = "record " + hex_of(id) + " is not a live " + std::string(k_heightfield);
    return false;
  }
  JsonValue properties = JsonValue::object();
  for (const auto& entry : object.properties) {
    if (entry.second != nullptr) properties[entry.first] = *entry.second;
  }
  schema::ReadContext ctx;
  ctx.options.ignore_unknown_fields = true;
  out = terrain::Heightfield{};
  if (!schema::from_json(out, properties, ctx) || !ctx.ok()) {
    error = "record " + hex_of(id) + ": ";
    for (const schema::Diagnostic& d : ctx.diagnostics)
      error += d.path + ": " + d.message + "; ";
    return false;
  }
  if (out.source.empty()) {
    error = "record " + hex_of(id) + " names no heightfield source";
    return false;
  }
  return true;
}

// Whether two reads of the record draw the same ground: its name is not drawn.
bool same_ground(const terrain::Heightfield& a, const terrain::Heightfield& b) noexcept {
  return a.source == b.source && a.origin.x == b.origin.x && a.origin.y == b.origin.y &&
         a.origin.z == b.origin.z && a.vertical_scale == b.vertical_scale &&
         a.materials == b.materials;
}

i64 header_int(const JsonValue& header, std::string_view key, i64 fallback) {
  const JsonValue* v = header.find(key);
  i64 out = fallback;
  if (v != nullptr) (void)v->get_i64(out);
  return out;
}

// **The record as the scene's terrain** (docs/subsystems/tileset.md, "The record"): the `tiles`
// ground provider over the heightfield the record names, moved by its origin and scaled by its
// vertical scale; the scene's own grid (`size`, `extent`) covering the heightfield round the
// origin, at its spacing as far as the grid's 4,097 points reach. The provider is made through the
// registry — so the set is built into the cache on a miss — and a build without it refuses the
// record with the registry's sentence. The header is read here for its extent only; the provider
// checks the rest.
// `refused` set (and true answered) when a path the record holds is refused by `roots`: the ground
// is left out, as a placement is, before any of its files is opened.
bool map_heightfield(Id128 id, const terrain::Heightfield& record, const std::string& base_dir,
                     std::string_view ddc, const io::Roots* roots, DocumentScene& out,
                     bool& refused, std::string& error) {
  refused = false;
  const std::string where = "record " + hex_of(id) + " (" + std::string(k_heightfield) + ")";
  std::string source;
  if (!stored(roots, base_dir, record.source, source)) {
    refused = true;
    return true;
  }
  auto materials = record.materials;
  if (materials.has_value()) {
    // Every image the layers name, `height` with the four (it was read as written, against the
    // process's directory, before 2026-11-18).
    for (auto& layer : materials->layers) {
      for (auto* path :
           {&layer.base_color, &layer.normal, &layer.roughness, &layer.mask, &layer.height}) {
        if (path->empty()) continue;
        std::string opened;
        if (!stored(roots, base_dir, *path, opened)) {
          refused = true;
          return true;
        }
        *path = std::move(opened);
      }
    }
  }
  std::string bytes;
  JsonValue header;
  if (io::read_file(source, bytes) != io::Status::Ok || !parse_json(bytes, header).ok ||
      !header.is_object()) {
    error = where + ": its source '" + record.source + "' does not read as a heightfield header";
    return false;
  }
  // **The grid the header names is held to the header's own directory** (T174): under a host's
  // roots the record's `source` was admitted, but the header's `data` is the header's author's,
  // and opened as written it would read any file and say whether it exists and how large it is.
  // Refused, the ground is left out as a refused path leaves it, before the provider opens it.
  if (roots != nullptr) {
    const JsonValue* data = header.find("data");
    if (data != nullptr && (!data->is_string() || !names_a_file_beside(data->as_string()))) {
      refused = true;
      return true;
    }
  }
  const i64 width = header_int(header, "width", 0);
  const i64 height = header_int(header, "height", 0);
  const i64 spacing = header_int(header, "spacing_mm", 1000);
  i64 origin[2] = {0, 0};
  if (const JsonValue* o = header.find("origin_mm");
      o != nullptr && o->is_array() && o->size() == 2) {
    (void)(*o)[0].get_i64(origin[0]);
    (void)(*o)[1].get_i64(origin[1]);
  }
  if (width < 2 || height < 2 || spacing < 1) {
    error = where + ": its source '" + record.source + "' has no grid of at least 2 x 2 points";
    return false;
  }
  // **The header's sizes are bounded before the extent is computed from them** (T190): the file
  // states `width`, `height` and `spacing_mm`, each up to 2^63 - 1, and `(width - 1) * spacing`
  // below overflowed `i64` (undefined behaviour) before the provider read the header. The bounds
  // are the tile set's own (`tileset::k_max_heightfield_side`, `k_max_spacing_mm` in
  // `domain/tileset/tileset.h`, repeated here because this bridge may not link the capability), so
  // a header this refuses is one the provider would refuse too, and the product stays under 2^36.
  constexpr i64 k_max_side = 65536;
  constexpr i64 k_max_spacing_mm = 1'000'000;
  if (width > k_max_side || height > k_max_side || spacing > k_max_spacing_mm) {
    error = where + ": its source '" + source + "' states a grid of " + std::to_string(width) +
            " x " + std::to_string(height) + " points " + std::to_string(spacing) +
            " mm apart; at most " + std::to_string(k_max_side) + " points a side, " +
            std::to_string(k_max_spacing_mm) + " mm apart, are read";
    return false;
  }
  scene::Terrain entry;
  entry.provider = "tiles";
  scene::TerrainTileSet tiles;
  tiles.heightfield = source;
  tiles.origin = record.origin;
  tiles.vertical_scale = record.vertical_scale;
  // **The host's root** (T104, T119): written into the block too, so the load's own sampler, made
  // from `desc.terrain`, finds the set this provider built rather than walking from the
  // heightfield.
  tiles.ddc = std::string(ddc);
  entry.tiles = std::move(tiles);
  entry.materials = std::move(materials);
  if (entry.materials.has_value()) {
    if (!renderer::validate_terrain_materials(*entry.materials, &error)) {
      error = where + ": " + error;
      return false;
    }
  }
  const f64 x0 = static_cast<f64>(origin[0]) / 1000.0 + record.origin.x;
  const f64 z0 = static_cast<f64>(origin[1]) / 1000.0 + record.origin.z;
  const f64 x1 = x0 + static_cast<f64>((width - 1) * spacing) / 1000.0;
  const f64 z1 = z0 + static_cast<f64>((height - 1) * spacing) / 1000.0;
  const f64 reach = std::max({std::fabs(x0), std::fabs(x1), std::fabs(z0), std::fabs(z1), 1.0});
  const f64 points = std::floor(2.0 * reach * 1000.0 / static_cast<f64>(spacing)) + 1.0;
  entry.extent = static_cast<f32>(reach);
  entry.size = static_cast<u32>(std::clamp(points, 2.0, 4097.0));
  const scene_gen::GeneratorRegistry& registry = scene_gen::GeneratorRegistry::global();
  const scene_gen::GroundProviderDesc* provider = registry.find_ground("tiles");
  if (provider == nullptr) {
    error = where + ": the terrain " + registry.unknown_ground("tiles");
    return false;
  }
  scene_gen::Context context;
  context.dir = base_dir;
  context.where = where;
  context.ddc = ddc;
  scene_gen::GroundProvider ground;
  std::string why;
  if (!provider->make(entry, context, ground, &why)) {
    error = where + ": " + why;
    return false;
  }
  out.desc.terrain.enabled = true;
  out.desc.terrain.provider = "tiles";
  out.desc.terrain.size = entry.size;
  out.desc.terrain.extent = entry.extent;
  out.desc.terrain.seed = 0;
  out.desc.terrain.tiles = entry.tiles;
  out.desc.terrain.materials = entry.materials;
  out.terrain_record = id;
  out.heightfield = record;
  out.terrain = std::move(entry);
  return true;
}

}  // namespace

u32 DocumentScene::instance_of(Id128 id) const noexcept {
  if (id.is_null()) return ~0u;
  for (u32 i = 0; i < records.size(); ++i) {
    if (records[i] == id) return i;
  }
  return ~0u;
}

bool read_document_scene(const doc::Document& document, const std::string& base_dir,
                         std::string_view ddc, DocumentScene& out, std::string& error,
                         const io::Roots* roots) {
  out = DocumentScene{};
  out.revision = document.revision();
  // `objects()` is sorted by id, so the instance order — and with it every pair and visibility id
  // — is a function of the document's content alone.
  Id128 ground;
  for (const Id128 id : document.objects()) {
    if (document.type_of(id) == k_heightfield) {
      if (!ground.is_null()) {
        error = "the document holds two " + std::string(k_heightfield) + " records, " +
                hex_of(ground) + " and " + hex_of(id) + "; a document has one ground today";
        return false;
      }
      ground = id;
      continue;
    }
    if (document.type_of(id) != k_mesh_placement) continue;
    // A placement with no mesh, or an empty one, names no file: left out and counted as a refused
    // path is (T174), rather than failing every viewer's load of the whole document.
    if (const JsonValue* named = document.property(id, "mesh");
        named == nullptr || (named->is_string() && named->as_string().empty())) {
      ++out.left_out;
      continue;
    }
    scene::MeshPlacement placement;
    if (!read_placement(document, id, placement, error)) return false;
    u32 mesh = mesh_index(out, placement.mesh);
    if (mesh == ~0u) {
      std::string opened;
      if (!stored(roots, base_dir, placement.mesh, opened)) {
        ++out.left_out;  // never opened, and no instance: the scene is drawn without it
        continue;
      }
      mesh = static_cast<u32>(out.mesh_paths.size());
      out.mesh_paths.push_back(placement.mesh);
      out.desc.meshes.push_back(std::move(opened));
    }
    out.desc.instances.push_back(instance_of_placement(placement, mesh));
    out.records.push_back(id);
  }
  if (out.records.empty()) {
    out.desc.meshes.push_back(std::string());
    out.desc.instances.push_back(renderer::SceneInstance{0, Transform3::identity()});
    out.records.push_back(Id128{});
  }
  if (!ground.is_null()) {
    terrain::Heightfield record;
    if (!read_heightfield_record(document, ground, record, error)) return false;
    bool refused = false;
    if (!map_heightfield(ground, record, base_dir, ddc, roots, out, refused, error)) return false;
    if (refused) ++out.left_out;  // the ground left out: the scene stands on no terrain
  }
  return true;
}

bool placement_instance(const doc::Document& document, Id128 id, const DocumentScene& scene,
                        renderer::SceneInstance& out, std::string& error) {
  scene::MeshPlacement placement;
  if (!read_placement(document, id, placement, error)) return false;
  const u32 mesh = mesh_index(scene, placement.mesh);
  if (mesh == ~0u) {
    error =
        "record " + hex_of(id) + " names mesh '" + placement.mesh + "', which the scene has not";
    return false;
  }
  out = instance_of_placement(placement, mesh);
  return true;
}

bool changes_since(const doc::Document& document, DocumentScene& scene, SceneChanges& out,
                   std::string& error) {
  out = SceneChanges{};
  Vector<Id128> changed;
  if (!document.changed_since(scene.revision, changed)) {
    out.rebuild = true;
    scene.revision = document.revision();
    return true;
  }
  scene.revision = document.revision();
  for (const Id128 id : changed) {
    // The authored ground: a record created, deleted, retyped, or changed in what it draws is a
    // rebuild (the scene's terrain and its set); a change of its name is nothing.
    const bool heightfield = document.exists(id) && document.type_of(id) == k_heightfield;
    if (heightfield || (!scene.terrain_record.is_null() && id == scene.terrain_record)) {
      terrain::Heightfield now;
      std::string why;
      if (id != scene.terrain_record || !heightfield ||
          !read_heightfield_record(document, id, now, why) ||
          !same_ground(now, scene.heightfield)) {
        out.rebuild = true;
      } else {
        scene.heightfield = now;
      }
      continue;
    }
    const u32 instance = scene.instance_of(id);
    const bool placement = document.exists(id) && document.type_of(id) == k_mesh_placement;
    if (instance == ~0u) {
      // A placement the scene does not hold yet: created, or retyped into one.
      if (placement) out.rebuild = true;
      continue;
    }
    if (!placement) {
      out.rebuild = true;  // deleted, or retyped out of being one
      continue;
    }
    renderer::SceneInstance moved;
    std::string why;
    if (!placement_instance(document, id, scene, moved, why) ||
        moved.mesh != scene.desc.instances[instance].mesh) {
      out.rebuild = true;  // another mesh, or one the scene has not
      continue;
    }
    scene.desc.instances[instance] = moved;
    out.moved.push_back(instance);
    out.placements.push_back(moved);
  }
  (void)error;
  return true;
}

}  // namespace engine::doc_scene
