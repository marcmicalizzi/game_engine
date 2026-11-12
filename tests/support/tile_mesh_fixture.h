#pragma once

// A fixture for meshes that arrive with their tiles (docs/subsystems/scene_gen.md, "A tile's
// meshes"; roadmap R142): a mesh generator that builds a different small rock for every recipe,
// and a placement generator whose tile hands its tile's rock with its placements — the contract's
// two halves, with nothing of a real solver in them, for the renderer's, the world's and the
// collision's tests. Header-only: a test that includes it registers both with `register_tile_rocks`
// (the registries take the same descriptor twice), and counts the builds its executable ran with
// `tile_rock_builds`.
//
// **The rock** (`<name>.tilerock.json`, recipe `{"x":3,"z":-2,"n":12}`): a dome six metres across
// on an `n` x `n` grid, its height and its bumps a hash of (x, z), in two primitives of one
// material each — so the map from cluster to material is exercised — with colours from (x, z) too,
// so a picture shows which tile's rock a pixel is. No C library in its shape: sums and products of
// hashed integers, so every compiler builds the same container.
//
// **The placements** (`test-tile-rocks`, parameters
// `{"reach":2,"n":12,"share":1,"resident":false}`): every tile within `reach` tiles of the origin
// gets two instances of its rock. `share` s makes each s x s group of tiles name one rock
// (residency by reference has something to share); `resident` lists every rock in `meshes` instead
// and hands none with a tile — the contract as it was, for a before-and-after of the same world.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/hash/hash.h>
#include <core/json/json_value.h>
#include <core/math/math.h>
#include <domain/assets/mesh_generator.h>
#include <domain/geometry/cluster_file.h>
#include <domain/geometry/cluster_lod.h>
#include <domain/geometry/cluster_pages.h>
#include <domain/scene_gen/scene_gen.h>
#include <domain/texture/material_textures.h>

#include <atomic>
#include <cstdio>
#include <span>
#include <string>

namespace engine::test {

inline constexpr f32 k_tile_rock_half = 3.0f;    // metres from its origin to its edge
inline constexpr f32 k_tile_rock_reach = 4.25f;  // to its corner: what the generator declares

inline std::atomic<u32>& tile_rock_builds() noexcept {
  static std::atomic<u32> builds{0};
  return builds;
}

// **A rock's look** (R147; texture.md, "Textures that arrive after the load"), the recipe's `"k"`:
// bit 0 gives its first material a base colour map and a normal map, bit 1 makes its second
// alpha-tested through a map with holes in it, and bits 8 and up pick which of several base colour
// maps the first has — so rocks of one set share their textures by content and rocks of two sets
// do not. The maps are 32 x 32 TGA files the container carries, written here from hashes. Zero is
// the rock as it was: no UVs, no images, the bytes every earlier pin was taken on.
inline constexpr u32 k_tile_rock_textured = 1u;
inline constexpr u32 k_tile_rock_masked = 2u;
inline constexpr u32 k_tile_rock_set_shift = 8u;

inline std::string tile_rock_recipe(i32 x, i32 z, u32 n, u32 look = 0) {
  char text[112];
  if (look == 0) {
    std::snprintf(text, sizeof(text), "{\"x\":%d,\"z\":%d,\"n\":%u}", x, z, n);
  } else {
    std::snprintf(text, sizeof(text), "{\"x\":%d,\"z\":%d,\"n\":%u,\"k\":%u}", x, z, n, look);
  }
  return text;
}

// A 32-bit uncompressed TGA, top row first, from RGBA texels.
inline void tile_rock_tga(u32 size, const Vector<u8>& rgba, Vector<u8>& out) {
  out.clear();
  const u8 header[18] = {0,
                         0,
                         2,
                         0,
                         0,
                         0,
                         0,
                         0,
                         0,
                         0,
                         0,
                         0,
                         static_cast<u8>(size & 0xffu),
                         static_cast<u8>(size >> 8),
                         static_cast<u8>(size & 0xffu),
                         static_cast<u8>(size >> 8),
                         32,
                         0x28};
  for (const u8 byte : header)
    out.push_back(byte);
  for (u32 i = 0; i < size * size; ++i) {
    out.push_back(rgba[i * 4 + 2]);
    out.push_back(rgba[i * 4 + 1]);
    out.push_back(rgba[i * 4 + 0]);
    out.push_back(rgba[i * 4 + 3]);
  }
}

inline constexpr u32 k_tile_rock_texels = 32;

// The three maps: 0 a base colour (a checker of two colours that set `set` picks, with grain), 1 a
// normal map of shallow bumps, 2 a base colour whose alpha has a hole in every other 8 x 8 cell.
inline void tile_rock_image(u32 which, u32 set, Vector<u8>& out) {
  const u32 n = k_tile_rock_texels;
  Vector<u8> rgba(n * n * 4);
  for (u32 y = 0; y < n; ++y) {
    for (u32 x = 0; x < n; ++x) {
      u8* t = &rgba[(y * n + x) * 4];
      const u64 h = hash_combine(hash_combine(0x726f636bull + which, x), y);
      const u32 grain = static_cast<u32>(h >> 56) / 8u;  // 0..31
      const bool odd = ((x / 8u) + (y / 8u)) % 2u != 0;
      if (which == 0) {
        const u64 c = hash_combine(0x736574ull, set);
        const u8 base[3] = {static_cast<u8>(96u + (c & 0x3fu)),
                            static_cast<u8>(80u + ((c >> 8) & 0x3fu)),
                            static_cast<u8>(64u + ((c >> 16) & 0x3fu))};
        for (u32 k = 0; k < 3; ++k)
          t[k] = static_cast<u8>((odd ? base[k] : base[k] + 64u) + grain);
        t[3] = 255;
      } else if (which == 1) {
        // A bump a cell: the slope of a paraboloid about the cell's centre, small enough that z
        // stays near one. Integers only, so every compiler writes the same file.
        const i32 dx = static_cast<i32>(x % 8u) * 2 - 7;
        const i32 dy = static_cast<i32>(y % 8u) * 2 - 7;
        t[0] = static_cast<u8>(128 + dx * 4);
        t[1] = static_cast<u8>(128 + dy * 4);
        t[2] = 240;
        t[3] = 255;
      } else {
        t[0] = static_cast<u8>(40u + grain);
        t[1] = static_cast<u8>(150u + grain);
        t[2] = static_cast<u8>(50u + grain);
        const u32 cx = x % 8u;
        const u32 cy = y % 8u;
        const bool hole = odd && cx >= 2 && cx < 6 && cy >= 2 && cy < 6;
        t[3] = hole ? 0 : 255;
      }
    }
  }
  tile_rock_tga(n, rgba, out);
}

inline std::string tile_rock_path(i32 x, i32 z) {
  char text[96];
  std::snprintf(text, sizeof(text), "rocks/rock_%d_%d.tilerock.json", x, z);
  return text;
}

// The whole number after `key` in `text` (`"x":` and the like).
inline bool tile_rock_number(const std::string& text, const char* key, i64& out) {
  const std::string name(key);
  const usize at = text.find(name);
  if (at == std::string::npos) return false;
  usize p = at + name.size();
  const bool negative = p < text.size() && text[p] == '-';
  if (negative) ++p;
  if (p >= text.size() || text[p] < '0' || text[p] > '9') return false;
  i64 value = 0;
  for (; p < text.size() && text[p] >= '0' && text[p] <= '9'; ++p)
    value = value * 10 + (text[p] - '0');
  out = negative ? -value : value;
  return true;
}

inline u32 tile_rock_look(std::span<const u8> bytes) {
  const std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  i64 look = 0;
  return tile_rock_number(text, "\"k\":", look) ? static_cast<u32>(look) : 0u;
}

inline bool tile_rock_parse(std::span<const u8> bytes, i32& x, i32& z, u32& n, std::string& error) {
  const std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  i64 px = 0;
  i64 pz = 0;
  i64 pn = 0;
  if (!tile_rock_number(text, "\"x\":", px) || !tile_rock_number(text, "\"z\":", pz) ||
      !tile_rock_number(text, "\"n\":", pn) || pn < 2 || pn > 96) {
    error = "not a tile rock: " + text;
    return false;
  }
  x = static_cast<i32>(px);
  z = static_cast<i32>(pz);
  n = static_cast<u32>(pn);
  return true;
}

// A number in [0, 1) from a hash: the top 24 bits, which a float holds exactly.
inline f32 tile_rock_unit(u64 h) noexcept { return static_cast<f32>(h >> 40) / 16777216.0f; }

inline u64 tile_rock_seed(i32 x, i32 z) noexcept {
  const i32 words[2] = {x, z};
  return hash_bytes(words, sizeof(words));
}

inline bool tile_rock_source_hash(std::span<const u8> bytes, u64& out, std::string& error) {
  i32 x = 0;
  i32 z = 0;
  u32 n = 0;
  if (!tile_rock_parse(bytes, x, z, n, error)) return false;
  const u64 words[3] = {tile_rock_seed(x, z), n, 0x74696c65726f636bull};  // and a version
  out = hash_bytes(words, sizeof(words));
  // A look is part of what the rock is; none leaves the hash every earlier pin was taken on.
  if (const u32 look = tile_rock_look(bytes); look != 0) out = hash_combine(out, look);
  return true;
}

inline bool tile_rock_build(std::span<const u8> bytes, const assets::MeshGeneratorInputs& inputs,
                            const geometry::ClusterLodOptions& lod, u32 page_bytes,
                            geometry::ClusterFileData& out, std::string& error) {
  i32 x = 0;
  i32 z = 0;
  u32 n = 0;
  if (!tile_rock_parse(bytes, x, z, n, error)) return false;
  tile_rock_builds().fetch_add(1, std::memory_order_relaxed);
  out = geometry::ClusterFileData{};
  const u64 seed = tile_rock_seed(x, z);
  const f32 height = 1.0f + 1.5f * tile_rock_unit(hash_combine(seed, 1));
  const u32 look = tile_rock_look(bytes);
  Vector<Vec3> positions;
  Vector<Vec2> uvs;  // only a rock with a look has any: three repeats across it
  Vector<u32> halves[2];
  for (u32 j = 0; j <= n; ++j) {
    for (u32 i = 0; i <= n; ++i) {
      const f32 u = 2.0f * static_cast<f32>(i) / static_cast<f32>(n) - 1.0f;
      const f32 v = 2.0f * static_cast<f32>(j) / static_cast<f32>(n) - 1.0f;
      if (look != 0) {
        uvs.push_back(Vec2{3.0f * static_cast<f32>(i) / static_cast<f32>(n),
                           3.0f * static_cast<f32>(j) / static_cast<f32>(n)});
      }
      const f32 dome = (1.0f - u * u) * (1.0f - v * v);  // zero on the square's edge
      const f32 bump = tile_rock_unit(hash_combine(hash_combine(seed, 2 + i), 1000 + j));
      positions.push_back(
          Vec3{u * k_tile_rock_half, dome * (height + 0.35f * bump), v * k_tile_rock_half});
    }
  }
  for (u32 j = 0; j < n; ++j) {
    for (u32 i = 0; i < n; ++i) {
      const u32 a = j * (n + 1) + i;
      const u32 quad[6] = {a, a + n + 1, a + n + 2, a, a + n + 2, a + 1};  // up is the front
      for (const u32 q : quad)
        halves[i < n / 2 ? 0 : 1].push_back(q);
    }
  }
  Vector<geometry::ClusterLodMesh> parts(2);
  geometry::AttributeSource attributes;
  attributes.uvs = std::span<const Vec2>(uvs.data(), uvs.size());
  for (u32 k = 0; k < 2; ++k) {
    if (!geometry::build_cluster_lod(std::span<const Vec3>(positions.data(), positions.size()),
                                     std::span<const u32>(halves[k].data(), halves[k].size()), lod,
                                     parts[k], &error, attributes)) {
      return false;
    }
  }
  Vector<u32> owners;
  if (!geometry::merge_cluster_lod(
          std::span<const geometry::ClusterLodMesh>(parts.data(), parts.size()), out.mesh, &owners,
          &error)) {
    return false;
  }
  if (page_bytes != 0) {
    Vector<u32> permutation;
    geometry::ClusterPagesOptions pages;
    pages.page_bytes = page_bytes;
    if (!geometry::build_cluster_pages(out.mesh, pages, out.pages, &error, &permutation))
      return false;
    geometry::permute_cluster_array(std::span<const u32>(permutation.data(), permutation.size()),
                                    owners);
  }
  for (const u32 owner : owners)
    out.cluster_material.push_back(owner);
  for (u32 k = 0; k < 2; ++k) {
    geometry::ClusterFileMaterial material;
    material.base_color = Vec4{0.25f + 0.6f * tile_rock_unit(hash_combine(seed, 10 + k)),
                               0.25f + 0.6f * tile_rock_unit(hash_combine(seed, 20 + k)),
                               0.25f + 0.6f * tile_rock_unit(hash_combine(seed, 30 + k)), 1.0f};
    material.metallic = 0.0f;
    material.roughness = 0.5f + 0.4f * tile_rock_unit(hash_combine(seed, 40 + k));
    out.materials.push_back(material);
  }
  // The look's maps, carried by the container as a glTF's embedded images are, with the texture
  // records the content build writes for them (so the host builds each into a `.tex` by its bytes).
  const auto add_image = [&](u32 which, u32 set) {
    geometry::ClusterImage image;
    tile_rock_image(which, set, image.bytes);
    out.image_paths.push_back(std::string());
    out.images.push_back(std::move(image));
    return static_cast<i32>(out.images.size()) - 1;
  };
  if ((look & k_tile_rock_textured) != 0) {
    out.materials[0].base_color = Vec4{1.0f, 1.0f, 1.0f, 1.0f};  // the map's own colours
    out.materials[0].base_color_image = add_image(0, look >> k_tile_rock_set_shift);
    out.materials[0].normal_image = add_image(1, 0);
  }
  if ((look & k_tile_rock_masked) != 0) {
    out.materials[1].base_color = Vec4{1.0f, 1.0f, 1.0f, 1.0f};
    out.materials[1].base_color_image = add_image(2, 0);
    out.materials[1].alpha = geometry::encode_alpha_word(1 /* mask */, false, 0.5f);
  }
  if (!out.image_paths.empty()) texture::fill_cluster_texture_records(out);
  if (!tile_rock_source_hash(bytes, out.source_hash, error)) return false;
  out.build_key = assets::generated_mesh_build_key(out.source_hash, inputs, lod, page_bytes);
  out.generator = "tilerock";
  out.generator_kind = "test rock";
  return true;
}

inline constexpr assets::MeshGeneratorDesc k_tile_rock{
    .name = "tilerock", .source_hash = &tile_rock_source_hash, .build = &tile_rock_build};

// ---- the placements
// ------------------------------------------------------------------------------

struct TileRocks {
  i32 reach = 2;
  u32 n = 12;
  i32 share = 1;
  bool resident = false;
  u32 look = 0;  // `k_tile_rock_textured | k_tile_rock_masked`: every rock's look
  i32 sets = 1;  // how many base colour maps the rocks are dealt among
  i32 side() const noexcept { return 2 * reach + 1; }
  u32 look_of(i32 gx, i32 gz) const noexcept {
    if ((look & k_tile_rock_textured) == 0 || sets <= 1) return look;
    const i32 dealt = ((gx * 7 + gz * 13) % sets + sets) % sets;
    return look | (static_cast<u32>(dealt) << k_tile_rock_set_shift);
  }
  bool holds(scene_gen::TileCoord t) const noexcept {
    return t.x >= -reach && t.x <= reach && t.z >= -reach && t.z <= reach;
  }
  // The rock a tile names: its own, or its `share` x `share` group's.
  static i32 group(i32 v, i32 s) noexcept { return v >= 0 ? v / s : -((-v + s - 1) / s); }
  scene_gen::PlacementMesh mesh(scene_gen::TileCoord t) const {
    const i32 gx = group(t.x, share);
    const i32 gz = group(t.z, share);
    scene_gen::PlacementMesh out;
    out.path = tile_rock_path(gx, gz);
    out.name = "rock " + std::to_string(gx) + " " + std::to_string(gz);
    out.recipe = tile_rock_recipe(gx, gz, n, look_of(gx, gz));
    out.reach = k_tile_rock_reach;
    return out;
  }
  u32 index(scene_gen::TileCoord t) const noexcept {
    return static_cast<u32>((t.z + reach) * side() + (t.x + reach));
  }
  // The tile's two placements of mesh `mesh`, standing on the floor.
  void place(scene_gen::TileCoord t, u32 mesh, const scene_gen::Context& context,
             scene_gen::Placements& out) const {
    const f64 size = static_cast<f64>(context.tile_size);
    for (u32 k = 0; k < 2; ++k) {
      scene_gen::Placement p;
      p.mesh = mesh;
      p.position = WorldPos{(static_cast<f64>(t.x) + 0.3 + 0.4 * k) * size, 0.0,
                            (static_cast<f64>(t.z) + 0.35 + 0.3 * k) * size};
      p.position.y = context.ground.floor(p.position);
      // A quarter turn for the second, exactly: (0, sin 45, 0, cos 45) to float precision.
      if (k == 1) p.rotation = Quat{0.0f, 0.70710678f, 0.0f, 0.70710678f};
      p.scale = k == 0 ? Vec3::one() : Vec3{0.75f, 0.75f, 0.75f};
      p.id = index(t) * 2 + k + 1;
      out.instances.push_back(p);
    }
    out.things += 1;
  }
};

inline bool tile_rocks_open(const JsonValue& params, const scene_gen::Context&, void** state,
                            std::string* error) {
  auto* rocks = new TileRocks;
  i64 number = 0;
  if (const JsonValue* v = params.find("reach"); v != nullptr && v->get_i64(number))
    rocks->reach = static_cast<i32>(number);
  if (const JsonValue* v = params.find("n"); v != nullptr && v->get_i64(number))
    rocks->n = static_cast<u32>(number);
  if (const JsonValue* v = params.find("share"); v != nullptr && v->get_i64(number))
    rocks->share = static_cast<i32>(number);
  if (const JsonValue* v = params.find("resident"); v != nullptr) v->get_bool(rocks->resident);
  if (const JsonValue* v = params.find("look"); v != nullptr && v->get_i64(number))
    rocks->look = static_cast<u32>(number);
  if (const JsonValue* v = params.find("sets"); v != nullptr && v->get_i64(number))
    rocks->sets = static_cast<i32>(number);
  if (rocks->reach < 0 || rocks->reach > 64 || rocks->share < 1) {
    if (error != nullptr) *error = "test-tile-rocks: reach in 0..64 and share at least 1";
    delete rocks;
    return false;
  }
  *state = rocks;
  return true;
}

inline void tile_rocks_close(void* state) noexcept { delete static_cast<TileRocks*>(state); }

inline bool tile_rocks_expand(void* state, const scene_gen::Context& context,
                              scene_gen::Placements& out, std::string*) {
  const TileRocks& rocks = *static_cast<const TileRocks*>(state);
  for (i32 z = -rocks.reach; z <= rocks.reach; ++z) {
    for (i32 x = -rocks.reach; x <= rocks.reach; ++x) {
      const scene_gen::TileCoord t{x, z};
      out.meshes.push_back(rocks.mesh(t));
      rocks.place(t, rocks.index(t), context, out);
    }
  }
  return true;
}

inline bool tile_rocks_meshes(void* state, const scene_gen::Context&,
                              Vector<scene_gen::PlacementMesh>& out, std::string*) {
  const TileRocks& rocks = *static_cast<const TileRocks*>(state);
  if (!rocks.resident) return true;  // nothing before a tile asks
  for (i32 z = -rocks.reach; z <= rocks.reach; ++z) {
    for (i32 x = -rocks.reach; x <= rocks.reach; ++x)
      out.push_back(rocks.mesh(scene_gen::TileCoord{x, z}));
  }
  return true;
}

inline bool tile_rocks_tile(void* state, scene_gen::TileCoord t, const scene_gen::Context& context,
                            scene_gen::Placements& out, std::string*) {
  const TileRocks& rocks = *static_cast<const TileRocks*>(state);
  if (!rocks.holds(t)) return true;
  if (rocks.resident) {
    rocks.place(t, rocks.index(t), context, out);  // one of the list `meshes` gave
  } else {
    out.meshes.push_back(rocks.mesh(t));  // the tile's own: index 0 past an empty resident list
    rocks.place(t, 0, context, out);
  }
  return true;
}

inline bool tile_rocks_occupies(const void* state, scene_gen::TileCoord t) noexcept {
  return static_cast<const TileRocks*>(state)->holds(t);
}

inline constexpr scene_gen::PlacementGeneratorDesc k_tile_rocks{.name = "test-tile-rocks",
                                                                .open = &tile_rocks_open,
                                                                .close = &tile_rocks_close,
                                                                .expand = &tile_rocks_expand,
                                                                .meshes = &tile_rocks_meshes,
                                                                .tile = &tile_rocks_tile,
                                                                .occupies = &tile_rocks_occupies,
                                                                .tile_meshes = true};

// The same generator under a second name that says its tiles may hand an alpha-tested mesh
// (`tile_meshes_masked`): what a scene whose rocks' look has `k_tile_rock_masked` names, so the
// plain one's scenes keep the pipelines they were pinned with.
inline constexpr scene_gen::PlacementGeneratorDesc k_tile_leaves{.name = "test-tile-leaves",
                                                                 .open = &tile_rocks_open,
                                                                 .close = &tile_rocks_close,
                                                                 .expand = &tile_rocks_expand,
                                                                 .meshes = &tile_rocks_meshes,
                                                                 .tile = &tile_rocks_tile,
                                                                 .occupies = &tile_rocks_occupies,
                                                                 .tile_meshes = true,
                                                                 .tile_meshes_masked = true};

// The generators, in this executable's registries; any number of times.
inline void register_tile_rocks() {
  (void)assets::MeshGeneratorRegistry::global().add(k_tile_rock);
  (void)scene_gen::GeneratorRegistry::global().add(k_tile_rocks);
  (void)scene_gen::GeneratorRegistry::global().add(k_tile_leaves);
}

}  // namespace engine::test
