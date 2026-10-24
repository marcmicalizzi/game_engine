#include <core/containers/hash_map.h>
#include <core/hash/hash.h>
#include <core/json/json.h>
#include <core/json/json_value.h>
#include <core/profiling/startup_trace.h>
#include <core/schema/json_reflect.h>
#include <core/time/time.h>
#include <domain/geometry/cluster_file.h>
#include <domain/scene_gen/scene_gen.h>
#include <domain/tileset/tileset.h>
#include <foundation/io/vfs.h>

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <limits>

namespace engine::tileset {

namespace {

// ---- small arithmetic ---------------------------------------------------------------------------

i64 floor_div(i64 a, i64 b) noexcept {
  const i64 q = a / b;
  return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}
i64 ceil_div(i64 a, i64 b) noexcept { return -floor_div(-a, b); }
i64 clamp_i64(i64 v, i64 lo, i64 hi) noexcept { return v < lo ? lo : (v > hi ? hi : v); }

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

u64 f64_bits(f64 v) noexcept {
  u64 bits = 0;
  std::memcpy(&bits, &v, sizeof(bits));
  return bits;
}

std::string diagnostics(const schema::ReadContext& ctx) {
  std::string out;
  for (const schema::Diagnostic& d : ctx.diagnostics) {
    if (!out.empty()) out += "; ";
    out += d.path + ": " + d.message;
  }
  return out;
}

bool read_json_file(const std::string& path, std::string& bytes, JsonValue& out,
                    std::string& error) {
  if (io::read_file(path, bytes) != io::Status::Ok) {
    error = "'" + path + "' could not be read";
    return false;
  }
  const JsonParseResult parsed = parse_json(bytes, out);
  if (!parsed.ok) {
    error =
        "'" + path + "' is not JSON: line " + std::to_string(parsed.line) + ": " + parsed.message;
    return false;
  }
  return true;
}

// ---- the tile file ------------------------------------------------------------------------------

// A tile file: this header, then (cells + 3)^2 little-endian i32 millimetres, rows of x in order
// of z, from the point one before the tile's first to one after its last. The engine's targets are
// x86-64, little-endian, so the integers are written as they lie.
struct TileHeader {
  char magic[4] = {'E', 'T', 'I', 'L'};
  u32 version = k_tileset_version;
  u32 cells = 0;
  u32 reserved = 0;
  i64 spacing_mm = 0;
  i32 tx = 0;
  i32 tz = 0;
};
static_assert(sizeof(TileHeader) == 32);

u64 tile_bytes(u32 cells) noexcept {
  const u64 a = u64{cells} + 3;
  return sizeof(TileHeader) + a * a * sizeof(i32);
}

}  // namespace

std::string tile_file(i64 spacing_mm, i32 tx, i32 tz) {
  return std::to_string(spacing_mm) + "/" + std::to_string(tx) + "_" + std::to_string(tz) + ".tile";
}

// ---- the source ---------------------------------------------------------------------------------

namespace {

std::string stem_of(std::string_view path) {
  const std::string_view name = io::file_name(path);
  const usize dot = name.rfind('.');
  return std::string(dot == std::string_view::npos ? name : name.substr(0, dot));
}

bool check_field(const Heightfield& f, std::string& error) {
  if (f.width < 2 || f.height < 2 || f.width > k_max_heightfield_side ||
      f.height > k_max_heightfield_side) {
    error = "a heightfield is 2 to 65,536 points a side; this one is " + std::to_string(f.width) +
            " by " + std::to_string(f.height);
    return false;
  }
  if (f.spacing_mm < 1 || f.spacing_mm > k_max_spacing_mm) {
    error = "a heightfield's spacing is 1 to 1,000,000 mm; this one's is " +
            std::to_string(f.spacing_mm);
    return false;
  }
  if (f.origin_x_mm % f.spacing_mm != 0 || f.origin_z_mm % f.spacing_mm != 0) {
    error =
        "a heightfield's origin is a whole number of its spacings, so its points are the "
        "world's lattice at that spacing; (" +
        std::to_string(f.origin_x_mm) + ", " + std::to_string(f.origin_z_mm) +
        ") mm is not one of " + std::to_string(f.spacing_mm) + " mm";
    return false;
  }
  if (!(f.vertical_mm_per_unit > 0.0) || !std::isfinite(f.vertical_mm_per_unit)) {
    error = "a heightfield's vertical_mm_per_unit is positive and finite";
    return false;
  }
  if (f.data.size() != static_cast<usize>(f.width) * f.height) {
    error = "a heightfield's grid is width times height floats";
    return false;
  }
  return true;
}

}  // namespace

bool write_heightfield(const std::string& json_path, const Heightfield& field, std::string& error) {
  if (!check_field(field, error)) return false;
  const std::string data_name = stem_of(json_path) + ".f32";
  const std::string dir(io::parent_path(json_path));
  const std::string data_path = dir.empty() ? data_name : io::join_path(dir, data_name);
  const std::string_view grid(reinterpret_cast<const char*>(field.data.data()),
                              field.data.size() * sizeof(f32));
  if (!dir.empty()) (void)io::make_directories(dir);
  if (io::write_file_atomic(data_path, grid) != io::Status::Ok) {
    error = "'" + data_path + "' could not be written";
    return false;
  }
  HeightfieldHeader header;
  header.format = std::string(k_heightfield_format);
  header.version = k_heightfield_version;
  header.width = field.width;
  header.height = field.height;
  header.spacing_mm = field.spacing_mm;
  header.origin_mm = {field.origin_x_mm, field.origin_z_mm};
  header.vertical_mm_per_unit = field.vertical_mm_per_unit;
  header.data = data_name;
  header.data_hash = hex16(hash_bytes(grid.data(), grid.size()));
  std::string text = write_json(schema::to_json(header));
  text += "\n";
  if (io::write_file_atomic(json_path, text) != io::Status::Ok) {
    error = "'" + json_path + "' could not be written";
    return false;
  }
  return true;
}

bool read_heightfield(const std::string& json_path, Heightfield& out, u64& source_hash,
                      std::string& error) {
  std::string bytes;
  JsonValue json;
  if (!read_json_file(json_path, bytes, json, error)) return false;
  HeightfieldHeader header;
  schema::ReadContext ctx;
  if (!schema::from_json(header, json, ctx) || !ctx.ok()) {
    error = "'" + json_path + "' is not a heightfield header: " + diagnostics(ctx);
    return false;
  }
  if (header.format != k_heightfield_format || header.version != k_heightfield_version) {
    error = "'" + json_path + "' is format '" + header.format + "' version " +
            std::to_string(header.version) + "; this build reads " +
            std::string(k_heightfield_format) + " version 1";
    return false;
  }
  out = Heightfield{};
  out.width = header.width;
  out.height = header.height;
  out.spacing_mm = header.spacing_mm;
  out.origin_x_mm = header.origin_mm[0];
  out.origin_z_mm = header.origin_mm[1];
  out.vertical_mm_per_unit = header.vertical_mm_per_unit;
  if (header.data.empty()) {
    error = "'" + json_path + "' names no data file";
    return false;
  }
  const std::string dir(io::parent_path(json_path));
  const std::string data_path = io::is_absolute_path(header.data) || dir.empty()
                                    ? header.data
                                    : io::join_path(dir, header.data);
  std::string grid;
  if (io::read_file(data_path, grid) != io::Status::Ok) {
    error = "'" + json_path + "': its grid '" + data_path + "' could not be read";
    return false;
  }
  const u64 want = u64{header.width} * header.height * sizeof(f32);
  if (header.width > k_max_heightfield_side || header.height > k_max_heightfield_side ||
      grid.size() != want) {
    error = "'" + json_path + "': its grid is " + std::to_string(grid.size()) + " bytes, not " +
            std::to_string(want) + " (width times height float32)";
    return false;
  }
  u64 recorded = 0;
  if (!parse_hex16(header.data_hash, recorded) ||
      recorded != hash_bytes(grid.data(), grid.size())) {
    error =
        "'" + json_path + "': its grid's bytes do not hash to data_hash '" + header.data_hash + "'";
    return false;
  }
  out.data.resize(header.width * header.height);
  std::memcpy(out.data.data(), grid.data(), grid.size());
  if (!check_field(out, error)) {
    error = "'" + json_path + "': " + error;
    return false;
  }
  source_hash = hash_bytes(bytes.data(), bytes.size());
  return true;
}

bool parse_synth_kind(std::string_view name, SynthKind& out) noexcept {
  if (name == "ridge") {
    out = SynthKind::ridge;
  } else if (name == "basin") {
    out = SynthKind::basin;
  } else if (name == "steps") {
    out = SynthKind::steps;
  } else if (name == "flat") {
    out = SynthKind::flat;
  } else if (name == "mountain") {
    out = SynthKind::mountain;
  } else {
    return false;
  }
  return true;
}

Heightfield synth_heightfield(SynthKind kind, u32 size, i64 spacing_mm, u64 seed, i64 origin_x_mm,
                              i64 origin_z_mm) {
  Heightfield f;
  f.width = size;
  f.height = size;
  f.spacing_mm = spacing_mm;
  f.origin_x_mm = origin_x_mm;
  f.origin_z_mm = origin_z_mm;
  f.vertical_mm_per_unit = 1000.0;
  f.data.resize(size * size);
  const f64 half = size > 1 ? static_cast<f64>(size - 1) * 0.5 : 1.0;
  for (u32 j = 0; j < size; ++j) {
    for (u32 i = 0; i < size; ++i) {
      // (u, v) in [-1, 1] over the grid.
      const f64 u = (static_cast<f64>(i) - half) / half;
      const f64 v = (static_cast<f64>(j) - half) / half;
      f64 h = 0.0;
      switch (kind) {
        case SynthKind::ridge: {
          // A ridge 40 m high along a diagonal, falling to nothing 0.4 of the half-side from its
          // line with a smooth polynomial shoulder, on ground that rises 6 m across the grid.
          const f64 d = std::fabs(u + 0.25 * v) / 0.4;
          const f64 t = d < 1.0 ? (1.0 - d * d) : 0.0;
          h = 40.0 * t * t + 3.0 * (v + 1.0);
          break;
        }
        case SynthKind::basin: {
          // A bowl 20 m deep inside a rim at the grid's inscribed circle, 5 m above zero outside.
          const f64 r2 = u * u + v * v;
          const f64 t = r2 < 1.0 ? (1.0 - r2) : 0.0;
          h = 5.0 - 20.0 * t * t;
          break;
        }
        case SynthKind::steps: {
          // Eight terraces 2 m apart across x: cliffs a spacing wide, what a box filter smears.
          h = 2.0 * std::floor((u + 1.0) * 4.0);
          break;
        }
        case SynthKind::flat: h = 0.0; break;
        case SynthKind::mountain: {
          // A mountain 180 m high centred at (0.35, -0.35), its foot 0.55 of the half-side out,
          // with the profile (1 - d^2)^3, whose slope rises from the summit to about 48 degrees
          // two-fifths of the way down on a 1 km grid — soil, scree and bedrock bands — on a plain
          // rising 8 m across the grid, with two pond bowls 2.5 m deep, 0.06 of the half-side
          // across, on it at nearly one height (ground-by-place, content/test-scenes).
          const f64 mu = u - 0.35, mv = v + 0.35;
          const f64 d2 = (mu * mu + mv * mv) / (0.55 * 0.55);
          const f64 m = d2 < 1.0 ? (1.0 - d2) * (1.0 - d2) * (1.0 - d2) : 0.0;
          h = 180.0 * m + 4.0 * (1.0 - v);
          const f64 ponds[2][2] = {{-0.5, 0.45}, {0.1, 0.6}};
          for (const auto& p : ponds) {
            const f64 pu = u - p[0], pv = v - p[1];
            const f64 q = (pu * pu + pv * pv) / (0.06 * 0.06);
            if (q < 1.0) h -= 2.5 * (1.0 - q) * (1.0 - q);
          }
          break;
        }
      }
      if (seed != 0) {
        const u64 r = mix64(hash_combine(seed, (u64{j} << 32) | i));
        h += (static_cast<f64>(r >> 40) / 16777216.0 - 0.5) * 0.25;
      }
      f.data[j * size + i] = static_cast<f32>(h);
    }
  }
  return f;
}

// ---- the build ----------------------------------------------------------------------------------

bool chain_spacings(const BuildOptions& o, Vector<i64>& out, std::string* error) {
  out.clear();
  if (o.ring_spacing_mm < 1 || o.far_ratio < 1 || o.far_levels > 16) {
    if (error != nullptr)
      *error =
          "the far levels' parameters are a ring spacing of at least 1 mm, a ratio of at "
          "least 1 and at most 16 levels";
    return false;
  }
  i64 s = o.ring_spacing_mm * static_cast<i64>(o.far_ratio);
  for (u32 k = 0; k < o.far_levels; ++k) {
    out.push_back(s);
    s *= 2;
  }
  return true;
}

u64 tileset_key(u64 source_hash, const BuildOptions& o) noexcept {
  u64 key = hash_combine(source_hash, 0x74696c6573657431ull);  // "tileset1"
  key = hash_combine(key, k_tileset_version);
  key = hash_combine(key, o.cells);
  key = hash_combine(key, static_cast<u64>(o.ring_spacing_mm));
  key = hash_combine(key, o.far_ratio);
  key = hash_combine(key, o.far_levels);
  key = hash_combine(key, f64_bits(o.vertical_scale));
  key = hash_combine(key, static_cast<u64>(o.offset_x_mm));
  key = hash_combine(key, static_cast<u64>(o.offset_y_mm));
  key = hash_combine(key, static_cast<u64>(o.offset_z_mm));
  return key;
}

std::string tileset_cache_dir(std::string_view ddc, u64 key) {
  return io::join_path(io::join_path(ddc, "tilesets"), hex16(key));
}

bool quantize_heights(const Heightfield& field, const BuildOptions& o, Vector<i32>& out,
                      std::string& error) {
  if (!check_field(field, error)) return false;
  if (!(o.vertical_scale > 0.0) || !(o.vertical_scale <= 1000.0)) {
    error = "a vertical scale is in (0, 1000]";
    return false;
  }
  out.resize(field.data.size());
  const f64 per_unit = field.vertical_mm_per_unit * o.vertical_scale;
  const f64 offset = static_cast<f64>(o.offset_y_mm);
  for (u32 p = 0; p < field.data.size(); ++p) {
    const f64 mm = std::floor(static_cast<f64>(field.data[p]) * per_unit + offset + 0.5);
    if (!std::isfinite(mm) || mm < -2147483647.0 || mm > 2147483647.0) {
      error = "the heightfield's point (" + std::to_string(p % field.width) + ", " +
              std::to_string(p / field.width) +
              ") is not a finite height within 2,147 km once scaled";
      return false;
    }
    out[p] = static_cast<i32>(mm);
  }
  return true;
}

namespace {

// One level of the set as the build sees it: the lattice it is on and how a point's millimetres
// are made from the finest's.
struct BuildLevel {
  i64 spacing = 0;
  i64 filter = 0;
  i64 ratio = 0;  // spacing over the finest's; 1 for the finest; 0 for a point level (bilinear)
  i64 imin = 0, imax = 0, jmin = 0, jmax = 0;
};

// The finest heights, clamped (the edge held past them).
struct Finest {
  const Vector<i32>* mm = nullptr;
  i64 w = 0, h = 0;
  i64 ox = 0, oz = 0;  // the first point's world lattice index
  i64 s = 0;
  i32 at(i64 fi, i64 fj) const noexcept {
    return (*mm)[static_cast<u32>(clamp_i64(fj, 0, h - 1) * w + clamp_i64(fi, 0, w - 1))];
  }
};

// The finest's bilinear surface at whole millimetres (TileSet::point_mm's arithmetic, operation
// for operation).
f64 bilinear(const Finest& f, i64 x_mm, i64 z_mm) noexcept {
  const i64 x0 = f.ox * f.s;
  const i64 z0 = f.oz * f.s;
  const i64 fx = clamp_i64(x_mm - x0, 0, (f.w - 1) * f.s);
  const i64 fz = clamp_i64(z_mm - z0, 0, (f.h - 1) * f.s);
  const i64 ci = fx / f.s;
  const i64 cj = fz / f.s;
  const f64 u = static_cast<f64>(fx - ci * f.s) / static_cast<f64>(f.s);
  const f64 v = static_cast<f64>(fz - cj * f.s) / static_cast<f64>(f.s);
  const f64 h00 = f.at(ci, cj);
  const f64 h10 = f.at(ci + 1, cj);
  const f64 h01 = f.at(ci, cj + 1);
  const f64 h11 = f.at(ci + 1, cj + 1);
  const f64 top = h00 + u * (h10 - h00);
  const f64 bottom = h01 + u * (h11 - h01);
  return top + v * (bottom - top);
}

i32 round_mm(f64 mm) noexcept { return static_cast<i32>(std::floor(mm + 0.5)); }

// The level's millimetres over its whole stored window, rows of x in order of z.
void fill_level(const Finest& f, const BuildLevel& L, Vector<i32>& out) {
  const i64 nx = L.imax - L.imin + 1;
  const i64 nz = L.jmax - L.jmin + 1;
  out.resize(static_cast<u32>(nx * nz));
  if (L.ratio == 1) {
    for (i64 j = 0; j < nz; ++j)
      for (i64 i = 0; i < nx; ++i)
        out[static_cast<u32>(j * nx + i)] = f.at(L.imin + i - f.ox, L.jmin + j - f.oz);
    return;
  }
  if (L.ratio == 0) {
    for (i64 j = 0; j < nz; ++j)
      for (i64 i = 0; i < nx; ++i)
        out[static_cast<u32>(j * nx + i)] =
            round_mm(bilinear(f, (L.imin + i) * L.spacing, (L.jmin + j) * L.spacing));
    return;
  }
  // **The box filter over each point's own cell**: the mean of the finest's bilinear surface over
  // the square one level spacing wide round the point, which on the finest's points is the
  // trapezoid rule — weights 1, 2, ..., 2, 1 over the r + 1 points a side the square spans —
  // summed exactly in i64, separably (rows, then the rows' sums), and rounded once, half up.
  const i64 r = L.ratio;
  const i64 half = r / 2;
  Vector<i64> rows(static_cast<u32>(nx * f.h));  // a level column's x-sum on every finest row
  for (i64 fj = 0; fj < f.h; ++fj) {
    for (i64 i = 0; i < nx; ++i) {
      const i64 c = (L.imin + i) * r - f.ox;
      i64 sum = 0;
      for (i64 k = -half; k <= half; ++k)
        sum += (k == -half || k == half ? 1 : 2) * static_cast<i64>(f.at(c + k, fj));
      rows[static_cast<u32>(fj * nx + i)] = sum;
    }
  }
  const i64 den = 4 * r * r;
  for (i64 j = 0; j < nz; ++j) {
    const i64 c = (L.jmin + j) * r - f.oz;
    for (i64 i = 0; i < nx; ++i) {
      i64 sum = 0;
      for (i64 k = -half; k <= half; ++k) {
        const i64 fj = clamp_i64(c + k, 0, f.h - 1);
        sum += (k == -half || k == half ? 1 : 2) * rows[static_cast<u32>(fj * nx + i)];
      }
      out[static_cast<u32>(j * nx + i)] = static_cast<i32>(floor_div(2 * sum + den, 2 * den));
    }
  }
}

bool make_levels(const Finest& f, const BuildOptions& o, Vector<BuildLevel>& out,
                 std::string& error) {
  Vector<i64> chain;
  if (!chain_spacings(o, chain, &error)) return false;
  out.clear();
  BuildLevel finest;
  finest.spacing = f.s;
  finest.filter = 0;
  finest.ratio = 1;
  finest.imin = f.ox;
  finest.imax = f.ox + f.w - 1;
  finest.jmin = f.oz;
  finest.jmax = f.oz + f.h - 1;
  out.push_back(finest);
  const i64 x0 = f.ox * f.s;
  const i64 x1 = (f.ox + f.w - 1) * f.s;
  const i64 z0 = f.oz * f.s;
  const i64 z1 = (f.oz + f.h - 1) * f.s;
  for (const i64 spacing : chain) {
    BuildLevel L;
    L.spacing = spacing;
    L.filter = spacing;
    if (spacing <= f.s) {
      // No coarser than the source: it carries everything the source has, its points the
      // bilinear surface's.
      L.ratio = 0;
    } else if (spacing % (2 * f.s) == 0) {
      L.ratio = spacing / f.s;
    } else {
      error = "the far level of " + std::to_string(spacing) +
              " mm is not a whole number of twice the heightfield's spacing (" +
              std::to_string(f.s) +
              " mm), so its cell's box filter does not land on the heightfield's points; choose "
              "a ring spacing or ratio whose far levels are";
      return false;
    }
    // Every point whose cell touches the data, and one more a side whose cell is wholly past it:
    // past that a point is that one's, which is what the edge held makes every point there.
    L.imin = floor_div(x0, spacing) - 1;
    L.imax = ceil_div(x1, spacing) + 1;
    L.jmin = floor_div(z0, spacing) - 1;
    L.jmax = ceil_div(z1, spacing) + 1;
    out.push_back(L);
  }
  return true;
}

}  // namespace

bool build_tileset(const Heightfield& field, u64 source_hash, std::string_view source_name,
                   const BuildOptions& options, const std::string& dir, BuildResult& out,
                   std::string& error) {
  out = BuildResult{};
  if (options.cells < 1 || options.cells > 4096) {
    error = "a tile is 1 to 4096 cells a side";
    return false;
  }
  if (options.offset_x_mm % field.spacing_mm != 0 || options.offset_z_mm % field.spacing_mm != 0) {
    error = "the origin a heightfield is moved by is a whole number of its spacings (" +
            std::to_string(field.spacing_mm) + " mm), so its points stay the world's lattice";
    return false;
  }
  Vector<i32> mm;
  if (!quantize_heights(field, options, mm, error)) return false;
  Finest f;
  f.mm = &mm;
  f.w = field.width;
  f.h = field.height;
  f.s = field.spacing_mm;
  f.ox = (field.origin_x_mm + options.offset_x_mm) / field.spacing_mm;
  f.oz = (field.origin_z_mm + options.offset_z_mm) / field.spacing_mm;
  Vector<BuildLevel> levels;
  if (!make_levels(f, options, levels, error)) return false;

  TileSetManifest manifest;
  manifest.format = std::string(k_tileset_format);
  manifest.version = k_tileset_version;
  out.key = tileset_key(source_hash, options);
  manifest.key = hex16(out.key);
  manifest.source_hash = hex16(source_hash);
  manifest.source = std::string(source_name);
  manifest.cells = options.cells;
  manifest.spacing_mm = f.s;
  manifest.origin_mm = {f.ox * f.s, f.oz * f.s};
  manifest.width = field.width;
  manifest.height = field.height;
  manifest.vertical_scale = options.vertical_scale;
  manifest.offset_y_mm = options.offset_y_mm;
  manifest.ring_spacing_mm = options.ring_spacing_mm;
  manifest.far_ratio = options.far_ratio;
  manifest.far_levels = options.far_levels;
  for (u32 l = 1; l < levels.size(); ++l)
    manifest.chain.push_back(levels[l].spacing);

  const i64 c = options.cells;
  const i64 a = c + 3;
  Vector<i32> values;
  std::string bytes;
  u64 set_hash = hash_combine(k_hash_seed, out.key);
  for (const BuildLevel& L : levels) {
    fill_level(f, L, values);
    const i64 nx = L.imax - L.imin + 1;
    const std::string level_dir = io::join_path(dir, std::to_string(L.spacing));
    if (io::make_directories(level_dir) != io::Status::Ok && !io::exists(level_dir)) {
      error = "'" + level_dir + "' could not be made";
      return false;
    }
    TileSetLevel record;
    record.spacing_mm = L.spacing;
    record.filter_mm = L.filter;
    record.index_min = {L.imin, L.jmin};
    record.index_max = {L.imax, L.jmax};
    const i32 tx0 = static_cast<i32>(floor_div(L.imin, c));
    const i32 tx1 = static_cast<i32>(floor_div(L.imax, c));
    const i32 tz0 = static_cast<i32>(floor_div(L.jmin, c));
    const i32 tz1 = static_cast<i32>(floor_div(L.jmax, c));
    record.tile_min = {tx0, tz0};
    record.tile_max = {tx1, tz1};
    u64 level_hash = hash_combine(k_hash_seed, static_cast<u64>(L.spacing));
    for (i32 tz = tz0; tz <= tz1; ++tz) {
      for (i32 tx = tx0; tx <= tx1; ++tx) {
        TileHeader header;
        header.cells = options.cells;
        header.spacing_mm = L.spacing;
        header.tx = tx;
        header.tz = tz;
        bytes.assign(tile_bytes(options.cells), '\0');
        std::memcpy(bytes.data(), &header, sizeof(header));
        i32* grid = reinterpret_cast<i32*>(bytes.data() + sizeof(header));
        for (i64 lj = 0; lj < a; ++lj) {
          const i64 j = clamp_i64(i64{tz} * c - 1 + lj, L.jmin, L.jmax) - L.jmin;
          for (i64 li = 0; li < a; ++li) {
            const i64 i = clamp_i64(i64{tx} * c - 1 + li, L.imin, L.imax) - L.imin;
            const i32 v = values[static_cast<u32>(j * nx + i)];
            std::memcpy(grid + lj * a + li, &v, sizeof(v));
          }
        }
        const std::string path = io::join_path(dir, tile_file(L.spacing, tx, tz));
        if (io::write_file(path, bytes) != io::Status::Ok) {
          error = "'" + path + "' could not be written";
          return false;
        }
        level_hash = hash_combine(level_hash, hash_bytes(bytes.data(), bytes.size()));
        ++record.tiles;
        out.bytes += bytes.size();
      }
    }
    record.hash = hex16(level_hash);
    set_hash = hash_combine(set_hash, level_hash);
    out.tiles += record.tiles;
    manifest.levels.push_back(std::move(record));
  }
  manifest.hash = hex16(set_hash);
  out.hash = set_hash;
  out.levels = manifest.levels.size();
  // The manifest last: a set whose manifest is there is whole.
  std::string text = write_json(schema::to_json(manifest));
  text += "\n";
  out.manifest = io::join_path(dir, k_manifest_name);
  if (io::write_file_atomic(out.manifest, text) != io::Status::Ok) {
    error = "'" + out.manifest + "' could not be written";
    return false;
  }
  return true;
}

bool ensure_tileset(const std::string& heightfield_json, const BuildOptions& options,
                    const std::string& dir, const std::string& ddc, bool force, BuildResult& out,
                    std::string& error) {
  Heightfield field;
  u64 source_hash = 0;
  if (!read_heightfield(heightfield_json, field, source_hash, error)) return false;
  const u64 key = tileset_key(source_hash, options);
  std::string target = dir;
  if (target.empty()) {
    if (ddc.empty()) {
      error = "no derived-data root to build '" + heightfield_json +
              "' into: name one, or an output directory";
      return false;
    }
    target = tileset_cache_dir(ddc, key);
  }
  const std::string manifest_path = io::join_path(target, k_manifest_name);
  if (!force && io::exists(manifest_path)) {
    // A hit is a manifest recording this build's key; the tiles beside it were written before it.
    std::string bytes;
    JsonValue json;
    std::string why;
    TileSetManifest manifest;
    schema::ReadContext ctx;
    ctx.options.ignore_unknown_fields = true;
    if (read_json_file(manifest_path, bytes, json, why) && schema::from_json(manifest, json, ctx) &&
        ctx.ok() && manifest.format == k_tileset_format && manifest.version == k_tileset_version &&
        manifest.key == hex16(key)) {
      out = BuildResult{};
      out.manifest = manifest_path;
      out.key = key;
      out.hit = true;
      out.levels = manifest.levels.size();
      for (const TileSetLevel& level : manifest.levels)
        out.tiles += level.tiles;
      out.bytes = u64{out.tiles} * tile_bytes(manifest.cells);
      (void)parse_hex16(manifest.hash, out.hash);
      return true;
    }
  }
  return build_tileset(field, source_hash, heightfield_json, options, target, out, error);
}

// ---- the reader ---------------------------------------------------------------------------------

struct TileSet::Level {
  i64 spacing = 0;
  i64 filter = 0;
  i64 imin = 0, imax = 0, jmin = 0, jmax = 0;
  i32 tx0 = 0, tz0 = 0;
  u32 ntx = 0, ntz = 0;
};

// **The cache: an index under a short lock, payloads read outside it** (tileset.md, "The reader's
// lock"). A bounded FIFO shared by every level. A request **pins** the tiles it reads (`Pins`),
// under the lock and only for the lookup, and then reads their heights with no lock at all; an
// entry is evicted only when no request pins it, so a pinned payload cannot move or go. A tile not
// resident is claimed under the lock (an entry marked `loading`, pinned by its claimant), read from
// disk with the lock released, and published under it again: a second caller asking for the same
// tile waits for that read alone (`ready`), never for the lock across someone else's IO, and never
// reads it twice. Neither the index nor the payloads are sized by the source extent.
struct TileSet::Cache {
  struct Key {
    i64 spacing;
    i32 x, z;
    bool operator==(const Key&) const = default;
  };
  struct KeyHash {
    u64 operator()(const Key& key) const noexcept { return hash_bytes(&key, sizeof(key)); }
  };
  struct Entry {
    Key key{};
    std::unique_ptr<i32[]> grid;
    u32 pins = 0;
    bool loading = false;  // claimed: its read is in flight, outside the lock
    bool live = false;     // in the index
  };
  HashMap<Key, u32, KeyHash> index;
  Vector<Entry> entries;
  u32 capacity = 0;
  u32 next = 0;
  u64 payload_bytes = 0;
  std::condition_variable ready;  // a claimed tile was published or failed
};

// A request's pinned tiles: looked up here first, without the lock (the last one asked before the
// rest, as the hint was), and unpinned together when the request ends.
struct TileSet::Pins {
  static constexpr u32 k_max = 8;
  struct Pin {
    Cache::Key key;
    u32 entry;
    const i32* grid;
  };
  explicit Pins(const TileSet& set) noexcept : set_(set) {}
  ~Pins() { set_.unpin(*this); }
  Pins(const Pins&) = delete;
  Pins& operator=(const Pins&) = delete;
  const TileSet& set_;
  Pin pins[k_max];
  u32 count = 0;
  u32 last = 0;
};

// The reader's counters (`ReaderStats`) and the cache's own (`CacheStats`): relaxed atomics, the
// most of each raised by a compare and swap, so reading them takes no lock. Nothing a request
// answers reads them.
struct TileSet::Counters {
  std::atomic<u64> locks{0};
  std::atomic<u64> lock_wait_ns{0};
  std::atomic<u64> max_lock_wait_ns{0};
  std::atomic<u64> lock_hold_ns{0};
  std::atomic<u64> max_lock_hold_ns{0};
  std::atomic<u64> hits{0};
  std::atomic<u64> misses{0};
  std::atomic<u64> io_bytes{0};
  std::atomic<u64> io_ns{0};
  std::atomic<u64> load_waits{0};
  std::atomic<u64> load_wait_ns{0};
  std::atomic<u32> resident{0};
  std::atomic<u32> evictions{0};
};

namespace {

// The calling thread's share, over every set (`TileSet::thread_reader_stats`).
thread_local TileSet::ReaderStats t_reader;

void raise(std::atomic<u64>& most, u64 value) noexcept {
  u64 seen = most.load(std::memory_order_relaxed);
  while (value > seen && !most.compare_exchange_weak(seen, value, std::memory_order_relaxed)) {
  }
}

u64 elapsed_ns(i64 from, i64 to) noexcept { return to > from ? static_cast<u64>(to - from) : 0; }

// The reader's lock, timed: the wait to take it and the time it was held, into the set's counters
// and the calling thread's. A wait on `ready` inside it is a wait for another caller's read, and
// counts as that (`load_wait_ns`), not as a hold.
class TimedLock {
 public:
  TimedLock(std::mutex& mutex, TileSet::Counters& counters) noexcept
      : lock_(mutex, std::defer_lock), counters_(counters) {
    const i64 asked = time::monotonic_ns();
    lock_.lock();
    taken_ = time::monotonic_ns();
    const u64 wait = elapsed_ns(asked, taken_);
    counters_.locks.fetch_add(1, std::memory_order_relaxed);
    counters_.lock_wait_ns.fetch_add(wait, std::memory_order_relaxed);
    raise(counters_.max_lock_wait_ns, wait);
    ++t_reader.locks;
    t_reader.lock_wait_ns += wait;
    t_reader.max_lock_wait_ns = std::max(t_reader.max_lock_wait_ns, wait);
  }
  ~TimedLock() {
    const u64 hold = elapsed_ns(taken_, time::monotonic_ns()) - waited_;
    lock_.unlock();
    counters_.lock_hold_ns.fetch_add(hold, std::memory_order_relaxed);
    raise(counters_.max_lock_hold_ns, hold);
    t_reader.lock_hold_ns += hold;
    t_reader.max_lock_hold_ns = std::max(t_reader.max_lock_hold_ns, hold);
  }
  TimedLock(const TimedLock&) = delete;
  TimedLock& operator=(const TimedLock&) = delete;

  // Waits on `ready` for another caller's read of a tile, the lock released meanwhile.
  void wait(std::condition_variable& ready) {
    const i64 from = time::monotonic_ns();
    ready.wait(lock_);
    const u64 waited = elapsed_ns(from, time::monotonic_ns());
    waited_ += waited;
    counters_.load_waits.fetch_add(1, std::memory_order_relaxed);
    counters_.load_wait_ns.fetch_add(waited, std::memory_order_relaxed);
    ++t_reader.load_waits;
    t_reader.load_wait_ns += waited;
  }

 private:
  std::unique_lock<std::mutex> lock_;
  TileSet::Counters& counters_;
  i64 taken_ = 0;
  u64 waited_ = 0;
};

}  // namespace

TileSet::TileSet(u64 cache_bytes)
    : cache_(std::make_unique<Cache>()),
      cache_bytes_(cache_bytes),
      counters_(std::make_unique<Counters>()) {}
TileSet::~TileSet() = default;

TileSet::ReaderStats TileSet::reader_stats() const noexcept {
  const Counters& c = *counters_;
  ReaderStats out;
  out.locks = c.locks.load(std::memory_order_relaxed);
  out.lock_wait_ns = c.lock_wait_ns.load(std::memory_order_relaxed);
  out.max_lock_wait_ns = c.max_lock_wait_ns.load(std::memory_order_relaxed);
  out.lock_hold_ns = c.lock_hold_ns.load(std::memory_order_relaxed);
  out.max_lock_hold_ns = c.max_lock_hold_ns.load(std::memory_order_relaxed);
  out.hits = c.hits.load(std::memory_order_relaxed);
  out.misses = c.misses.load(std::memory_order_relaxed);
  out.io_bytes = c.io_bytes.load(std::memory_order_relaxed);
  out.io_ns = c.io_ns.load(std::memory_order_relaxed);
  out.load_waits = c.load_waits.load(std::memory_order_relaxed);
  out.load_wait_ns = c.load_wait_ns.load(std::memory_order_relaxed);
  return out;
}

TileSet::ReaderStats TileSet::thread_reader_stats(bool take_max) noexcept {
  const ReaderStats out = t_reader;
  if (take_max) {
    t_reader.max_lock_wait_ns = 0;
    t_reader.max_lock_hold_ns = 0;
  }
  return out;
}

bool TileSet::open(const std::string& manifest_path, std::string& error) {
  ENGINE_STARTUP_STAGE_DETAIL("tileset open", manifest_path);
  levels_.clear();
  cache_->index.clear();
  cache_->entries.clear();
  cache_->capacity = 0;
  cache_->next = 0;
  cache_->payload_bytes = 0;
  counters_->resident.store(0, std::memory_order_relaxed);
  counters_->evictions.store(0, std::memory_order_relaxed);
  reads_.store(0, std::memory_order_relaxed);
  failures_.store(0, std::memory_order_relaxed);
  std::string bytes;
  JsonValue json;
  if (!read_json_file(manifest_path, bytes, json, error)) return false;
  schema::ReadContext ctx;
  manifest_ = TileSetManifest{};
  if (!schema::from_json(manifest_, json, ctx) || !ctx.ok()) {
    error = "'" + manifest_path + "' is not a tile set's manifest: " + diagnostics(ctx);
    return false;
  }
  if (manifest_.format != k_tileset_format || manifest_.version != k_tileset_version) {
    error = "'" + manifest_path + "' is format '" + manifest_.format + "' version " +
            std::to_string(manifest_.version) + "; this build reads " +
            std::string(k_tileset_format) + " version " + std::to_string(k_tileset_version);
    return false;
  }
  if (manifest_.levels.empty() || manifest_.levels[0].filter_mm != 0 || manifest_.cells < 1 ||
      manifest_.cells > 4096 || manifest_.spacing_mm < 1 ||
      manifest_.levels[0].spacing_mm != manifest_.spacing_mm) {
    error = "'" + manifest_path + "' has no finest level at its spacing";
    return false;
  }
  dir_ = std::string(io::parent_path(manifest_path));
  const u64 payload = (u64{manifest_.cells} + 3) * (manifest_.cells + 3) * sizeof(i32);
  if (cache_bytes_ < payload) {
    error = "the tile cache budget cannot hold one tile (" + std::to_string(payload) + " bytes)";
    return false;
  }
  cache_->payload_bytes = payload;
  cache_->capacity = static_cast<u32>(std::min<u64>(cache_bytes_ / payload, (u64{1} << 26)));
  // Grown as tiles come in rather than reserved whole: a budget of small tiles is millions of
  // entries, and their metadata would outweigh the payload budget it is meant to bound.
  const u32 reserve = std::min<u32>(cache_->capacity, 1024u);
  cache_->entries.reserve(reserve);
  cache_->index.reserve(reserve);
  for (const TileSetLevel& record : manifest_.levels) {
    auto level = std::make_unique<Level>();
    level->spacing = record.spacing_mm;
    level->filter = record.filter_mm;
    level->imin = record.index_min[0];
    level->jmin = record.index_min[1];
    level->imax = record.index_max[0];
    level->jmax = record.index_max[1];
    level->tx0 = record.tile_min[0];
    level->tz0 = record.tile_min[1];
    const i64 ntx = i64{record.tile_max[0]} - record.tile_min[0] + 1;
    const i64 ntz = i64{record.tile_max[1]} - record.tile_min[1] + 1;
    if (level->spacing < 1 || level->imax < level->imin || level->jmax < level->jmin || ntx < 1 ||
        ntz < 1 || ntx * ntz > (i64{1} << 26) ||
        (record.filter_mm != 0 && record.filter_mm != record.spacing_mm)) {
      error = "'" + manifest_path + "': its level of " + std::to_string(record.spacing_mm) +
              " mm is not one a build writes";
      levels_.clear();
      return false;
    }
    level->ntx = static_cast<u32>(ntx);
    level->ntz = static_cast<u32>(ntz);
    levels_.push_back(std::move(level));
  }
  return true;
}

void TileSet::unpin(Pins& pins) const noexcept {
  if (pins.count == 0) return;
  TimedLock lock(mutex_, *counters_);
  unpin_locked(pins);
}

void TileSet::unpin_locked(Pins& pins) const noexcept {
  Cache& cache = *cache_;
  for (u32 k = 0; k < pins.count; ++k) {
    Cache::Entry& entry = cache.entries[pins.pins[k].entry];
    if (entry.pins > 0) --entry.pins;
  }
  pins.count = 0;
  pins.last = 0;
}

// The tile's payload, pinned for the rest of the request; null for a tile the set does not hold or
// a read that failed. Under the lock only for the lookup, the claim and the publication.
const i32* TileSet::tile(const Level& level, i32 tx, i32 tz, Pins& pins) const noexcept {
  const i64 kx = i64{tx} - level.tx0;
  const i64 kz = i64{tz} - level.tz0;
  if (kx < 0 || kz < 0 || kx >= level.ntx || kz >= level.ntz) return nullptr;
  const Cache::Key key{level.spacing, tx, tz};
  // The request's own pins first, with no lock: a row of points reads one tile many times.
  if (pins.count > 0 && pins.pins[pins.last].key == key) return pins.pins[pins.last].grid;
  for (u32 k = 0; k < pins.count; ++k) {
    if (pins.pins[k].key == key) {
      pins.last = k;
      return pins.pins[k].grid;
    }
  }
  Cache& cache = *cache_;
  u32 slot = 0;
  {
    TimedLock lock(mutex_, *counters_);
    if (pins.count == Pins::k_max) unpin_locked(pins);
    for (;;) {
      const u32* found = cache.index.find_value(key);
      if (found == nullptr) break;
      Cache::Entry& entry = cache.entries[*found];
      if (!entry.loading) {
        ++entry.pins;
        counters_->hits.fetch_add(1, std::memory_order_relaxed);
        ++t_reader.hits;
        pins.pins[pins.count] = Pins::Pin{key, *found, entry.grid.get()};
        pins.last = pins.count++;
        return entry.grid.get();
      }
      // Another caller is reading it: wait for that read, then look again (it may have failed).
      lock.wait(cache.ready);
    }
    // Claim a slot: a free one while the cache has room, else the oldest no request pins — this
    // request's own pins let go first if that is what it takes, since every height it read through
    // them is already read. With every entry pinned by others — more requests at once than the
    // budget has tiles — the cache grows past its budget by the pins, and the next evictions bring
    // it back.
    const u32 size = cache.entries.size();
    if (size < cache.capacity) {
      slot = size;
      cache.entries.push_back(Cache::Entry{});
    } else {
      auto oldest_free = [&] {
        for (u32 k = 0; k < size; ++k) {
          const u32 at = (cache.next + k) % size;
          const Cache::Entry& entry = cache.entries[at];
          if (entry.pins == 0 && !entry.loading) return at;
        }
        return size;
      };
      slot = oldest_free();
      if (slot == size && pins.count > 0) {
        unpin_locked(pins);
        slot = oldest_free();
      }
      if (slot == size) {
        cache.entries.push_back(Cache::Entry{});
      } else {
        cache.next = (slot + 1) % size;
      }
    }
    Cache::Entry& entry = cache.entries[slot];
    if (entry.live) {
      cache.index.erase(entry.key);
      entry.live = false;
      if (entry.grid != nullptr) {
        counters_->evictions.fetch_add(1, std::memory_order_relaxed);
        counters_->resident.fetch_sub(1, std::memory_order_relaxed);
      }
    }
    entry.key = key;
    entry.loading = true;
    entry.live = true;
    entry.pins = 1;
    cache.index.insert_or_assign(key, slot);
  }

  // The read, with no lock held: another caller's lookups, hits and other tiles' reads go on.
  std::string bytes;
  const std::string path = io::join_path(dir_, tile_file(level.spacing, tx, tz));
  const i64 read_start = time::monotonic_ns();
  const io::Status status = io::read_file(path, bytes);
  const u64 read_ns = elapsed_ns(read_start, time::monotonic_ns());
  counters_->misses.fetch_add(1, std::memory_order_relaxed);
  counters_->io_bytes.fetch_add(bytes.size(), std::memory_order_relaxed);
  counters_->io_ns.fetch_add(read_ns, std::memory_order_relaxed);
  ++t_reader.misses;
  t_reader.io_bytes += bytes.size();
  t_reader.io_ns += read_ns;
  std::unique_ptr<i32[]> grid;
  TileHeader header;
  if (status == io::Status::Ok && bytes.size() == tile_bytes(manifest_.cells)) {
    std::memcpy(&header, bytes.data(), sizeof(header));
    if (std::memcmp(header.magic, "ETIL", 4) == 0 && header.version == k_tileset_version &&
        header.cells == manifest_.cells && header.spacing_mm == level.spacing && header.tx == tx &&
        header.tz == tz) {
      const usize count = (bytes.size() - sizeof(header)) / sizeof(i32);
      grid = std::make_unique<i32[]>(count);
      std::memcpy(grid.get(), bytes.data() + sizeof(header), count * sizeof(i32));
    }
  }

  const i32* held = grid.get();
  {
    TimedLock lock(mutex_, *counters_);
    Cache::Entry& entry = cache.entries[slot];
    entry.loading = false;
    if (held != nullptr) {
      entry.grid = std::move(grid);
      counters_->resident.fetch_add(1, std::memory_order_relaxed);
      if (pins.count == Pins::k_max) unpin_locked(pins);
      pins.pins[pins.count] = Pins::Pin{key, slot, held};
      pins.last = pins.count++;
    } else {
      // Nothing to publish: the slot is free again and the key is asked afresh next time.
      cache.index.erase(key);
      entry.live = false;
      entry.pins = 0;
      entry.grid.reset();
    }
  }
  cache.ready.notify_all();
  if (held == nullptr) {
    failures_.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
  }
  reads_.fetch_add(1, std::memory_order_relaxed);
  return held;
}

f64 TileSet::value(const Level& level, i64 i, i64 j, Pins& pins) const noexcept {
  const i64 c = manifest_.cells;
  const i64 ci = clamp_i64(i, level.imin, level.imax);
  const i64 cj = clamp_i64(j, level.jmin, level.jmax);
  const i32 tx = static_cast<i32>(floor_div(ci, c));
  const i32 tz = static_cast<i32>(floor_div(cj, c));
  const i32* grid = tile(level, tx, tz, pins);
  if (grid == nullptr) return std::numeric_limits<f64>::quiet_NaN();
  const i64 li = ci - i64{tx} * c + 1;
  const i64 lj = cj - i64{tz} * c + 1;
  return grid[lj * (c + 3) + li];
}

f64 TileSet::point_mm(i64 x_mm, i64 z_mm) const noexcept {
  Pins pins(*this);
  return point_mm_pinned(x_mm, z_mm, pins);
}

f64 TileSet::point_mm_pinned(i64 x_mm, i64 z_mm, Pins& pins) const noexcept {
  if (!is_open()) return std::numeric_limits<f64>::quiet_NaN();
  // `bilinear` above, over the tiles: the same arithmetic, operation for operation, so a level a
  // build wrote from it and a point asked of the reader agree.
  const Level& f = *levels_[0];
  const i64 s = f.spacing;
  const i64 x0 = f.imin * s;
  const i64 z0 = f.jmin * s;
  const i64 fx = clamp_i64(x_mm - x0, 0, (f.imax - f.imin) * s);
  const i64 fz = clamp_i64(z_mm - z0, 0, (f.jmax - f.jmin) * s);
  const i64 ci = fx / s;
  const i64 cj = fz / s;
  const f64 u = static_cast<f64>(fx - ci * s) / static_cast<f64>(s);
  const f64 v = static_cast<f64>(fz - cj * s) / static_cast<f64>(s);
  const f64 h00 = value(f, f.imin + ci, f.jmin + cj, pins);
  const f64 h10 = value(f, f.imin + ci + 1, f.jmin + cj, pins);
  const f64 h01 = value(f, f.imin + ci, f.jmin + cj + 1, pins);
  const f64 h11 = value(f, f.imin + ci + 1, f.jmin + cj + 1, pins);
  const f64 top = h00 + u * (h10 - h00);
  const f64 bottom = h01 + u * (h11 - h01);
  return top + v * (bottom - top);
}

f32 TileSet::height_mm(i64 x_mm, i64 z_mm) const noexcept {
  return static_cast<f32>(point_mm(x_mm, z_mm) / 1000.0);
}

bool TileSet::has_filter(i64 spacing_mm) const noexcept {
  for (const auto& level : levels_)
    if (level->filter != 0 && level->spacing == spacing_mm) return true;
  return false;
}

// **A window evaluated from pinned tiles, never under the lock**: each point's tile is pinned on
// first use (the lock taken for the lookup alone), its heights read with no lock, and every pin let
// go when the request returns.
bool TileSet::heights(i64 spacing_mm, i64 filter_mm, i32 i0, i32 j0, u32 nx, u32 nz,
                      u32 block_begin, u32 block_end, std::span<f32> out) const noexcept {
  if (!is_open() || spacing_mm < 1 || out.size() != static_cast<usize>(nx) * nz) return false;
  if (block_begin > block_end || block_end > scene_gen::window_blocks(nx, nz)) return false;
  const Level* level = nullptr;
  i64 ratio = 0;  // world lattice index to the level's: times this; 0, the bilinear surface
  if (filter_mm > 0) {
    // A lattice filtered to its spacing reads the chain level built at it, point for point.
    if (filter_mm != spacing_mm) return false;
    for (const auto& l : levels_)
      if (l->filter != 0 && l->spacing == spacing_mm) level = l.get();
    if (level == nullptr) return false;
    ratio = 1;
  } else if (spacing_mm % levels_[0]->spacing == 0) {
    level = levels_[0].get();
    ratio = spacing_mm / levels_[0]->spacing;
  }
  Pins pins(*this);
  const u32 per_side = (nx + scene_gen::k_height_block - 1) / scene_gen::k_height_block;
  for (u32 b = block_begin; b < block_end; ++b) {
    const u32 bx = (b % per_side) * scene_gen::k_height_block;
    const u32 bz = (b / per_side) * scene_gen::k_height_block;
    const u32 ex = std::min(nx, bx + scene_gen::k_height_block);
    const u32 ez = std::min(nz, bz + scene_gen::k_height_block);
    for (u32 z = bz; z < ez; ++z) {
      const i64 j = i64{j0} + z;
      for (u32 x = bx; x < ex; ++x) {
        const i64 i = i64{i0} + x;
        const f64 mm = level != nullptr ? value(*level, i * ratio, j * ratio, pins)
                                        : point_mm_pinned(i * spacing_mm, j * spacing_mm, pins);
        if (!std::isfinite(mm)) return false;
        out[static_cast<u32>(z) * nx + x] = static_cast<f32>(mm / 1000.0);
      }
    }
  }
  return true;
}

// Atomics only: a caller reading the cache's numbers waits for nobody.
TileSet::CacheStats TileSet::cache_stats() const noexcept {
  const u32 resident = counters_->resident.load(std::memory_order_relaxed);
  return CacheStats{cache_bytes_, cache_->payload_bytes * resident, resident,
                    counters_->evictions.load(std::memory_order_relaxed)};
}

namespace {

bool set_heights(const void* state, f64, i64 spacing_mm, i64 filter_mm, i32 i0, i32 j0, u32 nx,
                 u32 nz, u32 block_begin, u32 block_end, std::span<f32> out) noexcept {
  return static_cast<const TileSet*>(state)->heights(spacing_mm, filter_mm, i0, j0, nx, nz,
                                                     block_begin, block_end, out);
}
constexpr scene_gen::TileSourceOps k_set_ops{.heights = &set_heights, .travel_m = nullptr};

}  // namespace

scene_gen::TileSource TileSet::source() const noexcept {
  return is_open() ? scene_gen::TileSource{&k_set_ops, this} : scene_gen::TileSource{};
}

const TileSet* tile_set_of(const scene_gen::TileSource& source) noexcept {
  if (source.ops() != &k_set_ops || source.state() == nullptr) return nullptr;
  return static_cast<const TileSet*>(source.state());
}

}  // namespace engine::tileset
