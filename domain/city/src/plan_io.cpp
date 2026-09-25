// The plan as a derived node: its key, its cache directory, and its file (docs/subsystems/city.md,
// "The plan as a derived node"). The file holds the parameters, the coastline, the arterial grid
// and the plan's records in integer centimetres; the street graph and the tile index are rebuilt
// when it is read, by the functions that built them.
#include "plan_internal.h"

#include <core/json/json.h>
#include <core/schema/json_reflect.h>
#include <domain/city/plan.h>
#include <foundation/io/vfs.h>

#include <charconv>
#include <cstdio>

namespace engine::city {

namespace {

constexpr const char* k_plan_format = "engine.city-plan.v1";

std::string hex16(u64 value) {
  char text[17];
  std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(value));
  return text;
}

bool parse_hex16(const std::string& text, u64& out) {
  const auto r = std::from_chars(text.data(), text.data() + text.size(), out, 16);
  return r.ec == std::errc() && r.ptr == text.data() + text.size() && text.size() == 16;
}

void put_rect(const Rect& r, std::array<i32, 4>& out) { out = {r.x0, r.z0, r.x1, r.z1}; }
Rect get_rect(const std::array<i32, 4>& a) { return Rect{a[0], a[1], a[2], a[3]}; }

bool write_text(const std::string& path, const std::string& text, std::string* error) {
  const std::string_view dir = io::parent_path(path);
  if (!dir.empty() && io::make_directories(dir) != io::Status::Ok) {
    if (error != nullptr) *error = "cannot create " + std::string(dir);
    return false;
  }
  const io::Status status = io::write_file(path, text);
  if (status != io::Status::Ok) {
    if (error != nullptr) *error = "cannot write " + path + ": " + io::status_name(status);
    return false;
  }
  return true;
}

}  // namespace

u64 plan_key(const IslandParams& params) {
  // The canonical JSON of the parameters as the plan read them: defaults filled in, so two files
  // that differ only in spelling a default out are one key.
  const std::string text = write_json(schema::to_json(params));
  return hash_combine(hash_bytes(text.data(), text.size()), k_generator_version);
}

std::string plan_cache_dir(std::string_view ddc_root, u64 key) {
  return io::join_path(io::join_path(ddc_root, "city"), hex16(key));
}

void plan_to_schema(const Plan& plan, PlanFile& out) {
  out = PlanFile{};
  out.format = k_plan_format;
  out.generator = k_generator_version;
  out.key = hex16(plan.key);
  out.hash = hex16(hash_plan(plan));
  out.params = plan.params.source;
  out.coastline = plan.coastline;
  out.arterials_x = plan.arterials_x;
  out.arterials_z = plan.arterials_z;
  out.mountain_axis = {plan.mountain_ux, plan.mountain_uz};
  out.city_limit = plan.city_limit_cm;
  out.mountain_start = plan.mountain_start_cm;
  for (const District& d : plan.districts) {
    PlanDistrict r;
    r.id = d.id;
    r.kind = d.kind;
    r.pinned = d.pinned != 0;
    r.center = {d.cx, d.cz};
    r.superblocks = d.superblocks;
    r.area = static_cast<u64>(d.area_cm2);
    out.districts.push_back(r);
  }
  for (const Street& s : plan.streets) {
    PlanStreet r;
    r.id = s.id;
    r.street_class = s.cls;
    r.pinned = s.pinned != 0;
    r.along_x = s.along_x != 0;
    r.line = s.line_cm;
    r.from = s.from_cm;
    r.to = s.to_cm;
    r.width = s.width_cm;
    r.superblock = s.superblock;
    out.streets.push_back(r);
  }
  for (const Block& b : plan.blocks) {
    PlanBlock r;
    r.id = b.id;
    r.district = b.district;
    r.superblock = b.superblock;
    put_rect(b.rect, r.rect);
    put_rect(b.buildable, r.buildable);
    r.park = b.park;
    r.civic = b.civic;
    for (u32 k = 0; k < 4; ++k) {
      const u32 s = plan.find_street(b.streets[k]);
      r.streets[k] = s != k_no_id ? b.streets[k] : k_no_id;
    }
    out.blocks.push_back(r);
  }
  for (const Lot& l : plan.lots) {
    PlanLot r;
    r.id = l.id;
    r.block = plan.blocks[l.block].id;
    r.district = l.district;
    put_rect(l.rect, r.rect);
    r.front = l.front;
    r.street = l.street != k_no_id ? plan.streets[l.street].id : k_no_id;
    r.use = l.use;
    r.archetype = l.archetype;
    r.park = l.park;
    r.civic = l.civic;
    out.lots.push_back(r);
  }
  for (const Park& park : plan.parks) {
    PlanPark r;
    r.kind = park.kind;
    r.block = plan.blocks[park.block].id;
    r.lot = park.lot != k_no_id ? plan.lots[park.lot].id : k_no_id;
    r.district = park.district;
    put_rect(park.rect, r.rect);
    out.parks.push_back(r);
  }
}

bool plan_from_schema(const PlanFile& file, Plan& out, std::string& error) {
  if (file.format != k_plan_format) {
    error = "format is '" + file.format + "', not " + k_plan_format;
    return false;
  }
  if (file.generator != k_generator_version) {
    error = "the plan was made by generator version " + std::to_string(file.generator) +
            " and this build is version " + std::to_string(k_generator_version) +
            ": make it again from its parameters";
    return false;
  }
  out = Plan{};
  if (!params_from_schema(file.params, out.params, error)) return false;
  out.key = plan_key(out.params.source);
  u64 stored_key = 0;
  if (!parse_hex16(file.key, stored_key) || stored_key != out.key) {
    error = "the plan's key does not match its parameters";
    return false;
  }
  out.coastline = file.coastline;
  out.arterials_x = file.arterials_x;
  out.arterials_z = file.arterials_z;
  out.mountain_ux = file.mountain_axis[0];
  out.mountain_uz = file.mountain_axis[1];
  out.city_limit_cm = file.city_limit;
  out.mountain_start_cm = file.mountain_start;
  for (const PlanDistrict& r : file.districts) {
    District d;
    d.id = r.id;
    d.kind = r.kind;
    d.pinned = r.pinned ? 1 : 0;
    d.cx = r.center[0];
    d.cz = r.center[1];
    d.superblocks = static_cast<u16>(r.superblocks);
    d.area_cm2 = static_cast<i64>(r.area);
    out.districts.push_back(d);
  }
  for (const PlanStreet& r : file.streets) {
    Street s;
    s.id = r.id;
    s.cls = r.street_class;
    s.pinned = r.pinned ? 1 : 0;
    s.along_x = r.along_x ? 1 : 0;
    s.line_cm = r.line;
    s.from_cm = r.from;
    s.to_cm = r.to;
    s.width_cm = r.width;
    s.superblock = r.superblock;
    s.interior = r.superblock != k_no_id && (r.id & 1023u) >= 512 ? 1 : 0;
    out.streets.push_back(s);
  }
  for (const PlanBlock& r : file.blocks) {
    Block b;
    b.id = r.id;
    b.district = r.district;
    b.superblock = r.superblock;
    b.rect = get_rect(r.rect);
    b.buildable = get_rect(r.buildable);
    b.park = r.park;
    b.civic = r.civic;
    for (u32 k = 0; k < 4; ++k)
      b.streets[k] = r.streets[k];
    out.blocks.push_back(b);
  }
  for (const PlanLot& r : file.lots) {
    Lot l;
    l.id = r.id;
    l.block = out.find_block(r.block);
    l.district = static_cast<u16>(r.district);
    l.rect = get_rect(r.rect);
    l.front = r.front;
    l.street = r.street != k_no_id ? out.find_street(r.street) : k_no_id;
    l.use = r.use;
    l.archetype = r.archetype;
    l.park = r.park;
    l.civic = r.civic;
    if (l.block == k_no_id) {
      error = "lot " + std::to_string(r.id) + " names a block the plan does not have";
      return false;
    }
    out.lots.push_back(l);
  }
  for (u32 l = 0; l < out.lots.size(); ++l) {
    Block& b = out.blocks[out.lots[l].block];
    if (b.lot_count == 0) b.first_lot = l;
    ++b.lot_count;
  }
  for (const PlanPark& r : file.parks) {
    Park park;
    park.kind = r.kind;
    park.block = out.find_block(r.block);
    park.lot = r.lot != k_no_id ? out.find_lot(r.lot) : k_no_id;
    park.district = r.district;
    park.rect = get_rect(r.rect);
    out.parks.push_back(park);
  }
  build_graph(out);
  build_tile_index(out);
  u64 stored_hash = 0;
  if (!parse_hex16(file.hash, stored_hash) || stored_hash != hash_plan(out)) {
    error = "the plan's records do not hash to the hash it carries";
    return false;
  }
  return true;
}

bool write_plan_file(const std::string& path, const Plan& plan, std::string* error) {
  PlanFile file;
  plan_to_schema(plan, file);
  return write_text(path, write_json(schema::to_json(file)) + "\n", error);
}

bool read_plan_file(const std::string& path, Plan& out, std::string& error) {
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
  PlanFile file;
  schema::ReadContext ctx;
  if (!schema::from_json(file, root, ctx) || !ctx.ok()) {
    std::string text_errors;
    for (const schema::Diagnostic& d : ctx.diagnostics) {
      if (!text_errors.empty()) text_errors += "; ";
      text_errors += d.path.empty() ? d.message : d.path + ": " + d.message;
    }
    error = path + ": " + (text_errors.empty() ? std::string("not a city plan") : text_errors);
    return false;
  }
  if (!plan_from_schema(file, out, error)) {
    error = path + ": " + error;
    return false;
  }
  return true;
}

}  // namespace engine::city
