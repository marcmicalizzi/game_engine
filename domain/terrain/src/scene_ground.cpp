// The dunes as a ground provider (scene_ground.h; docs/subsystems/scene_gen.md). The field's
// conversion from the scene's terrain entry, the point and grid functions, the time-lapse's
// re-evaluation and the band travel were the renderer's (`terrain.cpp`, under
// `ENGINE_RENDERER_TERRAIN`), and the rings' wrapper was its `terrain_rings.cpp`'s; they moved here
// expression for expression, so the renderer's goldens and the capability's hold.
#include <core/jobs/job_system.h>
#include <domain/scene_gen/terrain_features.h>
#include <domain/terrain/ground_tiles.h>
#include <domain/terrain/rings.h>
#include <domain/terrain/scene_ground.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <span>

namespace engine::terrain {

namespace {

// The field, the time it is drawn at, and the entry's ridges and basins, which are added to its
// sand as the waves add them.
struct DunesGround {
  scene::Terrain entry;
  DuneField field;
  i64 time_us = 0;
};

i64 time_us_of(f64 time_s) noexcept {
  return static_cast<i64>(std::floor(time_s * 1'000'000.0 + 0.5));
}

// The ridges and basins, as one sum: `sand + features` is how the dunes' height has them.
f32 features(const DunesGround& g, f32 x, f32 z) noexcept {
  const scene_gen::TerrainFeatures f = scene_gen::terrain_features(
      g.entry.seed, std::span<const scene::Ridge>(g.entry.ridges.data(), g.entry.ridges.size()),
      std::span<const scene::Basin>(g.entry.basins.data(), g.entry.basins.size()), x, z);
  return f.ridges + f.basins;
}

f32 dunes_height(const void* state, f32 x, f32 z) noexcept {
  const auto& g = *static_cast<const DunesGround*>(state);
  // The generator thins and flattens its own sand over the same features (terrain.md, "Fixed
  // features"); the rock and the bowls are added here, as the waves have them.
  const f32 sand = height_m(g.field.height_um(to_mm(x), to_mm(z), g.time_us, Detail::dunes));
  return sand + features(g, x, z);
}

f32 dunes_floor(const void* state, f32 x, f32 z) noexcept {
  const auto& g = *static_cast<const DunesGround*>(state);
  const f32 floor = height_m(g.field.floor_um(to_mm(x), to_mm(z)));
  return floor + features(g, x, z);
}

bool dunes_evaluate(const void* state, f64 time_s, const scene_gen::Lattice& lattice, i32 i0,
                    i32 j0, u32 nx, u32 nz, u32 block_begin, u32 block_end,
                    std::span<f32> heights) noexcept {
  const auto& g = *static_cast<const DunesGround*>(state);
  if (heights.size() != static_cast<usize>(nx) * nz) return false;
  const i64 time_us = time_us_of(time_s);
  constexpr u32 k_block = scene_gen::k_height_block;
  const u32 per_side = (nx + k_block - 1) / k_block;
  // One gather per block, and the same heights as `height` (the generator's rule: any gather that
  // covers a point gives it the same primitives, which meet by their maximum and add across bands).
  Gather gather;
  for (u32 block = block_begin; block < block_end; ++block) {
    const u32 bx = (block % per_side) * k_block;
    const u32 bz = (block / per_side) * k_block;
    const u32 ez = std::min(nz, bz + k_block);
    const u32 ex = std::min(nx, bx + k_block);
    // A lattice that stands for more than its points (`Lattice::filter_mm`, a far level's) gathers
    // only the bands it carries and reads the rest as their means.
    g.field.gather(to_mm(lattice.x(i0 + static_cast<i32>(bx))),
                   to_mm(lattice.z(j0 + static_cast<i32>(bz))),
                   to_mm(lattice.x(i0 + static_cast<i32>(ex - 1))),
                   to_mm(lattice.z(j0 + static_cast<i32>(ez - 1))), time_us, nullptr,
                   lattice.filter_mm, gather);
    for (u32 zi = bz; zi < ez; ++zi) {
      const f32 z = lattice.z(j0 + static_cast<i32>(zi));
      for (u32 xi = bx; xi < ex; ++xi) {
        const f32 x = lattice.x(i0 + static_cast<i32>(xi));
        const f32 sand = height_m(g.field.height_um(gather, to_mm(x), to_mm(z), Detail::dunes));
        heights[static_cast<usize>(zi) * nx + xi] = sand + features(g, x, z);
      }
    }
  }
  return true;
}

void dunes_grid(const void* state, const scene_gen::Lattice& lattice, i32 i0, i32 j0, u32 nx,
                u32 nz, std::span<f32> heights) noexcept {
  const auto& g = *static_cast<const DunesGround*>(state);
  // The grid at the entry's own time, a block of vertices at a time: one gather per block rather
  // than one per vertex, and the same heights as `height`.
  dunes_evaluate(state, g.entry.time, lattice, i0, j0, nx, nz, 0, scene_gen::window_blocks(nx, nz),
                 heights);
}

f64 dunes_travel_m(const void* state, f64 from_s, f64 to_s, i64 filter_mm) noexcept {
  const auto& g = *static_cast<const DunesGround*>(state);
  const auto us = [](f64 s) { return static_cast<i64>(std::floor(s * 1'000'000.0 + 0.5)); };
  // The flux path length over the interval, cm^2, and each band's travel along it: cm^2 over the
  // band's celerity height in mm, times 100 for mm^2, which is Bagnold's rule as
  // `DuneField::displacement` takes it, applied to |flux| rather than to the vector. Of the bands
  // the lattice carries: the mean the rest stand in with does not move.
  const i64 magnitude = g.field.wind().between(us(from_s), us(to_s)).magnitude;
  f64 most = 0.0;
  for (u32 b = 0; b < g.field.band_count(); ++b) {
    if (!g.field.carries(b, filter_mm)) continue;
    const f64 mm = static_cast<f64>(magnitude) * 100.0 /
                   static_cast<f64>(std::max<i64>(1, g.field.band_height(b)));
    most = std::max(most, mm / 1000.0);
  }
  return std::abs(most);
}

// The ripples' transport at a game time: the wind record's flux path length since its epoch, cm^2
// to m^2 — the integral the dunes migrate by, storms' gains included — and the hour's strength over
// the record's mean (a storm's peak is 2.5).
bool dunes_transport(const void* state, f64 time_s, f64& moved_m2, f32& strength) noexcept {
  const auto& g = *static_cast<const DunesGround*>(state);
  const i64 t = time_us_of(time_s);
  moved_m2 = static_cast<f64>(g.field.wind().integral(t).magnitude) * 1e-4;
  strength = static_cast<f32>(static_cast<f64>(g.field.wind().wind_at(t).speed_q16) / 65536.0);
  return true;
}

// The ripples' wind at a game time: the field's own rule (`DuneField::ripple_wind`), which the
// renderer lays the ripples it draws across (renderer.md, "The sand close up").
bool dunes_wind(const void* state, f64 time_s, f32& x, f32& z) noexcept {
  const auto& g = *static_cast<const DunesGround*>(state);
  f64 dx = 1.0;
  f64 dz = 0.0;
  g.field.ripple_wind(time_us_of(time_s), dx, dz);
  x = static_cast<f32>(dx);
  z = static_cast<f32>(dz);
  return true;
}

// ---- the rings ----------------------------------------------------------------------------------

// The caller's heights, as the rings' builder asks for them.
class CallerHeights final : public RingHeights {
 public:
  scene_gen::RingHeights source;
  void heights(i64 x0, i64 z0, u32 nx, u32 nz, i64 spacing_mm, jobs::JobSystem* jobs,
               Vector<i64>& out_um) const override {
    source.fn(source.context, x0, z0, nx, nz, spacing_mm, jobs, out_um);
  }
};

struct DunesRings {
  RingParams params;
  CallerHeights source;
  TerrainRings rings;
};

void rings_destroy(void* rings) noexcept { delete static_cast<DunesRings*>(rings); }
u32 rings_count(const void* rings) noexcept {
  return static_cast<const DunesRings*>(rings)->params.count;
}
scene_gen::RingSpec rings_spec(const void* rings, u32 ring) noexcept {
  const RingSpec& s = static_cast<const DunesRings*>(rings)->params.ring[ring];
  return scene_gen::RingSpec{s.half_mm, s.spacing_mm, s.skirt_mm,
                             k_ring_chunk_cells * s.spacing_mm};
}
bool rings_reset(void* rings, i64 camera_x, i64 camera_z, const scene_gen::RingHeights& heights,
                 const geometry::ClusterLodOptions& options, jobs::JobSystem* jobs, u32 build_mask,
                 std::string* error) {
  auto& r = *static_cast<DunesRings*>(rings);
  r.source.source = heights;
  // No merged DAG: the renderer draws the chunks one by one and takes each chunk's geometry away.
  return r.rings.reset(r.params, camera_x, camera_z, r.source, options, jobs, error, build_mask,
                       false);
}
bool rings_update(void* rings, i64 camera_x, i64 camera_z, jobs::JobSystem* jobs, u32& rebuilt,
                  std::string* error) {
  return static_cast<DunesRings*>(rings)->rings.update(camera_x, camera_z, jobs, rebuilt, error);
}
void rings_layout(const void* rings, scene_gen::RingsLayout& out) noexcept {
  const RingLayout& l = static_cast<const DunesRings*>(rings)->rings.layout();
  out = scene_gen::RingsLayout{};
  out.count = l.count;
  for (u32 k = 0; k < l.count && k < scene_gen::k_max_rings; ++k)
    out.ring[k] = scene_gen::RingPlace{l.ring[k].cx, l.ring[k].cz, l.ring[k].half};
}
// The capability's own rule on a copy of the layout: a ring clamped at the scene's edge that the
// camera has left behind does not move, and asks for nothing. It reads the ring parameters and
// nothing `update` changes, so it may be asked while a re-centre is being built.
void rings_next_layout(const void* rings, i64 camera_x, i64 camera_z,
                       const scene_gen::RingsLayout& from, scene_gen::RingsLayout& out) noexcept {
  const RingParams& params = static_cast<const DunesRings*>(rings)->params;
  RingLayout copy;
  place_rings(params, 0, 0, copy);
  // The moving rings stand where `from` has them; the outer ring (the scene's grid) never moves.
  for (u32 k = 0; k + 1 < copy.count && k < from.count; ++k) {
    copy.ring[k].cx = from.ring[k].cx;
    copy.ring[k].cz = from.ring[k].cz;
    copy.ring[k].half = from.ring[k].half;
  }
  for (u32 k = 1; k < copy.count; ++k) {
    copy.ring[k].has_hole = true;
    copy.ring[k].hole_cx = copy.ring[k - 1].cx;
    copy.ring[k].hole_cz = copy.ring[k - 1].cz;
    copy.ring[k].hole_half = copy.ring[k - 1].half;
  }
  recentre_rings(params, camera_x, camera_z, copy);
  out = scene_gen::RingsLayout{};
  out.count = copy.count;
  for (u32 k = 0; k < copy.count && k < scene_gen::k_max_rings; ++k)
    out.ring[k] = scene_gen::RingPlace{copy.ring[k].cx, copy.ring[k].cz, copy.ring[k].half};
}
u32 rings_chunk_count(const void* rings, u32 ring) noexcept {
  return static_cast<const DunesRings*>(rings)->rings.chunks(ring).size();
}
scene_gen::RingChunkRef rings_chunk(void* rings, u32 ring, u32 index) noexcept {
  RingChunk& c = static_cast<DunesRings*>(rings)->rings.chunks_mut(ring)[index];
  return scene_gen::RingChunkRef{c.coord.i, c.coord.j, c.key, c.grid_vertices, &c.lod};
}
u32 rings_last_built(const void* rings) noexcept {
  return static_cast<const DunesRings*>(rings)->rings.last_built();
}
u32 rings_last_reused(const void* rings) noexcept {
  return static_cast<const DunesRings*>(rings)->rings.last_reused();
}

constexpr scene_gen::GroundRingsOps k_rings_ops{
    .destroy = &rings_destroy,
    .count = &rings_count,
    .spec = &rings_spec,
    .reset = &rings_reset,
    .update = &rings_update,
    .layout = &rings_layout,
    .next_layout = &rings_next_layout,
    .chunk_count = &rings_chunk_count,
    .chunk = &rings_chunk,
    .last_built = &rings_last_built,
    .last_reused = &rings_last_reused,
};

// The rings over a scene grid `extent_mm` either side at `spacing_mm`, from the ring tunables
// (`terrain.rings.*`): the scene's grid is their outer ring, which the caller draws itself.
bool dunes_make_rings(const void*, i64 extent_mm, i64 spacing_mm, scene_gen::GroundRings& out,
                      std::string* error) {
  auto made = std::make_unique<DunesRings>();
  made->params = ring_params_from_tunables(0, 0, extent_mm, spacing_mm);
  std::string why;
  if (!validate_rings(made->params, &why)) {
    if (error != nullptr) *error = why;
    return false;
  }
  if (made->params.count < 2) {
    if (error != nullptr)
      *error = "terrain rings: the ring tunables leave no ring inside the scene's grid";
    return false;
  }
  out = scene_gen::GroundRings(&k_rings_ops, made.release());
  return true;
}

// ---- the tiles in a world ----------------------------------------------------------------------

// The field's tiles a world's ground consumer holds, their overlays kept in the world's records
// (ground_tiles.h).
bool dunes_open_tiles(const void* state, const scene_gen::TileRecords& records, i64 tile_mm,
                      scene_gen::GroundTiles& out, std::string* error) {
  if (tile_mm <= 0) {
    if (error != nullptr) *error = "the dunes' tiles: a world's tile must be positive";
    return false;
  }
  out = make_dune_tiles(static_cast<const DunesGround*>(state)->field, records, tile_mm);
  return true;
}

// ---- the provider ------------------------------------------------------------------------------

void dunes_destroy(void* state) noexcept { delete static_cast<DunesGround*>(state); }

constexpr scene_gen::GroundOps k_dunes_ops{
    .destroy = &dunes_destroy,
    .height = &dunes_height,
    .floor = &dunes_floor,
    .grid = &dunes_grid,
    .evaluate = &dunes_evaluate,
    .travel_m = &dunes_travel_m,
    .wind = &dunes_wind,
    .transport = &dunes_transport,
    .make_rings = &dunes_make_rings,
    .open_tiles = &dunes_open_tiles,
    .record = k_overlay_record,
};

// The entry's generator fields checked as the scene reader always checked them, and the band table
// as the field would build it: a sentence, which the reader prefixes with the file.
bool dunes_make(const scene::Terrain& entry, const scene_gen::Context&,
                scene_gen::GroundProvider& out, std::string* error) {
  const auto fail = [&](std::string sentence) {
    if (error != nullptr) *error = std::move(sentence);
    return false;
  };
  if (!validate_terrain_entry(entry, error)) return false;
  const FieldDesc desc = field_desc_of(entry);
  if (entry.bands.has_value()) {
    if (desc.bands.empty())
      return fail("terrain.bands: the band table is empty: leave it out for the default");
    std::string why;
    if (!validate_bands(std::span<const BandDesc>(desc.bands.data(), desc.bands.size()), &why))
      return fail("terrain.bands: " + why);
  }
  out = scene_gen::GroundProvider(&k_dunes_ops,
                                  new DunesGround{entry, DuneField(desc), time_us_of(entry.time)});
  return true;
}

constexpr scene_gen::GroundProviderDesc k_dunes{
    .name = k_ground_provider,
    .make = &dunes_make,
    .flags = scene_gen::k_ground_moves | scene_gen::k_ground_rings | scene_gen::k_ground_tiles,
};
const scene_gen::Registrar k_dunes_registrar{k_dunes};

}  // namespace

bool validate_terrain_entry(const scene::Terrain& entry, std::string* error) {
  const auto fail = [&](std::string sentence) {
    if (error != nullptr) *error = std::move(sentence);
    return false;
  };
  if (!(entry.time >= 0.0) || !(entry.time < 3.0e11) || !(entry.sand_flux >= 0.0f) ||
      !(entry.sand_flux <= 100'000.0f) || entry.storms_per_year > 31 ||
      !(entry.storm_strength > 0.0f) || !(entry.storm_strength <= 3.0f)) {
    return fail(
        "the dune generator's time must be within [0, 3e11) s, its sand_flux within "
        "[0, 100000] m^2 a year, its storms_per_year within 0..31 and its storm_strength "
        "within (0, 3]");
  }
  const auto hour = [](f32 h) { return h >= 0.0f && h < 24.0f; };
  if (!(entry.diurnal_strength >= 0.0f && entry.diurnal_strength <= 2.0f) ||
      !hour(entry.diurnal_peak_hour) ||
      !(entry.diurnal_veer_deg >= 0.0f && entry.diurnal_veer_deg <= 90.0f) ||
      !hour(entry.diurnal_veer_hour)) {
    return fail(
        "the wind's day: diurnal_strength must be within [0, 2], diurnal_veer_deg within [0, 90] "
        "and the two hours within [0, 24)");
  }
  const auto gain = [](f32 g) { return g > 0.0f && g <= 1000.0f; };
  if (!gain(entry.storm_gain)) return fail("storm_gain must be within (0, 1000]");
  const u32 storms = entry.storms_per_year * static_cast<u32>(k_record_years);
  for (const scene::StormGain& g : entry.storm_gains) {
    if (!gain(g.gain)) return fail("storm_gains: a gain must be within (0, 1000]");
    if (g.storm >= storms) {
      return fail("storm_gains: storm " + std::to_string(g.storm) +
                  " is not in the record, which " + "holds " + std::to_string(storms) +
                  " (storms_per_year times " + std::to_string(k_record_years) + " years)");
    }
  }
  return true;
}

FieldDesc field_desc_of(const scene::Terrain& t) {
  FieldDesc f;
  f.seed = t.seed;
  f.wind.seed = t.seed;
  f.dune_height = to_mm(t.dune_height);
  f.wavelength = to_mm(t.dune_wavelength);
  // m^2 a year to cm^2 a day.
  f.wind.flux_cm2_per_day =
      static_cast<i32>(std::floor(static_cast<f64>(t.sand_flux) * 10'000.0 / 365.0 + 0.5));
  f.wind.storms_per_year = static_cast<i32>(t.storms_per_year);
  f.wind.storm_speed_q16 =
      static_cast<i32>(std::floor(static_cast<f64>(t.storm_strength) * 65'536.0 + 0.5));
  // The wind's day: hours to binary angles of the day, degrees to binary angles of a turn.
  const auto q16 = [](f32 v) {
    return static_cast<i32>(std::floor(static_cast<f64>(v) * 65'536.0 + 0.5));
  };
  const auto day_angle = [](f32 hour) {
    return static_cast<u16>(
        static_cast<i64>(std::floor(static_cast<f64>(hour) * 65'536.0 / 24.0 + 0.5)) & 0xFFFF);
  };
  f.wind.diurnal_q16 = q16(t.diurnal_strength);
  f.wind.diurnal_peak_turn = day_angle(t.diurnal_peak_hour);
  f.wind.veer_turn =
      static_cast<i32>(std::floor(static_cast<f64>(t.diurnal_veer_deg) * 65'536.0 / 360.0 + 0.5));
  f.wind.veer_phase_turn = day_angle(t.diurnal_veer_hour);
  f.wind.storm_gain_q16 = q16(t.storm_gain);
  for (const scene::StormGain& g : t.storm_gains)
    f.wind.storm_gains.push_back(WindParams::StormGain{static_cast<i32>(g.storm), q16(g.gain)});
  for (const scene::Ridge& r : t.ridges) {
    f.ridges.push_back(RidgeFeature{to_mm(r.from.x), to_mm(r.from.y), to_mm(r.to.x), to_mm(r.to.y),
                                    to_mm(r.width)});
  }
  for (const scene::Basin& b : t.basins)
    f.basins.push_back(BasinFeature{to_mm(b.center.x), to_mm(b.center.y), to_mm(b.radius)});
  if (t.bands.has_value()) {
    for (const scene::TerrainBand& band : *t.bands) {
      BandMetres m;
      m.name = band.name;
      m.kind = band.kind == scene::DuneKind::Barchan ? PrimitiveKind::barchan
                                                     : PrimitiveKind::transverse;
      m.height_min = band.height_min;
      m.height_max = band.height_max;
      m.cell = band.cell;
      m.share = band.share;
      m.length_min = band.length_min;
      m.length_max = band.length_max;
      m.stoss = band.stoss;
      m.bend = band.bend;
      m.sinuosity = band.sinuosity;
      m.spread_deg = band.spread_deg;
      m.sharpness = band.sharpness;
      m.side_days = band.side_days;
      m.sharp_days = band.sharp_days;
      const u8 couple = static_cast<u8>(band.couple);
      m.couple = static_cast<BandCouple>(couple <= 2 ? couple : 0);
      m.couple_width = band.couple_width;
      m.far = band.far;
      m.celerity_scale = band.celerity_scale;
      f.bands.push_back(band_from_metres(m));
    }
  }
  return f;
}

const DuneField* dune_field(const scene_gen::GroundProvider& ground) noexcept {
  if (ground.ops() != &k_dunes_ops) return nullptr;
  return &static_cast<const DunesGround*>(ground.state())->field;
}

}  // namespace engine::terrain
