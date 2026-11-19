// The subject digest of a review item (subject_digest.h; protocol.md, "What a close is tied to").
#include "subject_digest.h"

#include <core/hash/sha256.h>
#include <core/json/json.h>
#include <core/schema/json_reflect.h>
#include <core/time/time.h>
#include <domain/doc/blob_store.h>
#include <domain/doc/document.h>
#include <foundation/io/roots.h>
#include <foundation/io/vfs.h>

#include <algorithm>
#include <string>

namespace engine::protocol {

namespace {

constexpr std::string_view k_stream_version = "engine.protocol.subject-digest 1\n";
// A record's ancestors are walked to the root; the document refuses a cycle, and this bounds the
// walk anyway, so a damaged index cannot make a digest run without end.
constexpr u32 k_ancestors_max = 4096;
// How deep a property's value is searched for blob references and file names.
constexpr u32 k_value_depth_max = 32;
constexpr usize k_file_name_max = 1024;
constexpr u64 k_read_chunk = 1u << 20;

JsonValue hex_of(Id128 id) {
  char hex[Id128::k_hex_length + 1];
  id.to_hex(hex);
  return JsonValue(std::string_view(hex, Id128::k_hex_length));
}

void add_line(Sha256& h, const JsonValue& line) {
  h.update(write_json(line, JsonWriteOptions{.pretty = false}));  // one line: the compact form
  h.update(std::string_view("\n"));
}

bool ascii_alnum(char c) noexcept {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

// A string a record holds is taken for a file it names when its last component ends in an
// extension of one to eight letters or digits (`props/crate.glb`, `content://rock.png`): nothing
// in the schema marks a field as a path (`MeshPlacement.mesh` is a string), so the rule is one any
// record's value is measured by, never a list of types. A string that passes and names nothing
// the roots admit digests as not readable, which is a constant for that string and so harmless.
bool names_a_file(std::string_view s) noexcept {
  if (s.empty() || s.size() > k_file_name_max) return false;
  for (const char c : s) {
    const auto u = static_cast<unsigned char>(c);
    if (u < 0x20 || u == 0x7f) return false;
  }
  const usize slash = s.find_last_of("/\\");
  const std::string_view last = slash == std::string_view::npos ? s : s.substr(slash + 1);
  const usize dot = last.rfind('.');
  if (dot == std::string_view::npos || dot == 0) return false;
  const std::string_view extension = last.substr(dot + 1);
  if (extension.empty() || extension.size() > 8) return false;
  for (const char c : extension) {
    if (!ascii_alnum(c)) return false;
  }
  return true;
}

void file_names(const JsonValue& value, Vector<std::string>& out, u32 depth) {
  if (depth > k_value_depth_max) return;
  if (value.is_string()) {
    if (names_a_file(value.as_string())) out.push_back(std::string(value.as_string()));
  } else if (value.is_array()) {
    for (const JsonValue& element : value.as_array())
      file_names(element, out, depth + 1);
  } else if (value.is_object()) {
    for (auto [key, element] : value.as_object()) {
      (void)key;
      file_names(element, out, depth + 1);
    }
  }
}

void names_in(const JsonValue& value, HashSet<std::string>& blobs, Vector<std::string>& files) {
  doc::BlobStore::references(value, blobs);
  file_names(value, files, 0);
}

template <class T>
void sort_unique(Vector<T>& v) {
  std::sort(v.begin(), v.end());
  Vector<T> unique;
  for (const T& x : v) {
    if (unique.empty() || !(unique.back() == x)) unique.push_back(x);
  }
  v = std::move(unique);
}

// The SHA-256 of a file's bytes, read now in chunks: never its time or its size taken for
// "unchanged". False when it cannot be opened or read (a directory, a file gone between the
// admission and the open).
bool hash_file(const std::string& native, std::string& hex, u64& bytes) {
  io::FileHandle file;
  if (file.open(native) != io::Status::Ok) return false;
  Sha256 h;
  std::string buffer(static_cast<usize>(k_read_chunk), '\0');
  u64 at = 0;
  for (;;) {
    u64 got = 0;
    if (file.read_at(at, buffer.data(), k_read_chunk, got) != io::Status::Ok) return false;
    if (got == 0) break;
    h.update(std::string_view(buffer.data(), static_cast<usize>(got)));
    at += got;
  }
  bytes = at;
  hex = sha256_hex(h.finish());
  return true;
}

}  // namespace

void subject_digest(const Context& ctx, const Session& session, const ReviewSubject& subject,
                    SubjectState& out) {
  const i64 started = time::monotonic_ns();
  out = SubjectState{};
  const doc::Document& d = session.document();
  Sha256 h;
  h.update(k_stream_version);

  // 2. The layer stack, weakest first.
  for (u32 i = 0; i < d.layer_count(); ++i) {
    const doc::Layer& layer = d.layer(i);
    JsonValue line = JsonValue::array();
    line.push_back(JsonValue("layer"));
    line.push_back(JsonValue(layer.name()));
    line.push_back(JsonValue(static_cast<u64>(layer.role())));
    add_line(h, line);
  }
  // 3. The layer it is judged in.
  const i32 judged = subject.layer.empty() ? -1 : d.find_layer(subject.layer);
  {
    JsonValue line = JsonValue::array();
    line.push_back(JsonValue("judged in"));
    line.push_back(JsonValue(subject.layer));
    line.push_back(JsonValue(judged >= 0));
    add_line(h, line);
  }

  // 4. The records and their ancestors, sorted and each once.
  Vector<Id128> ids;
  for (const Id128 id : subject.records) {
    if (!d.exists(id)) out.gone.push_back(id);
    Id128 at = id;
    for (u32 n = 0; n < k_ancestors_max && !(at == Id128{}); ++n) {
      ids.push_back(at);
      at = d.parent_of(at);
    }
  }
  sort_unique(ids);
  HashSet<std::string> blobs;
  Vector<std::string> files;
  std::string line_text;
  for (const Id128 id : ids) {
    line_text.clear();
    doc::append_composed_record(d, id, line_text);
    h.update(line_text);
    doc::ResolvedObject resolved;
    if (d.resolve(id, resolved) && !resolved.deleted) {
      for (auto [name, value] : resolved.properties) {
        (void)name;
        names_in(*value, blobs, files);
      }
    }
    if (judged >= 0) {
      JsonValue line = JsonValue::array();
      line.push_back(JsonValue("in layer"));
      line.push_back(hex_of(id));
      const doc::ObjectRecord* own = d.layer(static_cast<u32>(judged)).find(id);
      if (own != nullptr) {
        JsonValue record = schema::to_json(*own);
        names_in(record, blobs, files);
        line.push_back(std::move(record));
      } else {
        line.push_back(JsonValue::null());
      }
      add_line(h, line);
    }
  }
  out.records = static_cast<u32>(ids.size());

  // 5. Blobs, by name: the name is the content.
  for (const std::string& b : subject.blobs)
    blobs.insert(b);
  Vector<std::string> blob_names;
  for (const std::string& b : blobs)
    blob_names.push_back(b);
  sort_unique(blob_names);
  for (const std::string& b : blob_names) {
    JsonValue line = JsonValue::array();
    line.push_back(JsonValue("blob"));
    line.push_back(JsonValue(b));
    line.push_back(JsonValue(doc::BlobStore::has(ctx.sessions->vfs(), session.dir(), b)));
    add_line(h, line);
  }
  out.blobs = static_cast<u32>(blob_names.size());

  // 6. Files, by spelling, their bytes read now through the roots.
  for (const std::string& a : subject.assets)
    files.push_back(a);
  sort_unique(files);
  for (const std::string& spelled : files) {
    io::Admitted admitted;
    io::Refusal why = io::Refusal::none;
    std::string hash;
    u64 bytes = 0;
    const bool read = ctx.roots != nullptr &&
                      ctx.roots->admit(spelled, session.dir(), io::Access::read, admitted, why) &&
                      io::exists(admitted.native) && hash_file(admitted.native, hash, bytes);
    JsonValue line = JsonValue::array();
    line.push_back(JsonValue("asset"));
    line.push_back(JsonValue(spelled));
    line.push_back(JsonValue(read ? std::string_view(hash) : std::string_view("not readable")));
    add_line(h, line);
    if (read) {
      out.asset_bytes += bytes;
    } else {
      out.unreadable.push_back(spelled);
    }
  }
  out.assets = static_cast<u32>(files.size());

  out.digest = sha256_hex(h.finish());
  out.ms = static_cast<f64>(time::monotonic_ns() - started) / 1.0e6;
}

}  // namespace engine::protocol
