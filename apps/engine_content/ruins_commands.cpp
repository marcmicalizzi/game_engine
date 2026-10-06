// engine-content ruins and ruins-kit: the ruin assembler on the command line (docs/subsystems/
// ruins.md, docs/subsystems/apps.md). Compiled against domain/ruins when this build has the
// capability (ENGINE_CONTENT_RUINS); without it both commands are refused with a sentence, as
// `tissue` is, and `msvc-minimal` is the proof that the rest of the app builds.
//
//   ruins <kit.json> <world seed> <x,z> --out <scene.json>   the building on one tile, as a scene
//                                                            fragment: the derived-node form for an
//                                                            authored place
//       --region <x0,z0,x1,z1> [--count <n> | --density <d>]  many tiles instead of one
//       --blocks <block-kit.json>                             laid block by block (blocks.h)
//   ruins-kit <directory> [--e33-sizes] [--inside-corner]     the synthetic kit of boxes, written
//                                                            where it is asked for
//   ruins-block-kit <directory> [--fidelity low|mid|high]     the synthetic block kit
//
// `ruins` prints one JSON line: the buildings, the pieces by kind (with --blocks, the blocks by
// role, standing and fallen), the pieces per building, the shapes, the milliseconds the assembly
// took and the hash of what it assembled — the number `tests/determinism_tests.cpp` pins, which
// is the same on every toolchain.
#include "content_commands.h"

#include <core/base/types.h>
#include <core/json/json.h>

#include <cstdio>
#include <string>
#include <string_view>

#if ENGINE_CONTENT_RUINS
#include <core/containers/vector.h>
#include <core/jobs/job_system.h>
#include <core/time/time.h>
#include <domain/ruins/fragment.h>
#include <domain/ruins/ruins.h>
#include <domain/ruins/synthetic_kit.h>
#include <foundation/io/vfs.h>

#include <algorithm>
#include <charconv>
#endif

namespace engine::content {

namespace {

constexpr int k_exit_ok = 0;
constexpr int k_exit_error = 1;
constexpr int k_exit_usage = 2;

const char* k_ruins_usage =
    "usage: engine-content ruins <kit.json> <world seed> <x,z> --out <scene.json> [options]\n"
    "  the ruined building the world seed puts on tile (x, z), assembled from the kit and written\n"
    "  as a scene fragment (engine.scene.Scene): its pieces as instances of the kit's meshes, "
    "each\n"
    "  with its RuinTag, the building in ruin_sites and its walls' sand in sand_drifts\n"
    "      --tile-size <m>     a tile's edge in metres (default 32)\n"
    "      --wind <deg>        where the prevailing wind blows from, degrees about +y from +x\n"
    "      --ground <m>        the height of the flat ground the ruins stand on (default 0)\n"
    "      --region <x0,z0,x1,z1>  every tile of the square instead of <x,z> (give '-' for it)\n"
    "      --count <n>         with --region: the n tiles the seed ranks first\n"
    "      --density <d>       with --region: every tile the seed gives a building at this\n"
    "                          density (default 0.05, the endless desert's rule)\n"
    "      --walls             leave the debris out: the capability's far tier\n"
    "      --blocks <block-kit.json>  lay each building block by block from this block kit\n"
    "                          (engine.scene.RuinBlockKit) instead of the kit's sections: the\n"
    "                          same footprint, ruin and openings, every block an instance\n"
    "      --jobs <n>          performance-pool workers for a region (default: one per CPU)\n"
    "      --no-write          assemble and report without writing the fragment\n"
    "usage: engine-content ruins-kit <directory> [--e33-sizes] [--inside-corner]\n"
    "  the synthetic kit of boxes: <directory>/kit.json and a GLB per member\n"
    "usage: engine-content ruins-block-kit <directory> [--fidelity low|mid|high]\n"
    "                                      [--thickness <m>] [--course <m>] [--stretcher <m>]\n"
    "  the synthetic block kit: <directory>/block-kit.json and a GLB per block, at 44 triangles a\n"
    "  block (low), about 5,000 (mid) or about 50,000 (high); the thickness should be the section\n"
    "  kit's (default 0.6, the kit of boxes'; E33's ashlar is 0.64)\n";

[[maybe_unused]] int usage(const char* message) {
  if (message != nullptr) std::fprintf(stderr, "engine-content ruins: %s\n", message);
  std::fputs(k_ruins_usage, stderr);
  return k_exit_usage;
}

int failed(const std::string& message) {
  std::fprintf(stderr, "engine-content ruins: %s\n", message.c_str());
  return k_exit_error;
}

[[maybe_unused]] void print_json(const JsonValue& value) {
  std::string text = write_json(value, JsonWriteOptions{.pretty = false});
  text.push_back('\n');
  std::fwrite(text.data(), 1, text.size(), stdout);
}

#if ENGINE_CONTENT_RUINS

bool parse_f32(std::string_view text, f32& out) {
  const char* end = text.data() + text.size();
  const auto r = std::from_chars(text.data(), end, out);
  return r.ec == std::errc() && r.ptr == end;
}

bool parse_u64(std::string_view text, u64& out) {
  const char* end = text.data() + text.size();
  const auto r = std::from_chars(text.data(), end, out);
  return r.ec == std::errc() && r.ptr == end && !text.empty();
}

// "x,z" or "x0,z0,x1,z1": comma-separated integers, exactly `count` of them.
bool parse_ints(std::string_view text, i32* out, u32 count) {
  for (u32 i = 0; i < count; ++i) {
    const usize comma = i + 1 < count ? text.find(',') : text.size();
    if (comma == std::string_view::npos) return false;
    const std::string_view part = text.substr(0, comma);
    const auto r = std::from_chars(part.data(), part.data() + part.size(), out[i]);
    if (r.ec != std::errc() || r.ptr != part.data() + part.size() || part.empty()) return false;
    text = i + 1 < count ? text.substr(comma + 1) : std::string_view();
  }
  return true;
}

// The ruins' ground speaks whole millimetres (ADR-0053); flat ground answers its one height.
f64 flat_ground(const void* context, i64, i64) noexcept {
  return static_cast<f64>(*static_cast<const f32*>(context));
}

// `ruins --blocks`: the same buildings laid block by block, reported by role, standing and fallen,
// with `hash_blocks` — the number the determinism test pins beside the assembler's.
int ruins_blocks(const ruins::Kit& kit, const std::string& blocks_path,
                 const ruins::Placement& placement, const Vector<ruins::TileCoord>& tiles,
                 jobs::JobSystem& pool, bool write, const std::string& out_path,
                 const std::string& seed_text, u64 seed) {
  ruins::BlockKit blocks;
  std::string error;
  if (!ruins::read_block_kit_file(blocks_path, blocks, error)) return failed(error);
  ruins::BlockOutput out;
  const i64 start = time::monotonic_ns();
  const bool ok = ruins::assemble_block_tiles(
      kit, blocks, placement, std::span<const ruins::TileCoord>(tiles.data(), tiles.size()),
      tiles.size() > 1 ? &pool : nullptr, out, &error);
  const i64 elapsed = time::monotonic_ns() - start;
  if (!ok) return failed(error);
  if (write) {
    const std::string name = kit.name + " ruins laid in " + blocks.name + ", seed " + seed_text;
    if (!ruins::write_block_fragment(out_path, kit, blocks, out, name, &error))
      return failed(error);
  }
  u32 roles[ruins::k_block_roles] = {};
  u32 fallen = 0;
  u32 eroded = 0;
  f64 rubble_cm3 = 0.0;
  for (const ruins::Block& b : out.blocks) {
    ++roles[b.role < ruins::k_block_roles ? b.role : 0];
    eroded += (b.flags & ruins::k_block_eroded) != 0 ? 1u : 0u;
    if ((b.flags & ruins::k_block_fallen) == 0) continue;
    ++fallen;
    const ruins::KitBlock& kb = blocks.blocks[b.block];
    rubble_cm3 += static_cast<f64>(kb.length_cm) * kb.height_cm * kb.depth_cm;
  }
  u32 lo = ~0u, hi = 0;
  for (const ruins::Site& site : out.sites) {
    lo = std::min(lo, site.instance_count);
    hi = std::max(hi, site.instance_count);
  }
  if (out.sites.empty()) lo = 0;
  JsonValue summary = JsonValue::object();
  summary.set("kit", JsonValue(kit.name));
  summary.set("block_kit", JsonValue(blocks.name));
  summary.set("seed", JsonValue(seed));
  summary.set("buildings", JsonValue(out.sites.size()));
  summary.set("instances", JsonValue(out.blocks.size()));
  summary.set("standing", JsonValue(out.blocks.size() - fallen));
  summary.set("fallen", JsonValue(fallen));
  summary.set("eroded", JsonValue(eroded));
  // The rubble: one fallen block a site of the building's rubble field, and their volume — what
  // the same buildings in sections heap their debris to (ruins.md, "The rubble rule").
  JsonValue rubble = JsonValue::object();
  rubble.set("sites", JsonValue(fallen));
  rubble.set("pieces", JsonValue(fallen));
  rubble.set("m3", JsonValue(rubble_cm3 * 1.0e-6));
  summary.set("rubble", std::move(rubble));
  summary.set("drifts", JsonValue(out.drifts.size()));
  JsonValue by_role = JsonValue::object();
  for (u32 r = 0; r < ruins::k_block_roles; ++r)
    by_role.set(ruins::block_role_name(static_cast<ruins::BlockRole>(r)), JsonValue(roles[r]));
  summary.set("roles", std::move(by_role));
  JsonValue per = JsonValue::object();
  per.set("min", JsonValue(lo));
  per.set("max", JsonValue(hi));
  per.set("mean", JsonValue(out.sites.empty() ? 0.0
                                              : static_cast<f64>(out.blocks.size()) /
                                                    static_cast<f64>(out.sites.size())));
  summary.set("per_building", std::move(per));
  summary.set("meshes", JsonValue(blocks.meshes.size()));
  summary.set("ms", JsonValue(static_cast<f64>(elapsed) / 1.0e6));
  summary.set("threads",
              JsonValue(tiles.size() > 1 ? pool.worker_count(jobs::Pool::Performance) + 1 : 1u));
  char hash[17];
  std::snprintf(hash, sizeof(hash), "%016llx",
                static_cast<unsigned long long>(ruins::hash_blocks(out)));
  summary.set("hash", JsonValue(std::string(hash)));
  if (write) summary.set("output", JsonValue(out_path));
  print_json(summary);
  return k_exit_ok;
}

int ruins(int argc, char** argv) {
  std::string positional[3];
  u32 count_positional = 0;
  std::string out_path;
  f32 tile_size = 32.0f;
  f32 wind = 0.0f;
  f32 ground = 0.0f;
  i32 region[4] = {0, 0, 0, 0};
  bool have_region = false;
  u32 count = 0;
  f32 density = 0.05f;
  bool walls = false;
  bool write = true;
  u64 jobs_wanted = 0;
  std::string blocks_path;
  for (int i = 2; i < argc; ++i) {
    const std::string_view a = argv[i];
    auto value = [&](std::string_view& v) {
      if (i + 1 >= argc) return false;
      v = argv[++i];
      return true;
    };
    std::string_view v;
    if (a == "--out") {
      if (!value(v)) return usage("--out needs a path");
      out_path = std::string(v);
    } else if (a == "--tile-size") {
      if (!value(v) || !parse_f32(v, tile_size) || !(tile_size > 0.0f))
        return usage("--tile-size is a positive number of metres");
    } else if (a == "--wind") {
      if (!value(v) || !parse_f32(v, wind)) return usage("--wind is degrees");
    } else if (a == "--ground") {
      if (!value(v) || !parse_f32(v, ground)) return usage("--ground is metres");
    } else if (a == "--region") {
      if (!value(v) || !parse_ints(v, region, 4) || region[2] < region[0] || region[3] < region[1])
        return usage("--region is x0,z0,x1,z1 with x0 <= x1 and z0 <= z1");
      have_region = true;
    } else if (a == "--count") {
      u64 n = 0;
      if (!value(v) || !parse_u64(v, n) || n == 0 || n > 0xffffffffull)
        return usage("--count is a positive number of buildings");
      count = static_cast<u32>(n);
    } else if (a == "--density") {
      if (!value(v) || !parse_f32(v, density) || density < 0.0f || density > 1.0f)
        return usage("--density is a share of tiles, 0 to 1");
    } else if (a == "--jobs") {
      if (!value(v) || !parse_u64(v, jobs_wanted) || jobs_wanted == 0 || jobs_wanted > 4096)
        return usage("--jobs is 1..4096");
    } else if (a == "--walls") {
      walls = true;
    } else if (a == "--blocks") {
      if (!value(v) || v.empty()) return usage("--blocks needs a block kit");
      blocks_path = std::string(v);
    } else if (a == "--no-write") {
      write = false;
    } else if (a.size() > 1 && a[0] == '-' && !(a[1] >= '0' && a[1] <= '9')) {
      return usage("unknown option");  // a negative tile, -3,2, is a value and not an option
    } else {
      if (count_positional == 3) return usage("ruins takes a kit, a world seed and a tile");
      positional[count_positional++] = std::string(a);
    }
  }
  if (count_positional != 3) return usage("ruins takes a kit, a world seed and a tile");
  if (write && out_path.empty()) return usage("ruins needs --out <scene.json>, or --no-write");
  u64 seed = 0;
  if (!parse_u64(positional[1], seed)) return usage("the world seed is a non-negative integer");
  i32 tile[2] = {0, 0};
  if (!have_region && !parse_ints(positional[2], tile, 2))
    return usage("the tile is x,z (or '-' with --region)");
  if (!have_region && (count != 0)) return usage("--count needs --region");

  ruins::Kit kit;
  std::string error;
  if (!ruins::read_kit_file(positional[0], kit, error)) return failed(error);

  Vector<ruins::TileCoord> tiles;
  if (have_region) {
    ruins::choose_tiles(seed, ruins::TileCoord{region[0], region[1]},
                        ruins::TileCoord{region[2], region[3]}, count, density, tiles);
  } else {
    tiles.push_back(ruins::TileCoord{tile[0], tile[1]});
  }
  ruins::Placement placement;
  placement.world_seed = seed;
  placement.tile_cm = ruins::to_cm(tile_size);
  placement.wind_step = ruins::yaw_step_from_degrees(wind);
  placement.detail = walls ? ruins::Detail::walls : ruins::Detail::full;
  placement.ground = ruins::Ground{&flat_ground, &ground};

  jobs::JobSystemConfig config;
  if (jobs_wanted > 0) config.performance_workers = static_cast<u32>(jobs_wanted);
  config.efficiency_workers = 1;
  jobs::JobSystem pool(config);
  if (!blocks_path.empty()) {
    return ruins_blocks(kit, blocks_path, placement, tiles, pool, write, out_path, positional[1],
                        seed);
  }
  ruins::Output out;
  const i64 start = time::monotonic_ns();
  const bool ok = ruins::assemble_tiles(
      kit, placement, std::span<const ruins::TileCoord>(tiles.data(), tiles.size()),
      tiles.size() > 1 ? &pool : nullptr, out, &error);
  const i64 elapsed = time::monotonic_ns() - start;
  if (!ok) return failed(error);

  if (write) {
    std::string name = kit.name + " ruins, seed " + positional[1];
    if (!ruins::write_fragment(out_path, kit, out, name, &error)) return failed(error);
  }

  // What it made: by kind, per building, by shape; and the rubble — the debris heaps, one a site
  // of the rubble field (a building's pieces of one wall and slot), and their volume.
  u32 kinds[ruins::k_piece_kinds] = {};
  u32 rubble_sites = 0;
  f64 rubble_cm3 = 0.0;
  for (u32 i = 0; i < out.instances.size(); ++i) {
    const ruins::Instance& piece = out.instances[i];
    ++kinds[piece.kind < ruins::k_piece_kinds ? piece.kind : 0];
    if (piece.kind != static_cast<u8>(ruins::PieceKind::debris)) continue;
    rubble_cm3 += static_cast<f64>(kit.members[piece.member].volume_cm3);
    // A heap's pieces are consecutive: a new site wherever the building, wall or slot changes.
    const ruins::Instance* prev = i > 0 ? &out.instances[i - 1] : nullptr;
    if (prev == nullptr || prev->kind != piece.kind || prev->building != piece.building ||
        prev->wall != piece.wall || prev->slot != piece.slot) {
      ++rubble_sites;
    }
  }
  u32 shapes[4] = {};
  u32 lo = ~0u, hi = 0;
  for (const ruins::Site& site : out.sites) {
    ++shapes[site.shape & 3u];
    lo = std::min(lo, site.instance_count);
    hi = std::max(hi, site.instance_count);
  }
  if (out.sites.empty()) lo = 0;
  JsonValue summary = JsonValue::object();
  summary.set("kit", JsonValue(kit.name));
  summary.set("profile", JsonValue(kit.profile));
  summary.set("seed", JsonValue(seed));
  summary.set("buildings", JsonValue(out.sites.size()));
  summary.set("instances", JsonValue(out.instances.size()));
  summary.set("drifts", JsonValue(out.drifts.size()));
  JsonValue by_kind = JsonValue::object();
  for (u32 k = 0; k < ruins::k_piece_kinds; ++k)
    by_kind.set(ruins::piece_kind_name(static_cast<ruins::PieceKind>(k)), JsonValue(kinds[k]));
  summary.set("kinds", std::move(by_kind));
  JsonValue rubble = JsonValue::object();
  rubble.set("sites", JsonValue(rubble_sites));
  rubble.set("pieces", JsonValue(kinds[static_cast<u32>(ruins::PieceKind::debris)]));
  rubble.set("m3", JsonValue(rubble_cm3 * 1.0e-6));
  summary.set("rubble", std::move(rubble));
  JsonValue per = JsonValue::object();
  per.set("min", JsonValue(lo));
  per.set("max", JsonValue(hi));
  per.set("mean", JsonValue(out.sites.empty() ? 0.0
                                              : static_cast<f64>(out.instances.size()) /
                                                    static_cast<f64>(out.sites.size())));
  summary.set("per_building", std::move(per));
  JsonValue by_shape = JsonValue::object();
  for (u32 s = 0; s < 4; ++s)
    by_shape.set(ruins::shape_name(static_cast<ruins::Shape>(s)), JsonValue(shapes[s]));
  summary.set("shapes", std::move(by_shape));
  summary.set("meshes", JsonValue(kit.meshes.size()));
  summary.set("ms", JsonValue(static_cast<f64>(elapsed) / 1.0e6));
  summary.set("threads",
              JsonValue(tiles.size() > 1 ? pool.worker_count(jobs::Pool::Performance) + 1 : 1u));
  char hash[17];
  std::snprintf(hash, sizeof(hash), "%016llx",
                static_cast<unsigned long long>(ruins::hash_output(out)));
  summary.set("hash", JsonValue(std::string(hash)));
  if (write) summary.set("output", JsonValue(out_path));
  print_json(summary);
  return k_exit_ok;
}

int ruins_kit(int argc, char** argv) {
  std::string dir;
  ruins::SyntheticKitOptions options;
  for (int i = 2; i < argc; ++i) {
    const std::string_view a = argv[i];
    if (a == "--e33-sizes") {
      const bool inside = options.inside_corner;
      options = ruins::e33_sized_options();
      options.inside_corner = inside;
    } else if (a == "--inside-corner") {
      options.inside_corner = true;
    } else if (!a.empty() && a[0] == '-') {
      return usage("unknown option for ruins-kit");
    } else if (dir.empty()) {
      dir = std::string(a);
    } else {
      return usage("ruins-kit takes one directory");
    }
  }
  if (dir.empty()) return usage("ruins-kit takes one directory");
  std::string error;
  std::string kit_path;
  if (!ruins::write_synthetic_kit(dir, options, &error, &kit_path)) return failed(error);
  ruins::Kit kit;
  if (!ruins::read_kit_file(kit_path, kit, error)) return failed(error);
  JsonValue summary = JsonValue::object();
  summary.set("kit", JsonValue(kit_path));
  summary.set("name", JsonValue(kit.name));
  summary.set("members", JsonValue(kit.members.size()));
  summary.set("meshes", JsonValue(kit.meshes.size()));
  print_json(summary);
  return k_exit_ok;
}

int ruins_block_kit(int argc, char** argv) {
  std::string dir;
  ruins::SyntheticBlockOptions options;
  for (int i = 2; i < argc; ++i) {
    const std::string_view a = argv[i];
    auto value = [&](std::string_view& v) {
      if (i + 1 >= argc) return false;
      v = argv[++i];
      return true;
    };
    std::string_view v;
    if (a == "--fidelity") {
      if (!value(v) || !ruins::parse_block_fidelity(v, options.fidelity))
        return usage("--fidelity is low, mid or high");
    } else if (a == "--thickness") {
      if (!value(v) || !parse_f32(v, options.thickness) || !(options.thickness >= 0.05f))
        return usage("--thickness is metres, at least 0.05");
    } else if (a == "--course") {
      if (!value(v) || !parse_f32(v, options.course) || !(options.course >= 0.05f))
        return usage("--course is metres, at least 0.05");
    } else if (a == "--stretcher") {
      if (!value(v) || !parse_f32(v, options.stretcher) || !(options.stretcher >= 0.1f))
        return usage("--stretcher is metres, at least 0.1");
    } else if (!a.empty() && a[0] == '-') {
      return usage("unknown option for ruins-block-kit");
    } else if (dir.empty()) {
      dir = std::string(a);
    } else {
      return usage("ruins-block-kit takes one directory");
    }
  }
  if (dir.empty()) return usage("ruins-block-kit takes one directory");
  std::string error;
  std::string kit_path;
  const i64 start = time::monotonic_ns();
  if (!ruins::write_synthetic_block_kit(dir, options, &error, &kit_path)) return failed(error);
  const i64 elapsed = time::monotonic_ns() - start;
  ruins::BlockKit kit;
  if (!ruins::read_block_kit_file(kit_path, kit, error)) return failed(error);
  ruins::SyntheticBlockKit made;
  make_synthetic_block_kit(options, made);
  u32 triangles = 0;
  for (const u32 t : made.triangles)
    triangles += t;
  JsonValue summary = JsonValue::object();
  summary.set("kit", JsonValue(kit_path));
  summary.set("name", JsonValue(kit.name));
  summary.set("fidelity", JsonValue(std::string(ruins::block_fidelity_name(options.fidelity))));
  summary.set("blocks", JsonValue(kit.blocks.size()));
  summary.set("meshes", JsonValue(kit.meshes.size()));
  summary.set("triangles", JsonValue(triangles));
  summary.set("ms", JsonValue(static_cast<f64>(elapsed) / 1.0e6));
  print_json(summary);
  return k_exit_ok;
}

#endif

[[maybe_unused]] int refuse() {
  return failed(
      "this engine-content was built without the ruins capability (ENGINE_WITH_RUINS=OFF or "
      "ENGINE_MINIMAL=ON); configure with it on to assemble ruins");
}

}  // namespace

int ruins_command(int argc, char** argv) {
  if (argc >= 3 && (std::string_view(argv[2]) == "--help" || std::string_view(argv[2]) == "-h")) {
    std::fputs(k_ruins_usage, stdout);
    return k_exit_ok;
  }
#if ENGINE_CONTENT_RUINS
  return ruins(argc, argv);
#else
  (void)argv;
  return refuse();
#endif
}

int ruins_kit_command(int argc, char** argv) {
#if ENGINE_CONTENT_RUINS
  return ruins_kit(argc, argv);
#else
  (void)argc;
  (void)argv;
  return refuse();
#endif
}

int ruins_block_kit_command(int argc, char** argv) {
#if ENGINE_CONTENT_RUINS
  return ruins_block_kit(argc, argv);
#else
  (void)argc;
  (void)argv;
  return refuse();
#endif
}

}  // namespace engine::content
