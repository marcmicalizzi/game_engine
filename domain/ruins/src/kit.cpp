#include "grid.h"

#include <core/json/json.h>
#include <core/schema/json_reflect.h>
#include <domain/ruins/assembler.h>
#include <domain/ruins/kit.h>
#include <foundation/io/vfs.h>

#include <algorithm>
#include <cmath>
#include <schemas/scene.h>

namespace engine::ruins {

namespace {

// 16 lower-case hex digits, as the scene format writes a content hash.
bool parse_hex16(std::string_view text, u64& out) noexcept {
  if (text.size() != 16) return false;
  u64 value = 0;
  for (const char c : text) {
    u64 digit = 0;
    if (c >= '0' && c <= '9') {
      digit = static_cast<u64>(c - '0');
    } else if (c >= 'a' && c <= 'f') {
      digit = static_cast<u64>(c - 'a' + 10);
    } else {
      return false;
    }
    value = value << 4 | digit;
  }
  out = value;
  return true;
}

// A unit axis in the xz plane, as a quarter turn (0 +x, 1 -z, 2 -x, 3 +z), or 4 when the vector
// is not one within a thousandth.
u32 axis_of(const Vec3& v) noexcept {
  constexpr f32 k_tol = 1.0e-3f;
  const bool y0 = std::fabs(v.y) < k_tol;
  if (!y0) return 4;
  if (std::fabs(v.x - 1.0f) < k_tol && std::fabs(v.z) < k_tol) return 0;
  if (std::fabs(v.z + 1.0f) < k_tol && std::fabs(v.x) < k_tol) return 1;
  if (std::fabs(v.x + 1.0f) < k_tol && std::fabs(v.z) < k_tol) return 2;
  if (std::fabs(v.z - 1.0f) < k_tol && std::fabs(v.x) < k_tol) return 3;
  return 4;
}

bool near_cm(f32 metres, i32 cm) noexcept { return to_cm(metres) == cm; }

std::string member_where(const scene::RuinMember& m, u32 i) {
  return "member " + std::to_string(i) + (m.name.empty() ? std::string() : " (" + m.name + ")");
}

// The sockets a member of each kind must have, checked against what the file says; on success
// the member's lengths are filled in from them.
bool read_sockets(const scene::RuinMember& m, u32 i, Member& out, std::string& error) {
  const std::string where = member_where(m, i);
  const PieceKind kind = out.kind;
  if (kind == PieceKind::debris) {
    if (!m.sockets.empty()) {
      error = where + " is debris and has sockets; debris joins nothing";
      return false;
    }
    return true;
  }
  if (m.sockets.size() != 2) {
    error = where + " has " + std::to_string(m.sockets.size()) +
            " sockets; a wall member has two, where the wall comes in and where it goes on";
    return false;
  }
  const scene::RuinSocket& first = m.sockets[0];
  const scene::RuinSocket& second = m.sockets[1];
  // Every member's first socket... is where the wall comes in, and its second where it leaves,
  // along +x. The shape between them is what the kind says.
  if (axis_of(second.direction) != 0 || !near_cm(second.position.y, 0) ||
      !near_cm(second.position.z, 0) || axis_of(second.outside) != 3) {
    error = where +
            ": the second socket must be on the x axis, running along +x with the outside at +z";
    return false;
  }
  const i32 second_x = to_cm(second.position.x);
  if (kind == PieceKind::section || kind == PieceKind::doorway || kind == PieceKind::window) {
    if (!near_cm(first.position.x, 0) || !near_cm(first.position.y, 0) ||
        !near_cm(first.position.z, 0) || axis_of(first.direction) != 0 ||
        axis_of(first.outside) != 3) {
      error = where +
              ": a straight member's first socket is at the origin, running along +x "
              "with the outside at +z";
      return false;
    }
    if (second_x <= 0) {
      error = where + ": its second socket is not past its first along +x";
      return false;
    }
    out.length_cm = second_x;
    return true;
  }
  // A corner: the incoming wall arrives at the vertex (the origin) and turns. An outside corner
  // turns left, so the wall comes in along +z from -z with the outside at -x; an inside corner
  // turns right, so it comes in along -z from +z with the outside at +x.
  const bool outside = kind == PieceKind::corner;
  const u32 in_dir = outside ? 3u : 1u;
  const u32 in_outside = outside ? 2u : 0u;
  const i32 first_z = to_cm(first.position.z);
  if (!near_cm(first.position.x, 0) || !near_cm(first.position.y, 0) ||
      axis_of(first.direction) != in_dir || axis_of(first.outside) != in_outside ||
      (outside ? first_z >= 0 : first_z <= 0) || second_x <= 0) {
    error = where + (outside ? ": an outside corner's first socket is on -z, running along +z "
                               "with the outside at -x, and its second on +x"
                             : ": an inside corner's first socket is on +z, running along -z "
                               "with the outside at +x, and its second on +x");
    return false;
  }
  out.arm_in_cm = outside ? -first_z : first_z;
  out.arm_out_cm = second_x;
  return true;
}

}  // namespace

i32 to_cm(f32 metres) noexcept {
  return static_cast<i32>(std::floor(static_cast<f64>(metres) * 100.0 + 0.5));
}

i32 to_q(f32 fraction) noexcept {
  return static_cast<i32>(std::floor(static_cast<f64>(fraction) * k_q_one + 0.5));
}

const char* piece_kind_name(PieceKind kind) noexcept {
  switch (kind) {
    case PieceKind::section: return "section";
    case PieceKind::corner: return "corner";
    case PieceKind::doorway: return "doorway";
    case PieceKind::window: return "window";
    case PieceKind::debris: return "debris";
    case PieceKind::inside_corner: return "inside_corner";
  }
  return "unknown";
}

bool kit_from_schema(const scene::RuinKit& file, std::string_view dir, Kit& out,
                     std::string& error) {
  out = Kit{};
  if (!file.format.empty() && file.format != "engine.ruin-kit.v1") {
    error = "format is '" + file.format + "', not engine.ruin-kit.v1";
    return false;
  }
  out.name = file.name;
  out.profile = file.profile;
  out.module_cm = to_cm(file.module);
  out.thickness_cm = to_cm(file.thickness);
  out.wall_height_cm = to_cm(file.wall_height);
  out.course_height_cm = to_cm(file.course_height);
  out.block_length_cm = to_cm(file.block_length);
  if (out.module_cm < 10 || out.thickness_cm < 1 || out.wall_height_cm < 10) {
    error = "module, thickness and wall_height must be at least 0.1, 0.01 and 0.1 m";
    return false;
  }
  if (out.course_height_cm < 5 || out.block_length_cm < 5 || out.course_height_cm > 1000 ||
      out.block_length_cm > 1000 || out.thickness_cm > 1000) {
    error =
        "course_height and block_length must be 0.05 to 10 m: the rubble rule counts in the "
        "profile's blocks";
    return false;
  }

  // ---- the rules, once, in the assembler's units ----------------------------------------------
  const scene::RuinRules& r = file.rules;
  Rules& rules = out.rules;
  rules.shape_weight[0] = r.rectangle;
  rules.shape_weight[1] = r.l_shape;
  rules.shape_weight[2] = r.u_shape;
  rules.shape_weight[3] = r.courtyard;
  rules.min_fill = r.min_fill;
  rules.max_fill = r.max_fill;
  rules.yaw_steps = r.yaw_steps;
  rules.collapse_q = to_q(r.collapse);
  rules.breach_q = to_q(r.breach);
  rules.windward_q = to_q(r.windward);
  rules.collapse_min_q = to_q(r.collapse_min);
  rules.collapse_max_q = to_q(r.collapse_max);
  rules.breach_min_cm = to_cm(r.breach_min);
  rules.breach_max_cm = to_cm(r.breach_max);
  rules.corner_drop_q = to_q(r.corner_drop);
  rules.corner_reach_cm = to_cm(r.corner_reach);
  rules.min_height_q = to_q(r.min_height);
  rules.max_sink_q = to_q(r.max_sink);
  rules.opening_q = to_q(r.opening);
  rules.debris_per_module_q = to_q(r.debris_per_module);
  rules.debris_spread_cm = to_cm(r.debris_spread);
  rules.debris_outward_q = to_q(r.debris_outward);
  rules.drift_windward_cm = to_cm(r.drift_windward);
  rules.drift_lee_cm = to_cm(r.drift_lee);
  rules.drift_reach_cm = to_cm(r.drift_reach);
  rules.embed_cm = to_cm(r.embed);
  const u32 shape_total =
      r.rectangle + r.l_shape + r.u_shape + r.courtyard;  // u32: four small weights
  if (shape_total == 0) {
    error = "rules: every shape weight is zero";
    return false;
  }
  if (rules.yaw_steps == 0 || 16 % rules.yaw_steps != 0) {
    error = "rules: yaw_steps must be 1, 2, 4, 8 or 16";
    return false;
  }
  if (rules.min_fill > rules.max_fill || rules.max_fill > 256) {
    error = "rules: min_fill must not exceed max_fill, and max_fill is at most 256 modules";
    return false;
  }
  auto fraction = [&](i32 q, const char* name) {
    if (q < 0 || q > k_q_one) {
      error = std::string("rules: ") + name + " is a fraction, 0 to 1";
      return false;
    }
    return true;
  };
  if (!fraction(rules.collapse_q, "collapse") || !fraction(rules.breach_q, "breach") ||
      !fraction(rules.windward_q, "windward") || !fraction(rules.collapse_min_q, "collapse_min") ||
      !fraction(rules.collapse_max_q, "collapse_max") ||
      !fraction(rules.corner_drop_q, "corner_drop") ||
      !fraction(rules.min_height_q, "min_height") || !fraction(rules.max_sink_q, "max_sink") ||
      !fraction(rules.opening_q, "opening") ||
      !fraction(rules.debris_outward_q, "debris_outward")) {
    return false;
  }
  if (rules.collapse_min_q > rules.collapse_max_q || rules.breach_min_cm > rules.breach_max_cm ||
      rules.breach_min_cm < 0 || rules.corner_reach_cm < 0 || rules.debris_spread_cm < 0 ||
      rules.debris_per_module_q < 0 || rules.drift_reach_cm < 0 || rules.drift_windward_cm < 0 ||
      rules.drift_lee_cm < 0 || rules.embed_cm < 0) {
    error = "rules: a minimum above its maximum, or a negative length or rate";
    return false;
  }
  if (rules.debris_per_module_q > 64 * k_q_one || rules.debris_spread_cm > 10000) {
    // A fully fallen module of ashlar is some thirty blocks: sixty-four lying beside it is more
    // rubble than the wall had, and a bound keeps a typo from laying millions of pieces.
    error = "rules: debris_per_module is at most 64 blocks, and debris_spread at most 100 m";
    return false;
  }

  // ---- the members ------------------------------------------------------------------------------
  const std::string base(dir);
  out.members.reserve(file.members.size());
  i32 largest_radius = 0;
  for (u32 i = 0; i < file.members.size(); ++i) {
    const scene::RuinMember& m = file.members[i];
    const std::string where = member_where(m, i);
    Member member;
    member.name = m.name;
    member.kind = static_cast<PieceKind>(static_cast<u8>(m.kind));
    if (static_cast<u32>(member.kind) >= k_piece_kinds) {
      error = where + " has an unknown kind";
      return false;
    }
    if (m.mesh.empty()) {
      error = where + " names no mesh";
      return false;
    }
    member.mesh =
        io::is_absolute_path(m.mesh) || base.empty() ? m.mesh : io::join_path(base, m.mesh);
    if (!m.hash.empty() && !parse_hex16(m.hash, member.hash)) {
      error = where + ": hash '" + m.hash + "' is not 16 lower-case hex digits";
      return false;
    }
    member.offset = m.offset;
    const i32 yaw_centi = static_cast<i32>(std::floor(static_cast<f64>(m.yaw_deg) * 100.0 + 0.5));
    if (yaw_centi % 2250 != 0) {
      error = where + ": yaw_deg must be a multiple of 22.5";
      return false;
    }
    member.yaw_step = static_cast<u8>(((yaw_centi / 2250) % 16 + 16) % 16);
    member.height_cm = to_cm(m.height);
    member.weight = m.weight;
    member.radius_cm = to_cm(m.radius);
    if (member.height_cm <= 0 && member.kind != PieceKind::debris) {
      error = where + " has no height";
      return false;
    }
    if (!read_sockets(m, i, member, error)) return false;
    const bool straight = member.kind == PieceKind::section || member.kind == PieceKind::doorway ||
                          member.kind == PieceKind::window;
    if (straight && member.length_cm % out.module_cm != 0) {
      error = where + " is " + std::to_string(member.length_cm) +
              " cm between its sockets, which is not a whole number of the kit's " +
              std::to_string(out.module_cm) + " cm module";
      return false;
    }
    if (member.kind == PieceKind::doorway || member.kind == PieceKind::window) {
      member.opening_start_cm = to_cm(m.opening_start);
      member.opening_end_cm = to_cm(m.opening_end);
      member.sill_cm = to_cm(m.sill);
      member.head_cm = to_cm(m.head);
      if (member.opening_start_cm < 0 || member.opening_end_cm <= member.opening_start_cm ||
          member.opening_end_cm > member.length_cm || member.head_cm <= member.sill_cm ||
          member.head_cm > member.height_cm) {
        error = where +
                ": the opening must lie inside the member, with its head above its sill "
                "and below the member's top";
        return false;
      }
    }
    if (member.kind == PieceKind::debris) {
      if (member.radius_cm <= 0 || member.radius_cm > 1000) {
        error = where + " is debris with no radius, or one over 10 m";
        return false;
      }
      if (!(m.volume >= 0.0f) || m.volume > 10.0f) {
        error = where + ": volume is cubic metres, 0 (estimated) to 10";
        return false;
      }
      largest_radius = std::max(largest_radius, member.radius_cm);
      // What a heap counts it as: the kit's measured volume, or the square prism of its height
      // inscribed in its radius — an estimate on the large side, so a kit that does not say puts
      // down fewer pieces rather than more.
      const i64 volume = static_cast<i64>(
          std::floor(static_cast<f64>(m.volume) * 1.0e6 + 0.5));  // m^3 to cm^3, in double
      const i64 h = member.height_cm > 0 ? member.height_cm : member.radius_cm;
      member.volume_cm3 = static_cast<i32>(std::clamp<i64>(
          volume > 0 ? volume : 2 * i64{member.radius_cm} * member.radius_cm * h, 1, i64{1} << 30));
    }
    // Members that draw the same file draw the same mesh.
    u32 mesh_index = out.meshes.size();
    for (u32 k = 0; k < out.meshes.size(); ++k) {
      if (out.meshes[k] == member.mesh) {
        mesh_index = k;
        break;
      }
    }
    if (mesh_index == out.meshes.size()) {
      out.meshes.push_back(member.mesh);
      out.mesh_hashes.push_back(member.hash);
    }
    member.mesh_index = mesh_index;
    out.by_kind[static_cast<u32>(member.kind)].push_back(i);
    out.members.push_back(std::move(member));
  }

  // ---- what the grammar needs, derived once -----------------------------------------------------
  const Vector<u32>& corners = out.by_kind[static_cast<u32>(PieceKind::corner)];
  if (corners.empty()) {
    error = "the kit has no corner: every join of a footprint is one";
    return false;
  }
  out.corner_in_cm = out.members[corners[0]].arm_in_cm;
  out.corner_out_cm = out.members[corners[0]].arm_out_cm;
  for (const u32 c : corners) {
    if (out.members[c].arm_in_cm != out.corner_in_cm ||
        out.members[c].arm_out_cm != out.corner_out_cm) {
      error =
          "the kit's outside corners have different arms; its height variants must join "
          "their walls at the same sockets";
      return false;
    }
  }
  const Vector<u32>& inside = out.by_kind[static_cast<u32>(PieceKind::inside_corner)];
  out.has_inside_corner = !inside.empty();
  if (out.has_inside_corner) {
    out.reflex_in_cm = out.members[inside[0]].arm_in_cm;
    out.reflex_out_cm = out.members[inside[0]].arm_out_cm;
    for (const u32 c : inside) {
      if (out.members[c].arm_in_cm != out.reflex_in_cm ||
          out.members[c].arm_out_cm != out.reflex_out_cm) {
        error = "the kit's inside corners have different arms";
        return false;
      }
    }
  } else {
    // The outside corner turned half round: its outgoing arm now points back along the incoming
    // wall and its incoming arm along the outgoing one, so the two swap.
    out.reflex_in_cm = out.corner_out_cm;
    out.reflex_out_cm = out.corner_in_cm;
  }
  const Vector<u32>& sections = out.by_kind[static_cast<u32>(PieceKind::section)];
  if (sections.empty()) {
    error = "the kit has no straight section";
    return false;
  }
  for (const u32 s : sections) {
    const u32 modules = static_cast<u32>(out.members[s].length_cm / out.module_cm);
    bool known = false;
    for (const u32 have : out.section_modules)
      known = known || have == modules;
    if (!known) out.section_modules.push_back(modules);
  }
  std::sort(out.section_modules.begin(), out.section_modules.end());
  // Which fills sections alone can make: an unbounded knapsack over the module lengths, up to the
  // longest wall a building could ask for.
  out.max_fill_modules = std::max(rules.max_fill * 4u + 16u, 64u);
  out.fillable.assign(out.max_fill_modules + 1, u8{0});
  out.fillable[0] = 1;
  for (u32 n = 1; n <= out.max_fill_modules; ++n) {
    for (const u32 len : out.section_modules) {
      if (len <= n && out.fillable[n - len] != 0) {
        out.fillable[n] = 1;
        break;
      }
    }
  }
  // The rubble rule's unit: the profile's block, and the radius every piece laid on a site stays
  // within — the block's half diagonal in plan, or the largest debris piece's if that is more.
  out.rubble_cm3 = out.block_length_cm * out.course_height_cm * out.thickness_cm;
  const i32 block_radius =
      static_cast<i32>((grid::isqrt_ceil(i64{out.block_length_cm} * out.block_length_cm +
                                         i64{out.thickness_cm} * out.thickness_cm) +
                        1) /
                       2);
  out.rubble_radius_cm = std::max(block_radius, largest_radius);
  out.margin_cm =
      out.thickness_cm / 2 +
      std::max(rules.debris_spread_cm + 2 * out.rubble_radius_cm + 2, rules.drift_reach_cm);
  return true;
}

bool read_kit_file(const std::string& path, Kit& out, std::string& error) {
  std::string text;
  const io::Status status = io::read_file(path, text);
  if (status != io::Status::Ok) {
    error = "cannot read " + path + ": " + io::status_name(status);
    return false;
  }
  JsonValue root;
  const JsonParseResult parsed = parse_json(text, root);
  if (!parsed.ok) {
    error = path + ":" + std::to_string(parsed.line) + ": " + parsed.message;
    return false;
  }
  scene::RuinKit file;
  schema::ReadContext ctx;
  if (!schema::from_json(file, root, ctx) || !ctx.ok()) {
    std::string text_errors;
    for (const schema::Diagnostic& d : ctx.diagnostics) {
      if (!text_errors.empty()) text_errors += "; ";
      text_errors += d.path.empty() ? d.message : d.path + ": " + d.message;
    }
    error = path + ": " + (text_errors.empty() ? std::string("not a ruin kit") : text_errors);
    return false;
  }
  if (!kit_from_schema(file, io::parent_path(path), out, error)) {
    error = path + ": " + error;
    return false;
  }
  out.path = path;
  return true;
}

}  // namespace engine::ruins
