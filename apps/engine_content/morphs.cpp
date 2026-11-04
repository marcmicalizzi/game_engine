// engine-content morphs: MakeHuman's shape targets appended to an authored canonical-topology body
// as morph channels (docs/subsystems/assets.md, "Morph channels from MakeHuman targets"; roadmap
// C59). It is the engine-native form of the forest proof of concept's recipe step
// (`scripts/build-player-asset.py`, P59), and its acceptance is byte identity with that script's
// `body.gltf` and `body.bin`, so the arithmetic below follows the script operation for operation:
// double where numpy is float64, float where numpy holds float32, the same accumulation order, and
// floating-point contraction off tree-wide (ADR-0035) so the products round where numpy's do.
//
// The mapping, which the roadmap row first had wrong: **a canonical id is not a MakeHuman vertex
// index**. The authoring release's `exports/control-data.json` holds `source_template_vertex_ids`,
// canonical id -> index into MakeHuman's `base.obj` vertex array, and that list must equal the
// sorted set of vertices the `body` group's faces use; the glTF's `_CANONICAL_ID` attribute maps
// an export vertex to its canonical id. A target's raw displacement reaches an export vertex as
// `target[mapping[id[v]]] * scale`, where `scale = 1.75 / height` and the height is the `body`
// vertices' y extent on the baseline body: `base.obj` plus a third of each of the three young-male
// ancestry targets (0.10113268608414239 m per raw unit on MakeHuman a8bc2d54).
//
// The NORMAL deltas are recomputed, not taken from the target: area-weighted face normals summed
// per canonical id (so the two sides of a UV seam agree), normalized, spread back to the export
// vertices, minus the same for the unmorphed body.

#include "content_commands.h"

#include <core/hash/sha1.h>
#include <core/json/json.h>
#include <domain/assets/gltf_document.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <engine_build_stamp.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>

namespace engine::content {

namespace {

namespace fs = std::filesystem;
using assets::DocValue;
using assets::GltfDocument;

constexpr int k_ok = 0;
constexpr int k_error = 1;
constexpr int k_usage = 2;

// Every line break of the files this command writes. Python's text-mode write on Windows, which
// wrote the bodies this command must reproduce, turns "\n" into "\r\n"; writing it on every
// platform keeps the bytes a function of the inputs and not of the machine.
constexpr std::string_view k_newline = "\r\n";

const char* const k_morphs_usage =
    "usage: engine-content morphs <body.gltf|body.glb> --mapping <control-data.json>\n"
    "           --controls <body-controls.json> --targets <dir> --references <makehuman dir>\n"
    "           --sex male|female --out <dir> [--id <revision>]\n"
    "  appends the controls' MakeHuman targets to every mesh whose vertices carry\n"
    "  _CANONICAL_ID as morph channels shape.<key>.less / shape.<key>.more (POSITION and\n"
    "  recomputed NORMAL deltas), pads every weights track with zero channels, and writes\n"
    "  <out>/body.gltf, body.bin, body.identity.json and provenance.json\n"
    "  --mapping     the authoring release's exports/control-data.json\n"
    "                (source_template_vertex_ids: canonical id -> MakeHuman base.obj vertex)\n"
    "  --controls    the catalogue: [{key, label, female, negative[], positive[]}]\n"
    "  --targets     the directory the catalogue's target paths are relative to\n"
    "  --references  the MakeHuman reference tree (base.obj, adult-body-baseline-targets)\n"
    "  --id          the identity file's revision (default: the output directory's name)\n";

int usage(const char* message) {
  if (message != nullptr) std::fprintf(stderr, "engine-content morphs: %s\n", message);
  std::fputs(k_morphs_usage, stderr);
  return k_usage;
}

int fail(const std::string& message) {
  std::fprintf(stderr, "engine-content morphs: %s\n", message.c_str());
  return k_error;
}

// --- SHA-256 (FIPS 180-4) ------------------------------------------------------------------
// For the identity and provenance records, which name files by the hash `hashlib.sha256` gives.
// A private copy: the tree's other one lives in the tissue capability, which this command must
// not need.

class Sha256 {
 public:
  void update(const u8* p, usize n) {
    for (usize k = 0; k < n; ++k) {
      block_[used_++] = p[k];
      if (used_ == 64) {
        compress();
        used_ = 0;
      }
    }
    length_ += n;
  }
  std::string hex() {
    const u64 bits = length_ * 8;
    const u8 one = 0x80;
    update(&one, 1);
    const u8 zero = 0;
    while (used_ != 56)
      update(&zero, 1);
    u8 tail[8];
    for (u32 k = 0; k < 8; ++k)
      tail[k] = static_cast<u8>(bits >> (56 - 8 * k));
    update(tail, 8);
    static const char k_hex[] = "0123456789abcdef";
    std::string out;
    for (const u32 word : h_)
      for (int shift = 28; shift >= 0; shift -= 4)
        out.push_back(k_hex[(word >> shift) & 0xF]);
    return out;
  }

 private:
  static u32 rotr(u32 x, u32 n) { return (x >> n) | (x << (32 - n)); }
  void compress() {
    static constexpr u32 k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
        0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
        0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
        0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
        0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
        0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
        0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
        0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
        0xc67178f2};
    u32 w[64];
    for (u32 i = 0; i < 16; ++i)
      w[i] = (static_cast<u32>(block_[i * 4]) << 24) | (static_cast<u32>(block_[i * 4 + 1]) << 16) |
             (static_cast<u32>(block_[i * 4 + 2]) << 8) | static_cast<u32>(block_[i * 4 + 3]);
    for (u32 i = 16; i < 64; ++i) {
      const u32 s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      const u32 s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    u32 a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
    for (u32 i = 0; i < 64; ++i) {
      const u32 t1 =
          h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
      const u32 t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
      h = g;
      g = f;
      f = e;
      e = d + t1;
      d = c;
      c = b;
      b = a;
      a = t1 + t2;
    }
    h_[0] += a;
    h_[1] += b;
    h_[2] += c;
    h_[3] += d;
    h_[4] += e;
    h_[5] += f;
    h_[6] += g;
    h_[7] += h;
  }

  std::array<u32, 8> h_{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  u8 block_[64] = {};
  u32 used_ = 0;
  u64 length_ = 0;
};

// --- files ----------------------------------------------------------------------------------

bool read_bytes(const fs::path& path, std::string& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  return !in.bad();
}

std::string sha256_of(std::string_view bytes) {
  Sha256 h;
  h.update(reinterpret_cast<const u8*>(bytes.data()), bytes.size());
  return h.hex();
}

bool sha256_file(const fs::path& path, std::string& out) {
  std::string bytes;
  if (!read_bytes(path, bytes)) return false;
  out = sha256_of(bytes);
  return true;
}

// Absolute and normalized, as Python's `os.path.abspath` makes one.
fs::path absolute_path(const std::string& path) {
  std::error_code ec;
  fs::path p = fs::absolute(fs::path(path), ec).lexically_normal();
  if (ec) p = fs::path(path).lexically_normal();
  if (p.has_filename() == false && p.has_parent_path() && p != p.root_path()) p = p.parent_path();
  return p;
}

// The path as Python's `str(Path)` prints it: absolute, normalized, native separators.
std::string native_path(const fs::path& path) {
  std::error_code ec;
  fs::path p = fs::absolute(path, ec).lexically_normal();
  p.make_preferred();
  return p.string();
}

bool read_json(const fs::path& path, DocValue& out, std::string& error) {
  std::string text;
  if (!read_bytes(path, text)) {
    error = "cannot read " + path.string();
    return false;
  }
  if (!assets::parse_document_json(text, out, &error)) {
    error = path.string() + ": " + error;
    return false;
  }
  return true;
}

bool write_json_file(const fs::path& path, const DocValue& value, std::string& error) {
  std::string text;
  if (!assets::write_document_json(value, text, k_newline)) {
    error = path.string() + ": a NaN or an infinity";
    return false;
  }
  text += k_newline;
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(text.data(), static_cast<std::streamsize>(text.size()));
  if (!out) {
    error = "cannot write " + path.string();
    return false;
  }
  return true;
}

// --- text the way Python's split() reads it ---------------------------------------------------

void split_lines(std::string_view text, Vector<std::string_view>& out) {
  out.clear();
  usize start = 0;
  for (usize i = 0; i <= text.size(); ++i) {
    if (i == text.size() || text[i] == '\n' || text[i] == '\r') {
      out.push_back(text.substr(start, i - start));
      start = i + 1;
    }
  }
}

void split_words(std::string_view line, Vector<std::string_view>& out) {
  out.clear();
  usize i = 0;
  const auto space = [](char c) {
    return c == ' ' || c == '\t' || c == '\v' || c == '\f' || (c >= 0x1c && c <= 0x1f);
  };
  while (i < line.size()) {
    while (i < line.size() && space(line[i]))
      ++i;
    const usize start = i;
    while (i < line.size() && !space(line[i]))
      ++i;
    if (i > start) out.push_back(line.substr(start, i - start));
  }
}

bool parse_f64(std::string_view token, f64& out) {
  if (!token.empty() && token[0] == '+') token.remove_prefix(1);
  const auto r = std::from_chars(token.data(), token.data() + token.size(), out);
  return r.ec == std::errc{} && r.ptr == token.data() + token.size();
}

bool parse_i64(std::string_view token, i64& out) {
  if (!token.empty() && token[0] == '+') token.remove_prefix(1);
  const auto r = std::from_chars(token.data(), token.data() + token.size(), out);
  return r.ec == std::errc{} && r.ptr == token.data() + token.size();
}

// --- MakeHuman ------------------------------------------------------------------------------

struct BaseMesh {
  Vector<f64> raw;       // x, y, z per vertex
  Vector<u32> body_ids;  // the vertices the `body` group's faces use, sorted
  u32 count() const { return raw.size() / 3; }
};

bool read_base_obj(const fs::path& path, BaseMesh& out, std::string& error) {
  std::string text;
  if (!read_bytes(path, text)) {
    error = "cannot read " + path.string();
    return false;
  }
  Vector<std::string_view> lines, words;
  split_lines(text, lines);
  Vector<u8> used;
  std::string group;
  for (const std::string_view line : lines) {
    split_words(line, words);
    if (words.empty()) continue;
    if (words[0] == "v") {
      if (words.size() < 4) {
        error = path.string() + ": a vertex line without three coordinates";
        return false;
      }
      for (u32 c = 1; c <= 3; ++c) {
        f64 v = 0.0;
        if (!parse_f64(words[c], v)) {
          error = path.string() + ": a coordinate that is not a number";
          return false;
        }
        out.raw.push_back(v);
      }
    } else if (words[0] == "g") {
      group.clear();
      for (u32 k = 1; k < words.size(); ++k) {
        if (k > 1) group.push_back(' ');
        group.append(words[k]);
      }
    } else if (words[0] == "f" && group == "body") {
      for (u32 k = 1; k < words.size(); ++k) {
        const std::string_view token = words[k].substr(0, words[k].find('/'));
        i64 index = 0;
        if (!parse_i64(token, index) || index < 1) {
          error = path.string() + ": a face index that is not a positive whole number";
          return false;
        }
        const u64 v = static_cast<u64>(index - 1);
        if (v >= used.size()) used.resize(static_cast<u32>(v + 1), 0);
        used[static_cast<u32>(v)] = 1;
      }
    }
  }
  if (used.size() > out.count()) {
    error = path.string() + ": a body face names a vertex past the vertex list";
    return false;
  }
  for (u32 v = 0; v < used.size(); ++v)
    if (used[v] != 0) out.body_ids.push_back(v);
  if (out.body_ids.empty()) {
    error = path.string() + ": no faces in a group named body";
    return false;
  }
  return true;
}

// A MakeHuman `.target`: lines of `index dx dy dz`, '#' comments; a vertex it does not name does
// not move, and a later line for the same vertex replaces an earlier one.
bool read_target(const fs::path& path, u32 vertex_count, Vector<f64>& out, std::string& error) {
  std::string text;
  if (!read_bytes(path, text)) {
    error = "cannot read " + path.string();
    return false;
  }
  out.clear();
  out.resize(vertex_count * 3, 0.0);
  Vector<std::string_view> lines, words;
  split_lines(text, lines);
  for (const std::string_view line : lines) {
    split_words(line, words);
    if (words.empty() || words[0][0] == '#') continue;
    i64 index = 0;
    if (words.size() < 4 || !parse_i64(words[0], index) || index < 0 || index >= vertex_count) {
      error = path.string() + ": a line that is not `index dx dy dz` over the base mesh";
      return false;
    }
    for (u32 c = 0; c < 3; ++c) {
      f64 v = 0.0;
      if (!parse_f64(words[c + 1], v)) {
        error = path.string() + ": a displacement that is not a number";
        return false;
      }
      out[static_cast<u32>(index) * 3 + c] = v;
    }
  }
  return true;
}

// Targets are named by several controls' sides and by both sexes' catalogues; read each once.
struct TargetCache {
  struct Entry {
    std::string path;
    Vector<f64> values;
  };
  Vector<Entry> entries;

  const Vector<f64>* get(const fs::path& path, u32 vertex_count, std::string& error) {
    const std::string key = path.string();
    for (const Entry& e : entries)
      if (e.path == key) return &e.values;
    Entry e;
    e.path = key;
    if (!read_target(path, vertex_count, e.values, error)) return nullptr;
    return &entries.push_back(std::move(e)).values;
  }
};

// --- the morph arithmetic ---------------------------------------------------------------------

// Per-export-vertex normals: area-weighted face normals summed per canonical id, normalized, read
// back through the ids. `T` is the type the face vectors are computed in — float for the body as
// stored (numpy's float32 `pos`), double for a morphed body (`pos + delta` is float64) — and the
// sums are double either way, in numpy's order: corner by corner, triangle by triangle.
template <class T>
void canonical_normals(const Vector<T>& q, const Vector<u32>& tri, const Vector<u32>& ids,
                       u32 id_count, Vector<f64>& out) {
  const u32 triangles = tri.size() / 3;
  Vector<T> face(triangles * 3);
  for (u32 t = 0; t < triangles; ++t) {
    const T* p0 = &q[tri[t * 3] * 3];
    const T* p1 = &q[tri[t * 3 + 1] * 3];
    const T* p2 = &q[tri[t * 3 + 2] * 3];
    const T a0 = p1[0] - p0[0], a1 = p1[1] - p0[1], a2 = p1[2] - p0[2];
    const T b0 = p2[0] - p0[0], b1 = p2[1] - p0[1], b2 = p2[2] - p0[2];
    // np.cross: each product rounded to T, then the difference.
    const T c0a = a1 * b2, c0b = a2 * b1;
    const T c1a = a2 * b0, c1b = a0 * b2;
    const T c2a = a0 * b1, c2b = a1 * b0;
    face[t * 3] = c0a - c0b;
    face[t * 3 + 1] = c1a - c1b;
    face[t * 3 + 2] = c2a - c2b;
  }
  Vector<f64> sum(id_count * 3, 0.0);
  for (u32 corner = 0; corner < 3; ++corner) {
    for (u32 t = 0; t < triangles; ++t) {
      const u32 id = ids[tri[t * 3 + corner]];
      for (u32 c = 0; c < 3; ++c)
        sum[id * 3 + c] += static_cast<f64>(face[t * 3 + c]);
    }
  }
  for (u32 id = 0; id < id_count; ++id) {
    f64* n = &sum[id * 3];
    const f64 xx = n[0] * n[0], yy = n[1] * n[1], zz = n[2] * n[2];
    const f64 length = std::sqrt((xx + yy) + zz);
    const f64 d = std::max(length, 1e-20);
    n[0] /= d;
    n[1] /= d;
    n[2] /= d;
  }
  out.resize(ids.size() * 3);
  for (u32 v = 0; v < ids.size(); ++v)
    for (u32 c = 0; c < 3; ++c)
      out[v * 3 + c] = sum[ids[v] * 3 + c];
}

f64 vector_length(const f64* v) {
  const f64 xx = v[0] * v[0], yy = v[1] * v[1], zz = v[2] * v[2];
  return std::sqrt((xx + yy) + zz);
}

Vector<f32> to_f32(const Vector<f64>& v) {
  Vector<f32> out(v.size());
  for (u32 k = 0; k < v.size(); ++k)
    out[k] = static_cast<f32>(v[k]);
  return out;
}

struct Control {
  std::string key, label;
  bool female = false;
  Vector<std::string> negative, positive;
};

bool read_controls(const fs::path& path, Vector<Control>& out, std::string& error) {
  DocValue doc;
  if (!read_json(path, doc, error)) return false;
  if (!doc.is_array()) {
    error = path.string() + ": the catalogue is not an array";
    return false;
  }
  for (const DocValue& item : doc.items) {
    Control c;
    const DocValue* key = item.find("key");
    const DocValue* label = item.find("label");
    const DocValue* female = item.find("female");
    if (key == nullptr || !key->is_string() || label == nullptr || !label->is_string()) {
      error = path.string() + ": a control without a key and a label";
      return false;
    }
    c.key = key->text;
    c.label = label->text;
    c.female = female != nullptr && female->kind == DocValue::Kind::boolean && female->boolean;
    for (const char* side : {"negative", "positive"}) {
      const DocValue* list = item.find(side);
      if (list == nullptr || !list->is_array() || list->items.empty()) {
        error = path.string() + ": control " + c.key + " has no " + side + " targets";
        return false;
      }
      for (const DocValue& p : list->items) {
        if (!p.is_string()) {
          error = path.string() + ": control " + c.key + " names a target that is not a path";
          return false;
        }
        (std::string_view(side) == "negative" ? c.negative : c.positive).push_back(p.text);
      }
    }
    out.push_back(std::move(c));
  }
  return true;
}

// The pinned-target checks the durable script makes before it reads a target: each file the
// download manifest names has the git blob hash it records, and the licence texts and breast
// targets beside them are the reference tree's, byte for byte. Each check runs when its source
// exists; the summary counts what was checked.
bool check_targets(const fs::path& targets, const fs::path& references, u32& pinned, u32& copies,
                   std::string& error) {
  pinned = 0;
  copies = 0;
  std::error_code ec;
  const fs::path manifest = targets / "target-downloads.json";
  if (fs::exists(manifest, ec)) {
    DocValue doc;
    if (!read_json(manifest, doc, error)) return false;
    for (const DocValue& item : doc.items) {
      const DocValue* rel = item.find("path");
      const DocValue* blob = item.find("git_blob");
      if (rel == nullptr || !rel->is_string() || blob == nullptr || !blob->is_string()) {
        error = manifest.string() + ": an entry without a path and a git_blob";
        return false;
      }
      std::string bytes;
      const fs::path file = targets / fs::path(rel->text);
      if (!read_bytes(file, bytes)) {
        error = "cannot read " + file.string();
        return false;
      }
      Sha1 h;
      h.update("blob " + std::to_string(bytes.size()));
      const u8 nul = 0;
      h.update(std::span<const u8>(&nul, 1));
      h.update(bytes);
      const Sha1Digest digest = h.finish();
      static const char k_hex[] = "0123456789abcdef";
      std::string hex;
      for (const u8 b : digest) {
        hex.push_back(k_hex[b >> 4]);
        hex.push_back(k_hex[b & 0xF]);
      }
      if (hex != blob->text) {
        error = file.string() + " is not the pinned download (git blob " + hex + ", pinned " +
                blob->text + ")";
        return false;
      }
      ++pinned;
    }
  }
  const auto same = [&](const fs::path& reference, const fs::path& copy) {
    std::string a, b;
    if (!read_bytes(reference, a)) {
      error = "cannot read " + reference.string();
      return false;
    }
    if (!read_bytes(copy, b) || a != b) {
      error = copy.string() + " is not the reference tree's " + reference.string();
      return false;
    }
    ++copies;
    return true;
  };
  for (const char* name : {"LICENSE.md", "LICENSE.ASSETS.md"})
    if (fs::exists(references / name, ec) && !same(references / name, targets / name)) return false;
  const fs::path breast = references / "adult-breast-targets";
  if (fs::is_directory(breast, ec)) {
    for (const auto& entry : fs::directory_iterator(breast, ec)) {
      if (entry.path().extension() != ".target") continue;
      if (!same(entry.path(), targets / entry.path().filename())) return false;
    }
  }
  return true;
}

struct Args {
  std::string source, mapping, controls, targets, references, sex, out, id;
};

// One mesh this command adds channels to.
struct MeshEdit {
  u32 mesh = 0;
  std::string name;
  u32 old_count = 0;
  u32 new_count = 0;
  Vector<std::string> original_names;
};

int run(const Args& args, int argc, char** argv) {
  std::string error;
  const fs::path targets_dir = absolute_path(args.targets);
  const fs::path refs = absolute_path(args.references);
  const fs::path out_dir = absolute_path(args.out);
  const fs::path source = absolute_path(args.source);
  const fs::path mapping_path = absolute_path(args.mapping);
  const fs::path controls_path = absolute_path(args.controls);
  const bool female = args.sex == "female";

  Vector<Control> controls;
  if (!read_controls(controls_path, controls, error)) return fail(error);
  u32 pinned = 0, copies = 0;
  if (!check_targets(targets_dir, refs, pinned, copies, error)) return fail(error);

  // The MakeHuman base and the scale from its raw units to metres.
  BaseMesh base;
  if (!read_base_obj(refs / "base.obj", base, error)) return fail(error);
  const u32 n = base.count();
  TargetCache cache;
  Vector<f64> baseline = base.raw;
  for (const char* ancestry : {"african", "asian", "caucasian"}) {
    const Vector<f64>* t = cache.get(
        refs / "adult-body-baseline-targets" / (std::string(ancestry) + "-male-young.target"), n,
        error);
    if (t == nullptr) return fail(error);
    for (u32 k = 0; k < baseline.size(); ++k)
      baseline[k] += (*t)[k] / 3.0;
  }
  f64 lo = baseline[base.body_ids[0] * 3 + 1];
  f64 hi = lo;
  for (const u32 v : base.body_ids) {
    lo = std::min(lo, baseline[v * 3 + 1]);
    hi = std::max(hi, baseline[v * 3 + 1]);
  }
  const f64 scale = 1.75 / (hi - lo);

  // canonical id -> MakeHuman vertex, which must be the body group's vertex set.
  DocValue control_data;
  if (!read_json(mapping_path, control_data, error)) return fail(error);
  const DocValue* template_ids = control_data.find("source_template_vertex_ids");
  if (template_ids == nullptr || !template_ids->is_array())
    return fail(mapping_path.string() + " has no source_template_vertex_ids");
  Vector<u32> mapping;
  for (const DocValue& v : template_ids->items) {
    i64 x = 0;
    if (!v.get_i64(x) || x < 0 || x >= n)
      return fail(mapping_path.string() + ": a template vertex id outside base.obj");
    mapping.push_back(static_cast<u32>(x));
  }
  if (mapping.size() != base.body_ids.size() ||
      !std::equal(mapping.begin(), mapping.end(), base.body_ids.begin()))
    return fail(
        "the mapping's source_template_vertex_ids are not base.obj's body group, sorted: this "
        "release was not authored on this MakeHuman base");

  GltfDocument doc;
  if (!assets::read_gltf_document(source.string(), doc, &error)) return fail(error);
  const Vector<u8> old_bytes = doc.bin;
  DocValue* meshes = doc.json.find("meshes");
  if (meshes == nullptr || !meshes->is_array()) return fail("the source has no meshes");

  // Which meshes carry the canonical ids; a mesh that does not is left exactly as it was.
  Vector<MeshEdit> edits;
  Vector<std::string> left;
  for (u32 m = 0; m < meshes->items.size(); ++m) {
    const DocValue& mesh = meshes->items[m];
    const DocValue* prims = mesh.find("primitives");
    const DocValue* name = mesh.find("name");
    const std::string mesh_name =
        name != nullptr && name->is_string() ? name->text : "mesh " + std::to_string(m);
    u32 with_ids = 0;
    if (prims != nullptr && prims->is_array())
      for (const DocValue& p : prims->items) {
        const DocValue* attrs = p.find("attributes");
        if (attrs != nullptr && attrs->find("_CANONICAL_ID") != nullptr) ++with_ids;
      }
    if (with_ids == 0) {
      left.push_back(mesh_name);
      continue;
    }
    if (prims->items.size() != 1)
      return fail("mesh " + mesh_name +
                  " carries _CANONICAL_ID and has more than one primitive, which is not supported");
    MeshEdit e;
    e.mesh = m;
    e.name = mesh_name;
    edits.push_back(std::move(e));
  }
  if (edits.empty()) return fail("no mesh of the source carries _CANONICAL_ID");

  // The identity file beside a .gltf source (an export's `*.identity.json`), first by name.
  fs::path identity_path;
  {
    std::error_code ec;
    Vector<std::string> found;
    for (const auto& entry : fs::directory_iterator(source.parent_path(), ec)) {
      const std::string file = entry.path().filename().string();
      if (file.size() > 14 && file.compare(file.size() - 14, 14, ".identity.json") == 0)
        found.push_back(entry.path().string());
    }
    std::sort(found.begin(), found.end());
    if (!found.empty()) identity_path = fs::path(found[0]);
  }
  DocValue identity;
  if (!identity_path.empty() && !read_json(identity_path, identity, error)) return fail(error);

  DocValue added = DocValue::make_array();
  u32 channels_added = 0;
  for (u32 e_index = 0; e_index < edits.size(); ++e_index) {
    MeshEdit& edit = edits[e_index];
    // Looked up afresh: an earlier mesh's appends may have added top-level keys.
    DocValue& mesh = doc.json.find("meshes")->items[edit.mesh];
    DocValue& prim = mesh.find("primitives")->items[0];
    const DocValue& attrs = *prim.find("attributes");
    const auto accessor_index = [&](const DocValue* v, u32& out) {
      i64 x = 0;
      if (v == nullptr || !v->get_i64(x) || x < 0) return false;
      out = static_cast<u32>(x);
      return true;
    };
    u32 id_acc = 0, pos_acc = 0, idx_acc = 0, comps = 0;
    if (!accessor_index(attrs.find("_CANONICAL_ID"), id_acc) ||
        !accessor_index(attrs.find("POSITION"), pos_acc) ||
        !accessor_index(prim.find("indices"), idx_acc))
      return fail("mesh " + edit.name + " needs _CANONICAL_ID, POSITION and indices");
    if (const DocValue* mode = prim.find("mode")) {
      i64 m = 0;
      if (!mode->get_i64(m) || m != 4) return fail("mesh " + edit.name + " is not triangles");
    }
    Vector<f64> raw_ids, raw_pos, raw_idx;
    if (!assets::read_accessor(doc, id_acc, raw_ids, comps, &error) || comps != 1)
      return fail("mesh " + edit.name +
                  "'s _CANONICAL_ID: " + (error.empty() ? "not SCALAR" : error));
    if (!assets::read_accessor(doc, pos_acc, raw_pos, comps, &error) || comps != 3)
      return fail("mesh " + edit.name + "'s POSITION: " + (error.empty() ? "not VEC3" : error));
    if (!assets::read_accessor(doc, idx_acc, raw_idx, comps, &error) || comps != 1)
      return fail("mesh " + edit.name + "'s indices: " + (error.empty() ? "not SCALAR" : error));
    const u32 verts = raw_pos.size() / 3;
    if (raw_ids.size() != verts) return fail("mesh " + edit.name + ": one id a vertex is needed");
    Vector<u32> ids(verts);
    for (u32 v = 0; v < verts; ++v) {
      const f64 x = raw_ids[v];
      if (!(x >= 0.0) || x >= static_cast<f64>(mapping.size()) || std::floor(x) != x)
        return fail("mesh " + edit.name + "'s _CANONICAL_ID holds " + std::to_string(x) +
                    " at vertex " + std::to_string(v) +
                    ", which is not a canonical id of the mapping");
      ids[v] = static_cast<u32>(x);
    }
    if (raw_idx.size() % 3 != 0)
      return fail("mesh " + edit.name + ": an index count not a multiple of 3");
    Vector<u32> tri(raw_idx.size());
    for (u32 k = 0; k < raw_idx.size(); ++k) {
      if (raw_idx[k] >= verts) return fail("mesh " + edit.name + ": an index past the vertices");
      tri[k] = static_cast<u32>(raw_idx[k]);
    }
    if (e_index == 0 && identity.is_object()) {
      const DocValue* maps = identity.find("mappings");
      const DocValue* export_ids = maps != nullptr && maps->is_array() && !maps->items.empty()
                                       ? maps->items[0].find("export_vertex_to_canonical_id")
                                       : nullptr;
      bool equal =
          export_ids != nullptr && export_ids->is_array() && export_ids->items.size() == verts;
      for (u32 v = 0; equal && v < verts; ++v) {
        i64 x = 0;
        equal = export_ids->items[v].get_i64(x) && x == ids[v];
      }
      if (!equal)
        return fail(identity_path.string() + "'s export_vertex_to_canonical_id is not mesh " +
                    edit.name + "'s _CANONICAL_ID");
    }

    Vector<f32> pos(verts * 3);
    for (u32 k = 0; k < pos.size(); ++k)
      pos[k] = static_cast<f32>(raw_pos[k]);
    Vector<f64> normal_base;
    canonical_normals(pos, tri, ids, mapping.size(), normal_base);

    // The names the mesh had, which must be one per existing target.
    DocValue* targets = prim.find("targets");
    edit.old_count = targets != nullptr && targets->is_array() ? targets->items.size() : 0;
    DocValue* extras = mesh.find("extras");
    DocValue* names = extras != nullptr ? extras->find("targetNames") : nullptr;
    if (names != nullptr && names->is_array()) {
      for (const DocValue& v : names->items)
        edit.original_names.push_back(v.text);
    }
    if (edit.original_names.size() != edit.old_count)
      return fail("mesh " + edit.name + " has " + std::to_string(edit.old_count) +
                  " morph targets and " + std::to_string(edit.original_names.size()) +
                  " extras.targetNames");
    if (targets == nullptr) targets = &prim.set("targets", DocValue::make_array());
    DocValue new_names = DocValue::make_array();
    if (names != nullptr) new_names = *names;

    Vector<f64> sum, delta(verts * 3), q(verts * 3), normal_morphed, dn(verts * 3);
    for (const Control& control : controls) {
      if (control.female && !female) continue;
      for (u32 side = 0; side < 2; ++side) {
        const Vector<std::string>& files = side == 0 ? control.negative : control.positive;
        const std::string channel = "shape." + control.key + (side == 0 ? ".less" : ".more");
        sum.clear();
        sum.resize(n * 3, 0.0);
        DocValue file_list = DocValue::make_array();
        for (const std::string& rel : files) {
          const fs::path path = (targets_dir / fs::path(rel)).make_preferred();
          const Vector<f64>* t = cache.get(path, n, error);
          if (t == nullptr) return fail(error);
          for (u32 k = 0; k < sum.size(); ++k)
            sum[k] = sum[k] + (*t)[k];
          std::string hash;
          if (!sha256_file(path, hash)) return fail("cannot read " + path.string());
          DocValue entry = DocValue::make_object();
          entry.set("path", DocValue::make_string(native_path(path)));
          entry.set("sha256", DocValue::make_string(hash));
          file_list.push_back(std::move(entry));
        }
        bool any = false;
        for (u32 v = 0; v < verts; ++v) {
          const u32 mh = mapping[ids[v]];
          for (u32 c = 0; c < 3; ++c) {
            const f64 d = sum[mh * 3 + c] * scale;
            if (!std::isfinite(d)) return fail(channel + ": a displacement that is not finite");
            any = any || d != 0.0;
            delta[v * 3 + c] = d;
            q[v * 3 + c] = static_cast<f64>(pos[v * 3 + c]) + d;
          }
        }
        if (!any) return fail(channel + " moves no vertex of mesh " + edit.name);
        canonical_normals(q, tri, ids, mapping.size(), normal_morphed);
        for (u32 k = 0; k < dn.size(); ++k)
          dn[k] = normal_morphed[k] - normal_base[k];

        const Vector<f32> delta32 = to_f32(delta);
        const Vector<f32> dn32 = to_f32(dn);
        DocValue target = DocValue::make_object();
        target.set("POSITION", DocValue::make_int(assets::append_float_accessor(doc, delta32, 3)));
        target.set("NORMAL", DocValue::make_int(assets::append_float_accessor(doc, dn32, 3)));
        // `doc` may have grown its arrays: find the mesh again before touching it.
        DocValue& prim_now = doc.json.find("meshes")->items[edit.mesh].find("primitives")->items[0];
        prim_now.find("targets")->push_back(std::move(target));
        new_names.push_back(DocValue::make_string(channel));
        ++channels_added;

        if (e_index == 0) {
          u32 moved = 0;
          f64 max_delta = 0.0;
          for (u32 v = 0; v < verts; ++v) {
            const f64 length = vector_length(&delta[v * 3]);
            if (length > 1e-8) ++moved;
            max_delta = v == 0 ? length : std::max(max_delta, length);
          }
          DocValue info = DocValue::make_object();
          info.set("name", DocValue::make_string(channel));
          info.set("label", DocValue::make_string(control.label));
          info.set("files", std::move(file_list));
          info.set("vertices_moved", DocValue::make_int(moved));
          info.set("max_delta_m", DocValue::make_real(max_delta));
          added.push_back(std::move(info));
        }
      }
    }
    // Pointers into the document may have moved with the appends; look everything up again.
    DocValue& mesh_now = doc.json.find("meshes")->items[edit.mesh];
    edit.new_count = new_names.items.size();
    if (mesh_now.find("extras") == nullptr) mesh_now.set("extras", DocValue::make_object());
    mesh_now.find("extras")->set("targetNames", std::move(new_names));
    DocValue weights = DocValue::make_array();
    if (const DocValue* old = mesh_now.find("weights")) {
      weights = *old;
    } else {
      for (u32 k = 0; k < edit.old_count; ++k)
        weights.push_back(DocValue::make_real(0.0));
    }
    for (u32 k = edit.old_count; k < edit.new_count; ++k)
      weights.push_back(DocValue::make_real(0.0));
    mesh_now.set("weights", std::move(weights));
    if (DocValue* nodes = doc.json.find("nodes"); nodes != nullptr && nodes->is_array()) {
      for (DocValue& node : nodes->items) {
        const DocValue* m = node.find("mesh");
        i64 index = -1;
        if (m == nullptr || !m->get_i64(index) || index != edit.mesh) continue;
        if (DocValue* w = node.find("weights"); w != nullptr && w->is_array())
          for (u32 k = edit.old_count; k < edit.new_count; ++k)
            w->push_back(DocValue::make_real(0.0));
      }
    }
  }

  // Every animation's weights track keeps one value per channel: zeros for the new ones.
  u32 tracks_padded = 0;
  if (DocValue* animations = doc.json.find("animations");
      animations != nullptr && animations->is_array()) {
    for (u32 a = 0; a < animations->items.size(); ++a) {
      Vector<u32> seen;
      const u32 channel_count = [&] {
        const DocValue* ch = animations->items[a].find("channels");
        return ch != nullptr && ch->is_array() ? ch->items.size() : 0u;
      }();
      for (u32 c = 0; c < channel_count; ++c) {
        DocValue& animation = doc.json.find("animations")->items[a];
        const DocValue& channel = animation.find("channels")->items[c];
        const DocValue* target = channel.find("target");
        const DocValue* path = target != nullptr ? target->find("path") : nullptr;
        i64 sampler_index = -1, node_index = -1;
        if (path == nullptr || path->text != "weights") continue;
        if (!channel.find("sampler") || !channel.find("sampler")->get_i64(sampler_index)) continue;
        if (std::find(seen.begin(), seen.end(), static_cast<u32>(sampler_index)) != seen.end())
          continue;
        const DocValue* node_value = target->find("node");
        if (node_value == nullptr || !node_value->get_i64(node_index)) continue;
        const DocValue* nodes = doc.json.find("nodes");
        if (nodes == nullptr || node_index < 0 || node_index >= nodes->items.size()) continue;
        const DocValue* mesh_value = nodes->items[static_cast<u32>(node_index)].find("mesh");
        i64 mesh_index = -1;
        if (mesh_value == nullptr || !mesh_value->get_i64(mesh_index)) continue;
        const MeshEdit* edit = nullptr;
        for (const MeshEdit& e : edits)
          if (e.mesh == mesh_index) edit = &e;
        if (edit == nullptr) continue;
        seen.push_back(static_cast<u32>(sampler_index));
        DocValue* samplers = animation.find("samplers");
        if (samplers == nullptr || sampler_index < 0 || sampler_index >= samplers->items.size())
          return fail("an animation's weights channel names a sampler that does not exist");
        i64 output = -1;
        const DocValue* out_value = samplers->items[static_cast<u32>(sampler_index)].find("output");
        if (out_value == nullptr || !out_value->get_i64(output) || output < 0)
          return fail("an animation sampler without an output");
        Vector<f64> values;
        u32 comps = 0;
        if (!assets::read_accessor(doc, static_cast<u32>(output), values, comps, &error))
          return fail(error);
        if (edit->old_count == 0 || values.size() % edit->old_count != 0)
          return fail("a weights track whose length is not a multiple of mesh " + edit->name +
                      "'s channel count");
        const u32 keys = values.size() / edit->old_count;
        Vector<f32> expanded(keys * edit->new_count, 0.0f);
        for (u32 k = 0; k < keys; ++k)
          for (u32 w = 0; w < edit->old_count; ++w)
            expanded[k * edit->new_count + w] = static_cast<f32>(values[k * edit->old_count + w]);
        const u32 index = assets::append_float_accessor(doc, expanded, 1);
        doc.json.find("animations")
            ->items[a]
            .find("samplers")
            ->items[static_cast<u32>(sampler_index)]
            .set("output", DocValue::make_int(index));
        ++tracks_padded;
      }
    }
  }
  if (doc.bin.size() < old_bytes.size() ||
      std::memcmp(doc.bin.data(), old_bytes.data(), old_bytes.size()) != 0)
    return fail("internal: the source's buffer did not survive as the output's prefix");

  std::error_code ec;
  fs::create_directories(out_dir, ec);
  if (!fs::is_directory(out_dir, ec)) return fail("cannot create " + out_dir.string());
  if (!assets::write_gltf_document(doc, out_dir.string(), "body", k_newline, &error))
    return fail(error);
  std::string gltf_hash, bin_hash, source_hash, mapping_hash, controls_hash;
  if (!sha256_file(out_dir / "body.gltf", gltf_hash) ||
      !sha256_file(out_dir / "body.bin", bin_hash) || !sha256_file(source, source_hash) ||
      !sha256_file(mapping_path, mapping_hash) || !sha256_file(controls_path, controls_hash))
    return fail("cannot hash the inputs and outputs");

  // The identity: the source's, with this revision, the new file's hash and where it came from;
  // a source with none (a GLB export) gets one made from its meshes' ids.
  const std::string revision = args.id.empty() ? out_dir.filename().string() : args.id;
  if (identity.is_object()) {
    identity.set("revision", DocValue::make_string(revision));
    identity.set("gltf_sha256", DocValue::make_string(gltf_hash));
    identity.set("source_identity", DocValue::make_string(native_path(identity_path)));
  } else {
    identity = DocValue::make_object();
    identity.set("schema", DocValue::make_string("canonical.export.identity/1"));
    identity.set("revision", DocValue::make_string(revision));
    identity.set("gltf_sha256", DocValue::make_string(gltf_hash));
    identity.set("attribute", DocValue::make_string("_CANONICAL_ID"));
    identity.set("source_identity", DocValue{});
  }
  if (!write_json_file(out_dir / "body.identity.json", identity, error)) return fail(error);

  // The provenance record, in the durable script's shape, with the recipe naming this tool.
  DocValue retained = DocValue::make_array();
  for (const std::string& s : edits[0].original_names)
    retained.push_back(DocValue::make_string(s));
  DocValue result = DocValue::make_object();
  result.set("asset", DocValue::make_string(native_path(out_dir / "body.gltf")));
  result.set("sex", DocValue::make_string(args.sex));
  DocValue src = DocValue::make_object();
  src.set("path", DocValue::make_string(native_path(source)));
  src.set("sha256", DocValue::make_string(source_hash));
  result.set("source", std::move(src));
  result.set("mapping_source", DocValue::make_string(native_path(mapping_path)));
  result.set("mapping_sha256", DocValue::make_string(mapping_hash));
  result.set("scale_raw_to_m", DocValue::make_real(scale));
  result.set("original_channels_retained", std::move(retained));
  result.set("total_channels", DocValue::make_int(edits[0].new_count));
  result.set("added_channels", std::move(added));
  DocValue checks = DocValue::make_object();
  checks.set("original_binary_prefix_unchanged", DocValue::make_bool(true));
  checks.set("identity_map_matches_export",
             DocValue::make_bool(identity_path.empty() ? false : true));
  checks.set("template_mapping_matches_body_obj_group", DocValue::make_bool(true));
  checks.set("added_delta_finite_nonzero", DocValue::make_bool(true));
  checks.set("qa_weight_tracks_extended", DocValue::make_bool(true));
  checks.set("pinned_targets_checked", DocValue::make_int(pinned));
  checks.set("reference_copies_checked", DocValue::make_int(copies));
  result.set("checks", std::move(checks));
  result.set("limits",
             DocValue::make_string(
                 "Artistic surface morphology, not a calibrated tissue simulation. Bind skeleton "
                 "and collision capsule unchanged. Combined extremes and gait need validation."));
  DocValue meshes_record = DocValue::make_array();
  for (const MeshEdit& e : edits) {
    DocValue m = DocValue::make_object();
    m.set("mesh", DocValue::make_int(e.mesh));
    m.set("name", DocValue::make_string(e.name));
    m.set("channels_before", DocValue::make_int(e.old_count));
    m.set("channels_after", DocValue::make_int(e.new_count));
    meshes_record.push_back(std::move(m));
  }
  result.set("meshes_morphed", std::move(meshes_record));
  DocValue left_record = DocValue::make_array();
  for (const std::string& s : left)
    left_record.push_back(DocValue::make_string(s));
  result.set("meshes_left_unchanged", std::move(left_record));
  DocValue recipe = DocValue::make_object();
  recipe.set("tool", DocValue::make_string("engine-content morphs"));
  recipe.set("commit", DocValue::make_string(build_stamp::commit()));
  recipe.set("dirty", DocValue::make_bool(build_stamp::dirty()));
  std::string command_line;
  for (int k = 0; k < argc; ++k) {
    if (k > 0) command_line.push_back(' ');
    command_line += argv[k];
  }
  recipe.set("command_line", DocValue::make_string(command_line));
  recipe.set("controls", DocValue::make_string(native_path(controls_path)));
  recipe.set("controls_sha256", DocValue::make_string(controls_hash));
  recipe.set("id", DocValue::make_string(revision));
  result.set("recipe", std::move(recipe));
  if (!write_json_file(out_dir / "provenance.json", result, error)) return fail(error);

  // The summary line every engine-content command prints.
  JsonValue summary = JsonValue::object();
  summary.set("command", "morphs");
  summary.set("source", native_path(source));
  summary.set("input", doc.from_glb ? "glb" : "gltf");
  summary.set("output", "gltf+bin");
  summary.set("out", native_path(out_dir));
  summary.set("sex", args.sex);
  summary.set("revision", revision);
  summary.set("scale_raw_to_m", scale);
  summary.set("channels_added", channels_added);
  summary.set("tracks_padded", tracks_padded);
  JsonValue morphed = JsonValue::array();
  for (const MeshEdit& e : edits) {
    JsonValue m = JsonValue::object();
    m.set("mesh", e.mesh);
    m.set("name", e.name);
    m.set("channels_before", e.old_count);
    m.set("channels_after", e.new_count);
    morphed.push_back(std::move(m));
  }
  summary.set("meshes_morphed", std::move(morphed));
  JsonValue unchanged = JsonValue::array();
  for (const std::string& s : left)
    unchanged.push_back(JsonValue(s));
  summary.set("meshes_left_unchanged", std::move(unchanged));
  summary.set("identity_source",
              identity_path.empty() ? JsonValue() : JsonValue(native_path(identity_path)));
  summary.set("pinned_targets_checked", pinned);
  summary.set("body_gltf_sha256", gltf_hash);
  summary.set("body_bin_sha256", bin_hash);
  std::string text = write_json(summary, JsonWriteOptions{.pretty = false});
  text.push_back('\n');
  std::fwrite(text.data(), 1, text.size(), stdout);
  return k_ok;
}

}  // namespace

int morphs_command(int argc, char** argv) {
  Args args;
  for (int i = 2; i < argc; ++i) {
    const std::string_view a = argv[i];
    if (a == "--help" || a == "-h") {
      std::fputs(k_morphs_usage, stdout);
      return k_ok;
    }
    std::string* slot = nullptr;
    if (a == "--mapping")
      slot = &args.mapping;
    else if (a == "--controls")
      slot = &args.controls;
    else if (a == "--targets")
      slot = &args.targets;
    else if (a == "--references")
      slot = &args.references;
    else if (a == "--sex")
      slot = &args.sex;
    else if (a == "--out")
      slot = &args.out;
    else if (a == "--id")
      slot = &args.id;
    if (slot != nullptr) {
      if (i + 1 >= argc) return usage("an option is missing its value");
      *slot = argv[++i];
      continue;
    }
    if (!a.empty() && a[0] == '-') return usage("unknown option");
    if (!args.source.empty()) return usage("one source body only");
    args.source = std::string(a);
  }
  if (args.source.empty() || args.mapping.empty() || args.controls.empty() ||
      args.targets.empty() || args.references.empty() || args.out.empty() || args.sex.empty())
    return usage(
        "the source, --mapping, --controls, --targets, --references, --sex and --out "
        "are all required");
  if (args.sex != "male" && args.sex != "female") return usage("--sex is male or female");
  return run(args, argc, argv);
}

}  // namespace engine::content
