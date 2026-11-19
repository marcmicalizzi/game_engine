#include <core/hash/sha256.h>
#include <core/time/time.h>
#include <domain/doc/blob_store.h>

#include <algorithm>
#include <filesystem>
#include <string_view>
#include <system_error>

namespace engine::doc {

namespace {

bool fail(std::string* error, std::string message) {
  if (error != nullptr) *error = std::move(message);
  return false;
}

bool is_hex(std::string_view text) noexcept {
  for (const char c : text)
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  return true;
}

}  // namespace

bool BlobStore::is_name(std::string_view text) noexcept {
  return text.size() == 64 && is_hex(text);
}

std::string BlobStore::name_of(std::string_view bytes) { return sha256_hex(sha256(bytes)); }

std::string BlobStore::path_of(std::string_view dir, std::string_view name) {
  std::string path = DocumentStore::path_of(dir, k_blobs_dir);
  path += '/';
  path += name.substr(0, 2);
  path += '/';
  path += name.substr(name.size() < 2 ? name.size() : 2);
  return path;
}

bool BlobStore::put(const io::Vfs& vfs, std::string_view dir, std::string_view bytes,
                    std::string_view media, BlobRef& out, bool* created, std::string* error) {
  out.blob = name_of(bytes);
  out.size = bytes.size();
  out.media = std::string(media);
  if (created != nullptr) *created = false;
  const std::string path = path_of(dir, out.blob);
  std::string native;
  if (const io::Status s = vfs.resolve(path, native, true); s != io::Status::Ok)
    return fail(error, path + ": " + io::status_name(s));
  // A blob already stored is the same bytes by its name; it is written again only when its file no
  // longer is (damaged on disk), which a read would refuse. A put is also the promise of a commit
  // naming the blob, and the file's time is what a collection's grace period counts from: a put of
  // bytes already stored starts it again (b68 review B3), or a blob left unnamed past the period,
  // put again and collected before the commit, would be refused to the client that just put it.
  io::FileInfo info;
  if (vfs.stat(path, info) == io::Status::Ok && !info.is_directory && info.size == bytes.size()) {
    std::string there;
    if (vfs.read(path, there) == io::Status::Ok && name_of(there) == out.blob) {
      std::error_code ec;
      std::filesystem::last_write_time(
          std::filesystem::path(
              std::u8string_view(reinterpret_cast<const char8_t*>(native.data()), native.size())),
          std::filesystem::file_time_type::clock::now(), ec);
      if (!ec) return true;
      // The time could not be set: writing the same bytes again gives the file a new one.
    }
  }
  if (const io::Status s = io::make_directories(io::parent_path(native)); s != io::Status::Ok)
    return fail(error, std::string(io::parent_path(path)) + ": " + io::status_name(s));
  // Atomic: a sibling file renamed into place, so a blob is whole or absent, never torn.
  if (const io::Status s = io::write_file_atomic(native, bytes); s != io::Status::Ok)
    return fail(error, path + ": " + io::status_name(s));
  if (created != nullptr) *created = true;
  return true;
}

bool BlobStore::has(const io::Vfs& vfs, std::string_view dir, std::string_view name,
                    u64 size) noexcept {
  if (!is_name(name)) return false;
  io::FileInfo info;
  if (vfs.stat(path_of(dir, name), info) != io::Status::Ok || info.is_directory) return false;
  return size == 0 || info.size == size;
}

bool BlobStore::read(const io::Vfs& vfs, std::string_view dir, std::string_view name, u64 size,
                     std::string& out, std::string* error) {
  if (!is_name(name))
    return fail(error, "'" + std::string(name) +
                           "' is not a blob name (64 lowercase hexadecimal characters)");
  const std::string path = path_of(dir, name);
  if (const io::Status s = vfs.read(path, out); s != io::Status::Ok)
    return fail(error, "blob " + std::string(name) + " is not in the document's store (" + path +
                           ": " + io::status_name(s) + ")");
  if (size != 0 && out.size() != size)
    return fail(error, "blob " + std::string(name) + " at " + path + " holds " +
                           std::to_string(out.size()) + " bytes where its reference says " +
                           std::to_string(size) + ": the file is damaged");
  if (name_of(out) != name)
    return fail(error, "blob " + std::string(name) + " at " + path +
                           " does not hash to its name: the file is damaged");
  return true;
}

bool BlobStore::list(const io::Vfs& vfs, std::string_view dir, Vector<StoredBlob>& out,
                     std::string* error) {
  out.clear();
  const std::string root = DocumentStore::path_of(dir, k_blobs_dir);
  Vector<io::DirEntry> fans;
  const io::Status s = vfs.list(root, fans);
  if (s == io::Status::NotFound) return true;
  if (s != io::Status::Ok) return fail(error, root + ": " + io::status_name(s));
  for (const io::DirEntry& fan : fans) {
    if (!fan.is_directory || fan.name.size() != 2 || !is_hex(fan.name)) continue;
    const std::string fan_path = root + "/" + fan.name;
    Vector<io::DirEntry> files;
    if (const io::Status ls = vfs.list(fan_path, files); ls != io::Status::Ok)
      return fail(error, fan_path + ": " + io::status_name(ls));
    for (const io::DirEntry& file : files) {
      if (file.is_directory) continue;
      const std::string name = fan.name + file.name;
      if (!is_name(name)) continue;
      io::FileInfo info;
      if (vfs.stat(fan_path + "/" + file.name, info) != io::Status::Ok) continue;
      out.push_back(StoredBlob{name, info.size, info.modified_unix_ms});
    }
  }
  // Listings come sorted by name, and the fan-out directory is the name's prefix.
  std::sort(out.begin(), out.end(),
            [](const StoredBlob& a, const StoredBlob& b) { return a.name < b.name; });
  return true;
}

void BlobStore::references(const JsonValue& value, HashSet<std::string>& out) {
  if (value.is_array()) {
    for (const JsonValue& v : value.as_array())
      references(v, out);
  } else if (value.is_object()) {
    if (const JsonValue* blob = value.find("blob"); blob != nullptr && blob->is_string()) {
      const std::string_view name = blob->as_string();
      if (is_name(name)) out.insert(std::string(name));
    }
    for (const auto& member : value.as_object())
      references(member.second, out);
  }
}

void BlobStore::references(const Command& command, HashSet<std::string>& out) {
  references(command.value, out);
  if (command.record.has_value()) {
    for (const auto& property : command.record->properties)
      references(property.second, out);
  }
}

void BlobStore::references(const Document& doc, HashSet<std::string>& out) {
  for (u32 l = 0; l < doc.layer_count(); ++l) {
    for (const auto& entry : doc.layer(l).records()) {
      for (const auto& property : entry.second.properties)
        references(property.second, out);
    }
  }
  for (const Patch& patch : doc.journal()) {
    for (const Command& c : patch.forward)
      references(c, out);
    for (const Command& c : patch.inverse)
      references(c, out);
  }
}

bool BlobStore::collect(const io::Vfs& vfs, std::string_view dir, const Document& doc,
                        std::span<const JsonValue> roots, const BlobCollectOptions& options,
                        BlobCollectReport& report, std::string* error) {
  report = BlobCollectReport{};
  HashSet<std::string> referenced;
  references(doc, referenced);
  for (const JsonValue& root : roots)
    references(root, referenced);
  report.referenced = referenced.size();

  Vector<StoredBlob> stored;
  if (!list(vfs, dir, stored, error)) return false;
  report.stored = stored.size();
  HashSet<std::string> present;
  const i64 now = options.now_unix_ms != 0 ? options.now_unix_ms : time::wall_unix_ms();
  for (const StoredBlob& blob : stored) {
    report.stored_bytes += blob.size;
    present.insert(blob.name);
    if (referenced.contains(blob.name)) continue;
    if (now - blob.modified_unix_ms < options.min_age_ms) {
      ++report.kept_young;
      continue;
    }
    if (!options.dry_run) {
      const std::string path = path_of(dir, blob.name);
      std::string native;
      if (const io::Status s = vfs.resolve(path, native, true); s != io::Status::Ok)
        return fail(error, path + ": " + io::status_name(s));
      if (const io::Status s = io::remove_file(native); s != io::Status::Ok)
        return fail(error, path + ": " + io::status_name(s));
    }
    report.removed_bytes += blob.size;
    report.removed.push_back(blob.name);
  }
  for (const std::string& name : referenced)
    if (!present.contains(name)) report.missing.push_back(name);
  std::sort(report.missing.begin(), report.missing.end());
  return true;
}

}  // namespace engine::doc
