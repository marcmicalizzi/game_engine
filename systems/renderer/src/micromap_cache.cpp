#include <core/hash/hash.h>
#include <core/time/time.h>
#include <foundation/io/vfs.h>
#include <systems/renderer/micromap_cache.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace engine::renderer {

namespace {

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

// The fixed prefix every stored bake begins with: magic, version, key, content hash, header length.
constexpr u64 k_prefix_bytes = 4 + 4 + 8 + 8 + 4;
constexpr usize k_hash_offset = 16;
// Every measure the bake made, as the header holds them, in this order.
constexpr u32 k_levels = gfx::k_micromap_max_level + 1;

void put_stats(std::string& out, const gfx::MicromapStats& s) {
  put_u32(out, s.triangles);
  for (u32 l = 0; l < k_levels; ++l)
    put_u32(out, s.entries_per_level[l]);
  put_u32(out, s.special_transparent);
  put_u32(out, s.special_opaque);
  put_u32(out, s.special_unknown);
  put_u64(out, s.micro_transparent);
  put_u64(out, s.micro_opaque);
  put_u64(out, s.micro_unknown);
  put_u64(out, s.micro_unknown_filter);
  for (u32 l = 0; l < k_levels; ++l)
    put_u64(out, s.micro_per_level[l]);
  for (u32 l = 0; l < k_levels; ++l)
    put_u64(out, s.unknown_per_level[l]);
  for (u32 l = 0; l < k_levels; ++l)
    put_u32(out, s.wanted_per_level[l]);
  put_u32(out, s.unique_entries);
  for (u32 l = 0; l < k_levels; ++l)
    put_u32(out, s.unique_per_level[l]);
  put_u32(out, s.dedup_mismatches);
  put_u32(out, s.level_cap);
  put_u32(out, s.levels_dropped);
  put_u64(out, s.data_bytes);
  put_u64(out, s.index_bytes);
}

// Reads what `put_stats` wrote; false if `bytes` is short.
bool get_stats(const u8*& p, const u8* end, gfx::MicromapStats& s) {
  auto u32_at = [&](u32& v) {
    if (end - p < 4) return false;
    v = get_u32(p);
    p += 4;
    return true;
  };
  auto u64_at = [&](u64& v) {
    if (end - p < 8) return false;
    v = get_u64(p);
    p += 8;
    return true;
  };
  bool ok = u32_at(s.triangles);
  for (u32 l = 0; l < k_levels; ++l)
    ok = ok && u32_at(s.entries_per_level[l]);
  ok = ok && u32_at(s.special_transparent) && u32_at(s.special_opaque) &&
       u32_at(s.special_unknown) && u64_at(s.micro_transparent) && u64_at(s.micro_opaque) &&
       u64_at(s.micro_unknown) && u64_at(s.micro_unknown_filter);
  for (u32 l = 0; l < k_levels; ++l)
    ok = ok && u64_at(s.micro_per_level[l]);
  for (u32 l = 0; l < k_levels; ++l)
    ok = ok && u64_at(s.unknown_per_level[l]);
  for (u32 l = 0; l < k_levels; ++l)
    ok = ok && u32_at(s.wanted_per_level[l]);
  ok = ok && u32_at(s.unique_entries);
  for (u32 l = 0; l < k_levels; ++l)
    ok = ok && u32_at(s.unique_per_level[l]);
  return ok && u32_at(s.dedup_mismatches) && u32_at(s.level_cap) && u32_at(s.levels_dropped) &&
         u64_at(s.data_bytes) && u64_at(s.index_bytes);
}

struct Section {
  const void* bytes;
  u64 size;
};

void sections_of(const gfx::MicromapSections& s, Section out[4]) {
  out[0] = Section{s.cluster_first.data(), u64{s.cluster_first.size()} * sizeof(u32)};
  out[1] = Section{s.indices.data(), u64{s.indices.size()} * sizeof(i32)};
  out[2] = Section{s.triangles.data(), u64{s.triangles.size()} * sizeof(gfx::MicromapTriangle)};
  out[3] = Section{s.data.data(), u64{s.data.size()} * sizeof(u32)};
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

u64 micromap_cache_key(const MicromapCacheKeyInput& in) noexcept {
  u64 h = hash_bytes("engine.micromap", 15);
  h = hash_combine(h, k_micromap_cache_version);
  h = hash_bytes(in.device, sizeof(in.device), h);
  h = hash_combine(h, in.shader);
  h = hash_combine(h, in.level_cap);
  h = hash_combine(h, in.device_limit);
  h = hash_combine(h, in.budget_bytes);
  h = hash_combine(h, bits_of(in.target_texels));
  h = hash_combine(h, in.cluster_count);
  h = hash_combine(h, in.items.size());
  h = hash_bytes(in.items.data(), in.items.size() * sizeof(u32), h);
  h = hash_combine(h, in.triangle_counts.size());
  h = hash_bytes(in.triangle_counts.data(), in.triangle_counts.size() * sizeof(u32), h);
  h = hash_combine(h, in.materials);
  h = hash_combine(h, in.geometry[0]);
  return hash_combine(h, in.geometry[1]);
}

u64 micromap_materials_digest(std::span<const MicromapMaterialWords> materials) noexcept {
  u64 h = hash_combine(k_hash_seed, materials.size());
  for (const MicromapMaterialWords& m : materials) {
    // Exactly what the bake reads of a record (opacity_micromap_bake.slang: `material_uv`, the
    // cutoff, the factor, the albedo and its sampler), the slots replaced by their contents.
    const gfx::ResolveMaterial& r = m.record;
    const u32 words[] = {r.flags,
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
    h = hash_bytes(words, sizeof(words), h);
    h = hash_combine(h, m.albedo_identity);
    h = hash_combine(h, m.sampler_identity);
  }
  return h;
}

void micromap_key_materials(MicromapKeyState& state, std::span<const u32> list,
                            std::span<const gfx::ResolveMaterial> table,
                            const HashMap<u32, u64>& slots, std::span<const u32> sampler_slots,
                            std::span<const u32> sampler_keys, std::string_view root) {
  state.materials = 0;
  state.unkeyed.clear();
  state.root.clear();
  if (list.empty()) return;
  if (root.empty()) {
    state.unkeyed = "the load reads no derived-data root";
    return;
  }
  // Each masked material once, in the order its first cluster names it: the list names the
  // materials by index, so the digest is of the records those indices hold.
  Vector<u8> seen(static_cast<u32>(table.size()), u8{0});
  Vector<MicromapMaterialWords> words;
  for (usize i = 1; i < list.size(); i += 2) {
    const u32 material = list[i];
    if (material >= table.size()) {
      state.unkeyed =
          "a masked cluster names material " + std::to_string(material) + ", past the table";
      return;
    }
    if (seen[material] != 0) continue;
    seen[material] = 1;
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
    words.push_back(w);
  }
  state.materials =
      micromap_materials_digest(std::span<const MicromapMaterialWords>(words.data(), words.size()));
  state.root = std::string(root);
}

std::string micromap_cache_path(std::string_view ddc_root, u64 key) {
  char name[32];
  std::snprintf(name, sizeof(name), "%016llx.omm", static_cast<unsigned long long>(key));
  return io::join_path(io::join_path(ddc_root, "micromaps"), name);
}

std::string micromap_file_header(u64 key, const gfx::MicromapSections& sections) {
  std::string out;
  put_u32(out, k_micromap_file_magic);
  put_u32(out, k_micromap_cache_version);
  put_u64(out, key);
  put_u64(out, 0);  // the content hash, filled below
  put_u32(out, 0);  // the header's length, filled below
  put_u32(out, static_cast<u32>(sections.cluster_first.size()));
  put_u32(out, static_cast<u32>(sections.indices.size()));
  put_u32(out, static_cast<u32>(sections.triangles.size()));
  put_u64(out, u64{sections.data.size()} * sizeof(u32));
  const gfx::MicromapStats empty;
  put_stats(out, sections.stats != nullptr ? *sections.stats : empty);
  const u32 length = static_cast<u32>(out.size());
  for (u32 i = 0; i < 4; ++i)
    out[k_prefix_bytes - 4 + i] = static_cast<char>((length >> (8 * i)) & 0xffu);
  u64 h = hash_bytes(out.data() + k_prefix_bytes, out.size() - k_prefix_bytes);
  Section parts[4];
  sections_of(sections, parts);
  for (const Section& part : parts)
    h = hash_piecewise(h, part.bytes, part.size);
  for (u32 i = 0; i < 8; ++i)
    out[k_hash_offset + i] = static_cast<char>((h >> (8 * i)) & 0xffu);
  return out;
}

bool write_micromap_file(const std::string& path, u64 key, const gfx::MicromapSections& sections,
                         std::string* error) {
  auto fail = [&](const std::string& why, const std::string& temp) {
    if (error != nullptr) *error = why;
    if (!temp.empty()) (void)io::remove_file(temp);
    return false;
  };
  const std::string dir = path.substr(0, path.find_last_of("/\\"));
  if (io::make_directories(dir) != io::Status::Ok) return fail("cannot make " + dir, {});
  // A name of this write's own, so two loads writing one key never write one file.
  const std::string temp = path + "." + std::to_string(time::monotonic_ns()) + ".tmp";
  if (io::write_file(temp, micromap_file_header(key, sections)) != io::Status::Ok)
    return fail("cannot write " + temp, temp);
  Section parts[4];
  sections_of(sections, parts);
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
    // Another load wrote the same key first: its file is the same bytes, and this one goes.
    if (io::exists(path)) {
      (void)io::remove_file(temp);
      return true;
    }
    return fail("cannot rename " + temp, temp);
  }
  return true;
}

namespace {

// The reader `restore_opacity_micromaps` fills from: the sections in order, each read a piece at a
// time into the bounce buffer, hashed, and copied to where it is uploaded from; the hash is checked
// when the last section the file holds has been read.
struct SectionReader {
  io::FileHandle file;
  u64 cursor = 0;
  u64 hash = 0;
  u64 expected_hash = 0;
  u64 sizes[4] = {};
  u32 next = 0;  // the section the next call must ask for
  u32 last = 0;  // the last section with bytes
  bool hash_ok = false;
  Vector<u8> bounce;

  static bool fill(void* context, gfx::MicromapSection section, void* dst, u64 bytes) {
    auto& self = *static_cast<SectionReader*>(context);
    const u32 s = static_cast<u32>(section);
    // Sections with no bytes are skipped by the restore; they are skipped here too.
    while (self.next < s && self.sizes[self.next] == 0)
      ++self.next;
    if (s != self.next || bytes != self.sizes[s]) return false;
    auto* out = static_cast<u8*>(dst);
    for (u64 at = 0; at < bytes;) {
      const u64 piece = std::min<u64>(k_piece, bytes - at);
      u64 read = 0;
      if (self.file.read_at(self.cursor, self.bounce.data(), piece, read) != io::Status::Ok ||
          read != piece) {
        return false;
      }
      self.hash = hash_bytes(self.bounce.data(), static_cast<usize>(piece), self.hash);
      std::memcpy(out + at, self.bounce.data(), static_cast<usize>(piece));
      self.cursor += piece;
      at += piece;
    }
    ++self.next;
    if (s == self.last) {
      self.hash_ok = self.hash == self.expected_hash && self.cursor == self.file.size();
      return self.hash_ok;
    }
    return true;
  }
};

}  // namespace

MicromapFileRead read_micromap_file(const gfx::Device& device, const std::string& path, u64 key,
                                    gfx::OpacityMicromapSet& out) {
  MicromapFileRead result;
  SectionReader reader;
  if (reader.file.open(path) != io::Status::Ok) return result;
  result.found = true;
  result.file_bytes = reader.file.size();
  u8 prefix[k_prefix_bytes];
  u64 read = 0;
  if (reader.file.read_at(0, prefix, k_prefix_bytes, read) != io::Status::Ok ||
      read != k_prefix_bytes) {
    result.refused = "shorter than a stored bake's prefix";
    return result;
  }
  if (get_u32(prefix) != k_micromap_file_magic) {
    result.refused = "not a stored bake";
    return result;
  }
  if (get_u32(prefix + 4) != k_micromap_cache_version) {
    result.refused = "made by version " + std::to_string(get_u32(prefix + 4)) + ", this is " +
                     std::to_string(k_micromap_cache_version);
    return result;
  }
  if (get_u64(prefix + 8) != key) {
    result.refused = "made for another key";
    return result;
  }
  reader.expected_hash = get_u64(prefix + k_hash_offset);
  const u32 header_bytes = get_u32(prefix + k_prefix_bytes - 4);
  if (header_bytes < k_prefix_bytes || header_bytes > result.file_bytes || header_bytes > 4096) {
    result.refused = "its header's length is not a header's";
    return result;
  }
  Vector<u8> header(header_bytes - k_prefix_bytes);
  if (reader.file.read_at(k_prefix_bytes, header.data(), header.size(), read) != io::Status::Ok ||
      read != header.size()) {
    result.refused = "its header did not read";
    return result;
  }
  const u8* p = header.data();
  const u8* end = header.data() + header.size();
  if (end - p < 20) {
    result.refused = "its header is short";
    return result;
  }
  gfx::MicromapRestore restore;
  restore.cluster_count = get_u32(p);
  restore.triangle_count = get_u32(p + 4);
  restore.entry_count = get_u32(p + 8);
  restore.data_bytes = get_u64(p + 12);
  p += 20;
  if (!get_stats(p, end, restore.stats) || p != end) {
    result.refused = "its measures did not read";
    return result;
  }
  reader.sizes[0] = u64{restore.cluster_count} * sizeof(u32);
  reader.sizes[1] = u64{restore.triangle_count} * sizeof(i32);
  reader.sizes[2] = u64{restore.entry_count} * sizeof(gfx::MicromapTriangle);
  reader.sizes[3] = restore.data_bytes;
  u64 total = header_bytes;
  for (u32 s = 0; s < 4; ++s) {
    total += reader.sizes[s];
    if (reader.sizes[s] != 0) reader.last = s;
  }
  if (total != result.file_bytes) {
    result.refused = "its length is not its sections' (" + std::to_string(result.file_bytes) +
                     " bytes, " + std::to_string(total) + " named)";
    return result;
  }
  reader.hash = hash_bytes(header.data(), header.size());
  reader.cursor = header_bytes;
  reader.bounce.resize(static_cast<u32>(k_piece));
  restore.fill = &SectionReader::fill;
  restore.context = &reader;
  std::string error;
  gfx::OpacityMicromapSet made;
  if (!gfx::restore_opacity_micromaps(device, restore, made, &error)) {
    result.refused = !reader.hash_ok && reader.next > reader.last
                         ? std::string("its content hash is not its bytes'")
                         : "the restore refused it: " + error;
    return result;
  }
  if (!reader.hash_ok) {
    gfx::destroy_opacity_micromaps(device, made);
    result.refused = "its last section was never read";
    return result;
  }
  gfx::destroy_opacity_micromaps(device, out);
  out = std::move(made);
  result.used = true;
  return result;
}

}  // namespace engine::renderer
