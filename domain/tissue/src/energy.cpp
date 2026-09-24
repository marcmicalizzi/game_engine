#include "energy.h"

#include <algorithm>
#include <cmath>

namespace engine::tissue::energy {
namespace {

using query::D3;

// The six edges of a tetrahedron, as corner pairs.
constexpr u32 k_edges[6][2] = {{0, 1}, {0, 2}, {0, 3}, {1, 2}, {1, 3}, {2, 3}};

// The gradient of a tetrahedron's signed volume (query::tet_volume's convention) with respect to
// each corner.
void volume_gradient(D3 a, D3 b, D3 c, D3 d, D3 out[4]) noexcept {
  const D3 ab = b - a;
  const D3 ac = c - a;
  const D3 ad = d - a;
  out[1] = query::cross(ac, ad) * (1.0 / 6.0);
  out[2] = query::cross(ad, ab) * (1.0 / 6.0);
  out[3] = query::cross(ab, ac) * (1.0 / 6.0);
  out[0] = (out[1] + out[2] + out[3]) * -1.0;
}

}  // namespace

bool BulkEdgeEnergy::build(std::span<const D3> rest, std::span<const u32> tets,
                           std::span<const CellMaterial> materials, std::string* error) {
  const auto refuse = [&](std::string why) {
    if (error != nullptr) *error = std::move(why);
    return false;
  };
  const u32 cells = static_cast<u32>(tets.size() / 4);
  if (materials.size() != cells) return refuse("one material per tetrahedron is needed");
  tets_.clear();
  rest_volume_.clear();
  volume_modulus_.clear();
  struct Entry {
    u64 key;
    f64 weight;
  };
  Vector<Entry> entries;
  for (u32 t = 0; t < cells; ++t) {
    const u32* v = tets.data() + 4 * t;
    for (u32 k = 0; k < 4; ++k) {
      if (v[k] >= rest.size())
        return refuse("tetrahedron " + std::to_string(t) + " names a node past the rest's");
      tets_.push_back(v[k]);
    }
    const f64 volume = query::tet_volume(rest[v[0]], rest[v[1]], rest[v[2]], rest[v[3]]);
    if (!(volume > 0.0))
      return refuse("tetrahedron " + std::to_string(t) + " has no positive rest volume");
    const CellMaterial& m = materials[t];
    const f64 bulk = m.bulk_modulus_pa - 5.0 * m.shear_modulus_pa / 3.0;
    if (!(m.shear_modulus_pa >= 0.0) || !(bulk >= 0.0))
      return refuse("tetrahedron " + std::to_string(t) +
                    "'s material has K < 5 mu / 3 (Poisson's ratio below 1/4), which a network of "
                    "edges cannot represent");
    rest_volume_.push_back(volume);
    volume_modulus_.push_back(bulk);
    for (const auto& e : k_edges) {
      const u32 a = std::min(v[e[0]], v[e[1]]);
      const u32 b = std::max(v[e[0]], v[e[1]]);
      entries.push_back(Entry{(u64{a} << 32) | u64{b}, 1.25 * m.shear_modulus_pa * volume});
    }
  }
  // Sorted by key, stable, so each edge's weight is summed in tetrahedron order.
  std::stable_sort(entries.begin(), entries.end(),
                   [](const Entry& x, const Entry& y) { return x.key < y.key; });
  edge_a_.clear();
  edge_b_.clear();
  edge_rest_.clear();
  edge_weight_.clear();
  for (u32 i = 0; i < entries.size();) {
    f64 weight = 0.0;
    u32 j = i;
    for (; j < entries.size() && entries[j].key == entries[i].key; ++j)
      weight += entries[j].weight;
    const u32 a = static_cast<u32>(entries[i].key >> 32);
    const u32 b = static_cast<u32>(entries[i].key & 0xffffffffu);
    const f64 length = query::length(rest[a] - rest[b]);
    if (!(length > 0.0))
      return refuse("edge (" + std::to_string(a) + ", " + std::to_string(b) +
                    ") has no rest length");
    edge_a_.push_back(a);
    edge_b_.push_back(b);
    edge_rest_.push_back(length);
    edge_weight_.push_back(weight);
    i = j;
  }
  return true;
}

BulkEdgeEnergy::Terms BulkEdgeEnergy::energy(std::span<const D3> x) const {
  Terms out;
  for (u32 t = 0; t < rest_volume_.size(); ++t) {
    const u32* v = tets_.data() + 4 * t;
    const f64 dv = query::tet_volume(x[v[0]], x[v[1]], x[v[2]], x[v[3]]) - rest_volume_[t];
    out.bulk += volume_modulus_[t] * dv * dv / (2.0 * rest_volume_[t]);
  }
  for (u32 e = 0; e < edge_rest_.size(); ++e) {
    const f64 strain =
        (query::length(x[edge_a_[e]] - x[edge_b_[e]]) - edge_rest_[e]) / edge_rest_[e];
    out.edge += edge_weight_[e] * strain * strain;
  }
  return out;
}

void BulkEdgeEnergy::add_gradient(std::span<const D3> x, std::span<D3> bulk,
                                  std::span<D3> edge) const {
  for (u32 t = 0; t < rest_volume_.size(); ++t) {
    const u32* v = tets_.data() + 4 * t;
    const f64 volume = query::tet_volume(x[v[0]], x[v[1]], x[v[2]], x[v[3]]);
    const f64 scale = volume_modulus_[t] * (volume - rest_volume_[t]) / rest_volume_[t];
    D3 g[4];
    volume_gradient(x[v[0]], x[v[1]], x[v[2]], x[v[3]], g);
    for (u32 k = 0; k < 4; ++k)
      bulk[v[k]] = bulk[v[k]] + g[k] * scale;
  }
  for (u32 e = 0; e < edge_rest_.size(); ++e) {
    const D3 d = x[edge_a_[e]] - x[edge_b_[e]];
    const f64 length = query::length(d);
    if (!(length > 0.0))
      continue;  // a collapsed edge has no direction; its gradient is a subgradient
    const f64 l0 = edge_rest_[e];
    const f64 scale = 2.0 * edge_weight_[e] * (length - l0) / (l0 * l0 * length);
    edge[edge_a_[e]] = edge[edge_a_[e]] + d * scale;
    edge[edge_b_[e]] = edge[edge_b_[e]] - d * scale;
  }
}

}  // namespace engine::tissue::energy
