// The scene-generator registry and the handles over a generator's state (scene_gen.h, sky.h).
#include <core/base/assert.h>
#include <domain/scene_gen/scene_gen.h>
#include <domain/scene_gen/sky.h>

#include <algorithm>

namespace engine::scene_gen {

// ---- lattices -----------------------------------------------------------------------------------

f32 Lattice::x(i32 i) const noexcept {
  if (scene_grid)
    return -extent + 2.0f * extent * (static_cast<f32>(i) / static_cast<f32>(size - 1));
  return static_cast<f32>(static_cast<f64>(i64{i} * spacing_mm) / 1000.0);
}

f32 Lattice::z(i32 j) const noexcept { return x(j); }

Lattice scene_lattice(f32 extent, u32 size) noexcept {
  Lattice l;
  l.scene_grid = true;
  l.extent = extent;
  l.size = std::max(size, 2u);
  l.origin_x = -static_cast<f64>(extent);
  l.origin_z = -static_cast<f64>(extent);
  l.spacing = 2.0 * static_cast<f64>(extent) / static_cast<f64>(l.size - 1);
  return l;
}

Lattice ring_lattice(i64 spacing_mm, i64 filter_mm) noexcept {
  Lattice l;
  l.spacing_mm = spacing_mm > 0 ? spacing_mm : 1;
  l.spacing = static_cast<f64>(l.spacing_mm) / 1000.0;
  l.filter_mm = filter_mm > 0 ? filter_mm : 0;
  return l;
}

u32 window_blocks(u32 nx, u32 nz) noexcept {
  return ((nx + k_height_block - 1) / k_height_block) *
         ((nz + k_height_block - 1) / k_height_block);
}

// ---- the handles --------------------------------------------------------------------------------

GroundRings& GroundRings::operator=(GroundRings&& other) noexcept {
  if (this != &other) {
    reset();
    ops_ = other.ops_;
    state_ = other.state_;
    other.ops_ = nullptr;
    other.state_ = nullptr;
  }
  return *this;
}

void GroundRings::reset() noexcept {
  if (ops_ != nullptr && ops_->destroy != nullptr) ops_->destroy(state_);
  ops_ = nullptr;
  state_ = nullptr;
}

GroundTiles& GroundTiles::operator=(GroundTiles&& other) noexcept {
  if (this != &other) {
    reset();
    ops_ = other.ops_;
    state_ = other.state_;
    other.ops_ = nullptr;
    other.state_ = nullptr;
  }
  return *this;
}

void GroundTiles::reset() noexcept {
  if (ops_ != nullptr && ops_->destroy != nullptr) ops_->destroy(state_);
  ops_ = nullptr;
  state_ = nullptr;
}

GroundProvider& GroundProvider::operator=(GroundProvider&& other) noexcept {
  if (this != &other) {
    reset();
    ops_ = other.ops_;
    state_ = other.state_;
    other.ops_ = nullptr;
    other.state_ = nullptr;
  }
  return *this;
}

void GroundProvider::reset() noexcept {
  if (ops_ != nullptr && ops_->destroy != nullptr) ops_->destroy(state_);
  ops_ = nullptr;
  state_ = nullptr;
}

void GroundProvider::grid(const Lattice& lattice, i32 i0, i32 j0, u32 nx, u32 nz,
                          std::span<f32> heights) const noexcept {
  if (ops_->grid != nullptr) {
    ops_->grid(state_, lattice, i0, j0, nx, nz, heights);
    return;
  }
  // A point at a time, rows of x in order of z: the order and the coordinates a caller asking
  // `height` at each lattice point would use, so the grid is those heights to the bit.
  for (u32 j = 0; j < nz; ++j) {
    const f32 z = lattice.z(j0 + static_cast<i32>(j));
    for (u32 i = 0; i < nx; ++i) {
      const f32 x = lattice.x(i0 + static_cast<i32>(i));
      heights[static_cast<usize>(j) * nx + i] = ops_->height(state_, x, z);
    }
  }
}

bool GroundProvider::make_rings(i64 extent_mm, i64 spacing_mm, GroundRings& out,
                                std::string* error) const {
  out.reset();
  if (!has_rings()) {
    if (error != nullptr) *error = "this ground has no rings";
    return false;
  }
  return ops_->make_rings(state_, extent_mm, spacing_mm, out, error);
}

bool GroundProvider::open_tiles(const TileRecords& records, i64 tile_mm, GroundTiles& out,
                                std::string* error) const {
  out.reset();
  if (!has_tiles()) {
    if (error != nullptr) *error = "this ground has no tiles a world could hold";
    return false;
  }
  return ops_->open_tiles(state_, records, tile_mm, out, error);
}

namespace {

f32 surface_of(const void* context, f32 x, f32 z) noexcept {
  return static_cast<const GroundProvider*>(context)->height(x, z);
}

f32 floor_of(const void* context, f32 x, f32 z) noexcept {
  return static_cast<const GroundProvider*>(context)->floor(x, z);
}

}  // namespace

Ground GroundProvider::view() const noexcept {
  if (!valid()) return Ground{};
  return Ground{&surface_of, &floor_of, this};
}

SkyProvider& SkyProvider::operator=(SkyProvider&& other) noexcept {
  if (this != &other) {
    reset();
    ops_ = other.ops_;
    state_ = other.state_;
    other.ops_ = nullptr;
    other.state_ = nullptr;
  }
  return *this;
}

void SkyProvider::reset() noexcept {
  if (ops_ != nullptr && ops_->destroy != nullptr) ops_->destroy(state_);
  ops_ = nullptr;
  state_ = nullptr;
}

// ---- a ground as a tile source (tile_source.h) --------------------------------------------------

bool TileSource::heights(f64 time_s, i64 spacing_mm, i32 i0, i32 j0, u32 nx, u32 nz,
                         std::span<f32> out) const noexcept {
  return heights(time_s, spacing_mm, i0, j0, nx, nz, 0, window_blocks(nx, nz), out);
}

bool TileSource::filtered(f64 time_s, i64 spacing_mm, i64 filter_mm, i32 i0, i32 j0, u32 nx, u32 nz,
                          std::span<f32> out) const noexcept {
  return filtered(time_s, spacing_mm, filter_mm, i0, j0, nx, nz, 0, window_blocks(nx, nz), out);
}

namespace {

// A moving ground: its re-evaluation at the time asked for, on the world's lattice.
bool moving_heights(const void* state, f64 time_s, i64 spacing_mm, i64 filter_mm, i32 i0, i32 j0,
                    u32 nx, u32 nz, u32 block_begin, u32 block_end, std::span<f32> out) noexcept {
  const auto& ground = *static_cast<const GroundProvider*>(state);
  return ground.evaluate(time_s, ring_lattice(spacing_mm, filter_mm), i0, j0, nx, nz, block_begin,
                         block_end, out);
}

// A still ground: its grid at its own time, whatever the time, a block at a time — the grid is the
// ground's `height` at each point, so a block's sub-window is the same heights as the whole.
bool still_heights(const void* state, f64, i64 spacing_mm, i64 filter_mm, i32 i0, i32 j0, u32 nx,
                   u32 nz, u32 block_begin, u32 block_end, std::span<f32> out) noexcept {
  const auto& ground = *static_cast<const GroundProvider*>(state);
  if (out.size() != static_cast<usize>(nx) * nz) return false;
  const Lattice lattice = ring_lattice(spacing_mm, filter_mm);
  const u32 per_side = (nx + k_height_block - 1) / k_height_block;
  f32 block[k_height_block * k_height_block];
  for (u32 b = block_begin; b < block_end; ++b) {
    const u32 bx = (b % per_side) * k_height_block;
    const u32 bz = (b / per_side) * k_height_block;
    const u32 w = std::min(nx - bx, k_height_block);
    const u32 h = std::min(nz - bz, k_height_block);
    ground.grid(lattice, i0 + static_cast<i32>(bx), j0 + static_cast<i32>(bz), w, h,
                std::span<f32>(block, static_cast<usize>(w) * h));
    for (u32 z = 0; z < h; ++z) {
      for (u32 x = 0; x < w; ++x)
        out[static_cast<usize>(bz + z) * nx + bx + x] = block[z * w + x];
    }
  }
  return true;
}

f64 ground_travel_m(const void* state, f64 from_s, f64 to_s, i64 filter_mm) noexcept {
  return static_cast<const GroundProvider*>(state)->travel_m(from_s, to_s, filter_mm);
}

constexpr TileSourceOps k_moving_tiles{.heights = &moving_heights, .travel_m = &ground_travel_m};
constexpr TileSourceOps k_still_tiles{.heights = &still_heights, .travel_m = nullptr};

}  // namespace

TileSource GroundProvider::tiles() const noexcept {
  if (!valid()) return TileSource{};
  return TileSource{moves() ? &k_moving_tiles : &k_still_tiles, this};
}

// ---- the registry -------------------------------------------------------------------------------

GeneratorRegistry& GeneratorRegistry::global() {
  // A function-local static, so a registrar in any translation unit finds it constructed whatever
  // the order static initialization runs the units in.
  static GeneratorRegistry registry;
  return registry;
}

namespace {

template <class Desc>
bool add_to(Vector<const Desc*>& list, const Desc& desc) noexcept {
  if (desc.name == nullptr || desc.name[0] == '\0') return false;
  const std::string_view name(desc.name);
  for (const Desc* existing : list) {
    if (std::string_view(existing->name) == name) return existing == &desc;
  }
  list.push_back(&desc);
  return true;
}

template <class Desc>
const Desc* find_in(const Vector<const Desc*>& list, std::string_view name) noexcept {
  for (const Desc* desc : list) {
    if (std::string_view(desc->name) == name) return desc;
  }
  return nullptr;
}

template <class Desc>
Vector<std::string_view> names_of(const Vector<const Desc*>& list) {
  Vector<std::string_view> out;
  out.reserve(list.size());
  for (const Desc* desc : list)
    out.push_back(std::string_view(desc->name));
  std::sort(out.begin(), out.end());
  return out;
}

std::string unknown(std::string_view kind, std::string_view name,
                    const Vector<std::string_view>& have) {
  std::string text = "names the " + std::string(kind) + " \"" + std::string(name) +
                     "\", which this build does not have: its capability is switched off or not "
                     "linked into this executable (it has ";
  if (have.empty()) {
    text += "none";
  } else {
    for (u32 i = 0; i < have.size(); ++i) {
      if (i > 0) text += ", ";
      text += std::string(have[i]);
    }
  }
  text += ")";
  return text;
}

}  // namespace

bool GeneratorRegistry::add(const GroundProviderDesc& desc) noexcept {
  if (desc.make == nullptr) return false;
  return add_to(grounds_, desc);
}

bool GeneratorRegistry::add(const PlacementGeneratorDesc& desc) noexcept {
  if (desc.open == nullptr || desc.close == nullptr || desc.expand == nullptr) return false;
  return add_to(placements_, desc);
}

const GroundProviderDesc* GeneratorRegistry::find_ground(std::string_view name) const noexcept {
  return find_in(grounds_, name);
}

const PlacementGeneratorDesc* GeneratorRegistry::find_placement(
    std::string_view name) const noexcept {
  return find_in(placements_, name);
}

bool GeneratorRegistry::add(const SkyProviderDesc& desc) noexcept {
  if (desc.make == nullptr) return false;
  return add_to(skies_, desc);
}

const SkyProviderDesc* GeneratorRegistry::find_sky(std::string_view name) const noexcept {
  return find_in(skies_, name);
}

Vector<std::string_view> GeneratorRegistry::ground_names() const { return names_of(grounds_); }

Vector<std::string_view> GeneratorRegistry::placement_names() const {
  return names_of(placements_);
}

Vector<std::string_view> GeneratorRegistry::sky_names() const { return names_of(skies_); }

std::string GeneratorRegistry::unknown_ground(std::string_view name) const {
  return unknown("ground provider", name, ground_names());
}

std::string GeneratorRegistry::unknown_placement(std::string_view name) const {
  return unknown("placement generator", name, placement_names());
}

std::string GeneratorRegistry::unknown_sky(std::string_view name) const {
  return unknown("sky provider", name, sky_names());
}

Registrar::Registrar(const GroundProviderDesc& desc) noexcept {
  ENGINE_VERIFY(GeneratorRegistry::global().add(desc),
                "scene_gen: a ground provider with no name or no make, or a second one under a "
                "name another holds");
}

Registrar::Registrar(const PlacementGeneratorDesc& desc) noexcept {
  ENGINE_VERIFY(GeneratorRegistry::global().add(desc),
                "scene_gen: a placement generator with no name, open, close or expand, or a second "
                "one under a name another holds");
}

Registrar::Registrar(const SkyProviderDesc& desc) noexcept {
  ENGINE_VERIFY(GeneratorRegistry::global().add(desc),
                "scene_gen: a sky provider with no name or no make, or a second one under a name "
                "another holds");
}

}  // namespace engine::scene_gen
