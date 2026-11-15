// `engine-content dwelling` (docs/subsystems/dwellings.md, "The filler as a tool"): the rural
// filler run once for a plot, a household and a seed. It writes the house as a
// `<name>.structure.json` a scene names like any generated mesh, its plan as text, and its walk:
// the tour through every room and a place to stand and look in each, in metres in the building's
// frame, for a camera path or a walk test. What it writes is a starting point its author owns;
// nothing runs it again.
#include "content_commands.h"

#include <cstdio>
#if ENGINE_CONTENT_DWELLINGS
#include <domain/dwellings/describe.h>
#include <domain/dwellings/dwellings.h>
#include <domain/dwellings/tour.h>
#include <domain/structures/structures.h>
#include <foundation/io/vfs.h>

#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#endif

namespace engine::content {

#if ENGINE_CONTENT_DWELLINGS
namespace {

void usage() {
  std::fprintf(stderr,
               "usage: engine-content dwelling <out-dir> [--name <name>] [--persons <n>] "
               "[--means <0|1|2>]\n"
               "         [--frontage <cm>] [--depth <cm>] [--seed <n>] "
               "[--scope whole|exterior|interior]\n"
               "         [--bare timber|furniture|dress|all]...\n");
}

// A length with two decimals (a centimetre) or three (a millimetre). Each format is a literal:
// GCC refuses one handed in as a parameter (-Werror=format-nonliteral).
std::string cm(double metres) {
  char buffer[48];
  std::snprintf(buffer, sizeof buffer, "%.2f", metres);
  return buffer;
}
std::string mm(double metres) {
  char buffer[48];
  std::snprintf(buffer, sizeof buffer, "%.3f", metres);
  return buffer;
}
// "[x, y, z]", x and z to the centimetre and y to the millimetre.
std::string point(double x, double y, double z) {
  return "[" + cm(x) + ", " + mm(y) + ", " + cm(z) + "]";
}

}  // namespace
#endif

int dwelling_command(int argc, char** argv) {
#if ENGINE_CONTENT_DWELLINGS
  if (argc < 3) {
    usage();
    return 2;
  }
  const std::string out_dir = argv[2];
  std::string name = "house";
  dwellings::RuralPlot plot{2400, 1500};
  dwellings::Household household;
  u64 seed = 1;
  structure::MeshScope scope = structure::MeshScope::whole;
  dwellings::RuralStyle style;
  for (int i = 3; i < argc; ++i) {
    const std::string_view a = argv[i];
    if (i + 1 >= argc) {
      usage();
      return 2;
    }
    const char* value = argv[++i];
    if (a == "--name") {
      name = value;
    } else if (a == "--persons") {
      household.persons = static_cast<u8>(std::strtoul(value, nullptr, 10));
    } else if (a == "--means") {
      household.means = static_cast<u8>(std::strtoul(value, nullptr, 10));
    } else if (a == "--frontage") {
      plot.frontage_cm = static_cast<i32>(std::strtol(value, nullptr, 10));
    } else if (a == "--depth") {
      plot.depth_cm = static_cast<i32>(std::strtol(value, nullptr, 10));
    } else if (a == "--seed") {
      seed = std::strtoull(value, nullptr, 10);
    } else if (a == "--scope") {
      const std::string_view s = value;
      if (s == "whole") {
        scope = structure::MeshScope::whole;
      } else if (s == "exterior") {
        scope = structure::MeshScope::exterior;
      } else if (s == "interior") {
        scope = structure::MeshScope::interior;
      } else {
        usage();
        return 2;
      }
    } else if (a == "--bare") {
      const std::string_view s = value;
      if (s != "timber" && s != "furniture" && s != "dress" && s != "all") {
        usage();
        return 2;
      }
      if (s == "timber" || s == "all") style.timber = false;
      if (s == "furniture" || s == "all") style.furniture = false;
      if (s == "dress" || s == "all") style.dressed = false;
    } else {
      usage();
      return 2;
    }
  }

  dwellings::RuralChoice choice;
  dwellings::InteriorPlan plan;
  std::string error;
  if (!dwellings::plan_rural(plot, household, style, seed, choice, plan, &error)) {
    std::fprintf(stderr, "engine-content dwelling: %s\n", error.c_str());
    return 1;
  }
  structure::Recipe recipe;
  dwellings::describe(plan, choice, name, seed, recipe.description);
  recipe.scope = scope;
  std::string why;
  if (!structures::validate(recipe.description, why)) {
    std::fprintf(stderr, "engine-content dwelling: the mesher refuses the description: %s\n",
                 why.c_str());
    return 1;
  }
  structures::MeshPart parts[structures::k_surfaces];
  structures::Report report;
  if (!structures::solve(recipe.description, scope, parts, report, why)) {
    std::fprintf(stderr, "engine-content dwelling: %s\n", why.c_str());
    return 1;
  }

  std::error_code ec;
  std::filesystem::create_directories(out_dir, ec);
  const std::string base = out_dir + "/" + name;
  if (io::write_file_atomic(base + ".structure.json", structures::canonical_json(recipe)) !=
      io::Status::Ok) {
    std::fprintf(stderr, "engine-content dwelling: cannot write %s.structure.json\n", base.c_str());
    return 1;
  }

  // The plan as text, a storey a block, the lane below.
  std::string text;
  for (u8 storey = 0; storey < plan.params.storeys; ++storey) {
    text += std::string(dwellings::plan_type_name(plan.params.type)) + ", storey " +
            std::to_string(storey) + ", 20 cm a character\n";
    text += dwellings::draw_interior(plan, storey, 20);
    text += "\n";
  }
  if (io::write_file_atomic(base + ".plan.txt", text) != io::Status::Ok) return 1;

  // The walk: metres in the description's frame, y the floor under a tour point.
  const double off = static_cast<double>(dwellings::description_offset_cm(choice));
  Vector<dwellings::TourPoint> tour;
  dwellings::tour_interior(plan, i32{choice.outer_wall_cm} + 150, tour);
  Vector<dwellings::RoomView> views;
  dwellings::room_views(plan, 1650, views);
  std::string walk = "{\n  \"format\": \"engine.dwelling.walk.v1\",\n  \"name\": \"" + name +
                     "\",\n  \"type\": \"" + dwellings::plan_type_name(plan.params.type) +
                     "\",\n  \"storeys\": " + std::to_string(plan.params.storeys) +
                     ",\n  \"tour\": [";
  for (u32 i = 0; i < tour.size(); ++i) {
    const dwellings::TourPoint& t = tour[i];
    walk += i == 0 ? "\n    " : ",\n    ";
    walk += "{ \"at\": " + point((t.x_cm + off) / 100.0, t.y_mm / 1000.0, (t.z_cm + off) / 100.0) +
            ", \"room\": ";
    walk += t.room == dwellings::k_no_room ? "-1 }" : std::to_string(t.room) + " }";
  }
  walk += "\n  ],\n  \"views\": [";
  for (u32 i = 0; i < views.size(); ++i) {
    const dwellings::RoomView& v = views[i];
    const dwellings::Room& room = plan.rooms[v.room];
    walk += i == 0 ? "\n    " : ",\n    ";
    walk += "{ \"room\": " + std::to_string(v.room) + ", \"kind\": \"" +
            dwellings::room_kind_name(room.kind) + "\", \"storey\": " + std::to_string(room.storey);
    walk += ", \"eye\": " +
            point((v.eye_x_cm + off) / 100.0, v.eye_y_mm / 1000.0, (v.eye_z_cm + off) / 100.0);
    walk += ", \"at\": " +
            point((v.at_x_cm + off) / 100.0, v.at_y_mm / 1000.0, (v.at_z_cm + off) / 100.0) + " }";
  }
  // The furniture and the timber, a piece a line: its footprint's low and high corners.
  dwellings::Furnishing furnishing;
  if (choice.timber) dwellings::timber_interior(plan, choice.ridge_along_front, furnishing);
  if (choice.furniture) dwellings::furnish_interior(plan, seed, furnishing);
  walk += "\n  ],\n  \"pieces\": [";
  for (u32 i = 0; i < furnishing.pieces.size(); ++i) {
    const dwellings::Piece& p = furnishing.pieces[i];
    walk += i == 0 ? "\n    " : ",\n    ";
    walk += "{ \"kind\": \"" + std::string(dwellings::piece_kind_name(p.kind)) +
            "\", \"room\": " + std::to_string(p.room) +
            ", \"storey\": " + std::to_string(p.storey) +
            ", \"members\": " + std::to_string(p.member_count);
    walk += ", \"low\": [" + cm((p.footprint.x0 + off) / 100.0) + ", " +
            cm((p.footprint.z0 + off) / 100.0) + "], \"high\": [" +
            cm((p.footprint.x1 + off) / 100.0) + ", " + cm((p.footprint.z1 + off) / 100.0) +
            "], \"height\": " + cm(p.height_cm / 100.0) + " }";
  }
  walk += "\n  ]\n}\n";
  if (io::write_file_atomic(base + ".walk.json", walk) != io::Status::Ok) return 1;

  // The path with forward slashes, so the line is JSON whatever the shell handed in.
  std::string shown = base;
  for (char& c : shown) {
    if (c == '\\') c = '/';
  }
  std::printf(
      "{\"name\":\"%s\",\"type\":\"%s\",\"storeys\":%u,\"rooms\":%u,\"hearths\":%u,"
      "\"clear_cm\":[%d,%d],\"outer_cm\":[%d,%d],\"ridge_along_front\":%s,"
      "\"demoted\":%s,\"triangles\":%u,\"pieces\":%u,\"members\":%u,"
      "\"structure\":\"%s.structure.json\"}\n",
      name.c_str(), dwellings::plan_type_name(plan.params.type),
      static_cast<unsigned>(plan.params.storeys), static_cast<unsigned>(plan.rooms.size()),
      static_cast<unsigned>(plan.hearths.size()), plan.params.width_cm, plan.params.depth_cm,
      choice.outer_width_cm, choice.outer_depth_cm, choice.ridge_along_front ? "true" : "false",
      choice.demoted ? "true" : "false", report.triangles,
      static_cast<unsigned>(furnishing.pieces.size()),
      static_cast<unsigned>(furnishing.members.size()), shown.c_str());
  return 0;
#else
  (void)argc;
  (void)argv;
  std::fprintf(stderr, "engine-content dwelling: this build has no dwellings capability\n");
  return 2;
#endif
}

}  // namespace engine::content
