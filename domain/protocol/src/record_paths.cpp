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

bool check_stored_paths(const io::Roots* roots, const doc::Document& document,
                        std::span<const doc::ObjectId> ids, std::string_view document_dir,
                        RpcError& error) {
  Vector<StoredPath> paths;
  for (const doc::ObjectId id : ids) {
    if (id.is_null() || !document.exists(id) || !holds_stored_paths(document.type_of(id))) continue;
    doc::ResolvedObject object;
    if (!document.resolve(id, object)) continue;
    stored_paths(object, paths);
    for (const StoredPath& p : paths) {
      io::Admitted admitted;
      io::Refusal why = io::Refusal::none;
      const bool ok = roots != nullptr
                          ? io::admit_stored_path(*roots, p.path, document_dir, admitted, why)
                          : io::check_stored_path_form(p.path) == io::Refusal::none;
      if (ok) continue;
      error = path_not_found(p.field, p.path, io::Access::read);
      error.data.set("object", JsonValue(hex_of(id)));
      return false;
    }
  }
  return true;
}

}  // namespace engine::protocol
