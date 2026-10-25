// One hash per source image per load, and a pack's from its index (image_identities.h).
#include <core/hash/hash.h>
#include <core/json/json.h>
#include <core/json/json_value.h>
#include <core/log/log.h>
#include <domain/content_build/image_identities.h>
#include <domain/materials/materials.h>
#include <foundation/io/vfs.h>

#include <algorithm>
#include <cstdio>

namespace engine::content_build {

namespace {

ENGINE_LOG_CATEGORY_DEFINE(log_identities, "content.images");

constexpr const char* k_index_format = "engine.texture.pack_identities";
constexpr u64 k_index_version = 1;

std::string hex16(u64 value) {
  char text[20];
  std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(value));
  return text;
}

u64 path_key(std::string_view path) noexcept { return hash_bytes(path.data(), path.size()); }

}  // namespace

std::string pack_index_path(std::string_view ddc, u64 pack_hash) {
  return io::join_path(io::join_path(io::join_path(ddc, "textures"), "packs"),
                       hex16(pack_hash) + ".json");
}

ImageIdentities::ImageIdentities(std::string ddc, const materials::Pack* pack)
    : ddc_(std::move(ddc)) {
  if (pack == nullptr || pack->roles.empty()) return;
  pack_hash_ = pack->hash;
  // The files the pack names, which are the only ones its index may answer for.
  for (const materials::PackRole& role : pack->roles) {
    for (const std::string* file : {&role.set.base_color, &role.set.normal, &role.set.roughness,
                                    &role.set.metallic, &role.set.height}) {
      if (file->empty()) continue;
      entries_[entry_of(io::normalize_path(*file))].pack = true;
    }
  }
  if (!ddc_.empty()) load_index();
}

u32 ImageIdentities::entry_of(const std::string& path) {
  const u64 key = path_key(path);
  if (const u32* found = index_.find_value(key); found != nullptr) {
    if (entries_[*found].path == path) return *found;
    for (u32 i = 0; i < entries_.size(); ++i)  // a 64-bit collision between two paths
      if (entries_[i].path == path) return i;
  } else {
    index_.try_emplace(key, entries_.size());
  }
  Entry entry;
  entry.path = path;
  entries_.push_back(std::move(entry));
  return entries_.size() - 1;
}

void ImageIdentities::load_index() {
  const std::string file = pack_index_path(ddc_, pack_hash_);
  std::string text;
  if (!io::exists(file) || io::read_file(file, text) != io::Status::Ok) return;
  JsonValue document;
  if (!parse_json(text, document).ok || !document.is_object()) return;
  const JsonValue* format = document.find("format");
  const JsonValue* pack = document.find("pack");
  const JsonValue* files = document.find("files");
  std::string_view format_text;
  std::string_view pack_text;
  if (format == nullptr || !format->get_string(format_text) || format_text != k_index_format ||
      pack == nullptr || !pack->get_string(pack_text) || pack_text != hex16(pack_hash_) ||
      files == nullptr || !files->is_array()) {
    ENGINE_LOG_WARN(log_identities, "pack index ignored", log::field("path", file));
    return;
  }
  for (const JsonValue& row : files->as_array()) {
    const JsonValue* path = row.find("path");
    const JsonValue* hash = row.find("hash");
    const JsonValue* bytes = row.find("bytes");
    std::string_view path_text;
    u64 hash_value = 0;
    u64 size = 0;
    if (path == nullptr || !path->get_string(path_text) || hash == nullptr ||
        !hash->get_u64(hash_value) || bytes == nullptr || !bytes->get_u64(size))
      continue;
    const std::string normalized = io::normalize_path(path_text);
    const u64 key = path_key(normalized);
    const u32* found = index_.find_value(key);
    // Only a file the pack names now: a row for one it no longer names answers nothing.
    if (found == nullptr || entries_[*found].path != normalized || !entries_[*found].pack) continue;
    Entry& entry = entries_[*found];
    entry.hash = hash_value;
    entry.size = size;
    entry.known = true;
    entry.indexed = true;
  }
}

bool ImageIdentities::identify(std::string_view path_in, u64& hash, u64& size, std::string& error) {
  const std::string path = io::normalize_path(path_in);
  std::unique_lock<std::mutex> lock(mutex_);
  u32 at = entry_of(path);
  // A reader already hashing it: wait for that answer rather than read the file twice.
  done_.wait(lock, [&] { return !entries_[at].pending; });
  {
    Entry& entry = entries_[at];
    if (entry.known) {
      hash = entry.hash;
      size = entry.size;
      if (entry.indexed) {
        ++counts_.from_index;
        entry.indexed = false;  // counted once; later asks are this load's own repeats
      } else {
        ++counts_.repeats;
      }
      return true;
    }
    if (!entry.error.empty()) {
      error = entry.error;
      return false;
    }
    entry.pending = true;
  }
  lock.unlock();
  std::string bytes;
  const io::Status status = io::read_file(path, bytes);
  const u64 got_hash = status == io::Status::Ok ? hash_bytes(bytes.data(), bytes.size()) : 0;
  lock.lock();
  // `entries_` may have grown while unlocked; the entry is found again by index.
  Entry& entry = entries_[at];
  entry.pending = false;
  ++counts_.reads;
  if (status != io::Status::Ok) {
    entry.error = "cannot read '" + path + "': " + io::status_name(status);
    error = entry.error;
    done_.notify_all();
    return false;
  }
  counts_.read_bytes += bytes.size();
  ++counts_.hashed;
  entry.hash = got_hash;
  entry.size = bytes.size();
  entry.known = true;
  if (entry.pack) dirty_ = true;
  hash = entry.hash;
  size = entry.size;
  done_.notify_all();
  return true;
}

bool ImageIdentities::read(std::string_view path_in, std::string& bytes, u64& hash,
                           std::string& error) {
  const std::string path = io::normalize_path(path_in);
  const io::Status status = io::read_file(path, bytes);
  const u64 got_hash = status == io::Status::Ok ? hash_bytes(bytes.data(), bytes.size()) : 0;
  const std::lock_guard<std::mutex> lock(mutex_);
  ++counts_.reads;
  if (status != io::Status::Ok) {
    error = "cannot read '" + path + "': " + io::status_name(status);
    return false;
  }
  counts_.read_bytes += bytes.size();
  Entry& entry = entries_[entry_of(path)];
  if (entry.known && entry.hash != got_hash) {
    // A pack's file that no longer hashes as its index says: the pack was edited in place without
    // its manifest. The bytes are the answer from here on, and the index learns them.
    ++counts_.pack_changed;
    ENGINE_LOG_WARN(log_identities, "a pack file changed without its manifest",
                    log::field("path", path), log::field("index", hex16(entry.hash)),
                    log::field("bytes", hex16(got_hash)));
  }
  if (!entry.known || entry.hash != got_hash) {
    entry.hash = got_hash;
    entry.size = bytes.size();
    entry.known = true;
    entry.indexed = false;
    if (entry.pack) dirty_ = true;
  }
  hash = got_hash;
  return true;
}

void ImageIdentities::save() {
  const std::lock_guard<std::mutex> lock(mutex_);
  if (!dirty_ || ddc_.empty() || pack_hash_ == 0) return;
  // Sorted by path, so the same answers write the same bytes whichever order the load met them in.
  Vector<u32> order;
  for (u32 i = 0; i < entries_.size(); ++i)
    if (entries_[i].pack && entries_[i].known) order.push_back(i);
  std::sort(order.begin(), order.end(),
            [&](u32 a, u32 b) { return entries_[a].path < entries_[b].path; });
  JsonValue files = JsonValue::array();
  for (const u32 i : order) {
    JsonValue row = JsonValue::object();
    row.set("path", JsonValue(entries_[i].path));
    row.set("hash", JsonValue(entries_[i].hash));
    row.set("bytes", JsonValue(entries_[i].size));
    files.push_back(std::move(row));
  }
  JsonValue document = JsonValue::object();
  document.set("format", JsonValue(k_index_format));
  document.set("version", JsonValue(k_index_version));
  document.set("pack", JsonValue(hex16(pack_hash_)));
  document.set("files", std::move(files));
  const std::string file = pack_index_path(ddc_, pack_hash_);
  if (io::make_directories(io::parent_path(file)) != io::Status::Ok ||
      io::write_file_atomic(file, write_json(document)) != io::Status::Ok) {
    ENGINE_LOG_WARN(log_identities, "pack index not written", log::field("path", file));
    return;
  }
  dirty_ = false;
}

ImageIdentities::Counts ImageIdentities::counts() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return counts_;
}

}  // namespace engine::content_build
