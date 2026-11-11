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

inline std::string tile_rock_recipe(i32 x, i32 z, u32 n) {
  char text[96];
  std::snprintf(text, sizeof(text), "{\"x\":%d,\"z\":%d,\"n\":%u}", x, z, n);
  return text;
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
  Vector<Vec3> positions;
  Vector<u32> halves[2];
  for (u32 j = 0; j <= n; ++j) {
    for (u32 i = 0; i <= n; ++i) {
      const f32 u = 2.0f * static_cast<f32>(i) / static_cast<f32>(n) - 1.0f;
      const f32 v = 2.0f * static_cast<f32>(j) / static_cast<f32>(n) - 1.0f;
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
  for (u32 k = 0; k < 2; ++k) {
    if (!geometry::build_cluster_lod(std::span<const Vec3>(positions.data(), positions.size()),
                                     std::span<const u32>(halves[k].data(), halves[k].size()), lod,
                                     parts[k], &error)) {
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
  i32 side() const noexcept { return 2 * reach + 1; }
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
    out.recipe = tile_rock_recipe(gx, gz, n);
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

// Both generators, in this executable's registries; any number of times.
inline void register_tile_rocks() {
  (void)assets::MeshGeneratorRegistry::global().add(k_tile_rock);
  (void)scene_gen::GeneratorRegistry::global().add(k_tile_rocks);
}

}  // namespace engine::test
