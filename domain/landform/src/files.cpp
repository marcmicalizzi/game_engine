// The landform's files (docs/subsystems/landform.md, "The field file"): the sketch read, a
// starting heightfield read, the solved heightfield and its fields written, a field's window read
// back, and the shaded relief a person looks at.
#include <core/hash/hash.h>
#include <core/json/json.h>
#include <core/schema/json_reflect.h>
#include <domain/landform/landform.h>
#include <foundation/io/vfs.h>

#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace engine::landform {

namespace {

constexpr std::string_view k_heightfield_format = "engine.terrain.heightfield";

std::string hex16(u64 v) {
  char text[17];
  std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(v));
  return std::string(text);
}

bool parse_hex16(std::string_view text, u64& out) noexcept {
  if (text.size() != 16) return false;
  u64 v = 0;
  for (const char c : text) {
    u64 d = 0;
    if (c >= '0' && c <= '9') {
      d = static_cast<u64>(c - '0');
    } else if (c >= 'a' && c <= 'f') {
      d = static_cast<u64>(c - 'a' + 10);
    } else {
      return false;
    }
    v = (v << 4) | d;
  }
  out = v;
  return true;
}

std::string beside(const std::string& path, const std::string& name) {
  const std::string dir(io::parent_path(path));
  return io::is_absolute_path(name) || dir.empty() ? name : io::join_path(dir, name);
}

template <class T>
bool read_typed(const std::string& path, T& out, const char* what, std::string& error) {
  std::string text;
  if (io::read_file(path, text) != io::Status::Ok) {
    error = "'" + path + "' could not be read";
    return false;
  }
  JsonValue json;
  const auto parsed = parse_json(text, json);
  if (!parsed.ok) {
    error = path + ":" + std::to_string(parsed.line) + ": " + parsed.message;
    return false;
  }
  schema::ReadContext ctx;
  if (!schema::from_json(out, json, ctx) || !ctx.ok()) {
    error = "'" + path + "' is not " + what + ":";
    for (const auto& d : ctx.diagnostics)
      error += " " + d.path + ": " + d.message + ";";
    return false;
  }
  return true;
}

template <class T>
bool write_grid(const std::string& path, const Vector<T>& grid, std::string& hash,
                std::string& error) {
  const std::string_view bytes(reinterpret_cast<const char*>(grid.data()),
                               static_cast<usize>(grid.size()) * sizeof(T));
  if (io::write_file_atomic(path, bytes) != io::Status::Ok) {
    error = "'" + path + "' could not be written";
    return false;
  }
  hash = hex16(hash_bytes(bytes.data(), bytes.size()));
  return true;
}

bool write_text(const std::string& path, const JsonValue& json, std::string& error) {
  std::string text = write_json(json);
  text += "\n";
  if (io::write_file_atomic(path, text) != io::Status::Ok) {
    error = "'" + path + "' could not be written";
    return false;
  }
  return true;
}

u32 type_bytes(std::string_view type) noexcept {
  if (type == "u8") return 1;
  if (type == "u32" || type == "i32") return 4;
  return 0;
}

u8 to_byte(f64 v) noexcept {
  const f64 r = std::floor(v + 0.5);
  return r <= 0.0 ? u8{0} : (r >= 255.0 ? u8{255} : static_cast<u8>(r));
}

}  // namespace

bool read_sketch(const std::string& json_path, Sketch& out, std::string& start_path,
                 std::string& error) {
  out = Sketch{};
  if (!read_typed(json_path, out, "a landform sketch", error)) return false;
  if (!validate_sketch(out, error)) {
    error = "'" + json_path + "': " + error;
    return false;
  }
  start_path = out.start ? beside(json_path, *out.start) : std::string();
  return true;
}

bool read_start_field(const std::string& json_path, StartField& out, std::string& error) {
  TerrainHeightfield header;
  if (!read_typed(json_path, header, "a heightfield header", error)) return false;
  if (header.format != k_heightfield_format || header.version != 1) {
    error = "'" + json_path + "' is format '" + header.format + "' version " +
            std::to_string(header.version) + "; a landform starts from " +
            std::string(k_heightfield_format) + " version 1";
    return false;
  }
  if (header.width < 2 || header.height < 2 || header.width > 65536 || header.height > 65536 ||
      header.spacing_mm < 1 || !(header.vertical_mm_per_unit > 0.0) || header.data.empty()) {
    error = "'" + json_path +
            "' is not a grid of 2 to 65,536 points a side with a spacing, a "
            "vertical unit and a data file";
    return false;
  }
  std::string grid;
  const std::string data_path = beside(json_path, header.data);
  if (io::read_file(data_path, grid) != io::Status::Ok) {
    error = "'" + json_path + "': its grid '" + data_path + "' could not be read";
    return false;
  }
  const u64 want = u64{header.width} * header.height * sizeof(f32);
  u64 recorded = 0;
  if (grid.size() != want || !parse_hex16(header.data_hash, recorded) ||
      recorded != hash_bytes(grid.data(), grid.size())) {
    error = "'" + json_path + "': its grid is not width times height float32 hashing to data_hash";
    return false;
  }
  out = StartField{};
  out.width = header.width;
  out.height = header.height;
  out.spacing_mm = header.spacing_mm;
  out.origin_x_mm = header.origin_mm[0];
  out.origin_z_mm = header.origin_mm[1];
  out.vertical_mm_per_unit = header.vertical_mm_per_unit;
  out.data.resize(header.width * header.height);
  std::memcpy(out.data.data(), grid.data(), grid.size());
  return true;
}

bool write_landform(const Landform& land, const Sketch& sketch, const std::string& dir,
                    std::string_view name, WrittenFiles& out, std::string& error) {
  const u32 n = land.width * land.height;
  if (n == 0 || land.height_mm.size() != n || land.drainage_area.size() != n ||
      land.receiver.size() != n || land.slope.size() != n || land.erosion_rate.size() != n ||
      land.fill.size() != n || land.sediment.size() != n) {
    error = "the landform is not solved";
    return false;
  }
  if (!dir.empty()) (void)io::make_directories(dir);
  const std::string stem(name);
  const auto in_dir = [&dir](const std::string& file) {
    return dir.empty() ? file : io::join_path(dir, file);
  };
  // The heightfield: each float one whole millimetre (`vertical_mm_per_unit` 1), which a float32
  // holds exactly below 2^24 and the tile set's build turns back into the same integer.
  Vector<f32> grid(n);
  for (u32 i = 0; i < n; ++i)
    grid[i] = static_cast<f32>(land.height_mm[i]);
  TerrainHeightfield header;
  header.format = std::string(k_heightfield_format);
  header.version = 1;
  header.width = land.width;
  header.height = land.height;
  header.spacing_mm = land.spacing_mm;
  header.origin_mm = {land.origin_x_mm, land.origin_z_mm};
  header.vertical_mm_per_unit = 1.0;
  header.data = stem + ".f32";
  if (!write_grid(in_dir(header.data), grid, header.data_hash, error)) return false;
  out.heightfield = in_dir(stem + ".json");
  if (!write_text(out.heightfield, schema::to_json(header), error)) return false;

  Fields fields;
  fields.format = std::string(k_fields_format);
  fields.version = k_fields_version;
  fields.width = land.width;
  fields.height = land.height;
  fields.spacing_mm = land.spacing_mm;
  fields.origin_mm = {land.origin_x_mm, land.origin_z_mm};
  if (land.has_sea) fields.sea_level_mm = land.sea_level_mm;
  fields.heightfield = stem + ".json";
  const std::string sketch_text = write_json(schema::to_json(sketch));
  fields.sketch_hash = hex16(hash_bytes(sketch_text.data(), sketch_text.size()));
  fields.solver_version = k_solver_version;
  fields.route_seed = land.route_seed;
  fields.steps = land.steps;
  fields.step_yr = land.step_yr;
  const auto add = [&](const char* field, const char* type, const char* unit, const auto& data) {
    FieldEntry entry;
    entry.name = field;
    entry.file = stem + "." + field + "." + type;
    entry.type = type;
    entry.unit = unit;
    if (!write_grid(in_dir(entry.file), data, entry.hash, error)) return false;
    fields.fields.push_back(entry);
    return true;
  };
  if (!add("drainage_area", "u32", "cells draining through the point, itself included",
           land.drainage_area) ||
      !add("receiver", "u8", "neighbour code 0 to 7 from east clockwise with +z down, 255 held",
           land.receiver) ||
      !add("slope", "i32", "fall to the receiver over the distance to it, parts per million",
           land.slope) ||
      !add("erosion_rate", "i32", "micrometres a year removed by the last step",
           land.erosion_rate) ||
      !add("fill", "u32", "millimetres under the depression's spill", land.fill) ||
      !add("sediment", "u32", "millimetres of sediment under the point", land.sediment))
    return false;
  out.fields = in_dir(stem + ".fields.json");
  return write_text(out.fields, schema::to_json(fields), error);
}

bool read_fields(const std::string& json_path, Fields& out, std::string& error) {
  out = Fields{};
  if (!read_typed(json_path, out, "a landform field file", error)) return false;
  if (out.format != k_fields_format || out.version != k_fields_version) {
    error = "'" + json_path + "' is format '" + out.format + "' version " +
            std::to_string(out.version) + "; this build reads " + std::string(k_fields_format) +
            " version " + std::to_string(k_fields_version);
    return false;
  }
  if (out.width == 0 || out.height == 0) {
    error = "'" + json_path + "' has no lattice";
    return false;
  }
  for (u32 i = 0; i < out.fields.size(); ++i)
    if (type_bytes(out.fields[i].type) == 0) {
      error = "'" + json_path + "': field '" + out.fields[i].name + "' is of type '" +
              out.fields[i].type + "'; a field is u8, u32 or i32";
      return false;
    }
  return true;
}

bool read_field_window(const std::string& json_path, const Fields& header, std::string_view field,
                       u32 x0, u32 z0, u32 w, u32 h, void* out, usize out_bytes,
                       std::string& error) {
  const FieldEntry* entry = nullptr;
  for (u32 i = 0; i < header.fields.size(); ++i)
    if (header.fields[i].name == field) entry = &header.fields[i];
  if (entry == nullptr) {
    error = "the field file has no field '" + std::string(field) + "'";
    return false;
  }
  const u64 size = type_bytes(entry->type);
  if (w == 0 || h == 0 || u64{x0} + w > header.width || u64{z0} + h > header.height) {
    error =
        "a window of a landform's fields lies inside its lattice: the fields hold nothing "
        "past the region's edge";
    return false;
  }
  if (out_bytes != u64{w} * h * size) {
    error = "the window's buffer is not width times height of the field's type";
    return false;
  }
  const std::string path = beside(json_path, entry->file);
  // A window as wide as the lattice is one run of the file; any other is a run a row.
  const u32 runs = w == header.width ? 1 : h;
  const u64 run_bytes = out_bytes / runs;
  for (u32 r = 0; r < runs; ++r) {
    const u64 offset = (u64{z0 + r} * header.width + x0) * size;
    u64 got = 0;
    if (io::read_file_range(path, offset, static_cast<char*>(out) + r * run_bytes, run_bytes,
                            got) != io::Status::Ok ||
        got != run_bytes) {
      error = "'" + path + "' is shorter than the lattice its header states";
      return false;
    }
  }
  return true;
}

void shaded_relief(const Landform& land, bool drainage, Vector<u8>& rgba) {
  const u32 w = land.width;
  const u32 h = land.height;
  rgba.resize(w * h * 4);
  if (w < 2 || h < 2) return;
  const i32* mm = land.height_mm.data();
  const f64 run = 2.0 * static_cast<f64>(land.spacing_mm);
  // Toward the light: north-west (-x, -z), 45 degrees up.
  const f64 lx = -0.5, ly = 0.70710678118654757, lz = -0.5;
  for (u32 j = 0; j < h; ++j) {
    for (u32 i = 0; i < w; ++i) {
      const u32 p = j * w + i;
      const u32 xa = i > 0 ? p - 1 : p;
      const u32 xb = i + 1 < w ? p + 1 : p;
      const u32 za = j > 0 ? p - w : p;
      const u32 zb = j + 1 < h ? p + w : p;
      // One-sided at an edge: half the run, so the difference is doubled.
      const f64 gx = static_cast<f64>(mm[xb] - mm[xa]) / run * (xa == p || xb == p ? 2.0 : 1.0);
      const f64 gz = static_cast<f64>(mm[zb] - mm[za]) / run * (za == p || zb == p ? 2.0 : 1.0);
      const f64 lambert = (-gx * lx + ly - gz * lz) / std::sqrt(1.0 + gx * gx + gz * gz);
      const f64 shade = 0.2 + 0.8 * (lambert > 0.0 ? lambert : 0.0);
      f64 r = 255.0 * shade;
      f64 g = r;
      f64 b = r;
      if (land.has_sea && mm[p] <= land.sea_level_mm) {
        r *= 0.45;
        g *= 0.62;
        b = b * 0.75 + 60.0;
      } else if (drainage) {
        // Sediment as sand under the channels: faint from half a metre, full from ten.
        if (land.sediment.size() == w * h && land.sediment[p] >= 500) {
          f64 t = 0.25 + 0.75 * static_cast<f64>(land.sediment[p]) / 10000.0;
          t = t > 1.0 ? 1.0 : t;
          r += (r * 1.0 - r) * t;
          g += (g * 0.82 - g) * t;
          b += (b * 0.45 - b) * t;
        }
        // A channel by the bits of its area: faint from 64 cells, full from 65,536.
        const i32 bits = static_cast<i32>(std::bit_width(land.drainage_area[p]));
        f64 t = static_cast<f64>(bits - 6) / 10.0;
        t = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
        r += (25.0 - r) * t;
        g += (70.0 - g) * t;
        b += (150.0 - b) * t;
      }
      u8* px = rgba.data() + p * 4;
      px[0] = to_byte(r);
      px[1] = to_byte(g);
      px[2] = to_byte(b);
      px[3] = 255;
    }
  }
}

}  // namespace engine::landform
