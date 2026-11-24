#include <core/hash/hash.h>
#include <core/platform/random.h>
#include <core/time/time.h>
#include <foundation/io/vfs.h>
#include <systems/renderer/micromap_cache.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <system_error>

namespace engine::renderer {

namespace {

// A UTF-8 path as the standard library's, as foundation/io makes it.
std::filesystem::path path_of(std::string_view utf8) {
  return std::filesystem::path(
      std::u8string_view(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

constexpr u32 k_levels = gfx::k_micromap_max_level + 1;

u32 bits_of(f32 value) noexcept {
  u32 bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  return bits;
}

void put_u32(std::string& out, u32 v) {
  for (u32 i = 0; i < 4; ++i)
    out.push_back(static_cast<char>((v >> (8 * i)) & 0xffu));
}
void put_u64(std::string& out, u64 v) {
  for (u32 i = 0; i < 8; ++i)
    out.push_back(static_cast<char>((v >> (8 * i)) & 0xffu));
}
u32 get_u32(const u8* p) noexcept {
  return u32{p[0]} | (u32{p[1]} << 8) | (u32{p[2]} << 16) | (u32{p[3]} << 24);
}
u64 get_u64(const u8* p) noexcept { return u64{get_u32(p)} | (u64{get_u32(p + 4)} << 32); }

// The fixed prefix every pack begins with: magic, version, generation, content hash, header length.
constexpr u64 k_prefix_bytes = 4 + 4 + 8 + 8 + 4;
constexpr usize k_hash_offset = 16;

std::string hex16(u64 v) {
  char text[20];
  std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(v));
  return text;
}

bool is_hex16(std::string_view name) noexcept {
  if (name.size() != 16) return false;
  for (const char c : name)
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  return true;
}

bool ends_with(std::string_view s, std::string_view tail) noexcept {
  return s.size() >= tail.size() && s.substr(s.size() - tail.size()) == tail;
}

struct Section {
  const void* bytes;
  u64 size;
};

void sections_of(const MicromapPackView& p, Section out[3]) {
  out[0] = Section{p.indices.data(), u64{p.indices.size()} * sizeof(i32)};
  out[1] = Section{p.triangles.data(), u64{p.triangles.size()} * sizeof(gfx::MicromapTriangle)};
  out[2] = Section{p.data.data(), u64{p.data.size()} * sizeof(u32)};
}

// The content hash: the header after its prefix, then every section's bytes in order, each a
// chained `hash_bytes` over 4 MiB pieces so a reader that sees them a piece at a time agrees.
constexpr u64 k_piece = u64{4} << 20;

u64 hash_piecewise(u64 seed, const void* bytes, u64 size) noexcept {
  const auto* p = static_cast<const u8*>(bytes);
  for (u64 at = 0; at < size; at += k_piece)
    seed = hash_bytes(p + at, static_cast<usize>(std::min(k_piece, size - at)), seed);
  return seed;
}

}  // namespace

u64 micromap_generation(const u8 device[28], u64 shader) noexcept {
  u64 h = hash_bytes("engine.micromap.generation", 26);
  h = hash_combine(h, k_micromap_cache_version);
  h = hash_bytes(device, 28, h);
  return hash_combine(h, shader);
}

std::string micromap_generation_dir(std::string_view ddc_root, u64 generation) {
  return io::join_path(io::join_path(ddc_root, "micromaps"), hex16(generation));
}

u64 micromap_material_digest(const MicromapMaterialWords& m) noexcept {
  // Exactly what the bake reads of a record (opacity_micromap_bake.slang: `material_uv`, the
  // cutoff, the factor, the albedo and its sampler), the slots replaced by their contents. Of the
  // flags, only the bit the bake reads (`material_uv`'s; R207-L3): the foliage, normal, mip and
  // stochastic bits shade but do not bake, and keyed they would only cost a rebake. The sampler's
  // description stays whole: Vulkan applies its LOD bias and clamp to an explicit level-0 sample
  // too, so a sampler word is not provably outside what the bake reads.
  const gfx::ResolveMaterial& r = m.record;
  const u32 words[] = {r.flags & gfx::k_material_uv_transform,
                       bits_of(r.uv_scale),
                       bits_of(r.uv_transform.x),
                       bits_of(r.uv_transform.y),
                       bits_of(r.uv_transform.z),
                       bits_of(r.uv_transform.w),
                       bits_of(r.uv_offset.x),
                       bits_of(r.uv_offset.y),
                       bits_of(r.uv_offset.z),
                       bits_of(r.uv_offset.w),
                       r.albedo_texture == gfx::k_no_texture ? 0u : 1u};
  u64 h = hash_bytes(words, sizeof(words), k_hash_seed);
  h = hash_combine(h, m.albedo_identity);
  return hash_combine(h, m.sampler_identity);
}

u64 micromap_part_key(const MicromapPartKeyInput& in) noexcept {
  u64 h = hash_bytes("engine.micromap.part", 20);
  h = hash_combine(h, in.generation);
  h = hash_combine(h, in.level_cap);
  h = hash_combine(h, bits_of(in.target_texels));
  const usize n = in.locals.size();
  h = hash_combine(h, n);
  for (usize i = 0; i < n; ++i) {
    h = hash_combine(h, in.locals[i]);
    h = hash_combine(h, i < in.triangle_counts.size() ? in.triangle_counts[i] : ~0u);
    h = hash_combine(h, i < in.materials.size() ? in.materials[i] : ~u64{0});
    h = hash_combine(h, i < in.digests.size() ? in.digests[i] : ~u64{0});
  }
  return h;
}

void micromap_key_materials(MicromapKeyState& state, std::span<const u32> list,
                            std::span<const gfx::ResolveMaterial> table,
                            const HashMap<u32, u64>& slots, std::span<const u32> sampler_slots,
                            std::span<const u32> sampler_keys, std::string_view root) {
  state.material_digests.clear();
  state.unkeyed.clear();
  state.root.clear();
  if (list.empty()) return;
  if (root.empty()) {
    state.unkeyed = "the load reads no derived-data root";
    return;
  }
  // Each masked material once: the list names the materials by index, and a part's key names
  // the digest of the record each index holds.
  Vector<u64> digests(static_cast<u32>(table.size()), u64{0});
  for (usize i = 1; i < list.size(); i += 2) {
    const u32 material = list[i];
    if (material >= table.size()) {
      state.unkeyed =
          "a masked cluster names material " + std::to_string(material) + ", past the table";
      return;
    }
    if (digests[material] != 0) continue;
    MicromapMaterialWords w;
    w.record = table[material];
    if (w.record.albedo_texture != gfx::k_no_texture) {
      const u64* content = slots.find_value(w.record.albedo_texture);
      if (content == nullptr) {
        state.unkeyed =
            "material " + std::to_string(material) + "'s albedo texture has no content identity";
        return;
      }
      w.albedo_identity = *content;
      for (usize s = 0; s < sampler_slots.size() && s < sampler_keys.size(); ++s)
        if (sampler_slots[s] == w.record.sampler)
          w.sampler_identity = u64{sampler_keys[s]} | (u64{1} << 32);
      if (w.sampler_identity == 0) {
        state.unkeyed =
            "material " + std::to_string(material) + "'s sampler is not one the materials made";
        return;
      }
    }
    digests[material] = micromap_material_digest(w) | 1u;  // never 0: 0 is "not masked"
  }
  state.material_digests = std::move(digests);
  state.root = std::string(root);
}

// ---- packs --------------------------------------------------------------------------------------

std::string micromap_pack_path(std::string_view ddc_root, u64 generation,
                               std::span<const MicromapPackPart> parts) {
  u64 h = hash_bytes("engine.micromap.pack", 20);
  for (const MicromapPackPart& p : parts)
    h = hash_combine(h, p.key);
  return io::join_path(micromap_generation_dir(ddc_root, generation), hex16(h) + ".omm");
}

std::string micromap_pack_header(u64 generation, const MicromapPackView& pack) {
  std::string out;
  put_u32(out, k_micromap_file_magic);
  put_u32(out, k_micromap_cache_version);
  put_u64(out, generation);
  put_u64(out, 0);  // the content hash, filled below
  put_u32(out, 0);  // the header's length, filled below
  put_u32(out, static_cast<u32>(pack.parts.size()));
  put_u32(out, static_cast<u32>(pack.indices.size()));
  put_u32(out, static_cast<u32>(pack.triangles.size()));
  put_u64(out, u64{pack.data.size()} * sizeof(u32));
  for (const MicromapPackPart& p : pack.parts) {
    put_u64(out, p.key);
    put_u32(out, p.triangle_count);
    for (u32 l = 0; l < k_levels; ++l)
      put_u32(out, p.wanted[l]);
  }
  const u32 length = static_cast<u32>(out.size());
  for (u32 i = 0; i < 4; ++i)
    out[k_prefix_bytes - 4 + i] = static_cast<char>((length >> (8 * i)) & 0xffu);
  u64 h = hash_bytes(out.data() + k_prefix_bytes, out.size() - k_prefix_bytes);
  Section parts[3];
  sections_of(pack, parts);
  for (const Section& part : parts)
    h = hash_piecewise(h, part.bytes, part.size);
  for (u32 i = 0; i < 8; ++i)
    out[k_hash_offset + i] = static_cast<char>((h >> (8 * i)) & 0xffu);
  return out;
}

bool write_micromap_pack(const std::string& path, u64 generation, const MicromapPackView& pack,
                         std::string* error) {
  auto fail = [&](const std::string& why, const std::string& temp) {
    if (error != nullptr) *error = why;
    if (!temp.empty()) (void)io::remove_file(temp);
    return false;
  };
  const std::string dir(io::parent_path(path));
  if (io::make_directories(dir) != io::Status::Ok) return fail("cannot make " + dir, {});
  // A name of this write's own, so two loads writing one pack never write one file: the clock and
  // 64 bits from the system's generator, since two processes can read one clock value (R207-L1).
  u64 token = 0;
  (void)platform::secure_random(std::span<u8>(reinterpret_cast<u8*>(&token), sizeof(token)));
  const std::string temp =
      path + "." + std::to_string(time::monotonic_ns()) + "." + hex16(token) + ".tmp";
  if (io::write_file(temp, micromap_pack_header(generation, pack)) != io::Status::Ok)
    return fail("cannot write " + temp, temp);
  Section parts[3];
  sections_of(pack, parts);
  constexpr u64 k_chunk = u64{64} << 20;
  for (const Section& part : parts) {
    const auto* p = static_cast<const char*>(part.bytes);
    for (u64 at = 0; at < part.size; at += k_chunk) {
      const std::string_view chunk(p + at, static_cast<usize>(std::min(k_chunk, part.size - at)));
      if (io::append_file(temp, chunk) != io::Status::Ok)
        return fail("cannot append to " + temp, temp);
    }
  }
  if (io::rename_path(temp, path) != io::Status::Ok) {
    // Not stored (R207-L2): the file at `path` may be held open by another load reading it; its
    // parts are the same keys, so nothing is lost but this write.
    return fail(io::exists(path)
                    ? "cannot rename " + temp + " over " + path + ": the file there is in use"
                    : "cannot rename " + temp,
                temp);
  }
  return true;
}

MicromapPackView MicromapPackData::view() const {
  MicromapPackView v;
  v.parts = std::span<const MicromapPackPart>(parts.data(), parts.size());
  v.indices = std::span<const i32>(indices.data(), indices.size());
  v.triangles = std::span<const gfx::MicromapTriangle>(triangles.data(), triangles.size());
  v.data = std::span<const u32>(data.data(), data.size());
  return v;
}

bool read_micromap_pack_index(const std::string& path, u64 generation, MicromapPackIndex& out) {
  out = MicromapPackIndex{};
  out.path = path;
  io::FileHandle file;
  if (file.open(path) != io::Status::Ok) {
    out.refused = "it did not open";
    return false;
  }
  out.file_bytes = file.size();
  // From here a refusal is of the bytes, and the pack is damaged, unless a read itself failed.
  out.damaged = true;
  u8 prefix[k_micromap_pack_fixed_bytes];
  u64 read = 0;
  if (out.file_bytes < sizeof(prefix)) {
    out.refused = "shorter than a pack's header";
    return false;
  }
  if (file.read_at(0, prefix, sizeof(prefix), read) != io::Status::Ok || read != sizeof(prefix)) {
    out.damaged = false;
    out.refused = "its header did not read";
    return false;
  }
  if (get_u32(prefix) != k_micromap_file_magic) {
    out.refused = "not a stored bake";
    return false;
  }
  if (get_u32(prefix + 4) != k_micromap_cache_version) {
    out.refused = "made by version " + std::to_string(get_u32(prefix + 4)) + ", this is " +
                  std::to_string(k_micromap_cache_version);
    return false;
  }
  if (get_u64(prefix + 8) != generation) {
    out.refused = "made for another generation";
    return false;
  }
  out.content_hash = get_u64(prefix + k_hash_offset);
  out.header_bytes = get_u32(prefix + k_prefix_bytes - 4);
  const u32 part_count = get_u32(prefix + k_prefix_bytes);
  out.triangle_count = get_u32(prefix + k_prefix_bytes + 4);
  out.entry_count = get_u32(prefix + k_prefix_bytes + 8);
  out.data_bytes = get_u64(prefix + k_prefix_bytes + 12);
  // The file bounds every count before anything is allocated: the header is exactly its parts'
  // records, every section fits the file, and the sections and the header are the file.
  if (u64{part_count} * k_micromap_part_record_bytes + k_micromap_pack_fixed_bytes !=
          out.header_bytes ||
      out.header_bytes > out.file_bytes) {
    out.refused = "its header's length is not its parts'";
    return false;
  }
  const u64 sizes[3] = {u64{out.triangle_count} * sizeof(i32),
                        u64{out.entry_count} * sizeof(gfx::MicromapTriangle), out.data_bytes};
  u64 total = out.header_bytes;
  for (const u64 size : sizes) {
    if (size > out.file_bytes) {
      out.refused = "a section names more bytes than the file holds";
      return false;
    }
    total += size;
  }
  if (total != out.file_bytes || out.data_bytes % sizeof(u32) != 0) {
    out.refused = "its length is not its sections' (" + std::to_string(out.file_bytes) +
                  " bytes, " + std::to_string(total) + " named)";
    return false;
  }
  if (out.entry_count > out.triangle_count) {
    out.refused = "it stores more entries than it has triangles";
    return false;
  }
  Vector<u8> records(out.header_bytes - k_micromap_pack_fixed_bytes);
  if (!records.empty() && (file.read_at(k_micromap_pack_fixed_bytes, records.data(), records.size(),
                                        read) != io::Status::Ok ||
                           read != records.size())) {
    out.damaged = false;
    out.refused = "its part records did not read";
    return false;
  }
  u64 triangles = 0;
  out.parts.resize(part_count);
  for (u32 p = 0; p < part_count; ++p) {
    const u8* r = records.data() + u64{p} * k_micromap_part_record_bytes;
    MicromapPackPart& part = out.parts[p];
    part.key = get_u64(r);
    part.triangle_count = get_u32(r + 8);
    for (u32 l = 0; l < k_levels; ++l)
      part.wanted[l] = get_u32(r + 12 + 4 * l);
    triangles += part.triangle_count;
  }
  if (triangles != out.triangle_count) {
    out.refused = "its parts' triangles are not its own count";
    return false;
  }
  out.damaged = false;
  return true;
}

bool read_micromap_pack(const MicromapPackIndex& index, MicromapPackData& out, std::string* refused,
                        bool* damaged) {
  // `content`: the bytes were read and are refused for what they say (R208-L7); otherwise the
  // file did not open, moved under the read or a read failed, and it is a miss and left alone.
  auto fail = [&](const std::string& why, bool content) {
    if (refused != nullptr) *refused = why;
    if (damaged != nullptr) *damaged = content;
    out = MicromapPackData{};
    return false;
  };
  if (damaged != nullptr) *damaged = false;
  io::FileHandle file;
  if (file.open(index.path) != io::Status::Ok) return fail("it did not open", false);
  if (file.size() != index.file_bytes)
    return fail("its length changed since its header was read", false);
  Vector<u8> header(index.header_bytes - k_prefix_bytes);
  u64 read = 0;
  if (file.read_at(k_prefix_bytes, header.data(), header.size(), read) != io::Status::Ok ||
      read != header.size()) {
    return fail("its header did not read", false);
  }
  out.parts = index.parts;
  out.indices.resize(index.triangle_count);
  out.triangles.resize(index.entry_count);
  out.data.resize(static_cast<u32>(index.data_bytes / sizeof(u32)));
  u64 h = hash_bytes(header.data(), header.size());
  u64 cursor = index.header_bytes;
  const Section parts[3] = {
      {out.indices.data(), u64{out.indices.size()} * sizeof(i32)},
      {out.triangles.data(), u64{out.triangles.size()} * sizeof(gfx::MicromapTriangle)},
      {out.data.data(), u64{out.data.size()} * sizeof(u32)}};
  for (const Section& part : parts) {
    auto* dst = static_cast<u8*>(const_cast<void*>(part.bytes));
    for (u64 at = 0; at < part.size; at += k_piece) {
      const u64 piece = std::min(k_piece, part.size - at);
      if (file.read_at(cursor, dst + at, piece, read) != io::Status::Ok || read != piece)
        return fail("a section did not read", false);
      h = hash_bytes(dst + at, static_cast<usize>(piece), h);
      cursor += piece;
    }
  }
  if (h != index.content_hash) return fail("its content hash is not its bytes'", true);
  // The tables the assembly reads, held to the pack's own counts before anything is copied.
  for (u32 t = 0; t < out.indices.size(); ++t) {
    if (!gfx::micromap_index_valid(out.indices[t], index.entry_count)) {
      return fail("an index names no stored entry (triangle " + std::to_string(t) + ", " +
                      std::to_string(out.indices[t]) + " of " + std::to_string(index.entry_count) +
                      ")",
                  true);
    }
  }
  for (u32 e = 0; e < out.triangles.size(); ++e) {
    const gfx::MicromapTriangle& r = out.triangles[e];
    if (r.level > gfx::k_micromap_max_level || r.format != gfx::k_micromap_format_4state ||
        r.data_offset % sizeof(u32) != 0 ||
        u64{r.data_offset} + gfx::micromap_entry_bytes(r.level) > index.data_bytes) {
      return fail(
          "a triangle record names states the pack does not hold (entry " + std::to_string(e) + ")",
          true);
    }
  }
  out.first_triangle.resize(static_cast<u32>(out.parts.size()));
  u32 first = 0;
  for (u32 p = 0; p < out.parts.size(); ++p) {
    out.first_triangle[p] = first;
    first += out.parts[p].triangle_count;
  }
  return true;
}

// ---- the assembly -------------------------------------------------------------------------------

namespace {

// An entry's micro-triangles by state: the first 4^level two-bit states of its words (the rest of
// the last word is padding).
void count_states(const u32* words, u32 level, u64 out[3]) noexcept {
  const u64 states = u64{1} << (2 * level);
  out[0] = out[1] = out[2] = 0;
  for (u64 s = 0; s < states; ++s) {
    const u32 v = (words[s / 16] >> (2 * (s % 16))) & 3u;
    if (v == gfx::k_micromap_transparent)
      ++out[0];
    else if (v == gfx::k_micromap_opaque)
      ++out[1];
    else
      ++out[2];
  }
}

}  // namespace

bool assemble_micromaps(std::span<const MicromapPackView> sources,
                        std::span<const MicromapRun> runs, MicromapAssembly& out) {
  out = MicromapAssembly{};
  gfx::MicromapStats& stats = out.stats;
  u32 triangle_count = 0;
  for (const MicromapRun& run : runs) {
    // Every run inside its source's indices, checked rather than assumed (R208-M2); the records and
    // states each index names were held to the source's own counts when it was read.
    if (run.source >= sources.size() ||
        u64{run.first} + run.count > sources[run.source].indices.size()) {
      return false;
    }
    triangle_count += run.count;
  }
  out.indices.reserve(triangle_count);
  // Per source, its entries' assembled numbers (~0 until first named), and per assembled entry its
  // micro-triangles by state.
  Vector<Vector<u32>> named(static_cast<u32>(sources.size()));
  for (u32 s = 0; s < sources.size(); ++s)
    named[s].assign(static_cast<u32>(sources[s].triangles.size()), ~0u);
  Vector<u64> counts;  // 3 per assembled entry
  HashMap<u64, u32> by_states;
  u64 data_bytes = 0;
  for (const MicromapRun& run : runs) {
    const MicromapPackView& src = sources[run.source];
    if (run.wanted != nullptr)
      for (u32 l = 0; l < k_levels; ++l)
        stats.wanted_per_level[l] += run.wanted[l];
    for (u32 t = 0; t < run.count; ++t) {
      const i32 index = src.indices[run.first + t];
      if (index < 0) {
        out.indices.push_back(index);
        if (index == gfx::k_micromap_fully_transparent)
          ++stats.special_transparent;
        else if (index == gfx::k_micromap_fully_opaque)
          ++stats.special_opaque;
        else
          ++stats.special_unknown;
        continue;
      }
      u32& entry = named[run.source][static_cast<u32>(index)];
      const gfx::MicromapTriangle& record = src.triangles[static_cast<u32>(index)];
      const u32 words = static_cast<u32>(gfx::micromap_entry_bytes(record.level) / sizeof(u32));
      const u32* states = src.data.data() + record.data_offset / sizeof(u32);
      if (entry == ~0u) {
        // Shared with an earlier entry of the same level and the same words, else a new one.
        const u64 hash =
            hash_bytes(states, words * sizeof(u32), hash_combine(k_hash_seed, record.level));
        const u32* earlier = by_states.find_value(hash);
        if (earlier != nullptr) {
          const u32 e = *earlier;
          const MicromapPackView& other = sources[out.entry_source[e]];
          const gfx::MicromapTriangle& theirs = other.triangles[out.entry_index[e]];
          if (theirs.level == record.level &&
              std::memcmp(other.data.data() + theirs.data_offset / sizeof(u32), states,
                          words * sizeof(u32)) == 0) {
            entry = e;
          } else {
            ++stats.dedup_mismatches;
          }
        }
        if (entry == ~0u) {
          entry = static_cast<u32>(out.triangles.size());
          if (earlier == nullptr) by_states.try_emplace(hash, entry);
          gfx::MicromapTriangle made;
          made.data_offset = static_cast<u32>(data_bytes);
          made.level = record.level;
          out.triangles.push_back(made);
          out.entry_source.push_back(run.source);
          out.entry_index.push_back(static_cast<u32>(index));
          u64 c[3];
          count_states(states, record.level, c);
          counts.push_back(c[0]);
          counts.push_back(c[1]);
          counts.push_back(c[2]);
          data_bytes += gfx::micromap_entry_bytes(record.level);
          ++stats.unique_per_level[record.level];
        }
      }
      out.indices.push_back(static_cast<i32>(entry));
      ++stats.entries_per_level[record.level];
      const u64* c = counts.data() + u64{entry} * 3;
      stats.micro_transparent += c[0];
      stats.micro_opaque += c[1];
      stats.micro_unknown += c[2];
      stats.micro_per_level[record.level] += c[0] + c[1] + c[2];
      stats.unknown_per_level[record.level] += c[2];
    }
  }
  stats.triangles = triangle_count;
  for (u32 l = 0; l < k_levels; ++l)
    stats.unique_entries += stats.unique_per_level[l];
  stats.data_bytes = data_bytes;
  stats.index_bytes = u64{triangle_count} * sizeof(i32);
  out.total_bytes =
      data_bytes + stats.index_bytes + u64{out.triangles.size()} * gfx::k_micromap_triangle_bytes;
  return true;
}

namespace {

struct AssemblyFill {
  const Vector<u32>* cluster_first = nullptr;
  std::span<const MicromapPackView> sources;
  const MicromapAssembly* assembly = nullptr;

  static bool fill(void* context, gfx::MicromapSection section, void* dst, u64 bytes) {
    const auto& self = *static_cast<const AssemblyFill*>(context);
    const MicromapAssembly& a = *self.assembly;
    switch (section) {
      case gfx::MicromapSection::cluster_first:
        if (bytes != u64{self.cluster_first->size()} * sizeof(u32)) return false;
        std::memcpy(dst, self.cluster_first->data(), static_cast<usize>(bytes));
        return true;
      case gfx::MicromapSection::indices:
        if (bytes != u64{a.indices.size()} * sizeof(i32)) return false;
        std::memcpy(dst, a.indices.data(), static_cast<usize>(bytes));
        return true;
      case gfx::MicromapSection::triangles:
        if (bytes != u64{a.triangles.size()} * sizeof(gfx::MicromapTriangle)) return false;
        std::memcpy(dst, a.triangles.data(), static_cast<usize>(bytes));
        return true;
      case gfx::MicromapSection::data: {
        if (bytes != a.stats.data_bytes) return false;
        auto* out = static_cast<u8*>(dst);
        for (u32 e = 0; e < a.triangles.size(); ++e) {
          const MicromapPackView& src = self.sources[a.entry_source[e]];
          const gfx::MicromapTriangle& from = src.triangles[a.entry_index[e]];
          std::memcpy(out + a.triangles[e].data_offset,
                      reinterpret_cast<const u8*>(src.data.data()) + from.data_offset,
                      static_cast<usize>(gfx::micromap_entry_bytes(from.level)));
        }
        return true;
      }
    }
    return false;
  }
};

}  // namespace

bool restore_assembled_micromaps(const gfx::Device& device, u32 cluster_count,
                                 std::span<const u32> items, std::span<const u32> triangle_counts,
                                 std::span<const MicromapPackView> sources,
                                 const MicromapAssembly& assembly, gfx::OpacityMicromapSet& out,
                                 std::string* error) {
  Vector<u32> cluster_first;
  gfx::micromap_cluster_first(cluster_count, items, triangle_counts, cluster_first);
  AssemblyFill fill;
  fill.cluster_first = &cluster_first;
  fill.sources = sources;
  fill.assembly = &assembly;
  gfx::MicromapRestore restore;
  restore.cluster_count = cluster_count;
  restore.triangle_count = static_cast<u32>(assembly.indices.size());
  restore.entry_count = static_cast<u32>(assembly.triangles.size());
  restore.data_bytes = assembly.stats.data_bytes;
  restore.stats = assembly.stats;
  restore.fill = &AssemblyFill::fill;
  restore.context = &fill;
  return gfx::restore_opacity_micromaps(device, restore, out, error);
}

// ---- the folder ---------------------------------------------------------------------------------

void touch_micromap_generation(std::string_view ddc_root, u64 generation) {
  const std::string dir = micromap_generation_dir(ddc_root, generation);
  if (io::make_directories(dir) != io::Status::Ok) return;
  // The time as text, so the file's own time moves even where a write of the same bytes would not.
  (void)io::overwrite_file(io::join_path(dir, "used"), std::to_string(time::wall_unix_ms()));
}

void prune_micromap_store(std::string_view ddc_root, u64 generation,
                          std::span<const std::string> damaged, i64 now_unix_ms,
                          MicromapPruneReport& out) {
  out = MicromapPruneReport{};
  const std::string root = io::join_path(ddc_root, "micromaps");
  const std::string current = hex16(generation);
  // A temporary file a minute old: a younger one may be another process's store under way.
  auto stale = [&](const std::string& path) {
    io::FileInfo info;
    return io::stat_file(path, info) == io::Status::Ok &&
           now_unix_ms - info.modified_unix_ms >= k_micromap_temp_age_ms;
  };
  // A generation's temporary files that are stale (the current one's, and the kept other's).
  auto sweep = [&](const std::string& dir) {
    Vector<io::DirEntry> files;
    if (io::list_directory(dir, files) != io::Status::Ok) return;
    for (const io::DirEntry& e : files) {
      if (e.is_directory || !ends_with(e.name, ".tmp")) continue;
      const std::string path = io::join_path(dir, e.name);
      if (stale(path) && io::remove_file(path) == io::Status::Ok) ++out.swept;
    }
  };
  // The current generation: packs a read refused, and temporary files a minute old.
  for (const std::string& path : damaged)
    if (io::remove_file(path) == io::Status::Ok) ++out.packs;
  sweep(io::join_path(root, current));
  // The root: R207's whole-scene files and their temporaries, and the generations past the kept.
  Vector<io::DirEntry> entries;
  if (io::list_directory(root, entries) != io::Status::Ok) return;
  struct Other {
    std::string name;
    i64 used = 0;
  };
  Vector<Other> others;
  for (const io::DirEntry& e : entries) {
    if (!e.is_directory) {
      // R207's temporaries by the same age rule as any other (R208-L4): a binary of that format
      // may be storing into this root as this runs. Its whole-scene files are of no use here.
      const std::string path = io::join_path(root, e.name);
      if (ends_with(e.name, ".tmp")) {
        if (stale(path) && io::remove_file(path) == io::Status::Ok) ++out.swept;
      } else if (ends_with(e.name, ".omm") && io::remove_file(path) == io::Status::Ok) {
        ++out.packs;
      }
      continue;
    }
    if (!is_hex16(e.name) || e.name == current) continue;
    io::FileInfo info;
    Other o;
    o.name = e.name;
    if (io::stat_file(io::join_path(io::join_path(root, e.name), "used"), info) == io::Status::Ok)
      o.used = info.modified_unix_ms;
    others.push_back(o);
  }
  std::sort(others.begin(), others.end(), [](const Other& a, const Other& b) {
    return a.used != b.used ? a.used > b.used : a.name < b.name;
  });
  // The kept others' dead stores' temporaries (R208-L6), which would otherwise wait for that
  // generation to be current again.
  for (u32 i = 0; i < others.size() && i + 1 < k_micromap_generations_kept; ++i)
    sweep(io::join_path(root, others[i].name));
  for (u32 i = k_micromap_generations_kept - 1; i < others.size(); ++i) {
    // Only what a store writes is removed, file by file; a folder left with anything else in it
    // is left.
    const std::string dir = io::join_path(root, others[i].name);
    Vector<io::DirEntry> files;
    if (io::list_directory(dir, files) != io::Status::Ok) continue;
    bool empty = true;
    for (const io::DirEntry& f : files) {
      const bool ours = !f.is_directory && (ends_with(f.name, ".omm") ||
                                            ends_with(f.name, ".tmp") || f.name == "used");
      if (ours && io::remove_file(io::join_path(dir, f.name)) == io::Status::Ok) {
        if (ends_with(f.name, ".omm")) ++out.packs;
      } else {
        empty = false;
      }
    }
    // Removed only if it is empty now (R208-L1): a file another process put there after the
    // listing, a pack it just renamed in, keeps the folder and the file.
    std::error_code ec;
    if (empty && std::filesystem::remove(path_of(dir), ec) && !ec) ++out.generations;
  }
}

}  // namespace engine::renderer
