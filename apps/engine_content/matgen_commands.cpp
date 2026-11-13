// `engine-content matgen` (docs/subsystems/apps.md, "engine-content"; docs/subsystems/matgen.md,
// "The command"): the generated material sets built into a pack directory. Each set is a derived
// node: its directory holds the four images, a metallic image of zeros (a pack's role names five
// maps) and `set.json`, which records the key the images were built for, so a second run builds
// only the sets whose recipe or generator moved. `pack.json` beside them is a material pack's
// manifest (materials.md, "A pack"), which `materials::pack_at` reads from `<pack>/generated`
// behind the fetched pack's own roles. Nothing here is committed: the default directory is under
// the git-ignored `content/materials/`.
#include "content_commands.h"

#include <cstdio>
#if ENGINE_CONTENT_MATGEN
#include <core/containers/vector.h>
#include <core/json/json.h>
#include <core/platform/process.h>
#include <core/schema/json_reflect.h>
#include <core/time/time.h>
#include <domain/materials/materials.h>
#include <domain/matgen/matgen.h>
#include <foundation/image/png.h>
#include <foundation/io/vfs.h>

#include <charconv>
#include <span>
#include <string>
#include <string_view>
#endif

namespace engine::content {

#if ENGINE_CONTENT_MATGEN
namespace {

std::string hex16(u64 value) {
  char text[17];
  std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(value));
  return std::string(text);
}

bool write_image(const std::string& path, u32 size, u32 channels, const Vector<u8>& pixels,
                 u64& bytes) {
  if (image::write_png(path, size, size, channels,
                       std::span<const u8>(pixels.data(), pixels.size())) != io::Status::Ok)
    return false;
  io::FileInfo info;
  if (io::stat_file(path, info) == io::Status::Ok) bytes += info.size;
  return true;
}

JsonValue map_entry(const std::string& role, const char* file, u32 size) {
  JsonValue map = JsonValue::object();
  map.set("file", JsonValue(role + "/" + file));
  map.set("width", JsonValue(size));
  map.set("height", JsonValue(size));
  return map;
}

}  // namespace
#endif

int matgen_command(int argc, char** argv) {
#if !ENGINE_CONTENT_MATGEN
  (void)argc;
  (void)argv;
  std::fprintf(stderr, "matgen requires the matgen capability (ENGINE_WITH_MATGEN)\n");
  return 1;
#else
  const char* usage =
      "usage: engine-content matgen [--out <pack dir>] [--texels <n>] [--role <role>]...\n"
      "                              [--set <recipe.json>]... [--force]\n"
      "  builds generated material sets into a pack directory (pack.json and, a role, five\n"
      "  images and set.json); every default role when neither --role nor --set is given;\n"
      "  the directory defaults to <repo>/content/materials/generated, which every host's\n"
      "  default pack lookup reads behind the fetched pack; a set whose set.json records the\n"
      "  recipe's key is not rebuilt (--force rebuilds); --texels overrides each recipe's\n";
  std::string out_dir;
  u32 texels = 0;
  bool force = false;
  Vector<matgen::Set> sets;
  const auto fail = [](const std::string& e) {
    std::fprintf(stderr, "matgen: %s\n", e.c_str());
    return 1;
  };
  for (int i = 2; i < argc; ++i) {
    const std::string_view flag = argv[i];
    if (flag == "--force") {
      force = true;
      continue;
    }
    if (i + 1 >= argc) {
      std::fputs(usage, stderr);
      return 2;
    }
    const std::string value = argv[++i];
    if (flag == "--out") {
      out_dir = value;
    } else if (flag == "--texels") {
      const auto r = std::from_chars(value.data(), value.data() + value.size(), texels);
      if (r.ec != std::errc{} || r.ptr != value.data() + value.size()) {
        std::fputs(usage, stderr);
        return 2;
      }
    } else if (flag == "--role") {
      matgen::Set set;
      if (!matgen::default_set(value, set))
        return fail("no default set for the role '" + value + "'");
      sets.push_back(set);
    } else if (flag == "--set") {
      std::string text, error;
      if (io::read_file(value, text) != io::Status::Ok)
        return fail("'" + value + "' could not be read");
      matgen::Set set;
      if (!matgen::read_set(text, set, error)) return fail(value + ": " + error);
      sets.push_back(set);
    } else {
      std::fputs(usage, stderr);
      return 2;
    }
  }
  if (sets.empty()) sets = matgen::default_sets();
  if (out_dir.empty()) {
    // Where the default pack is written, which the test switch that blinds every reader does not
    // touch (materials.h, `repository_pack_dir`).
    const std::string pack = materials::repository_pack_dir(platform::executable_directory());
    if (pack.empty()) return fail("no repository above the executable: name --out <pack dir>");
    out_dir = io::join_path(pack, "generated");
  }

  // The roles a pack already there holds stay in it; this run's replace their own.
  const std::string manifest = io::join_path(out_dir, "pack.json");
  JsonValue roles = JsonValue::object();
  {
    std::string text;
    JsonValue old;
    if (io::read_file(manifest, text) == io::Status::Ok && parse_json(text, old).ok &&
        old.is_object()) {
      const JsonValue* held = old.find("roles");
      if (held != nullptr && held->is_object()) {
        for (const auto& [role, entry] : held->as_object())
          roles.set(role, entry);
      }
    }
  }

  for (matgen::Set& set : sets) {
    if (texels != 0) set.texels = texels;
    std::string error;
    if (!matgen::validate_set(set, error)) return fail(set.role + ": " + error);
    const std::string key = hex16(matgen::set_key(set));
    const std::string dir = io::join_path(out_dir, set.role);
    const std::string sidecar = io::join_path(dir, "set.json");
    const char* files[5] = {"base_color.png", "normal.png", "roughness.png", "height.png",
                            "metallic.png"};
    bool current = !force;
    if (current) {
      std::string text;
      JsonValue held;
      current = io::read_file(sidecar, text) == io::Status::Ok && parse_json(text, held).ok &&
                held.is_object() && held.find("key") != nullptr && held.find("key")->is_string() &&
                held.find("key")->as_string() == key;
      for (const char* file : files)
        current = current && io::exists(io::join_path(dir, file));
    }
    JsonValue line = JsonValue::object();
    line.set("role", JsonValue(set.role));
    line.set("key", JsonValue(key));
    line.set("dir", JsonValue(dir));
    line.set("texels", JsonValue(set.texels));
    line.set("tile_mm", JsonValue(set.tile_mm));
    line.set("built", JsonValue(!current));
    if (!current) {
      if (io::make_directories(dir) != io::Status::Ok)
        return fail("'" + dir + "' could not be made");
      matgen::SetImages images;
      matgen::SetStats stats;
      const i64 began = time::monotonic_ns();
      if (!matgen::build_set(set, images, &stats, error)) return fail(set.role + ": " + error);
      const f64 build_ms = static_cast<f64>(time::monotonic_ns() - began) * 1.0e-6;
      u64 bytes = 0;
      Vector<u8> none;
      none.assign(16, static_cast<u8>(0));
      const i64 wrote = time::monotonic_ns();
      if (!write_image(io::join_path(dir, files[0]), images.size, 3, images.base_color, bytes) ||
          !write_image(io::join_path(dir, files[1]), images.size, 3, images.normal, bytes) ||
          !write_image(io::join_path(dir, files[2]), images.size, 1, images.roughness, bytes) ||
          !write_image(io::join_path(dir, files[3]), images.size, 1, images.height, bytes) ||
          !write_image(io::join_path(dir, files[4]), 4, 1, none, bytes))
        return fail("an image of '" + dir + "' could not be written");
      const f64 write_ms = static_cast<f64>(time::monotonic_ns() - wrote) * 1.0e-6;
      // The sidecar last: a run that died half way leaves no key, and the next one rebuilds.
      JsonValue record = JsonValue::object();
      record.set("format", JsonValue("engine.matgen.set/1"));
      record.set("key", JsonValue(key));
      record.set("generator_version", JsonValue(matgen::k_matgen_version));
      record.set("recipe", schema::to_json(set));
      JsonValue measured = JsonValue::object();
      measured.set("mean_albedo_r", JsonValue(stats.mean_color[0]));
      measured.set("mean_albedo_g", JsonValue(stats.mean_color[1]));
      measured.set("mean_albedo_b", JsonValue(stats.mean_color[2]));
      measured.set("mean_roughness", JsonValue(stats.mean_roughness));
      measured.set("height_min_m", JsonValue(stats.height_min_m));
      measured.set("height_max_m", JsonValue(stats.height_max_m));
      measured.set("d16_mm", JsonValue(stats.d16_mm));
      measured.set("d50_mm", JsonValue(stats.d50_mm));
      measured.set("d84_mm", JsonValue(stats.d84_mm));
      measured.set("clast_cover", JsonValue(stats.clast_cover));
      measured.set("clasts", JsonValue(stats.clasts_drawn));
      measured.set("fresh_share", JsonValue(stats.fresh_share));
      measured.set("lichen_cover", JsonValue(stats.lichen_cover));
      measured.set("moss_share", JsonValue(stats.moss_share));
      measured.set("thatch_cover", JsonValue(stats.thatch_cover));
      measured.set("live_cover", JsonValue(stats.live_cover));
      measured.set("buried_share", JsonValue(stats.buried_share));
      measured.set("streak_share", JsonValue(stats.streak_share));
      measured.set("moss_cover", JsonValue(stats.moss_cover));
      measured.set("landmark_contrast", JsonValue(stats.landmark_contrast));
      measured.set("block_contrast", JsonValue(stats.block_contrast));
      measured.set("contrast_5cm", JsonValue(stats.contrast_5cm));
      measured.set("contrast_25cm", JsonValue(stats.contrast_25cm));
      measured.set("relief_rms_mm", JsonValue(stats.relief_rms_mm));
      measured.set("latewood_share", JsonValue(stats.latewood_share));
      measured.set("check_share", JsonValue(stats.check_share));
      measured.set("bark_share", JsonValue(stats.bark_share));
      measured.set("knots", JsonValue(stats.knots));
      record.set("measured", measured);
      if (io::write_file_atomic(sidecar, write_json(record)) != io::Status::Ok)
        return fail("'" + sidecar + "' could not be written");
      line.set("build_ms", JsonValue(build_ms));
      line.set("write_ms", JsonValue(write_ms));
      line.set("png_bytes", JsonValue(bytes));
      line.set("measured", measured);
    }
    std::printf("%s\n", write_json(line, {.pretty = false}).c_str());

    JsonValue role = JsonValue::object();
    role.set("tile_m", JsonValue(static_cast<f64>(set.tile_mm) * 0.001));
    role.set("normal_scale", JsonValue(static_cast<f64>(set.normal_scale_permille) * 0.001));
    role.set("normal_convention", JsonValue("opengl"));
    role.set("sampling", JsonValue(set.stochastic ? "stochastic" : "tiled"));
    JsonValue maps = JsonValue::object();
    maps.set("base_color", map_entry(set.role, files[0], set.texels));
    maps.set("normal", map_entry(set.role, files[1], set.texels));
    maps.set("roughness", map_entry(set.role, files[2], set.texels));
    maps.set("height", map_entry(set.role, files[3], set.texels));
    maps.set("metallic", map_entry(set.role, files[4], 4));
    role.set("maps", maps);
    JsonValue provenance = JsonValue::object();
    provenance.set("generator", JsonValue("engine.matgen"));
    provenance.set("generator_version", JsonValue(matgen::k_matgen_version));
    provenance.set("key", JsonValue(key));
    role.set("provenance", provenance);
    role.set("licence", JsonValue("generated by the engine from its own recipe; no third-party "
                                  "content"));
    roles.set(set.role, role);
  }

  JsonValue pack = JsonValue::object();
  pack.set("format", JsonValue(materials::k_pack_format));
  pack.set("name", JsonValue("generated"));
  pack.set("version", JsonValue(std::string("matgen-") + std::to_string(matgen::k_matgen_version)));
  pack.set("licence", JsonValue("generated by the engine; no third-party content"));
  pack.set("roles", roles);
  if (io::make_directories(out_dir) != io::Status::Ok ||
      io::write_file_atomic(manifest, write_json(pack)) != io::Status::Ok)
    return fail("'" + manifest + "' could not be written");
  // What the engine will read back: the pack parses, or this run failed.
  materials::Pack check;
  std::string error;
  if (!materials::parse_pack(write_json(pack), out_dir, check, error))
    return fail("the pack written does not parse: " + error);
  JsonValue summary = JsonValue::object();
  summary.set("pack", JsonValue(manifest));
  summary.set("roles", JsonValue(static_cast<u32>(check.roles.size())));
  std::printf("%s\n", write_json(summary, {.pretty = false}).c_str());
  return 0;
#endif
}

}  // namespace engine::content
