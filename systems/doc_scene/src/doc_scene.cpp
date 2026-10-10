#include <core/json/json_value.h>
#include <core/math/math.h>
#include <core/schema/json_reflect.h>
#include <systems/doc_scene/doc_scene.h>

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

}  // namespace

u32 DocumentScene::instance_of(Id128 id) const noexcept {
  if (id.is_null()) return ~0u;
  for (u32 i = 0; i < records.size(); ++i) {
    if (records[i] == id) return i;
  }
  return ~0u;
}

bool read_document_scene(const doc::Document& document, const std::string& base_dir,
                         DocumentScene& out, std::string& error) {
  out = DocumentScene{};
  out.revision = document.revision();
  // `objects()` is sorted by id, so the instance order — and with it every pair and visibility id
  // — is a function of the document's content alone.
  for (const Id128 id : document.objects()) {
    if (document.type_of(id) != k_mesh_placement) continue;
    scene::MeshPlacement placement;
    if (!read_placement(document, id, placement, error)) return false;
    u32 mesh = mesh_index(out, placement.mesh);
    if (mesh == ~0u) {
      mesh = static_cast<u32>(out.mesh_paths.size());
      out.mesh_paths.push_back(placement.mesh);
      out.desc.meshes.push_back(resolve_path(base_dir, placement.mesh));
    }
    out.desc.instances.push_back(instance_of_placement(placement, mesh));
    out.records.push_back(id);
  }
  if (out.records.empty()) {
    out.desc.meshes.push_back(std::string());
    out.desc.instances.push_back(renderer::SceneInstance{0, Transform3::identity()});
    out.records.push_back(Id128{});
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
