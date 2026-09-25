#include <core/hash/hash.h>
#include <core/json/json.h>
#include <core/json/json_value.h>
#include <core/log/log.h>
#include <core/schema/json_reflect.h>
#include <core/schema/type_info.h>
#include <foundation/io/vfs.h>
#include <foundation/store/event_log.h>
#include <systems/world/save_game.h>
#include <systems/world/state_hash.h>

#include <algorithm>
#include <schemas/doc.h>
#include <schemas/world.h>
#include <schemas/world_tiles.h>
#include <string>
#include <utility>

namespace engine::world {

namespace {

ENGINE_LOG_CATEGORY_DEFINE(log_world_save, "world.save");

std::string io_message(std::string_view what, std::string_view path, io::Status status) {
  return std::string(what) + " '" + std::string(path) + "': " + io::status_name(status);
}

bool within(const std::string& path, const std::string& dir) {
  return path == dir || (path.size() > dir.size() && path.compare(0, dir.size(), dir) == 0 &&
                         path[dir.size()] == '/');
}

// An empty or absent directory, made; anything else is refused, so a save never lands on top of
// another one's files or a load on top of a document.
bool fresh_directory(const std::string& dir, const char* what, std::string& error) {
  if (io::exists(dir)) {
    Vector<io::DirEntry> entries;
    const io::Status listed = io::list_directory(dir, entries);
    if (listed != io::Status::Ok) {
      error = io_message(std::string("cannot read the ") + what + " directory", dir, listed);
      return false;
    }
    if (!entries.empty()) {
      error = std::string("the ") + what + " directory '" + dir +
              "' is not empty: it is written into a new directory, never over one";
      return false;
    }
    return true;
  }
  const io::Status made = io::make_directories(dir);
  if (made != io::Status::Ok) {
    error = io_message(std::string("cannot create the ") + what + " directory", dir, made);
    return false;
  }
  return true;
}

// A directory tree, file by file. At the top of a document directory the store's own files are
// left out: the store is backed up, never copied while it may be open.
bool copy_tree(const std::string& from, const std::string& to, bool skip_store,
               std::string& error) {
  const io::Status made = io::make_directories(to);
  if (made != io::Status::Ok) {
    error = io_message("cannot create", to, made);
    return false;
  }
  Vector<io::DirEntry> entries;
  const io::Status listed = io::list_directory(from, entries);
  if (listed != io::Status::Ok) {
    error = io_message("cannot read", from, listed);
    return false;
  }
  std::string bytes;
  for (const io::DirEntry& entry : entries) {
    if (skip_store && is_store_file(entry.name)) continue;
    const std::string source = io::join_path(from, entry.name);
    const std::string target = io::join_path(to, entry.name);
    if (entry.is_directory) {
      if (!copy_tree(source, target, false, error)) return false;
      continue;
    }
    io::Status status = io::read_file(source, bytes);
    if (status != io::Status::Ok) {
      error = io_message("cannot read", source, status);
      return false;
    }
    status = io::write_file(target, bytes);
    if (status != io::Status::Ok) {
      error = io_message("cannot write", target, status);
      return false;
    }
  }
  return true;
}

// Every file under `root`, as paths relative to it with forward slashes, in the order a
// name-sorted walk visits them — the same on every machine for the same tree.
bool list_files(const std::string& root, const std::string& relative, Vector<std::string>& out,
                std::string& error) {
  const std::string dir = relative.empty() ? root : io::join_path(root, relative);
  Vector<io::DirEntry> entries;
  const io::Status listed = io::list_directory(dir, entries);
  if (listed != io::Status::Ok) {
    error = io_message("cannot read", dir, listed);
    return false;
  }
  for (const io::DirEntry& entry : entries) {
    const std::string path = relative.empty() ? entry.name : relative + "/" + entry.name;
    if (entry.is_directory) {
      if (!list_files(root, path, out, error)) return false;
    } else {
      out.push_back(path);
    }
  }
  return true;
}

bool hash_file(const std::string& path, u64& bytes, u64& hash, std::string& error) {
  std::string content;
  const io::Status status = io::read_file(path, content);
  if (status != io::Status::Ok) {
    error = io_message("cannot read", path, status);
    return false;
  }
  bytes = content.size();
  hash = hash_bytes(content.data(), content.size());
  return true;
}

void add_version(Vector<SaveVersion>& out, std::string_view name, u32 version) {
  for (const SaveVersion& v : out) {
    if (v.name == name) return;
  }
  SaveVersion v;
  v.name = std::string(name);
  v.version = version;
  out.push_back(std::move(v));
}

template <class T>
void add_type(Vector<SaveVersion>& out) {
  add_version(out, schema::type_of<T>().qualified_name, schema::type_of<T>().version);
}

// Every format the save's pieces are written in, sorted by name so the manifest's bytes are a
// function of what it holds.
void collect_versions(const SaveInputs& inputs, const SaveManifest& manifest,
                      Vector<SaveVersion>& out) {
  out.clear();
  add_type<SaveManifest>(out);
  add_type<SaveFile>(out);
  add_type<SaveVersion>(out);
  add_type<SaveRngStream>(out);
  if (manifest.ring.has_value()) {
    add_type<SaveRing>(out);
    add_type<SaveObserver>(out);
    add_type<SaveTile>(out);
    if (manifest.ring->player.has_value()) {
      add_type<SavePlayer>(out);
      add_version(out, k_input_log_format, input::k_log_version);
    }
  }
  // The document's own files, and every record type it holds: a record type newer than a build's
  // is refused on load, an older one materializes with the defaults of the fields it gained.
  add_type<doc::DocumentManifest>(out);
  add_type<doc::LayerFile>(out);
  add_type<doc::Patch>(out);
  if (inputs.document != nullptr) {
    for (const Id128& id : inputs.document->objects()) {
      const std::string_view type = inputs.document->type_of(id);
      const schema::TypeInfo* info = schema::Registry::global().find(type);
      if (info != nullptr) add_version(out, info->qualified_name, info->version);
    }
  }
  // The store: its tables, and the two payloads written into them today.
  if (inputs.store != nullptr) {
    add_version(out, k_store_tables, static_cast<u32>(store::EventLog::schema_version()));
    add_type<TileProjection>(out);
    add_type<WriteBack>(out);
  }
  std::sort(out.begin(), out.end(),
            [](const SaveVersion& a, const SaveVersion& b) { return a.name < b.name; });
}

// ---- @since, for a save written at an older version -------------------------------------------

struct StoredVersions {
  Vector<std::pair<std::string, u32>> entries;
  bool find(std::string_view name, u32& out) const {
    for (const auto& [n, v] : entries) {
      if (n == name) {
        out = v;
        return true;
      }
    }
    return false;
  }
};

bool check_since_ref(const schema::TypeRef& ref, const JsonValue& json,
                     const StoredVersions& stored, const std::string& path, std::string& error);

// The schema language's rule for an older object (schemas/README.md, "Evolution rules"): a field
// introduced after the version the object was written at cannot be in it — refused, naming the
// field — and one that is absent takes its default when the object is read. Nested structs are held
// to the version the save names for their own type; one it does not name is held to this build's.
bool check_since(const schema::TypeInfo& type, const JsonValue& json, const StoredVersions& stored,
                 const std::string& path, std::string& error) {
  if (!json.is_object()) return true;  // the reader says what is wrong with it
  u32 version = type.version;
  stored.find(type.qualified_name, version);
  for (const schema::FieldInfo& field : type.fields) {
    const JsonValue* value = json.find(field.name);
    if (value == nullptr) continue;
    const std::string where = path.empty() ? field.name : path + "." + field.name;
    if (field.since_version > version) {
      error = where + " was added to " + type.qualified_name + " in version " +
              std::to_string(field.since_version) + ", after the version " +
              std::to_string(version) + " the save says it was written at";
      return false;
    }
    if (!check_since_ref(field.type, *value, stored, where, error)) return false;
  }
  return true;
}

bool check_since_ref(const schema::TypeRef& ref, const JsonValue& json,
                     const StoredVersions& stored, const std::string& path, std::string& error) {
  switch (ref.kind) {
    case schema::Kind::Struct:
      return ref.type == nullptr || check_since(*ref.type, json, stored, path, error);
    case schema::Kind::Optional:
      return json.is_null() || ref.element == nullptr ||
             check_since_ref(*ref.element, json, stored, path, error);
    case schema::Kind::Array:
    case schema::Kind::FixedArray: {
      if (!json.is_array() || ref.element == nullptr) return true;
      for (usize i = 0; i < json.size(); ++i) {
        if (!check_since_ref(*ref.element, json[i], stored, path + "[" + std::to_string(i) + "]",
                             error)) {
          return false;
        }
      }
      return true;
    }
    default: return true;
  }
}

}  // namespace

bool is_store_file(std::string_view name) noexcept {
  return name == k_save_store_file || (name.size() > 9 && name.compare(0, 9, "world.db-") == 0);
}

bool current_version(std::string_view name, u32& out) {
  if (name == k_store_tables) {
    out = static_cast<u32>(store::EventLog::schema_version());
    return true;
  }
  if (name == k_input_log_format) {
    out = input::k_log_version;
    return true;
  }
  const schema::TypeInfo* type = schema::Registry::global().find(name);
  if (type == nullptr || type->kind != schema::Kind::Struct) return false;
  out = type->version;
  return true;
}

bool write_save(std::string_view dir, const SaveInputs& inputs, SaveManifest& manifest,
                std::string& error) {
  const std::string root = io::normalize_path(dir);
  const std::string document = io::normalize_path(inputs.document_dir);
  if (inputs.document_dir.empty()) {
    error = "the session has no document directory to save";
    return false;
  }
  if (within(root, document)) {
    error = "a save cannot go inside the document it saves ('" + root + "' is in '" + document +
            "'): the next save would copy the last one into itself";
    return false;
  }
  if (!fresh_directory(root, "save", error)) return false;

  // The document, as its store wrote it: the session saves every commit before it answers, so the
  // files are the document.
  if (!copy_tree(document, io::join_path(root, k_save_document_dir), true, error)) return false;
  manifest.document = k_save_document_dir;

  // The store: a consistent, canonical backup, never a copy of a file a connection has open.
  manifest.store.clear();
  if (inputs.store != nullptr) {
    const std::string target = io::join_path(root, k_save_store_file);
    const store::Status status = inputs.store->backup_to(target);
    if (status != store::Status::Ok) {
      error = "the store could not be backed up to '" + target +
              "': " + store::status_name(status) + " " + std::string(inputs.store->last_error());
      return false;
    }
    manifest.store = k_save_store_file;
  }

  // The player: the log up to the save's tick — the input that happened — and its map.
  if (manifest.ring.has_value() && manifest.ring->player.has_value()) {
    if (inputs.player == nullptr || !inputs.player->bound()) {
      error = "the manifest names a player the world does not have";
      return false;
    }
    input::InputLog log;
    inputs.player->recorded(manifest.tick, log);
    const std::string log_path = io::join_path(root, k_save_input_log);
    io::Status status = log.save(log_path);
    if (status != io::Status::Ok) {
      error = io_message("cannot write", log_path, status);
      return false;
    }
    const std::string map_path = io::join_path(root, k_save_input_map);
    std::string map_text = write_json(inputs.player->map().to_json());
    map_text.push_back('\n');
    status = io::write_file(map_path, map_text);
    if (status != io::Status::Ok) {
      error = io_message("cannot write", map_path, status);
      return false;
    }
    manifest.ring->player->log = k_save_input_log;
    manifest.ring->player->map = k_save_input_map;
  }

  manifest.format = k_save_format;
  manifest.version = SaveManifest::k_schema_version;
  collect_versions(inputs, manifest, manifest.versions);

  // Every file, then the manifest that names them, last.
  Vector<std::string> paths;
  if (!list_files(root, "", paths, error)) return false;
  manifest.files.clear();
  for (const std::string& path : paths) {
    if (path == k_save_manifest_file) continue;
    SaveFile file;
    file.path = path;
    u64 hash = 0;
    if (!hash_file(io::join_path(root, path), file.bytes, hash, error)) return false;
    file.hash = hash_hex(hash);
    manifest.files.push_back(std::move(file));
  }
  std::string text = write_json(schema::to_json(manifest));
  text.push_back('\n');
  const std::string manifest_path = io::join_path(root, k_save_manifest_file);
  const io::Status status = io::write_file_atomic(manifest_path, text);
  if (status != io::Status::Ok) {
    error = io_message("cannot write", manifest_path, status);
    return false;
  }
  ENGINE_LOG_INFO(log_world_save, "saved", log::field("path", root),
                  log::field("tick", manifest.tick), log::field("files", manifest.files.size()),
                  log::field("state_hash", manifest.state_hash));
  return true;
}

bool read_save(std::string_view dir, SaveManifest& out, SaveCheck& check, std::string& error) {
  const std::string root = io::normalize_path(dir);
  check = SaveCheck{};
  const std::string manifest_path = io::join_path(root, k_save_manifest_file);
  if (!io::exists(manifest_path)) {
    error = "'" + root + "' is not a save: it has no " + k_save_manifest_file +
            " (a save that did not finish has none either)";
    return false;
  }
  std::string text;
  const io::Status read = io::read_file(manifest_path, text);
  if (read != io::Status::Ok) {
    error = io_message("cannot read", manifest_path, read);
    return false;
  }
  JsonValue json;
  const JsonParseResult parsed = parse_json(text, json);
  if (!parsed.ok || !json.is_object()) {
    error = manifest_path + " is not a JSON object" +
            (parsed.ok ? std::string() : std::string(": ") + parsed.message);
    return false;
  }
  std::string_view format;
  const JsonValue* format_value = json.find("format");
  if (format_value == nullptr || !format_value->get_string(format) || format != k_save_format) {
    error =
        manifest_path + " is not a save's manifest: its format is not \"" + k_save_format + "\"";
    return false;
  }

  // The versions first, raw: they say how to read everything else, the manifest included.
  StoredVersions stored;
  if (const JsonValue* versions = json.find("versions"); versions != nullptr) {
    if (!versions->is_array()) {
      error = manifest_path + ": versions is not an array";
      return false;
    }
    for (usize i = 0; i < versions->size(); ++i) {
      const JsonValue& entry = (*versions)[i];
      std::string_view name;
      u64 version = 0;
      const JsonValue* n = entry.find("name");
      const JsonValue* v = entry.find("version");
      if (n == nullptr || !n->get_string(name) || v == nullptr || !v->get_u64(version) ||
          version > 0xFFFFFFFFull) {
        error = manifest_path + ": versions[" + std::to_string(i) + "] is not a name and a version";
        return false;
      }
      stored.entries.push_back({std::string(name), static_cast<u32>(version)});
    }
  }
  u64 manifest_version = 0;
  const JsonValue* version_value = json.find("version");
  if (version_value == nullptr || !version_value->get_u64(manifest_version) ||
      manifest_version == 0) {
    error = manifest_path + " has no version";
    return false;
  }
  bool named = false;
  for (const auto& entry : stored.entries) {
    if (entry.first == schema::type_of<SaveManifest>().qualified_name) named = true;
  }
  if (!named) {
    stored.entries.push_back(
        {schema::type_of<SaveManifest>().qualified_name, static_cast<u32>(manifest_version)});
  }
  for (const auto& [name, version] : stored.entries) {
    u32 current = 0;
    if (!current_version(name, current)) {
      error = "the save was written with " + name + ", which this build does not have";
      return false;
    }
    if (version > current) {
      error = "the save's " + name + " is version " + std::to_string(version) +
              ", newer than this build's " + std::to_string(current) +
              ": it was written by a newer engine";
      return false;
    }
    if (version < current) {
      check.migrations.push_back(name + " " + std::to_string(version) + " -> " +
                                 std::to_string(current));
    }
  }

  // The manifest under the version it was written at: nothing newer than it, defaults for what it
  // lacks, and nothing this build does not know.
  if (!check_since(schema::type_of<SaveManifest>(), json, stored, "", error)) {
    error = manifest_path + ": " + error;
    return false;
  }
  SaveManifest manifest;
  schema::ReadContext ctx;
  if (!schema::from_json(manifest, json, ctx) || !ctx.ok()) {
    error = manifest_path + ": " +
            (ctx.diagnostics.empty() ? std::string("not a SaveManifest")
                                     : ctx.diagnostics[0].path + ": " + ctx.diagnostics[0].message);
    return false;
  }

  // Every file the manifest names, as it names it.
  for (const SaveFile& file : manifest.files) {
    if (file.path.empty() || file.path.find("..") != std::string::npos ||
        io::is_absolute_path(file.path)) {
      error = "the save names a file outside itself: '" + file.path + "'";
      return false;
    }
    const std::string path = io::join_path(root, file.path);
    if (!io::exists(path)) {
      error = "the save's file " + file.path + " is missing";
      return false;
    }
    u64 bytes = 0;
    u64 hash = 0;
    if (!hash_file(path, bytes, hash, error)) return false;
    if (bytes != file.bytes) {
      error = "the save's file " + file.path + " is " + std::to_string(bytes) +
              " bytes where the manifest says " + std::to_string(file.bytes) +
              ": it changed after the save was written";
      return false;
    }
    if (hash_hex(hash) != file.hash) {
      error = "the save's file " + file.path + " hashes " + hash_hex(hash) +
              " where the manifest says " + file.hash + ": it changed after the save was written";
      return false;
    }
    check.bytes += bytes;
  }
  // And the pieces the rest of the manifest names are among them — the document a directory of
  // listed files inside the save, never a path a load would copy from somewhere else.
  auto listed = [&](std::string_view path) {
    for (const SaveFile& file : manifest.files) {
      if (file.path == path) return true;
    }
    return false;
  };
  bool document_listed = false;
  for (const SaveFile& file : manifest.files) {
    document_listed = document_listed ||
                      (file.path.size() > manifest.document.size() + 1 &&
                       file.path.compare(0, manifest.document.size(), manifest.document) == 0 &&
                       file.path[manifest.document.size()] == '/');
  }
  if (manifest.document.empty() || manifest.document.find("..") != std::string::npos ||
      io::is_absolute_path(manifest.document) || !document_listed) {
    error = "the save's document '" + manifest.document + "' is not a directory of its files";
    return false;
  }
  if (!manifest.store.empty() && !listed(manifest.store)) {
    error = "the save's store " + manifest.store + " is not among its files";
    return false;
  }
  if (manifest.ring.has_value() && manifest.ring->player.has_value()) {
    const SavePlayer& player = *manifest.ring->player;
    if (!listed(player.log) || !listed(player.map)) {
      error = "the save's player names a log or a map that is not among its files";
      return false;
    }
  }
  out = std::move(manifest);
  return true;
}

bool restore_save_files(std::string_view dir, const SaveManifest& manifest, std::string_view target,
                        std::string& error) {
  const std::string root = io::normalize_path(dir);
  const std::string to = io::normalize_path(target);
  if (!copy_tree(
          io::join_path(root, manifest.document.empty() ? k_save_document_dir : manifest.document),
          to, true, error)) {
    return false;
  }
  if (manifest.store.empty()) return true;
  std::string bytes;
  const std::string from = io::join_path(root, manifest.store);
  io::Status status = io::read_file(from, bytes);
  if (status != io::Status::Ok) {
    error = io_message("cannot read", from, status);
    return false;
  }
  const std::string store_path = io::join_path(to, k_save_store_file);
  status = io::write_file(store_path, bytes);
  if (status != io::Status::Ok) {
    error = io_message("cannot write", store_path, status);
    return false;
  }
  return true;
}

bool read_save_player(std::string_view dir, const SavePlayer& player, input::ActionMap& map,
                      input::InputLog& log, std::string& error) {
  const std::string root = io::normalize_path(dir);
  const std::string map_path = io::join_path(root, player.map);
  std::string text;
  io::Status status = io::read_file(map_path, text);
  if (status != io::Status::Ok) {
    error = io_message("cannot read", map_path, status);
    return false;
  }
  JsonValue json;
  std::string why;
  if (!parse_json(text, json).ok || !map.from_json(json, &why)) {
    error = map_path + " is not an action map" + (why.empty() ? "" : ": " + why);
    return false;
  }
  const std::string log_path = io::join_path(root, player.log);
  status = log.load(log_path, &why);
  if (status != io::Status::Ok) {
    error =
        io_message("cannot read the input log", log_path, status) + (why.empty() ? "" : ": " + why);
    return false;
  }
  return true;
}

}  // namespace engine::world
