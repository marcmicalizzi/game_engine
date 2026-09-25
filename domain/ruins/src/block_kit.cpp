#include "grid.h"

#include <core/json/json.h>
#include <core/schema/json_reflect.h>
#include <domain/ruins/blocks.h>
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

std::string block_where(const scene::RuinBlock& b, u32 i) {
  return "block " + std::to_string(i) + (b.name.empty() ? std::string() : " (" + b.name + ")");
}

// The groups of `role`, by length ascending (ties by first appearance).
void groups_of(const BlockKit& kit, BlockRole role, Vector<u32>& out) {
  out.clear();
  for (u32 g = 0; g < kit.groups.size(); ++g)
    if (kit.groups[g].role == role) out.push_back(g);
  std::stable_sort(out.begin(), out.end(),
                   [&](u32 a, u32 b) { return kit.groups[a].length_cm < kit.groups[b].length_cm; });
}

}  // namespace

const char* block_role_name(BlockRole role) noexcept {
  switch (role) {
    case BlockRole::stretcher: return "stretcher";
    case BlockRole::half: return "half";
    case BlockRole::quoin: return "quoin";
    case BlockRole::lintel: return "lintel";
    case BlockRole::sill: return "sill";
  }
  return "unknown";
}

bool block_kit_from_schema(const scene::RuinBlockKit& file, std::string_view dir, BlockKit& out,
                           std::string& error) {
  out = BlockKit{};
  if (!file.format.empty() && file.format != "engine.ruin-block-kit.v1") {
    error = "format is '" + file.format + "', not engine.ruin-block-kit.v1";
    return false;
  }
  if (!file.bond.empty() && file.bond != "running") {
    error = "bond is '" + file.bond + "'; running bond is the one there is";
    return false;
  }
  out.name = file.name;
  out.profile = file.profile;
  out.course_cm = to_cm(file.course_height);
  out.thickness_cm = to_cm(file.thickness);
  if (out.course_cm < 5 || out.thickness_cm < 5) {
    error = "course_height and thickness must be at least 0.05 m";
    return false;
  }

  // ---- the rules, once, in the layer's units ----------------------------------------------------
  const scene::RuinBlockRules& r = file.rules;
  BlockRules& rules = out.rules;
  rules.top_drop_q = to_q(r.top_drop);
  rules.eroded_exposed_q = to_q(r.eroded_exposed);
  rules.eroded_q = to_q(r.eroded);
  rules.support_q = to_q(r.support);
  rules.lintel_bearing_cm = to_cm(r.lintel_bearing);
  rules.debris_kept_q = to_q(r.debris_kept);
  const i32 fractions[] = {rules.top_drop_q, rules.eroded_exposed_q, rules.eroded_q,
                           rules.support_q, rules.debris_kept_q};
  for (const i32 q : fractions) {
    if (q < 0 || q > k_q_one) {
      error =
          "rules: top_drop, eroded_exposed, eroded, support and debris_kept are fractions, 0 to 1";
      return false;
    }
  }
  if (rules.lintel_bearing_cm < 0) {
    error = "rules: lintel_bearing is a length and not negative";
    return false;
  }

  // ---- the blocks -------------------------------------------------------------------------------
  const std::string base(dir);
  out.blocks.reserve(file.blocks.size());
  for (u32 i = 0; i < file.blocks.size(); ++i) {
    const scene::RuinBlock& b = file.blocks[i];
    const std::string where = block_where(b, i);
    KitBlock block;
    block.name = b.name;
    block.role = static_cast<BlockRole>(static_cast<u8>(b.role));
    if (static_cast<u32>(block.role) >= k_block_roles) {
      error = where + " has an unknown role";
      return false;
    }
    if (b.mesh.empty()) {
      error = where + " names no mesh";
      return false;
    }
    block.mesh =
        io::is_absolute_path(b.mesh) || base.empty() ? b.mesh : io::join_path(base, b.mesh);
    if (!b.hash.empty() && !parse_hex16(b.hash, block.hash)) {
      error = where + ": hash '" + b.hash + "' is not 16 lower-case hex digits";
      return false;
    }
    block.offset = b.offset;
    const i32 yaw_centi = static_cast<i32>(std::floor(static_cast<f64>(b.yaw_deg) * 100.0 + 0.5));
    if (yaw_centi % 2250 != 0) {
      error = where + ": yaw_deg must be a multiple of 22.5";
      return false;
    }
    block.yaw_step = static_cast<u8>(((yaw_centi / 2250) % 16 + 16) % 16);
    block.length_cm = to_cm(b.length);
    block.height_cm = to_cm(b.height);
    block.depth_cm = to_cm(b.depth);
    if (block.length_cm <= 0 || block.height_cm <= 0 || block.depth_cm <= 0) {
      error = where + ": length, height and depth must be positive";
      return false;
    }
    // Lying down it reaches half its diagonal in plan past its centre: what a fallen block keeps
    // clear of the walls and the tile's edge by.
    block.radius_cm = static_cast<i32>((grid::isqrt_ceil(i64{block.length_cm} * block.length_cm +
                                                         i64{block.depth_cm} * block.depth_cm) +
                                        1) /
                                       2);
    block.eroded = b.eroded;
    block.weight = b.weight;
    // Blocks that draw the same file draw the same mesh.
    u32 mesh_index = out.meshes.size();
    for (u32 k = 0; k < out.meshes.size(); ++k) {
      if (out.meshes[k] == block.mesh) {
        mesh_index = k;
        break;
      }
    }
    if (mesh_index == out.meshes.size()) {
      out.meshes.push_back(block.mesh);
      out.mesh_hashes.push_back(block.hash);
    }
    block.mesh_index = mesh_index;
    // Its group: the blocks of one role and one length.
    u32 group = out.groups.size();
    for (u32 g = 0; g < out.groups.size(); ++g) {
      if (out.groups[g].role == block.role && out.groups[g].length_cm == block.length_cm) {
        group = g;
        break;
      }
    }
    if (group == out.groups.size()) {
      BlockKit::Group made;
      made.role = block.role;
      made.length_cm = block.length_cm;
      out.groups.push_back(std::move(made));
    }
    (block.eroded ? out.groups[group].eroded : out.groups[group].crisp).push_back(i);
    out.blocks.push_back(std::move(block));
  }

  // ---- what the layer needs, derived once -------------------------------------------------------
  Vector<u32> stretchers;
  groups_of(out, BlockRole::stretcher, stretchers);
  if (stretchers.empty()) {
    error = "the kit has no stretcher: every run of a wall is laid in them";
    return false;
  }
  // The bond's block: the stretcher length with the most weight, the longer on a tie.
  u64 best_weight = 0;
  for (const u32 g : stretchers) {
    u64 weight = 0;
    for (const u32 b : out.groups[g].crisp)
      weight += out.blocks[b].weight;
    for (const u32 b : out.groups[g].eroded)
      weight += out.blocks[b].weight;
    if (weight >= best_weight) {
      best_weight = weight;
      out.stretcher_cm = out.groups[g].length_cm;
    }
  }
  Vector<u32> halves;
  groups_of(out, BlockRole::half, halves);
  for (const u32 g : stretchers)
    out.straight.push_back(g);
  for (const u32 g : halves)
    out.straight.push_back(g);
  std::stable_sort(out.straight.begin(), out.straight.end(),
                   [&](u32 a, u32 b) { return out.groups[a].length_cm < out.groups[b].length_cm; });
  groups_of(out, BlockRole::lintel, out.lintels);
  groups_of(out, BlockRole::sill, out.sills);
  Vector<u32> quoins;
  groups_of(out, BlockRole::quoin, quoins);
  if (quoins.size() > 1) {
    error =
        "the kit's quoins are of two lengths; the two walls at a corner alternate by one quoin's "
        "length, so every quoin is the same";
    return false;
  }
  if (!quoins.empty()) {
    out.quoin_group = static_cast<i32>(quoins[0]);
    out.quoin_cm = out.groups[quoins[0]].length_cm;
    if (out.quoin_cm <= out.thickness_cm) {
      error = "the kit's quoin is " + std::to_string(out.quoin_cm) +
              " cm, no longer than the wall is thick (" + std::to_string(out.thickness_cm) +
              " cm): the courses would not alternate at the corners, which is stack bond";
      return false;
    }
  }
  return true;
}

bool read_block_kit_file(const std::string& path, BlockKit& out, std::string& error) {
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
  scene::RuinBlockKit file;
  schema::ReadContext ctx;
  if (!schema::from_json(file, root, ctx) || !ctx.ok()) {
    std::string text_errors;
    for (const schema::Diagnostic& d : ctx.diagnostics) {
      if (!text_errors.empty()) text_errors += "; ";
      text_errors += d.path.empty() ? d.message : d.path + ": " + d.message;
    }
    error = path + ": " + (text_errors.empty() ? std::string("not a block kit") : text_errors);
    return false;
  }
  if (!block_kit_from_schema(file, io::parent_path(path), out, error)) {
    error = path + ": " + error;
    return false;
  }
  out.path = path;
  return true;
}

}  // namespace engine::ruins
