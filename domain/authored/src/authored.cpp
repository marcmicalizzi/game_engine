// The `authored` placement generator (authored.h; docs/subsystems/authored.md).

#include <domain/authored/authored.h>

#include <core/containers/vector.h>
#include <core/schema/json_reflect.h>
#include <schemas/scene.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>

namespace engine::authored {

bool anchor_tile(const WorldPos& origin, f32 tile_size, scene_gen::TileCoord& out) noexcept {
  if (!(tile_size > 0.0f) || !std::isfinite(origin.x) || !std::isfinite(origin.z)) return false;
  const f64 x = std::floor(origin.x / static_cast<f64>(tile_size));
  const f64 z = std::floor(origin.z / static_cast<f64>(tile_size));
  constexpr f64 k_limit = 2147483647.0;
  if (!(x >= -k_limit && x <= k_limit && z >= -k_limit && z <= k_limit)) return false;
  out = scene_gen::TileCoord{static_cast<i32>(x), static_cast<i32>(z)};
  return true;
}

namespace {

// A tile's key, sorted as (x, z): what the buckets are found by.
u64 key_of(scene_gen::TileCoord t) noexcept {
  return (static_cast<u64>(static_cast<u32>(t.x)) << 32) | static_cast<u64>(static_cast<u32>(t.z));
}

std::string schema_errors(const schema::ReadContext& ctx) {
  std::string text;
  for (const schema::Diagnostic& d : ctx.diagnostics) {
    if (!text.empty()) text += "; ";
    text += d.path.empty() ? d.message : d.path + ": " + d.message;
  }
  return text.empty() ? std::string("not an engine.scene.AuthoredSet") : text;
}

// The set, filed by tile: `order` is every instance index sorted by (tile, id), `keys` the tiles
// that hold one in order and `first` where each one's run of `order` starts (one past the end
// last), so a tile is a binary search and a run.
struct Entry {
  scene::AuthoredSet set;
  Vector<u64> keys;
  Vector<u32> first;
  Vector<u32> order;
  u32 rings = 1;  // the longest form set's rings: past it every ring draws the same
};

u8 ring_of(const Entry& entry, const scene_gen::Context& context) noexcept {
  const u32 ring = context.ring == scene_gen::k_no_ring ? 0u : context.ring;
  return static_cast<u8>(ring < entry.rings ? ring : entry.rings - 1);
}

bool open(const JsonValue& params, const scene_gen::Context& context, void** state,
          std::string* error) {
  const auto fail = [&](std::string sentence) {
    if (error != nullptr) *error = std::string(context.where) + ": " + sentence;
    return false;
  };
  auto entry = std::make_unique<Entry>();
  schema::ReadContext ctx;
  if (!schema::from_json(entry->set, params, ctx) || !ctx.ok()) return fail(schema_errors(ctx));
  const scene::AuthoredSet& set = entry->set;
  if (!(set.tile_size > 0.0f) || set.tile_size != context.tile_size) {
    return fail("the authored set is on " + std::to_string(set.tile_size) +
                " m tiles and the world's are " + std::to_string(context.tile_size) + " m");
  }
  for (u32 f = 0; f < set.forms.size(); ++f) {
    if (set.forms[f].rings.empty()) return fail("form set " + std::to_string(f) + " has no ring");
    entry->rings = std::max(entry->rings, static_cast<u32>(set.forms[f].rings.size()));
  }
  struct Filed {
    u64 key;
    u32 id;
    u32 index;
  };
  Vector<Filed> filed;
  filed.reserve(set.instances.size());
  for (u32 i = 0; i < set.instances.size(); ++i) {
    const scene::AuthoredInstance& instance = set.instances[i];
    if (instance.forms >= set.forms.size()) {
      return fail("instance " + std::to_string(i) + " names form set " +
                  std::to_string(instance.forms) + " of " + std::to_string(set.forms.size()));
    }
    scene_gen::TileCoord tile;
    if (!anchor_tile(instance.translation, set.tile_size, tile))
      return fail("instance " + std::to_string(i) + " stands where no tile reaches");
    filed.push_back(Filed{key_of(tile), instance.id, i});
  }
  std::sort(filed.begin(), filed.end(), [](const Filed& a, const Filed& b) {
    if (a.key != b.key) return a.key < b.key;
    if (a.id != b.id) return a.id < b.id;
    return a.index < b.index;
  });
  entry->order.reserve(filed.size());
  for (u32 i = 0; i < filed.size(); ++i) {
    if (i == 0 || filed[i].key != filed[i - 1].key) {
      entry->keys.push_back(filed[i].key);
      entry->first.push_back(i);
    }
    entry->order.push_back(filed[i].index);
  }
  entry->first.push_back(filed.size());
  *state = entry.release();
  return true;
}

void close(void* state) noexcept { delete static_cast<Entry*>(state); }

// One instance in the ring's form, as a placement of the entry's resident mesh.
scene_gen::Placement placement_of(const Entry& entry, u32 index, u8 ring) noexcept {
  const scene::AuthoredInstance& instance = entry.set.instances[index];
  const Vector<u32>& rings = entry.set.forms[instance.forms].rings;
  scene_gen::Placement p;
  p.position = instance.translation;
  p.rotation = instance.rotation;
  p.scale = instance.scale;
  p.mesh = rings[ring < rings.size() ? ring : rings.size() - 1];
  p.id = instance.id;
  return p;
}

// A scene read whole keeps its instances its own (the reader never streams them), so this is the
// set in its innermost form for a host that asks anyway. The meshes are the entry's resident ones,
// which only the scene reader holds: `meshes` stays empty.
bool expand(void* state, const scene_gen::Context&, scene_gen::Placements& out, std::string*) {
  const Entry& entry = *static_cast<const Entry*>(state);
  out.clear();
  out.instances.reserve(entry.order.size());
  for (const u32 index : entry.order)
    out.instances.push_back(placement_of(entry, index, 0));
  return true;
}

bool meshes(void*, const scene_gen::Context&, Vector<scene_gen::PlacementMesh>& out,
            std::string*) {
  out.clear();
  return true;
}

bool find(const Entry& entry, scene_gen::TileCoord tile, u32& from, u32& to) noexcept {
  const u64 key = key_of(tile);
  const auto it = std::lower_bound(entry.keys.begin(), entry.keys.end(), key);
  if (it == entry.keys.end() || *it != key) return false;
  const u32 k = static_cast<u32>(it - entry.keys.begin());
  from = entry.first[k];
  to = entry.first[k + 1];
  return true;
}

bool occupies(const void* state, scene_gen::TileCoord tile) noexcept {
  u32 from = 0, to = 0;
  return find(*static_cast<const Entry*>(state), tile, from, to);
}

bool tile(void* state, scene_gen::TileCoord at, const scene_gen::Context& context,
          scene_gen::Placements& out, std::string*) {
  const Entry& entry = *static_cast<const Entry*>(state);
  out.clear();
  u32 from = 0, to = 0;
  if (!find(entry, at, from, to)) return true;
  const u8 ring = ring_of(entry, context);
  out.instances.reserve(to - from);
  for (u32 i = from; i < to; ++i)
    out.instances.push_back(placement_of(entry, entry.order[i], ring));
  out.kind = ring < 3 ? ring : 3;
  return true;
}

u8 representation(const void* state, const scene_gen::Context& context) noexcept {
  return ring_of(*static_cast<const Entry*>(state), context);
}

constexpr scene_gen::PlacementGeneratorDesc k_authored{
    .name = k_generator,
    .open = &open,
    .close = &close,
    .expand = &expand,
    .meshes = &meshes,
    .tile = &tile,
    .occupies = &occupies,
    .representation = &representation,
};
const scene_gen::Registrar k_authored_registrar{k_authored};

}  // namespace

}  // namespace engine::authored
