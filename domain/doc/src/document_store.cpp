#include <core/containers/flat_set.h>
#include <core/json/json.h>
#include <core/profiling/profile.h>
#include <core/schema/json_reflect.h>
#include <domain/doc/document_store.h>
#include <domain/doc/partition.h>

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

std::string file_text(const LayerFile& file) { return write_json(schema::to_json(file)); }

bool write_text(const io::Vfs& vfs, const std::string& path, std::string_view text,
                std::string* error, SaveReport& report) {
  if (const io::Status s = vfs.write(path, text); s != io::Status::Ok) {
    set_error(error, path, io::status_name(s));
    return false;
  }
  ++report.files_written;
  return true;
}

template <class T>
bool read_schema_file(const io::Vfs& vfs, const std::string& path, T& out, std::string* error) {
  std::string text;
  if (const io::Status s = vfs.read(path, text); s != io::Status::Ok) {
    set_error(error, path, io::status_name(s));
    return false;
  }
  JsonValue value;
  if (const JsonParseResult r = parse_json(text, value); !r.ok) {
    set_error(error, path, r.message);
    return false;
  }
  schema::ReadContext ctx;
  out = T{};
  if (!schema::from_json(out, value, ctx)) {
    set_error(error, path, ctx);
    return false;
  }
  return true;
}

std::string hex_of(ObjectId id) {
  char hex[Id128::k_hex_length + 1];
  id.to_hex(hex);
  return std::string(hex, Id128::k_hex_length);
}

// --- partitioned layers -----------------------------------------------------------------------

struct LayerPaths {
  std::string dir;        // layers/<layer>
  std::string tiles_dir;  // layers/<layer>/tiles
  std::string index;      // layers/<layer>/index.json
  std::string untiled;    // layers/<layer>/untiled.json
};

LayerPaths paths_of(std::string_view layers_dir, std::string_view layer_name) {
  LayerPaths p;
  p.dir = DocumentStore::path_of(layers_dir, DocumentStore::layer_dir_name(layer_name));
  p.tiles_dir = DocumentStore::path_of(p.dir, DocumentStore::k_tiles_dir);
  p.index = DocumentStore::path_of(p.dir, DocumentStore::k_index_file);
  p.untiled = DocumentStore::path_of(p.dir, DocumentStore::k_untiled_file);
  return p;
}

// Drops every file of a layer's partitioned form. Used when a layer becomes a single file again
// and when a layer leaves the document.
void remove_partition_files(const io::Vfs& vfs, std::string_view layers_dir,
                            std::string_view layer_name, SaveReport& report) {
  const LayerPaths p = paths_of(layers_dir, layer_name);
  if (!vfs.exists(p.index)) return;
  Vector<io::DirEntry> entries;
  if (vfs.list(p.tiles_dir, entries) == io::Status::Ok) {
    for (const io::DirEntry& e : entries) {
      if (e.is_directory) continue;
      (void)vfs.remove(DocumentStore::path_of(p.tiles_dir, e.name));
      ++report.tiles_removed;
    }
  }
  (void)vfs.remove(p.untiled);
  (void)vfs.remove(p.index);
}

bool save_partitioned(const io::Vfs& vfs, std::string_view layers_dir, const Layer& layer,
                      const HashSet<ObjectId>& dirty, std::string* error, SaveReport& report) {
  const LayerPaths p = paths_of(layers_dir, layer.name());
  const LayerIndex index = build_layer_index(layer);
  std::string index_text = write_json(schema::to_json(index));
  index_text.push_back('\n');

  // What is on disk now. An index that is missing or unreadable means "rewrite everything",
  // which is always correct and never leaves a stale tile behind.
  LayerIndex previous;
  std::string previous_text;
  bool comparable = vfs.read(p.index, previous_text) == io::Status::Ok;
  if (comparable) {
    JsonValue value;
    schema::ReadContext ctx;
    comparable = parse_json(previous_text, value).ok && schema::from_json(previous, value, ctx) &&
                 previous.partition == index.partition;
  }

  FlatSet<TileCoord> write_tiles;
  bool write_untiled = !index.untiled.empty();
  if (!comparable) {
    for (const TileRef& t : index.tiles)
      write_tiles.insert(TileCoord{t.x, t.y});
  } else {
    write_untiled = false;
    // A tile whose record list changed, walking the two indexes together: both are in (x, y)
    // order, so this is one pass.
    u32 i = 0;
    for (const TileRef& t : index.tiles) {
      const TileCoord tile{t.x, t.y};
      while (i < previous.tiles.size() &&
             TileCoord{previous.tiles[i].x, previous.tiles[i].y} < tile)
        ++i;
      const bool same = i < previous.tiles.size() &&
                        TileCoord{previous.tiles[i].x, previous.tiles[i].y} == tile &&
                        previous.tiles[i].objects == t.objects;
      if (!same) write_tiles.insert(tile);
    }
    if (!(previous.untiled == index.untiled)) write_untiled = true;
    // A record that changed without moving: its tile is rewritten even though the list is the
    // same. An id is reported per document, so a layer that does not hold it is skipped.
    for (const ObjectId id : dirty.keys()) {
      const ObjectRecord* r = layer.find(id);
      if (r == nullptr) continue;
      TileCoord tile;
      if (tile_of(*r, index.partition, tile)) {
        write_tiles.insert(tile);
      } else {
        write_untiled = true;
      }
    }
  }

  if (const io::Status s = vfs.make_directories(p.tiles_dir); s != io::Status::Ok) {
    set_error(error, p.tiles_dir, io::status_name(s));
    return false;
  }
  report.tiles_total += index.tiles.size();
  for (const TileRef& t : index.tiles) {
    if (!write_tiles.contains(TileCoord{t.x, t.y})) continue;
    if (!write_text(vfs, DocumentStore::path_of(p.tiles_dir, t.file),
                    file_text(layer.to_file(t.objects)), error, report))
      return false;
    ++report.tiles_written;
  }

  // Tile files with no records left. Listing the directory rather than trusting the old index
  // is what keeps "the same content writes the same set of files" true after a crash.
  Vector<io::DirEntry> entries;
  if (vfs.list(p.tiles_dir, entries) == io::Status::Ok) {
    FlatSet<std::string_view> keep;
    for (const TileRef& t : index.tiles)
      keep.insert(std::string_view(t.file));
    for (const io::DirEntry& e : entries) {
      if (e.is_directory || keep.contains(std::string_view(e.name))) continue;
      (void)vfs.remove(DocumentStore::path_of(p.tiles_dir, e.name));
      ++report.tiles_removed;
    }
  }

  if (index.untiled.empty()) {
    if (vfs.exists(p.untiled)) (void)vfs.remove(p.untiled);
  } else if (write_untiled &&
             !write_text(vfs, p.untiled, file_text(layer.to_file(index.untiled)), error, report)) {
    return false;
  }
  if ((!comparable || previous_text != index_text) &&
      !write_text(vfs, p.index, index_text, error, report))
    return false;
  return true;
}

bool load_partitioned(const io::Vfs& vfs, std::string_view layers_dir, const LayerRef& ref,
                      Layer& out, std::string* error) {
  const LayerPaths p = paths_of(layers_dir, ref.name);
  LayerIndex index;
  if (!read_schema_file(vfs, p.index, index, error)) return false;
  if (index.name != ref.name) {
    set_error(error, p.index,
              "index names layer '" + index.name + "', the manifest says '" + ref.name + "'");
    return false;
  }
  if (!(index.partition.tile_size > 0)) {
    set_error(error, p.index, "partition tile_size must be greater than zero");
    return false;
  }
  if (ref.partition.has_value() && !(*ref.partition == index.partition)) {
    set_error(error, p.index, "the index's partition differs from the manifest's");
    return false;
  }

  out = Layer(ref.name, ref.role);
  out.set_partition(index.partition);
  FlatSet<std::string_view> listed;
  for (u32 i = 0; i < index.tiles.size(); ++i) {
    const TileRef& t = index.tiles[i];
    const TileCoord tile{t.x, t.y};
    if (i > 0 && !(TileCoord{index.tiles[i - 1].x, index.tiles[i - 1].y} < tile)) {
      set_error(error, p.index, "tiles are not in (x, y) order at " + tile_file_name(tile));
      return false;
    }
    if (t.file != tile_file_name(tile)) {
      set_error(error, p.index, "tile " + tile_file_name(tile) + " is listed as '" + t.file + "'");
      return false;
    }
    listed.insert(std::string_view(t.file));

    const std::string path = DocumentStore::path_of(p.tiles_dir, t.file);
    LayerFile file;
    if (!read_schema_file(vfs, path, file, error)) return false;
    if (file.name != ref.name) {
      set_error(error, path,
                "tile names layer '" + file.name + "', the manifest says '" + ref.name + "'");
      return false;
    }
    if (file.objects.size() != t.objects.size()) {
      set_error(error, path,
                "holds " + std::to_string(file.objects.size()) + " records, the index lists " +
                    std::to_string(t.objects.size()));
      return false;
    }
    for (u32 k = 0; k < file.objects.size(); ++k) {
      ObjectRecord& r = file.objects[k];
      if (!(r.id == t.objects[k])) {
        set_error(error, path, "record " + hex_of(r.id) + " is not the one the index lists there");
        return false;
      }
      if (k > 0 && !(t.objects[k - 1] < r.id)) {
        set_error(error, p.index, "records are not in id order at " + hex_of(r.id));
        return false;
      }
      TileCoord actual;
      if (!tile_of(r, index.partition, actual) || !(actual == tile)) {
        set_error(error, path, "record " + hex_of(r.id) + " does not fall in this tile");
        return false;
      }
      if (out.find(r.id) != nullptr) {
        set_error(error, path, "record " + hex_of(r.id) + " is already in another tile");
        return false;
      }
      out.set(std::move(r));
    }
  }

  Vector<io::DirEntry> entries;
  if (vfs.list(p.tiles_dir, entries) == io::Status::Ok) {
    for (const io::DirEntry& e : entries) {
      if (e.is_directory || listed.contains(std::string_view(e.name))) continue;
      set_error(error, DocumentStore::path_of(p.tiles_dir, e.name),
                "a tile file the index does not list");
      return false;
    }
  }

  if (index.untiled.empty()) {
    if (vfs.exists(p.untiled)) {
      set_error(error, p.untiled, "present although the index lists no untiled records");
      return false;
    }
    return true;
  }
  LayerFile untiled;
  if (!read_schema_file(vfs, p.untiled, untiled, error)) return false;
  if (untiled.objects.size() != index.untiled.size()) {
    set_error(error, p.untiled,
              "holds " + std::to_string(untiled.objects.size()) + " records, the index lists " +
                  std::to_string(index.untiled.size()));
    return false;
  }
  for (u32 k = 0; k < untiled.objects.size(); ++k) {
    ObjectRecord& r = untiled.objects[k];
    if (!(r.id == index.untiled[k])) {
      set_error(error, p.untiled, "record " + hex_of(r.id) + " is not the one the index lists");
      return false;
    }
    TileCoord tile;
    if (tile_of(r, index.partition, tile)) {
      set_error(
          error, p.untiled,
          "record " + hex_of(r.id) + " has a position and belongs in " + tile_file_name(tile));
      return false;
    }
    if (out.find(r.id) != nullptr) {
      set_error(error, p.untiled, "record " + hex_of(r.id) + " is already in a tile");
      return false;
    }
    out.set(std::move(r));
  }
  return true;
}

}  // namespace

std::string DocumentStore::path_of(std::string_view dir, std::string_view file) {
  std::string p(dir);
  if (!p.empty() && p.back() != '/' && p.back() != '\\') p.push_back('/');
  p.append(file);
  return p;
}

std::string DocumentStore::layer_dir_name(std::string_view layer_name) {
  std::string name;
  name.reserve(layer_name.size());
  for (const char c : layer_name) {
    const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                    c == '_' || c == '.' || c == '-';
    name.push_back(ok ? c : '_');
  }
  if (name.empty()) name = "layer";
  return name;
}

std::string DocumentStore::layer_file_name(std::string_view layer_name) {
  std::string name = layer_dir_name(layer_name);
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

bool DocumentStore::save(const io::Vfs& vfs, std::string_view dir, Document& doc,
                         DocumentManifest& manifest, std::string* error, SaveReport* report) {
  ENGINE_PROFILE_ZONE_NAMED("doc.store.save");
  SaveReport discarded;
  SaveReport& stats = report != nullptr ? *report : discarded;
  stats = SaveReport{};

  const Document& document = doc;  // the const accessor: reading a layer is not an edit
  const std::string layers_dir = path_of(dir, k_layers_dir);
  if (const io::Status s = vfs.make_directories(layers_dir); s != io::Status::Ok) {
    set_error(error, layers_dir, io::status_name(s));
    return false;
  }

  const Vector<LayerRef> previous = manifest.layers;
  manifest.layers.clear();
  for (u32 i = 0; i < document.layer_count(); ++i) {
    const Layer& layer = document.layer(i);
    LayerRef ref;
    ref.name = layer.name();
    ref.role = layer.role();
    if (layer.partitioned()) {
      ref.file = layer_dir_name(layer.name()) + "/" + std::string(k_index_file);
      ref.partition = layer.partition();
      if (!save_partitioned(vfs, layers_dir, layer, document.dirty(), error, stats)) return false;
      // A layer that used to be one file leaves that file behind otherwise.
      (void)vfs.remove(path_of(layers_dir, layer_file_name(layer.name())));
    } else {
      ref.file = layer_file_name(layer.name());
      remove_partition_files(vfs, layers_dir, layer.name(), stats);
      if (!write_text(vfs, path_of(layers_dir, ref.file), layer.to_json_text(), error, stats))
        return false;
    }
    manifest.layers.push_back(std::move(ref));
  }

  // Files of layers the document no longer has.
  for (const LayerRef& gone : previous) {
    if (document.find_layer(gone.name) >= 0) continue;
    remove_partition_files(vfs, layers_dir, gone.name, stats);
    (void)vfs.remove(path_of(layers_dir, layer_file_name(gone.name)));
  }

  manifest.edit_layer =
      document.layer_count() > 0 ? document.layer(document.edit_layer()).name() : "";
  std::string text = write_json(schema::to_json(manifest));
  text.push_back('\n');
  if (!write_text(vfs, path_of(dir, k_manifest_file), text, error, stats)) return false;
  doc.clear_dirty();
  return true;
}

bool DocumentStore::repartition(const io::Vfs& vfs, std::string_view dir, Document& doc,
                                DocumentManifest& manifest, u32 layer_index,
                                const LayerPartition& partition, std::string* error,
                                SaveReport* report) {
  if (layer_index >= doc.layer_count()) {
    set_error(error, dir, "no layer " + std::to_string(layer_index));
    return false;
  }
  // Anything without a positive tile size is "one file", spelled one way.
  doc.set_layer_partition(layer_index, partition.tile_size > 0 ? partition : LayerPartition{});
  return save(vfs, dir, doc, manifest, error, report);
}

bool DocumentStore::load(const io::Vfs& vfs, std::string_view dir, Document& out,
                         DocumentManifest& manifest, std::string* error) {
  ENGINE_PROFILE_ZONE_NAMED("doc.store.load");
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
  const std::string layers_dir = path_of(dir, k_layers_dir);
  for (const LayerRef& ref : manifest.layers) {
    Layer layer(ref.name, ref.role);
    if (ref.partition.has_value() && ref.partition->tile_size > 0) {
      if (!load_partitioned(vfs, layers_dir, ref, layer, error)) return false;
    } else {
      const std::string path = path_of(layers_dir, ref.file);
      std::string layer_text;
      if (const io::Status s = vfs.read(path, layer_text); s != io::Status::Ok) {
        set_error(error, path, io::status_name(s));
        return false;
      }
      schema::ReadContext layer_ctx;
      if (!Layer::from_json_text(layer_text, layer, layer_ctx)) {
        set_error(error, path, layer_ctx);
        return false;
      }
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
  doc.clear_dirty();  // what is in memory is what is on disk
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
