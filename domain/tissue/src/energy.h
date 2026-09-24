#pragma once

// The declared energy of a tetrahedral region and its exact gradient: what the equilibrium-gap row
// evaluates (docs/subsystems/tissue.md, "The equilibrium gap"). Private to the module;
// content-build code, in double, deterministic (every sum runs in tetrahedron or sorted-edge
// order).
//
// The law is named, `bulk-edge-v0`, because a gap is only comparable between two implementations
// that evaluate the same energy. It is built from the declared moduli alone (a definition carries
// physical moduli and never compliances), per tetrahedron t of rest volume V0, bulk modulus K and
// shear modulus mu:
//
//     bulk:   (K - 5 mu / 3) (V - V0)^2 / (2 V0)
//     edges:  for each of its six edges, (5/4) mu V0 ((l - l0) / l0)^2
//
// which are the energies of an XPBD volume constraint (C = 6V - 6V0, compliance 36 V0 / K', the
// conversion physics/deformable.h derives) and of a distance constraint per edge (C = l - l0), so
// the gap is measured on the energy those constraints stand for. The constants are what make the
// declared (K, mu) mean what they say, exactly and for any cell shape, at small strain:
//
//   - A dilation strains every edge alike, so the edges add 6 * (5/4) mu V0 s^2 = (5/3) mu (3s)^2
//     V0 / 2 to the bulk term's (K - 5 mu / 3) (3s)^2 V0 / 2: the cell's bulk modulus is K.
//   - Averaged over the five independent pure shears (an orthonormal basis of traceless strains),
//     the edges' energy is (5/4) mu V0 * 6 * (2/3) / 5 |e|^2 = mu V0 |e|^2, the continuum's,
//     because each edge's strain is d.e.d and |dev(d d^T)|^2 = 2/3 for a unit d whatever the cell's
//     shape.
//
// What it is not: a network of central springs is anisotropic cell by cell (a regular
// tetrahedron's edges carry 1.25 mu along the three off-diagonal shears and 0.625 mu along the two
// diagonal ones), which is the affine drift the authoring side measured on its own edge network,
// and it cannot represent Poisson's ratio below 1/4 (Cauchy's relation), which is why `build`
// refuses K < 5 mu / 3. Soft tissue sits near 0.5.

#include "mesh_query.h"

#include <core/base/types.h>
#include <core/containers/vector.h>

#include <span>
#include <string>

namespace engine::tissue::energy {

inline constexpr const char* k_law = "bulk-edge-v0";

struct CellMaterial {
  f64 bulk_modulus_pa = 0.0;
  f64 shear_modulus_pa = 0.0;
};

class BulkEdgeEnergy {
 public:
  // False, with a sentence, when a rest volume is not positive, an edge has no rest length, or a
  // cell's material has K < 5 mu / 3.
  bool build(std::span<const query::D3> rest, std::span<const u32> tets,
             std::span<const CellMaterial> materials, std::string* error);

  struct Terms {
    f64 bulk = 0.0;
    f64 edge = 0.0;
  };
  Terms energy(std::span<const query::D3> x) const;

  // Adds each term's gradient, in newtons, to its node: `bulk` and `edge` hold one entry per node.
  void add_gradient(std::span<const query::D3> x, std::span<query::D3> bulk,
                    std::span<query::D3> edge) const;

  u32 cell_count() const noexcept { return static_cast<u32>(rest_volume_.size()); }
  std::span<const f64> rest_volumes() const noexcept {
    return std::span<const f64>(rest_volume_.data(), rest_volume_.size());
  }
  u32 edge_count() const noexcept { return static_cast<u32>(edge_rest_.size()); }

 private:
  Vector<u32> tets_;
  Vector<f64> rest_volume_;
  Vector<f64> volume_modulus_;  // K - 5 mu / 3
  Vector<u32> edge_a_;
  Vector<u32> edge_b_;
  Vector<f64> edge_rest_;
  Vector<f64> edge_weight_;  // (5/4) of the sum of mu V0 over the cells that share the edge, joules
};

}  // namespace engine::tissue::energy
