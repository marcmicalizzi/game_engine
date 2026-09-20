#include <domain/geometry/stress_mesh.h>

#include <algorithm>
#include <cmath>
#include <string>

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

// ---- the morph fixture -------------------------------------------------------------------------

namespace {

// The sphere's surface point and normal at parameter (u, v), u around and v pole to pole. The
// normal of a unit sphere is the direction, which is what makes the analytic displaced normal
// below cheap to write down.
Vec3 sphere_direction(f32 u, f32 v) noexcept {
  const f32 phi = u * k_two_pi;
  const f32 theta = v * 3.14159265358979323846f;
  const f32 st = std::sin(theta);
  return Vec3{st * std::cos(phi), std::cos(theta), st * std::sin(phi)};
}

}  // namespace

void build_morph_sphere(const MorphFixtureOptions& options, MorphFixtureMesh& out) {
  out = MorphFixtureMesh{};
  const u32 segments = options.segments < 3 ? 3u : options.segments;
  const u32 rings = options.rings < 2 ? 2u : options.rings;
  const f32 radius = options.radius > 0.0f ? options.radius : 1.0f;

  // A grid of (segments + 1) x (rings + 1) vertices. The u = 0 and u = 1 columns are the same
  // point of the sphere held by two vertices, which is the seam every UV sphere has and which
  // `split_seam` turns into a *morph* discontinuity as well as a UV one.
  const u32 columns = segments + 1;
  for (u32 r = 0; r <= rings; ++r) {
    const f32 v = static_cast<f32>(r) / static_cast<f32>(rings);
    for (u32 c = 0; c < columns; ++c) {
      const f32 u = static_cast<f32>(c) / static_cast<f32>(segments);
      // The wrap column is the **same point** as column 0, computed the same way rather than at
      // u = 1: `cos(2 pi)` in single precision is not exactly `cos(0)`, and two positions that
      // differ in the last bit are two positions to every weld in the tree, which would make the
      // fixture's seam a position seam instead of the pure attribute seam it is meant to be.
      const Vec3 direction = sphere_direction(c == segments ? 0.0f : u, v);
      out.positions.push_back(direction * radius);
      out.normals.push_back(direction);
      out.uvs.push_back(Vec2{u, v});
    }
  }
  for (u32 r = 0; r < rings; ++r) {
    for (u32 c = 0; c < segments; ++c) {
      const u32 a = r * columns + c;
      out.indices.push_back(a);
      out.indices.push_back(a + columns);
      out.indices.push_back(a + 1);
      out.indices.push_back(a + 1);
      out.indices.push_back(a + columns);
      out.indices.push_back(a + columns + 1);
    }
  }

  // The channels. Each is a Gaussian in the angle between the vertex direction and the channel's
  // centre, displacing along the normal; its normal delta is the analytic normal of the displaced
  // surface minus the rest one, so a test comparing delta normals against the truth has a truth.
  const u32 vertex_count = out.positions.size();
  const f32 peak = options.amplitude * radius;
  const f32 sigma = options.falloff > 1.0e-4f ? options.falloff : 1.0e-4f;
  u32 state = options.seed == 0 ? 1u : options.seed;
  for (u32 k = 0; k < options.channels; ++k) {
    // Centres spread deterministically over the sphere: a golden-angle spiral, jittered by the
    // seed so two rigs of the same shape are not the same rig.
    const f32 jitter = static_cast<f32>(next_random(state) & 0xffffu) / 65535.0f;
    const f32 cv = (static_cast<f32>(k) + 0.5f + 0.25f * (jitter - 0.5f)) /
                   static_cast<f32>(options.channels == 0 ? 1u : options.channels);
    const f32 cu = std::fmod(static_cast<f32>(k) * 0.6180339887f + jitter * 0.1f, 1.0f);
    const Vec3 centre = sphere_direction(cu, cv);
    MorphChannelSource channel;
    channel.name = "channel" + std::to_string(k);
    channel.default_weight = 0.0f;
    for (u32 i = 0; i < vertex_count; ++i) {
      const Vec3 direction = out.normals[i];
      // `split_seam` makes the u = 1 column's deltas differ from the u = 0 column's by a fixed
      // factor, so the two coincident vertices disagree about every channel that reaches them.
      const bool second_seam_copy = options.split_seam && (i % columns) == segments;
      const f32 cosine = dot(direction, centre);
      const f32 clamped = cosine < -1.0f ? -1.0f : (cosine > 1.0f ? 1.0f : cosine);
      const f32 angle = std::acos(clamped);
      f32 weight = std::exp(-0.5f * (angle / sigma) * (angle / sigma));
      if (second_seam_copy) weight *= 0.5f;
      if (weight < options.cutoff) continue;
      const f32 height = peak * weight;
      channel.vertices.push_back(i);
      channel.position_deltas.push_back(direction * height);
      if (options.normals) {
        // r(theta) = radius + height(theta) displaced radially: the displaced normal tilts by the
        // surface gradient of the height field. Taking it numerically along the two parameter
        // directions is both simpler and exactly what a DCC tool writes.
        const f32 u = static_cast<f32>(i % columns) / static_cast<f32>(segments);
        const f32 v = static_cast<f32>(i / columns) / static_cast<f32>(rings);
        const f32 du = 1.0f / static_cast<f32>(segments);
        const f32 dv = 1.0f / static_cast<f32>(rings);
        auto displaced = [&](f32 su, f32 sv) {
          const Vec3 d = sphere_direction(su, sv);
          const f32 a = std::acos(std::min(1.0f, std::max(-1.0f, dot(d, centre))));
          const f32 w = std::exp(-0.5f * (a / sigma) * (a / sigma));
          return d * (radius + peak * (w < options.cutoff ? 0.0f : w));
        };
        const Vec3 p = displaced(u, v);
        const Vec3 tu = displaced(std::fmod(u + du, 1.0f), v) - p;
        const Vec3 tv = displaced(u, v + dv <= 1.0f ? v + dv : v - dv) - p;
        Vec3 moved = cross(tu, tv);
        if (v + dv > 1.0f) moved = moved * -1.0f;
        if (length_squared(moved) > 1.0e-20f) {
          moved = normalize(moved);
          if (dot(moved, direction) < 0.0f) moved = moved * -1.0f;
        } else {
          moved = direction;
        }
        channel.normal_deltas.push_back(moved - direction);
      }
    }
    out.morph.push_back(channel);
  }

  // How many vertices carry a delta that their position twin disagrees about — which is what the
  // weld key and `ClusterLodOptions::morph_seams` are there to keep apart.
  Vector<u32> keys;
  morph_vertex_keys(std::span<const MorphChannelSource>(out.morph.data(), out.morph.size()),
                    vertex_count, keys);
  for (u32 r = 0; r <= rings; ++r) {
    const u32 first = r * columns;
    const u32 last = first + segments;
    if (keys[first] != keys[last]) ++out.morph_seam_vertices;
  }
}

Vec3 morph_fixture_position(const MorphFixtureMesh& mesh, u32 vertex,
                            std::span<const f32> weights) noexcept {
  if (vertex >= mesh.positions.size()) return Vec3{};
  Vec3 p = mesh.positions[vertex];
  for (u32 c = 0; c < mesh.morph.size() && c < weights.size(); ++c) {
    const MorphChannelSource& channel = mesh.morph[c];
    for (u32 i = 0; i < channel.vertices.size(); ++i) {
      if (channel.vertices[i] != vertex) continue;
      p = p + channel.position_deltas[i] * weights[c];
      break;
    }
  }
  return p;
}

Vec3 morph_fixture_normal(const MorphFixtureMesh& mesh, u32 vertex,
                          std::span<const f32> weights) noexcept {
  if (vertex >= mesh.normals.size()) return Vec3{0.0f, 1.0f, 0.0f};
  Vec3 n = mesh.normals[vertex];
  for (u32 c = 0; c < mesh.morph.size() && c < weights.size(); ++c) {
    const MorphChannelSource& channel = mesh.morph[c];
    if (channel.normal_deltas.size() != channel.vertices.size()) continue;
    for (u32 i = 0; i < channel.vertices.size(); ++i) {
      if (channel.vertices[i] != vertex) continue;
      n = n + channel.normal_deltas[i] * weights[c];
      break;
    }
  }
  return length_squared(n) > 1.0e-20f ? normalize(n) : mesh.normals[vertex];
}

}  // namespace engine::geometry
