// The ruins as a placement generator (scene_generator.h; docs/subsystems/scene_gen.md). The whole
// read is the renderer's `expand_ruins` and `stream_ruins` as they were under
// `ENGINE_RENDERER_RUINS`, and a tile is the world's `RuinsTiles::build` as it was under
// `ENGINE_WORLD_RUINS`: the same kits, seeds, tiles, wind and ground, so the same buildings, and
// the sentences a scene is refused with are the same words.
#include <core/jobs/job_system.h>
#include <core/log/log.h>
#include <core/schema/json_reflect.h>
#include <core/time/time.h>
#include <domain/ruins/assembler.h>
#include <domain/ruins/blocks.h>
#include <domain/ruins/kit.h>
#include <domain/ruins/scene_generator.h>
#include <domain/scene_gen/scene_gen.h>
#include <foundation/io/vfs.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <schemas/scene.h>
#include <string>

namespace engine::ruins {

namespace {

ENGINE_LOG_CATEGORY_DEFINE(log_ruins_scene, "ruins.scene");

// The entry, read once: its kits, its placement, the tiles that hold its buildings, and the
// assemblers a tile at a time reuses (their scratch and their output), which is why a state is one
// entry's and is used by one thread at a time.
struct Entry {
  scene::RuinScatter scatter;
  std::string where;
  std::string kit_path;
  Kit kit;
  bool blocks = false;  // drawn in blocks, with its block kit
  std::string block_path;
  BlockKit block_kit;
  Placement placement;
  Vector<TileCoord> tiles;  // rank order, as `choose_tiles` gives them
  Vector<u64> keys;         // the same, sorted: what `occupies` searches
  std::unique_ptr<Assembler> assembler;
  std::unique_ptr<BlockAssembler> block_assembler;
  Output out;
  BlockOutput block_out;
};

u64 key_of(TileCoord t) noexcept {
  return (u64{static_cast<u32>(t.x) ^ 0x80000000u} << 32) |
         u64{static_cast<u32>(t.z) ^ 0x80000000u};
}

std::string schema_errors(const schema::ReadContext& ctx) {
  std::string text;
  for (const schema::Diagnostic& d : ctx.diagnostics) {
    if (!text.empty()) text += "; ";
    text += d.path.empty() ? d.message : d.path + ": " + d.message;
  }
  return text.empty() ? std::string("not a ruins entry") : text;
}

// A kit's meshes named "<kit>/<file>", as a scene names them, with the hashes the kit gives.
void kit_meshes(const std::string& kit_name, const Vector<std::string>& meshes,
                const Vector<u64>& hashes, Vector<scene_gen::PlacementMesh>& out) {
  for (u32 m = 0; m < meshes.size(); ++m) {
    scene_gen::PlacementMesh mesh;
    mesh.path = meshes[m];
    mesh.name = kit_name + "/" + std::string(io::file_name(meshes[m]));
    mesh.hash = hashes[m];
    out.push_back(std::move(mesh));
  }
}

// ADR-0053 seam: the assembler asks its ground at float32 metres until the ruins place in f64. The
// scene's ground (`scene_gen::Ground`, which the `Context` holds for the call) asked at the
// millimetre nearest the float.
f32 floor_at_metres(const void* context, f32 x, f32 z) noexcept {
  const auto& ground = *static_cast<const scene_gen::Ground*>(context);
  return static_cast<f32>(ground.floor(WorldPos{static_cast<f64>(x), 0.0, static_cast<f64>(z)}));
}

// The ground the buildings stand on: the floor of whatever ground the scene has (the dunes'
// interdune floor, which does not move; the waves' surface), or flat ground with no terrain.
Ground ground_of(const scene_gen::Context& context) noexcept {
  if (!context.ground.present()) return Ground{};
  return Ground{&floor_at_metres, &context.ground};
}

// ADR-0053 seam: the assembler places in float32 metres until the ruins place in f64; its place is
// widened here and nowhere else.
WorldPos placed_at(Vec3 at) noexcept { return absolute(WorldPos::origin(), at); }

scene_gen::Placement yawed(u32 mesh, Vec3 at, u32 yaw_step, bool rubble) {
  scene_gen::Placement p;
  p.mesh = mesh;
  p.position = placed_at(at);
  p.rotation =
      quat_from_axis_angle(Vec3{0.0f, 1.0f, 0.0f}, radians(22.5f * static_cast<f32>(yaw_step)));
  p.tag = rubble ? u8{1} : u8{0};
  return p;
}

// What the scene's world block says a ring draws: blocks near, sections with their debris in the
// middle, walls alone beyond (`engine.scene.WorldRing::ruins`), and walls past its last ring.
u8 ring_representation(const scene_gen::Context& context) noexcept {
  if (context.world == nullptr || context.ring >= context.world->rings.size()) return k_drawn_walls;
  return static_cast<u8>(context.world->rings[context.ring].ruins);
}

bool open(const JsonValue& params, const scene_gen::Context& context, void** state,
          std::string* error) {
  const auto fail = [&](std::string sentence) {
    if (error != nullptr) *error = std::move(sentence);
    return false;
  };
  auto entry = std::make_unique<Entry>();
  entry->where = std::string(context.where);
  const std::string& where = entry->where;
  schema::ReadContext ctx;
  if (!schema::from_json(entry->scatter, params, ctx) || !ctx.ok())
    return fail(where + ": " + schema_errors(ctx));
  const scene::RuinScatter& scatter = entry->scatter;
  const std::string dir(context.dir);
  const auto resolve = [&](const std::string& p) {
    return io::is_absolute_path(p) || dir.empty() ? p : io::join_path(dir, p);
  };
  if (scatter.kit.empty()) return fail(where + " names no kit");
  // A streamed entry is assembled one ring tile at a time, so its grid must be the ring's.
  const bool streamed = context.world != nullptr;
  if (streamed && std::abs(scatter.tile_size - context.tile_size) > 1e-4f) {
    return fail(
        where + ": its tile_size is " + std::to_string(scatter.tile_size) + " and the world's " +
        std::to_string(context.tile_size) +
        "; a streamed entry is assembled one ring tile at a time, so they must be one grid");
  }
  std::string why;
  entry->kit_path = resolve(scatter.kit);
  if (!read_kit_file(entry->kit_path, entry->kit, why)) return fail(where + ": " + why);
  entry->blocks = scatter.representation == scene::RuinRepresentation::Blocks;
  if (entry->blocks) {
    if (scatter.block_kit.empty())
      return fail(where + " is drawn in blocks and names no block_kit");
    entry->block_path = resolve(scatter.block_kit);
    if (!read_block_kit_file(entry->block_path, entry->block_kit, why))
      return fail(where + ": " + why);
  }
  const i32 tile_cm = to_cm(scatter.tile_size);
  if (tile_cm <= 0) return fail(where + ": tile_size must be positive");
  choose_tiles(scatter.seed, TileCoord{scatter.tile_min[0], scatter.tile_min[1]},
               TileCoord{scatter.tile_max[0], scatter.tile_max[1]}, scatter.count, scatter.density,
               entry->tiles);
  for (const TileCoord& t : entry->tiles)
    entry->keys.push_back(key_of(t));
  std::sort(entry->keys.begin(), entry->keys.end());
  entry->placement.world_seed = scatter.seed;
  entry->placement.tile_cm = tile_cm;
  entry->placement.wind_step = yaw_step_from_degrees(scatter.wind_deg);
  *state = entry.release();
  return true;
}

void close(void* state) noexcept { delete static_cast<Entry*>(state); }

// The whole entry: every building assembled standing on the ground, and only the meshes it draws
// (a blocks entry does not name its kit's members, which nothing would instance and E33's are
// hundreds of megabytes on the GPU). More than 32 tiles go to the job system's performance pool;
// the buildings are joined in tile order, so the instances are the ones a single thread makes.
bool expand(void* state, const scene_gen::Context& context, scene_gen::Placements& out,
            std::string* error) {
  Entry& entry = *static_cast<Entry*>(state);
  Placement placement = entry.placement;
  placement.ground = ground_of(context);
  const i64 start = time::monotonic_ns();
  jobs::JobSystem* workers = entry.tiles.size() > 32 ? context.jobs() : nullptr;
  const u32 threads = workers != nullptr ? workers->worker_count(jobs::Pool::Performance) + 1 : 1;
  const std::span<const TileCoord> span(entry.tiles.data(), entry.tiles.size());
  std::string why;
  if (!entry.blocks) {
    Output built;
    if (!assemble_tiles(entry.kit, placement, span, workers, built, &why)) {
      if (error != nullptr) *error = entry.where + ": " + why;
      return false;
    }
    kit_meshes(entry.kit.name, entry.kit.meshes, entry.kit.mesh_hashes, out.meshes);
    out.instances.reserve(out.instances.size() + built.instances.size());
    for (const Instance& piece : built.instances) {
      out.instances.push_back(yawed(
          entry.kit.members[piece.member].mesh_index, instance_translation(entry.kit, piece),
          instance_yaw_step(entry.kit, piece), piece.kind == static_cast<u8>(PieceKind::debris)));
    }
    out.things += built.sites.size();
    out.kind = k_drawn_sections;
    ENGINE_LOG_INFO(log_ruins_scene, "ruins assembled", log::field("entry", entry.where),
                    log::field("representation", "sections"), log::field("kit", entry.kit.name),
                    log::field("buildings", built.sites.size()),
                    log::field("instances", built.instances.size()), log::field("threads", threads),
                    log::field("ms", static_cast<f64>(time::monotonic_ns() - start) / 1.0e6),
                    log::field("hash", hash_output(built)));
    return true;
  }
  BlockOutput built;
  if (!assemble_block_tiles(entry.kit, entry.block_kit, placement, span, workers, built, &why)) {
    if (error != nullptr) *error = entry.where + ": " + why;
    return false;
  }
  kit_meshes(entry.block_kit.name, entry.block_kit.meshes, entry.block_kit.mesh_hashes, out.meshes);
  out.instances.reserve(out.instances.size() + built.blocks.size());
  u32 fallen = 0;
  for (const Block& block : built.blocks) {
    const bool rubble = (block.flags & k_block_fallen) != 0;
    out.instances.push_back(yawed(entry.block_kit.blocks[block.block].mesh_index,
                                  block_translation(entry.block_kit, block),
                                  block_yaw_step(entry.block_kit, block), rubble));
    fallen += rubble ? 1u : 0u;
  }
  out.things += built.sites.size();
  out.kind = k_drawn_blocks;
  ENGINE_LOG_INFO(log_ruins_scene, "ruins assembled", log::field("entry", entry.where),
                  log::field("representation", "blocks"), log::field("kit", entry.kit.name),
                  log::field("block_kit", entry.block_kit.name),
                  log::field("buildings", built.sites.size()),
                  log::field("instances", built.blocks.size()), log::field("fallen", fallen),
                  log::field("threads", threads),
                  log::field("ms", static_cast<f64>(time::monotonic_ns() - start) / 1.0e6),
                  log::field("hash", hash_blocks(built)));
  return true;
}

// What a streamed scene holds resident: the kit's members always (the rings beyond the inner one
// draw sections) and the block kit's when the entry is drawn in blocks, in that order — the
// indices `tile`'s placements name.
bool meshes(void* state, const scene_gen::Context&, Vector<scene_gen::PlacementMesh>& out,
            std::string*) {
  const Entry& entry = *static_cast<const Entry*>(state);
  kit_meshes(entry.kit.name, entry.kit.meshes, entry.kit.mesh_hashes, out);
  if (entry.blocks)
    kit_meshes(entry.block_kit.name, entry.block_kit.meshes, entry.block_kit.mesh_hashes, out);
  return true;
}

bool occupies(const void* state, scene_gen::TileCoord tile) noexcept {
  const Entry& entry = *static_cast<const Entry*>(state);
  return std::binary_search(entry.keys.begin(), entry.keys.end(),
                            key_of(TileCoord{tile.x, tile.z}));
}

u8 representation(const void*, const scene_gen::Context& context) noexcept {
  return ring_representation(context);
}

// One tile's building in the representation its ring draws: exactly the building `expand` puts on
// the tile, from the same seed, tile, wind and ground.
bool tile(void* state, scene_gen::TileCoord at, const scene_gen::Context& context,
          scene_gen::Placements& out, std::string* error) {
  Entry& entry = *static_cast<Entry*>(state);
  if (!occupies(state, at)) return true;
  const TileCoord t{at.x, at.z};
  const u8 want = ring_representation(context);
  Placement placement = entry.placement;
  placement.ground = ground_of(context);
  std::string why;
  if (want == k_drawn_blocks && entry.blocks) {
    if (entry.block_assembler == nullptr)
      entry.block_assembler = std::make_unique<BlockAssembler>(entry.kit, entry.block_kit);
    entry.block_out.clear();
    placement.detail = Detail::full;
    if (!entry.block_assembler->assemble(placement, t, entry.block_out, &why)) {
      if (error != nullptr) *error = "a tile's building was not laid: " + why;
      return false;
    }
    const u32 first = entry.kit.meshes.size();
    for (const Block& block : entry.block_out.blocks) {
      out.instances.push_back(yawed(first + entry.block_kit.blocks[block.block].mesh_index,
                                    block_translation(entry.block_kit, block),
                                    block_yaw_step(entry.block_kit, block),
                                    (block.flags & k_block_fallen) != 0));
    }
    out.kind = k_drawn_blocks;
  } else {
    if (entry.assembler == nullptr) entry.assembler = std::make_unique<Assembler>(entry.kit);
    entry.out.clear();
    placement.detail = want == k_drawn_walls ? Detail::walls : Detail::full;
    if (!entry.assembler->assemble(placement, t, entry.out, &why)) {
      if (error != nullptr) *error = "a tile's building was not assembled: " + why;
      return false;
    }
    for (const Instance& piece : entry.out.instances) {
      out.instances.push_back(yawed(
          entry.kit.members[piece.member].mesh_index, instance_translation(entry.kit, piece),
          instance_yaw_step(entry.kit, piece), piece.kind == static_cast<u8>(PieceKind::debris)));
    }
    out.kind = want == k_drawn_walls ? k_drawn_walls : k_drawn_sections;
  }
  ++out.things;
  return true;
}

constexpr scene_gen::PlacementGeneratorDesc k_ruins{
    .name = k_placement_generator,
    .open = &open,
    .close = &close,
    .expand = &expand,
    .meshes = &meshes,
    .tile = &tile,
    .occupies = &occupies,
    .representation = &representation,
};
const scene_gen::Registrar k_ruins_registrar{k_ruins};

}  // namespace

}  // namespace engine::ruins
