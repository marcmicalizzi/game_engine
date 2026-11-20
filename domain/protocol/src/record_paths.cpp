#include <core/json/json_value.h>
#include <domain/protocol/paths.h>
#include <domain/protocol/record_paths.h>

#include <string>

namespace engine::protocol {

namespace {

// The record types and fields systems/doc_scene opens (doc_scene.cpp: `read_placement`,
// `map_heightfield`): named here by their schema names, because the protocol is below the reader
// and may not include it.
constexpr std::string_view k_placement = "engine.scene.MeshPlacement";
constexpr std::string_view k_heightfield = "engine.terrain.Heightfield";
constexpr const char* k_layer_images[] = {"base_color", "normal", "roughness", "mask", "height"};

const JsonValue* property(const doc::ResolvedObject& object, std::string_view name) {
  for (const auto& entry : object.properties) {
    if (entry.first == name) return entry.second;
  }
  return nullptr;
}

void add(Vector<StoredPath>& out, std::string field, const JsonValue* value) {
  if (value == nullptr || !value->is_string() || value->as_string().empty()) return;
  StoredPath& p = out.emplace_back();
  p.field = std::move(field);
  p.path = std::string(value->as_string());
}

std::string hex_of(doc::ObjectId id) {
  char text[33];
  id.to_hex(text);
  return std::string(text);
}

}  // namespace

bool holds_stored_paths(std::string_view type) noexcept {
  return type == k_placement || type == k_heightfield;
}

void stored_paths(const doc::ResolvedObject& object, Vector<StoredPath>& out) {
  out.clear();
  if (object.deleted) return;
  if (object.type == k_placement) {
    add(out, "mesh", property(object, "mesh"));
    return;
  }
  if (object.type != k_heightfield) return;
  add(out, "source", property(object, "source"));
  const JsonValue* materials = property(object, "materials");
  const JsonValue* layers =
      materials != nullptr && materials->is_object() ? materials->find("layers") : nullptr;
  if (layers == nullptr || !layers->is_array()) return;
  for (usize i = 0; i < layers->size(); ++i) {
    const JsonValue& layer = (*layers)[i];
    if (!layer.is_object()) continue;
    for (const char* image : k_layer_images)
      add(out, "materials.layers[" + std::to_string(i) + "]." + image, layer.find(image));
  }
}

namespace {

bool check_paths(const io::Roots* roots, const doc::ResolvedObject& object,
                 std::string_view document_dir, Vector<StoredPath>& paths, RpcError& error) {
  stored_paths(object, paths);
  for (const StoredPath& p : paths) {
    io::Admitted admitted;
    io::Refusal why = io::Refusal::none;
    const bool ok = roots != nullptr
                        ? io::admit_stored_path(*roots, p.path, document_dir, admitted, why)
                        : io::check_stored_path_form(p.path) == io::Refusal::none;
    if (ok) continue;
    error = path_not_found(p.field, p.path, io::Access::read);
    error.data.set("object", JsonValue(hex_of(object.id)));
    return false;
  }
  return true;
}

}  // namespace

bool check_stored_paths(const io::Roots* roots, const doc::Document& document, u32 layer,
                        std::span<const doc::ObjectId> ids, std::string_view document_dir,
                        RpcError& error) {
  Vector<StoredPath> paths;
  for (const doc::ObjectId id : ids) {
    if (id.is_null()) continue;
    // The composed record: what every reader opens.
    doc::ResolvedObject object;
    const bool defined = document.resolve(id, object);
    if (defined && !check_paths(roots, object, document_dir, paths, error)) return false;
    // The record in the layer the command wrote, which a stronger layer may hide from the composed
    // one: what that layer's file is saved, journaled and checkpointed with (T174).
    if (layer >= document.layer_count()) continue;
    const doc::ObjectRecord* own = document.layer(layer).find(id);
    if (own == nullptr || own->deleted) continue;
    doc::ResolvedObject written;
    written.id = id;
    written.type = !own->type.empty() ? std::string_view(own->type) : object.type;
    for (const auto& [name, value] : own->properties)
      written.properties.insert_or_assign(std::string_view(name), &value);
    if (!check_paths(roots, written, document_dir, paths, error)) return false;
  }
  return true;
}

}  // namespace engine::protocol
