#include <core/json/json.h>
#include <core/schema/json_reflect.h>
#include <domain/doc/document_store.h>

namespace engine::doc {

namespace {

void set_error(std::string* error, std::string_view path, std::string_view what) {
  if (error == nullptr) return;
  error->assign(path);
  error->append(": ");
  error->append(what);
}

void set_error(std::string* error, std::string_view path, const schema::ReadContext& ctx) {
  if (error == nullptr) return;
  error->assign(path);
  error->append(": ");
  bool first = true;
  for (const auto& d : ctx.diagnostics) {
    if (!first) error->append("; ");
    first = false;
    if (!d.path.empty()) {
      error->append(d.path);
      error->append(": ");
    }
    error->append(d.message);
  }
}

}  // namespace

std::string DocumentStore::path_of(std::string_view dir, std::string_view file) {
  std::string p(dir);
  if (!p.empty() && p.back() != '/' && p.back() != '\\') p.push_back('/');
  p.append(file);
  return p;
}

std::string DocumentStore::layer_file_name(std::string_view layer_name) {
  std::string name;
  name.reserve(layer_name.size() + 5);
  for (const char c : layer_name) {
    const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                    c == '_' || c == '.' || c == '-';
    name.push_back(ok ? c : '_');
  }
  if (name.empty()) name = "layer";
  name.append(".json");
  return name;
}

bool DocumentStore::exists(const io::Vfs& vfs, std::string_view dir) {
  return vfs.exists(path_of(dir, k_manifest_file));
}

bool DocumentStore::create(const io::Vfs& vfs, std::string_view dir, std::string_view name,
                           Document& out, DocumentManifest& manifest, std::string* error) {
  if (exists(vfs, dir)) {
    set_error(error, dir, "a document already exists here");
    return false;
  }
  out = Document{};
  out.add_layer(std::string(k_default_layer), LayerRole::Base);
  out.set_edit_layer(0);
  manifest = DocumentManifest{};
  manifest.name = std::string(name);
  manifest.undo_position = 0;
  if (!write_journal(vfs, dir, {}, error)) return false;
  return save(vfs, dir, out, manifest, error);
}

bool DocumentStore::save(const io::Vfs& vfs, std::string_view dir, const Document& doc,
                         DocumentManifest& manifest, std::string* error) {
  const std::string layers_dir = path_of(dir, k_layers_dir);
  if (const io::Status s = vfs.make_directories(layers_dir); s != io::Status::Ok) {
    set_error(error, layers_dir, io::status_name(s));
    return false;
  }
  manifest.layers.clear();
  for (u32 i = 0; i < doc.layer_count(); ++i) {
    const Layer& layer = doc.layer(i);
    LayerRef ref;
    ref.name = layer.name();
    ref.role = layer.role();
    ref.file = layer_file_name(layer.name());
    const std::string path = path_of(layers_dir, ref.file);
    if (const io::Status s = vfs.write(path, layer.to_json_text()); s != io::Status::Ok) {
      set_error(error, path, io::status_name(s));
      return false;
    }
    manifest.layers.push_back(std::move(ref));
  }
  manifest.edit_layer = doc.layer_count() > 0 ? doc.layer(doc.edit_layer()).name() : "";
  std::string text = write_json(schema::to_json(manifest));
  text.push_back('\n');
  const std::string manifest_path = path_of(dir, k_manifest_file);
  if (const io::Status s = vfs.write(manifest_path, text); s != io::Status::Ok) {
    set_error(error, manifest_path, io::status_name(s));
    return false;
  }
  return true;
}

bool DocumentStore::load(const io::Vfs& vfs, std::string_view dir, Document& out,
                         DocumentManifest& manifest, std::string* error) {
  const std::string manifest_path = path_of(dir, k_manifest_file);
  std::string text;
  if (const io::Status s = vfs.read(manifest_path, text); s != io::Status::Ok) {
    set_error(error, manifest_path, io::status_name(s));
    return false;
  }
  JsonValue value;
  if (const JsonParseResult r = parse_json(text, value); !r.ok) {
    set_error(error, manifest_path, r.message);
    return false;
  }
  schema::ReadContext ctx;
  manifest = DocumentManifest{};
  if (!schema::from_json(manifest, value, ctx)) {
    set_error(error, manifest_path, ctx);
    return false;
  }

  Document doc;
  for (const LayerRef& ref : manifest.layers) {
    const std::string path = path_of(path_of(dir, k_layers_dir), ref.file);
    std::string layer_text;
    if (const io::Status s = vfs.read(path, layer_text); s != io::Status::Ok) {
      set_error(error, path, io::status_name(s));
      return false;
    }
    Layer layer(ref.name, ref.role);
    schema::ReadContext layer_ctx;
    if (!Layer::from_json_text(layer_text, layer, layer_ctx)) {
      set_error(error, path, layer_ctx);
      return false;
    }
    doc.add_layer(std::move(layer));
  }
  if (doc.layer_count() == 0) {
    set_error(error, manifest_path, "document has no layers");
    return false;
  }
  const i32 edit = doc.find_layer(manifest.edit_layer);
  doc.set_edit_layer(edit >= 0 ? static_cast<u32>(edit) : doc.layer_count() - 1);

  Vector<Patch> journal;
  if (!load_journal(vfs, dir, journal, error)) return false;
  if (manifest.undo_position > journal.size()) manifest.undo_position = journal.size();
  doc.set_journal(std::move(journal));
  out = std::move(doc);
  return true;
}

bool DocumentStore::append_journal(const io::Vfs& vfs, std::string_view dir, const Patch& patch,
                                   std::string* error) {
  const std::string path = path_of(dir, k_journal_file);
  std::string line = write_json(schema::to_json(patch), JsonWriteOptions{.pretty = false});
  line.push_back('\n');
  if (const io::Status s = vfs.append(path, line); s != io::Status::Ok) {
    set_error(error, path, io::status_name(s));
    return false;
  }
  return true;
}

bool DocumentStore::write_journal(const io::Vfs& vfs, std::string_view dir,
                                  std::span<const Patch> patches, std::string* error) {
  std::string text;
  for (const Patch& patch : patches) {
    write_json(schema::to_json(patch), text, JsonWriteOptions{.pretty = false});
    text.push_back('\n');
  }
  const std::string path = path_of(dir, k_journal_file);
  if (const io::Status s = vfs.write(path, text); s != io::Status::Ok) {
    set_error(error, path, io::status_name(s));
    return false;
  }
  return true;
}

bool DocumentStore::load_journal(const io::Vfs& vfs, std::string_view dir, Vector<Patch>& out,
                                 std::string* error) {
  out.clear();
  const std::string path = path_of(dir, k_journal_file);
  std::string text;
  const io::Status s = vfs.read(path, text);
  if (s == io::Status::NotFound) return true;  // no journal yet
  if (s != io::Status::Ok) {
    set_error(error, path, io::status_name(s));
    return false;
  }
  std::string_view rest = text;
  u32 line_number = 0;
  while (!rest.empty()) {
    const usize nl = rest.find('\n');
    std::string_view line = rest.substr(0, nl);
    rest = nl == std::string_view::npos ? std::string_view{} : rest.substr(nl + 1);
    ++line_number;
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (line.empty()) continue;
    JsonValue value;
    if (const JsonParseResult r = parse_json(line, value); !r.ok) {
      set_error(error, path, std::string("line ") + std::to_string(line_number) + ": " + r.message);
      return false;
    }
    Patch patch;
    schema::ReadContext ctx;
    if (!schema::from_json(patch, value, ctx)) {
      set_error(error, path, ctx);
      return false;
    }
    out.push_back(std::move(patch));
  }
  return true;
}

}  // namespace engine::doc
