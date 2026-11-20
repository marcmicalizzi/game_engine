// The subject digest of a review item (subject_digest.h; protocol.md, "What a close is tied to").
#include "subject_digest.h"

#include <core/hash/sha256.h>
#include <core/json/json.h>
#include <core/schema/json_reflect.h>
#include <domain/doc/blob_store.h>
#include <domain/doc/document.h>
#include <foundation/io/roots.h>
#include <foundation/io/vfs.h>
#include <foundation/tunables/tunables.h>

#include <algorithm>
#include <string>

namespace engine::protocol {

namespace {

constexpr std::string_view k_stream_version = "engine.protocol.subject-digest 1\n";
// How deep a property's value is searched for blob references and file names.
constexpr u32 k_value_depth_max = 32;
constexpr usize k_file_name_max = 1024;
constexpr u64 k_read_chunk = 1u << 20;

// The four bounds (protocol.md, "What it costs"). What one digest reads is the sum of what its
// subject names, so each is a bound on that sum and none is hidden: a raise past one is refused
// naming it, and a detail or a close past one says so in the stream.
tunables::Int files_tunable{"protocol.review.subject_files", 256, 1, 1 << 16,
                            "Files one review item's subject may name in all, listed and named by "
                            "its records together"};
tunables::Int file_bytes_tunable{"protocol.review.file_bytes", i64{256} << 20, 1, i64{1} << 40,
                                 "Bytes of one file a review item's subject digest reads"};
tunables::Int subject_bytes_tunable{"protocol.review.subject_bytes", i64{1} << 30, 1, i64{1} << 44,
                                    "Bytes a review item's subject digest reads of all its files "
                                    "together"};
tunables::Int records_tunable{"protocol.review.subject_records", 4096, 1, 1 << 20,
                              "Records a review item's subject digest composes, the subject's "
                              "and their ancestors, each once"};

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
// digests as missing, which is a constant for that string and so harmless.
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

void note_over(Vector<DigestOver>& over, const char* bound, u64 limit, u64 figure) {
  for (const DigestOver& o : over) {
    if (o.bound == bound) return;  // the first figure a bound was passed at is the one told
  }
  over.push_back(DigestOver{bound, limit, figure});
}

// What a file's line says when it was not read: admitted and not there, admitted and not read, or
// outside this host's roots, three things a reader is told apart (protocol.md, "What a close is
// tied to").
constexpr const char* k_missing = "missing";
constexpr const char* k_not_readable = "not readable";
constexpr const char* k_too_large = "too large";
constexpr const char* k_over_bound = "over the subject's bound";
constexpr const char* k_outside = "outside this host's roots";

// Opens a regular file for the digest and holds it to the bounds by the size it has now: never a
// FIFO, a device or a directory, which `open_regular` refuses without waiting. False with the
// file's line saying why it will not be read.
bool open_file(SubjectPlan::File& f, const DigestBounds& bounds, u64& total, SubjectPlan& plan,
               io::FileHandle& file) {
  if (const io::Status s = file.open_regular(f.native); s != io::Status::Ok) {
    f.line = s == io::Status::NotFound ? k_missing : k_not_readable;
    return false;
  }
  if (file.size() > bounds.file_bytes) {
    note_over(plan.over, file_bytes_tunable.name(), bounds.file_bytes, file.size());
    f.line = k_too_large;
    file.close();
    return false;
  }
  if (total + file.size() > bounds.subject_bytes) {
    note_over(plan.over, subject_bytes_tunable.name(), bounds.subject_bytes, total + file.size());
    f.line = k_over_bound;
    file.close();
    return false;
  }
  total += file.size();
  return true;
}

// The SHA-256 of an open file's bytes, read now in chunks to the size it had at the open: never its
// time or its size taken for "unchanged". `keep`, when given, receives the bytes too (a glTF's
// JSON, which is held to the file bound like any file).
void hash_file(SubjectPlan::File& f, const io::FileHandle& file, SubjectPlan& plan,
               std::string* keep = nullptr) {
  Sha256 h;
  std::string buffer(static_cast<usize>(k_read_chunk), '\0');
  u64 at = 0;
  for (;;) {
    u64 got = 0;
    if (file.read_at(at, buffer.data(), k_read_chunk, got) != io::Status::Ok) {
      plan.bytes_read += at;
      f.line = k_not_readable;
      return;
    }
    if (got == 0) break;
    const std::string_view bytes(buffer.data(), static_cast<usize>(got));
    h.update(bytes);
    if (keep != nullptr) keep->append(bytes);
    at += got;
  }
  plan.bytes_read += at;
  f.line = sha256_hex(h.finish());
  f.read = true;
}

bool is_gltf(std::string_view spelled) noexcept {
  if (spelled.size() < 5) return false;
  const std::string_view tail = spelled.substr(spelled.size() - 5);
  for (usize i = 0; i < 5; ++i) {
    const char c =
        tail[i] >= 'A' && tail[i] <= 'Z' ? static_cast<char>(tail[i] - 'A' + 'a') : tail[i];
    if (c != ".gltf"[i]) return false;
  }
  return true;
}

i32 hex_digit(char c) noexcept {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// A glTF URI as a path under the glTF's own directory, by the importer's rule (domain/assets,
// `decoded_uri`): percent-decoded, then relative, no `..` component, not absolute, no scheme or
// drive, no control character. False when it is not one.
bool uri_path(std::string_view uri, std::string& out) {
  out.clear();
  for (usize i = 0; i < uri.size(); ++i) {
    if (uri[i] == '%' && i + 2 < uri.size() && hex_digit(uri[i + 1]) >= 0 &&
        hex_digit(uri[i + 2]) >= 0) {
      out.push_back(static_cast<char>(hex_digit(uri[i + 1]) * 16 + hex_digit(uri[i + 2])));
      i += 2;
    } else {
      out.push_back(uri[i] == '\\' ? '/' : uri[i]);
    }
  }
  if (out.empty() || out.size() > k_file_name_max || out.front() == '/' ||
      out.find(':') != std::string::npos)
    return false;
  for (const char c : out) {
    const auto u = static_cast<unsigned char>(c);
    if (u < 0x20 || u == 0x7f) return false;
  }
  usize start = 0;
  while (start <= out.size()) {
    const usize slash = out.find('/', start);
    const std::string_view part = std::string_view(out).substr(
        start, slash == std::string::npos ? out.size() - start : slash - start);
    if (part == "..") return false;
    if (slash == std::string::npos) break;
    start = slash + 1;
  }
  return true;
}

// The external buffers and images a glTF's JSON names, in its order and each once; a `data:` URI
// is in the file already, and a `.glb` holds its own, so neither names anything.
void gltf_uris(std::string_view text, Vector<std::string>& out) {
  JsonValue doc;
  if (!parse_json(text, doc).ok || !doc.is_object()) return;
  for (const char* list : {"buffers", "images"}) {
    const JsonValue* entries = doc.find(list);
    if (entries == nullptr || !entries->is_array()) continue;
    for (const JsonValue& entry : entries->as_array()) {
      const JsonValue* uri = entry.is_object() ? entry.find("uri") : nullptr;
      if (uri == nullptr || !uri->is_string()) continue;
      const std::string_view u = uri->as_string();
      if (u.substr(0, 5) == "data:") continue;
      if (std::find(out.begin(), out.end(), u) == out.end()) out.push_back(std::string(u));
    }
  }
}

}  // namespace

DigestBounds DigestBounds::now() noexcept {
  DigestBounds b;
  b.files = static_cast<u64>(files_tunable.get());
  b.file_bytes = static_cast<u64>(file_bytes_tunable.get());
  b.subject_bytes = static_cast<u64>(subject_bytes_tunable.get());
  b.records = static_cast<u64>(records_tunable.get());
  return b;
}

void compose_subject(const Context& ctx, const Session& session, const ReviewSubject& subject,
                     const DigestBounds& bounds, SubjectPlan& plan) {
  plan = SubjectPlan{};
  const doc::Document& d = session.document();
  plan.session = &session;
  plan.document = &d;
  plan.revision = d.revision();
  plan.journal = d.journal().size();
  plan.undo_position = session.undo_position();
  plan.layers = d.layer_count();
  Sha256& h = plan.hash;
  h.update(k_stream_version);

  // 2. The layer it is judged in. The stack itself is not hashed: what composes for the subject
  // is in its records' composed lines and the judged layer's own record, and a layer that holds
  // nothing of the subject (another agent's proposal, opened beside it) is nothing its reviewer
  // saw, where hashing it refused every honest close each time one opened.
  const i32 judged = subject.layer.empty() ? -1 : d.find_layer(subject.layer);
  {
    JsonValue line = JsonValue::array();
    line.push_back(JsonValue("judged in"));
    line.push_back(JsonValue(subject.layer));
    line.push_back(JsonValue(judged >= 0));
    add_line(h, line);
  }

  // 3. The records and their ancestors, each once, counted as they are met: a walk up from a
  // record stops at the first ancestor already met, whose own ancestors are met with it, so the
  // work is the distinct records and never the records times their depth; and it stops at the
  // bound, which a damaged index (a cycle the document refuses anyway) cannot run past either.
  Vector<Id128> ids;
  HashSet<Id128> met;
  for (const Id128 id : subject.records) {
    if (!d.exists(id)) plan.gone.push_back(id);
    for (Id128 at = id; !(at == Id128{}); at = d.parent_of(at)) {
      if (met.contains(at)) break;
      if (ids.size() >= bounds.records) {
        note_over(plan.over, records_tunable.name(), bounds.records, bounds.records + 1);
        break;
      }
      met.insert(Id128(at));
      ids.push_back(at);
    }
  }
  std::sort(ids.begin(), ids.end());
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
  plan.records = static_cast<u32>(ids.size());

  // 4. Blobs, by name: the name is the content.
  for (const std::string& b : subject.blobs)
    blobs.insert(std::string(b));
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
  plan.blobs = static_cast<u32>(blob_names.size());

  // 5. Files, by spelling, admitted through the roots now; their bytes are read with the host
  // let go. Past the bound on how many, the rest are named and never admitted or read.
  for (const std::string& a : subject.assets)
    files.push_back(a);
  sort_unique(files);
  if (files.size() > bounds.files)
    note_over(plan.over, files_tunable.name(), bounds.files, files.size());
  for (u32 i = 0; i < files.size(); ++i) {
    SubjectPlan::File& f = plan.files.emplace_back();
    f.spelled = files[i];
    if (i >= bounds.files) {
      f.line = k_over_bound;
      continue;
    }
    io::Admitted admitted;
    io::Refusal why = io::Refusal::none;
    if (ctx.roots != nullptr &&
        ctx.roots->admit(f.spelled, session.dir(), io::Access::read, admitted, why)) {
      f.native = std::move(admitted.native);
    } else {
      // A root this host does not have, or a place its roots do not admit: never opened, and a
      // line of its own, so a digest here is never taken for a change of the file.
      f.outside = true;
      f.line = k_outside;
    }
  }
}

void read_subject_files(SubjectPlan& plan, const DigestBounds& bounds) {
  // Every file opened and held to the bounds first, by the sizes they have now, so a subject past
  // one is known before a byte is read (one handle at a time: the files bound may be raised far
  // past what a process may hold open); then each that passed opened again, held to the bounds
  // again by what it is then, and read.
  u64 total = 0;
  for (SubjectPlan::File& f : plan.files) {
    if (!f.line.empty()) continue;  // decided under the lock
    if (f.native.empty()) {
      f.line = k_not_readable;
      continue;
    }
    io::FileHandle file;
    (void)open_file(f, bounds, total, plan, file);
  }
  if (!plan.over.empty() && plan.refuse_over) return;  // a raise refuses it unread
  total = 0;
  const u32 listed = static_cast<u32>(plan.files.size());
  for (u32 i = 0; i < listed; ++i) {
    if (!plan.files[i].line.empty()) continue;
    io::FileHandle file;
    if (!open_file(plan.files[i], bounds, total, plan, file)) continue;
    std::string gltf;
    const bool named = is_gltf(plan.files[i].spelled);
    hash_file(plan.files[i], file, plan, named ? &gltf : nullptr);
    if (!named || !plan.files[i].read) continue;
    // What the glTF names is part of it: each file read as the subject's own, under the same
    // bounds, and named by the glTF and its URI.
    Vector<std::string> uris;
    gltf_uris(gltf, uris);
    const std::string base(io::parent_path(plan.files[i].native));
    const std::string by = plan.files[i].spelled;
    for (const std::string& uri : uris) {
      SubjectPlan::File n;
      n.spelled = by + "#" + uri;
      n.named_by = by;
      n.uri = uri;
      std::string path;
      if (plan.files.size() >= bounds.files) {
        n.line = k_over_bound;
      } else if (!uri_path(uri, path)) {
        n.line = k_not_readable;
      } else {
        n.native = io::join_path(base, path);
        io::FileHandle named_file;
        if (open_file(n, bounds, total, plan, named_file)) hash_file(n, named_file, plan);
      }
      plan.files.push_back(std::move(n));
    }
  }
  if (plan.files.size() > bounds.files)
    note_over(plan.over, files_tunable.name(), bounds.files, plan.files.size());
}

bool same_document(const Session* session, const SubjectPlan& plan) noexcept {
  if (session == nullptr || session != plan.session) return false;
  const doc::Document& d = session->document();
  return &d == plan.document && d.revision() == plan.revision &&
         d.journal().size() == plan.journal && session->undo_position() == plan.undo_position &&
         d.layer_count() == plan.layers;
}

void finish_subject(SubjectPlan& plan, SubjectState& out) {
  out = SubjectState{};
  Sha256& h = plan.hash;
  for (const SubjectPlan::File& f : plan.files) {
    JsonValue line = JsonValue::array();
    if (f.named_by.empty()) {
      line.push_back(JsonValue("asset"));
      line.push_back(JsonValue(f.spelled));
    } else {
      line.push_back(JsonValue("named"));
      line.push_back(JsonValue(f.named_by));
      line.push_back(JsonValue(f.uri));
    }
    line.push_back(JsonValue(f.line));
    add_line(h, line);
    if (f.outside) {
      out.outside.push_back(f.spelled);
    } else if (!f.read) {
      out.unreadable.push_back(f.spelled);
    }
  }
  out.comparable = out.outside.empty();
  // 6. Each bound passed, by its name: a subject digested past one is never equal to one that was
  // not, so nothing is digested silently weaker.
  for (const DigestOver& o : plan.over) {
    JsonValue line = JsonValue::array();
    line.push_back(JsonValue("over"));
    line.push_back(JsonValue(std::string_view(o.bound)));
    line.push_back(JsonValue(o.limit));
    line.push_back(JsonValue(o.figure));
    add_line(h, line);
  }
  out.gone = plan.gone;
  out.records = plan.records;
  out.blobs = plan.blobs;
  out.assets = static_cast<u32>(plan.files.size());
  out.asset_bytes = plan.bytes_read;
  out.digest = sha256_hex(h.finish());
}

std::string promotion_digest(std::string_view proposal, std::string_view target,
                             const doc::Layer& layer, const Vector<MergeConflictInfo>& conflicts) {
  Sha256 h;
  h.update(std::string_view("engine.protocol.promotion-digest 1\n"));
  {
    JsonValue line = JsonValue::array();
    line.push_back(JsonValue("promote"));
    line.push_back(JsonValue(proposal));
    line.push_back(JsonValue(target));
    add_line(h, line);
  }
  // The layer's records in id order (a FlatMap is sorted), each as the layer holds it.
  for (auto [id, record] : layer.records()) {
    const doc::ObjectRecord& held = record;
    JsonValue line = JsonValue::array();
    line.push_back(JsonValue("record"));
    line.push_back(hex_of(id));
    line.push_back(schema::to_json(held));
    add_line(h, line);
  }
  // The conflicts in the merge's order, which is a function of the three layers (merge.h).
  for (const MergeConflictInfo& c : conflicts) {
    JsonValue line = JsonValue::array();
    line.push_back(JsonValue("conflict"));
    line.push_back(hex_of(c.object));
    line.push_back(JsonValue(c.property));
    line.push_back(JsonValue(static_cast<u64>(c.kind)));
    line.push_back(c.ours);
    add_line(h, line);
  }
  return sha256_hex(h.finish());
}

}  // namespace engine::protocol
