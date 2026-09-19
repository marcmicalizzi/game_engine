#include <domain/geometry/stress_mesh.h>

#include <cmath>

namespace engine::geometry {

namespace {

constexpr f32 k_two_pi = 6.28318530717958647692f;

// xorshift32: a permutation of the islands has to be a function of the seed alone, and this is
// the smallest generator that is reproducible on every compiler without <random>'s engines
// (whose distributions are not specified to agree across standard libraries).
u32 next_random(u32& state) noexcept {
  state ^= state << 13;
  state ^= state >> 17;
  state ^= state << 5;
  return state;
}

// A saturated colour per atlas slot. Hue from the slot's index through a large odd stride, so
// neighbouring slots are far apart in hue and a collapse that crosses one slot into the next is
// as visible as one that crosses the whole atlas.
void slot_colour(u32 slot, u32 slot_count, f32& r, f32& g, f32& b) noexcept {
  const u32 spread = slot_count == 0 ? 1u : slot_count;
  const f32 hue =
      static_cast<f32>((u64{slot} * 2654435761ull) % u64{spread}) / static_cast<f32>(spread) * 6.0f;
  const u32 sector = static_cast<u32>(hue) % 6u;
  const f32 fraction = hue - std::floor(hue);
  const f32 rising = fraction;
  const f32 falling = 1.0f - fraction;
  switch (sector) {
    case 0:
      r = 1.0f;
      g = rising;
      b = 0.0f;
      break;
    case 1:
      r = falling;
      g = 1.0f;
      b = 0.0f;
      break;
    case 2:
      r = 0.0f;
      g = 1.0f;
      b = rising;
      break;
    case 3:
      r = 0.0f;
      g = falling;
      b = 1.0f;
      break;
    case 4:
      r = rising;
      g = 0.0f;
      b = 1.0f;
      break;
    default:
      r = 1.0f;
      g = 0.0f;
      b = falling;
      break;
  }
}

}  // namespace

void build_shredded_atlas_torus(const ShreddedAtlasOptions& options, ShreddedAtlasMesh& out) {
  out = ShreddedAtlasMesh{};
  const u32 side = options.island_side == 0 ? 1u : options.island_side;
  const u32 segments = ((options.segments == 0 ? 1u : options.segments) + side - 1) / side * side;
  const u32 rings = ((options.rings == 0 ? 1u : options.rings) + side - 1) / side * side;
  const u32 islands_u = segments / side;
  const u32 islands_v = rings / side;
  const u32 island_count = islands_u * islands_v;
  u32 cells = options.atlas_cells;
  while (u64{cells} * cells < u64{island_count})
    ++cells;
  if (cells == 0) cells = 1;
  out.island_count = island_count;
  out.atlas_cells = cells;

  // Which atlas slot each island lands in. A shuffle rather than an ordered layout, because an
  // ordered one would put neighbouring islands in neighbouring slots and a collapse across an
  // island edge would then land somewhere that looks almost right.
  const u32 slot_count = cells * cells;
  Vector<u32> slot_of_island(slot_count);
  for (u32 i = 0; i < slot_count; ++i)
    slot_of_island[i] = i;
  u32 state = options.seed == 0 ? 1u : options.seed;
  for (u32 i = slot_count; i > 1; --i) {
    const u32 j = next_random(state) % i;
    const u32 swap = slot_of_island[i - 1];
    slot_of_island[i - 1] = slot_of_island[j];
    slot_of_island[j] = swap;
  }

  const f32 cell_size = 1.0f / static_cast<f32>(cells);
  const f32 margin = cell_size * options.island_margin;
  const f32 usable = cell_size - 2.0f * margin;
  const u32 island_vertices = (side + 1) * (side + 1);
  out.positions.reserve(island_count * island_vertices);
  out.normals.reserve(island_count * island_vertices);
  out.uvs.reserve(island_count * island_vertices);
  out.indices.reserve(island_count * side * side * 6);

  for (u32 iv = 0; iv < islands_v; ++iv) {
    for (u32 iu = 0; iu < islands_u; ++iu) {
      const u32 island = iv * islands_u + iu;
      const u32 slot = slot_of_island[island];
      const f32 slot_u = static_cast<f32>(slot % cells) * cell_size + margin;
      const f32 slot_v = static_cast<f32>(slot / cells) * cell_size + margin;
      const u32 base = out.positions.size();
      for (u32 lv = 0; lv <= side; ++lv) {
        for (u32 lu = 0; lu <= side; ++lu) {
          // The position is a function of the *global* parameter with the wrap taken modulo, so
          // every copy of a shared vertex is bit-identical: the two sides of a seam land on the
          // same float, which is what a well-behaved exporter writes and what the weld keys on.
          const u32 gu = (iu * side + lu) % segments;
          const u32 gv = (iv * side + lv) % rings;
          const f32 phi = k_two_pi * static_cast<f32>(gu) / static_cast<f32>(segments);
          const f32 theta = k_two_pi * static_cast<f32>(gv) / static_cast<f32>(rings);
          const f32 cos_theta = std::cos(theta);
          const f32 sin_theta = std::sin(theta);
          const f32 cos_phi = std::cos(phi);
          const f32 sin_phi = std::sin(phi);
          const f32 ring = options.major_radius + options.minor_radius * cos_theta;
          out.positions.push_back(
              Vec3{ring * cos_phi, options.minor_radius * sin_theta, ring * sin_phi});
          out.normals.push_back(Vec3{cos_theta * cos_phi, sin_theta, cos_theta * sin_phi});
          out.uvs.push_back(Vec2{slot_u + usable * static_cast<f32>(lu) / static_cast<f32>(side),
                                 slot_v + usable * static_cast<f32>(lv) / static_cast<f32>(side)});
          if (lu == 0 || lu == side || lv == 0 || lv == side) ++out.seam_vertices;
        }
      }
      for (u32 lv = 0; lv < side; ++lv) {
        for (u32 lu = 0; lu < side; ++lu) {
          const u32 a = base + lv * (side + 1) + lu;
          const u32 b = a + 1;
          const u32 c = a + (side + 1);
          const u32 d = c + 1;
          out.indices.push_back(a);
          out.indices.push_back(c);
          out.indices.push_back(b);
          out.indices.push_back(b);
          out.indices.push_back(c);
          out.indices.push_back(d);
        }
      }
    }
  }
}

void build_atlas_probe_texture(u32 size, u32 atlas_cells, Vector<u8>& rgba) {
  rgba.clear();
  if (size == 0) return;
  const u32 cells = atlas_cells == 0 ? 1u : atlas_cells;
  rgba.resize(size * size * 4, 0u);
  for (u32 y = 0; y < size; ++y) {
    for (u32 x = 0; x < size; ++x) {
      const u32 cx = x * cells / size;
      const u32 cy = y * cells / size;
      const u32 slot = cy * cells + cx;
      f32 r = 0.0f;
      f32 g = 0.0f;
      f32 b = 0.0f;
      slot_colour(slot, cells * cells, r, g, b);
      // A gentle gradient inside the slot, so a UV error small enough to stay in its own island
      // still shows as a shade rather than as nothing at all.
      const f32 texels_per_cell = static_cast<f32>(size) / static_cast<f32>(cells);
      const f32 local_x =
          (static_cast<f32>(x) - static_cast<f32>(cx) * texels_per_cell) / texels_per_cell;
      const f32 local_y =
          (static_cast<f32>(y) - static_cast<f32>(cy) * texels_per_cell) / texels_per_cell;
      const f32 shade = 0.65f + 0.35f * (0.5f * local_x + 0.5f * local_y);
      u8* texel = &rgba[(y * size + x) * 4];
      texel[0] = static_cast<u8>(r * shade * 255.0f + 0.5f);
      texel[1] = static_cast<u8>(g * shade * 255.0f + 0.5f);
      texel[2] = static_cast<u8>(b * shade * 255.0f + 0.5f);
      texel[3] = 255u;
    }
  }
}

}  // namespace engine::geometry
