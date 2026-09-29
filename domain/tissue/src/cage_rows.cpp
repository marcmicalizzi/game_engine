#include "cage_rows.h"

#include "body.h"
#include "energy.h"
#include "mesh_query.h"

#include <domain/tissue/cage.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>

namespace engine::tissue::detail {

namespace {

// The thresholds, each chosen from what the fixtures measure (docs/subsystems/tissue.md, "How far
// the cage is from its reference", has the numbers and the margin each was given).
//
// The mass at the construction, relative: a derivation that keeps the boundary keeps the mass to
// the reference's curvature, which the supine fixtures' straight construction cells do not have.
constexpr f64 k_mass_relative = 0.01;
// The volume at every carried state, relative to the reference's exact volume there.
constexpr f64 k_volume_relative = 0.02;
// The boundaries' largest distance at any carried state, chordal, metres.
constexpr f64 k_boundary_m = 0.002;
// A carried state at the cage's nodes against the reference's field there: the carried state is
// that field, so what is left is rounding (the positions are f32).
constexpr f64 k_node_error_m = 1.0e-6;
// The cage's own field at the reference's nodes against the reference's, at any carried state.
constexpr f64 k_field_error_m = 0.002;

// A number in four significant digits, for a sentence.
std::string g4(f64 v) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%.4g", v);
  return buffer;
}

std::string mm_text(f64 m) { return g4(m * k_mm) + " mm"; }

// A distribution in millimetres: p50, p95, max.
JsonValue mm_stats(Vector<f64> values) { return stats(std::move(values), k_mm); }

Vector<D3> to_d3(const Vector<Vec3>& v) {
  Vector<D3> out(v.size());
  for (u32 i = 0; i < v.size(); ++i)
    out[i] = d3(v[i]);
  return out;
}

// The nodes a boundary's faces use.
Vector<u32> face_nodes(const Vector<u32>& faces) {
  Vector<u32> out = faces;
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

// The declared energy's total at a state, over a body's linear cells with each cell's material.
bool body_energy(const Body& body, const BodyState& rest, energy::BulkEdgeEnergy& out,
                 std::string& error) {
  Vector<energy::CellMaterial> materials(body.parent.size());
  for (u32 t = 0; t < body.parent.size(); ++t) {
    materials[t].bulk_modulus_pa = body.bulk_pa[body.parent[t]];
    materials[t].shear_modulus_pa = body.shear_pa[body.parent[t]];
  }
  const Vector<D3> x = to_d3(rest.nodes);
  return out.build(std::span<const D3>(x.data(), x.size()), body.tets, materials, &error);
}

}  // namespace

void check_cage(const TissueFile& file, const Vector<RegionModel>& regions,
                const ValidateOptions& options, TissueReport& report) {
  RowWriter w{report};
  for (const RegionModel& region : regions) {
    if (!region.def->derivation.has_value()) continue;
    const CageDerivation& derivation = *region.def->derivation;
    const std::string subject = "region " + region.def->name;

    // ---- cage.source
    const TissueFile* source_file = nullptr;
    std::string where;
    {
      ValidationRow& r = w.row(
          "cage.source", subject, Severity::error,
          "the derivation's method is one this build derives with; its build key is the one its "
          "method, source, budget and hero flag give; its source region is a Reference region of "
          "this file, or of the source file handed to the validator whose SHA-256 is the recorded "
          "one",
          "a derived cage names what it was derived from, and a cage whose source moved is not "
          "that source's cage");
      r.value.set("method", JsonValue(derivation.method));
      r.value.set("source_sha256", JsonValue(derivation.source_sha256));
      r.value.set("source_definition", JsonValue(derivation.source_definition));
      r.value.set("source_region", JsonValue(derivation.source_region));
      r.value.set("node_budget", JsonValue(derivation.node_budget));
      r.value.set("corner_nodes", JsonValue(derivation.corner_nodes));
      r.value.set("hero", JsonValue(region.def->hero));
      if (derivation.method != k_cage_method)
        RowWriter::fail(r, "method '" + derivation.method +
                               "', which this build does not derive "
                               "with");
      JsonValue omitted = JsonValue::array();
      for (const std::string& s : derivation.omitted_states)
        omitted.push_back(JsonValue(s));
      r.value.set("omitted_states", std::move(omitted));
      const std::string key =
          cage_build_key(derivation.source_sha256, derivation.source_region, derivation.node_budget,
                         region.def->hero, derivation.omitted_states);
      if (key != derivation.build_key)
        RowWriter::fail(r,
                        "the build key is not the one the record's method, source, budget and "
                        "hero flag give");
      if (derivation.source_sha256.empty()) {
        for (const Region& other : file.definition.regions)
          if (other.name == derivation.source_region && other.role == RegionRole::Reference &&
              &other != region.def)
            source_file = &file;
        where = "this file";
        if (source_file == nullptr)
          RowWriter::fail(r, "the source region '" + derivation.source_region +
                                 "' is not a Reference region of this file");
      } else if (options.cage_source != nullptr) {
        where = "the supplied source";
        r.value.set("supplied_sha256", JsonValue(options.cage_source_sha256));
        if (options.cage_source_sha256 != derivation.source_sha256)
          RowWriter::fail(r, "the supplied source's SHA-256 is " + options.cage_source_sha256 +
                                 ", not the recorded " + derivation.source_sha256);
        else
          source_file = options.cage_source;
      } else {
        where = "not supplied";
      }
      r.value.set("source", JsonValue(where));
    }
    Body cage;
    std::string cage_error;
    const bool cage_ok = load_body(file, region.def->name, cage, &cage_error);
    Body source;
    std::string source_error;
    bool source_ok = false;
    if (source_file != nullptr) {
      source_ok = load_body(*source_file, derivation.source_region, source, &source_error);
    } else {
      source_error = where == "not supplied"
                         ? "the source is another file, which the validator was not handed "
                           "(engine-content tissue validate --source <reference.tissue>)"
                         : "the source does not resolve";
    }
    if (!cage_ok) source_error = "the cage does not resolve: " + cage_error;
    const bool both = cage_ok && source_ok;

    // ---- cage.volume
    {
      const std::string threshold =
          "the cage's mass at the construction within " + g4(k_mass_relative * 100.0) +
          "% of the reference's (its exact ten-node cells'), from the recorded ledger; with the "
          "source, the cage's volume within " +
          g4(k_volume_relative * 100.0) +
          "% of the reference's exact volume at every carried state; and the recorded cage "
          "numbers the file's own";
      ValidationRow& r =
          w.row("cage.volume", subject, Severity::warning, threshold,
                "the mass ledger has to close: the cage holds the reference's material where the "
                "reference held it, and loses only what its coarser boundary cannot hold");
      r.value.set("source_volume_ml", number(derivation.source_volume_m3 * k_ml));
      r.value.set("source_mass_kg", number(derivation.source_mass_kg));
      r.value.set("cage_volume_ml_recorded", number(derivation.cage_volume_m3 * k_ml));
      r.value.set("cage_mass_kg_recorded", number(derivation.cage_mass_kg));
      if (cage_ok) {
        const BodyMass mass = body_mass(cage);
        r.value.set("cage_volume_ml", number(mass.volume_m3 * k_ml));
        r.value.set("cage_mass_kg", number(mass.mass_kg));
        const f64 mass_difference = derivation.source_mass_kg - mass.mass_kg;
        const f64 relative =
            derivation.source_mass_kg > 0.0 ? mass_difference / derivation.source_mass_kg : 0.0;
        r.value.set("mass_difference_kg", number(mass_difference));
        r.value.set("mass_difference_relative", number(relative));
        r.value.set("volume_difference_ml",
                    number((derivation.source_volume_m3 - mass.volume_m3) * k_ml));
        const auto differs = [](f64 a, f64 b) {
          return std::fabs(a - b) > 1.0e-9 * std::max(std::fabs(a), std::fabs(b));
        };
        if (differs(mass.mass_kg, derivation.cage_mass_kg) ||
            differs(mass.volume_m3, derivation.cage_volume_m3))
          RowWriter::fail(r,
                          "the cage's own mass or volume is not the recorded one: the cage "
                          "changed after it was derived");
        if (std::fabs(relative) > k_mass_relative)
          RowWriter::fail(r, "mass " + g4(mass.mass_kg) + " kg against the reference's " +
                                 g4(derivation.source_mass_kg) + " kg");
      }
      if (both) {
        JsonValue per_state = JsonValue::object();
        f64 worst = 0.0;
        std::string worst_state;
        for (const BodyState& s : cage.states) {
          const BodyState* ref = source.state(s.name);
          if (ref == nullptr) continue;
          f64 v_ref = 0.0;
          for (u32 c = 0; c < source.cell_count(); ++c)
            v_ref += source.cell_volume(c, ref->nodes);
          f64 v_cage = 0.0;
          for (u32 c = 0; c < cage.cell_count(); ++c)
            v_cage += cage.cell_volume(c, s.nodes);
          const f64 rel = v_ref != 0.0 ? (v_cage - v_ref) / v_ref : 0.0;
          JsonValue e = JsonValue::object();
          e.set("reference_ml", number(v_ref * k_ml));
          e.set("cage_ml", number(v_cage * k_ml));
          e.set("relative", number(rel));
          per_state.set(s.name, std::move(e));
          if (std::fabs(rel) > worst) {
            worst = std::fabs(rel);
            worst_state = s.name;
          }
        }
        r.value.set("states", std::move(per_state));
        r.value.set("volume_relative_max", number(worst));
        if (worst > k_volume_relative)
          RowWriter::fail(r, "state " + worst_state + ": volume " + g4(worst * 100.0) +
                                 "% from the reference's");
      } else {
        r.value.set("states", JsonValue::null());
        r.value.set("states_not_measured", JsonValue(source_error));
      }
    }

    // The rows below need the reference's cells.
    const auto skipped = [&](ValidationRow& r) {
      r.verdict = Verdict::skipped;
      r.witness = source_error;
    };

    // ---- cage.boundary_distance
    {
      const std::string threshold =
          "at every carried state, every boundary node of each body within " + mm_text(k_boundary_m) +
          " of the other's boundary: chordal — the reference's curved faces are their four chords "
          "(its linear subdivision's boundary), and the distance is from each side's nodes to the "
          "other's triangles, not a continuous Hausdorff distance";
      ValidationRow& r = w.row(
          "cage.boundary_distance", subject, Severity::warning, threshold,
          "the cage's boundary is where contact, containment and the render binding will read "
          "the body; how far it is from the reference's is the first thing a coarser cage costs");
      if (!both) {
        skipped(r);
      } else {
        const Vector<u32> ref_faces = boundary_faces(source.tets, source.nodes);
        const Vector<u32> cage_faces = boundary_faces(cage.tets, cage.nodes);
        const Vector<u32> ref_nodes = face_nodes(ref_faces);
        const Vector<u32> cage_nodes = face_nodes(cage_faces);
        JsonValue per_state = JsonValue::object();
        f64 worst = 0.0;
        std::string worst_where;
        for (const BodyState& s : cage.states) {
          const BodyState* ref = source.state(s.name);
          if (ref == nullptr) continue;
          query::SignedSurface cage_surface;
          cage_surface.build(s.nodes, cage_faces);
          query::TriangleBvh ref_bvh;
          ref_bvh.build(ref->nodes, ref_faces);
          f64 ref_to_cage = 0.0;
          f64 outside = 0.0;  // the reference past the cage's boundary, outward
          f64 inside = 0.0;   // the reference inside the cage's boundary
          u32 ref_witness = 0;
          for (const u32 v : ref_nodes) {
            const query::SignedSurface::Result q = cage_surface.query(d3(ref->nodes[v]));
            const f64 d = std::fabs(q.distance);
            if (d > ref_to_cage) {
              ref_to_cage = d;
              ref_witness = v;
            }
            outside = std::max(outside, q.distance);
            inside = std::max(inside, -q.distance);
          }
          f64 cage_to_ref = 0.0;
          u32 cage_witness = 0;
          for (const u32 v : cage_nodes) {
            const f64 d = ref_bvh.nearest(d3(s.nodes[v])).distance;
            if (d > cage_to_ref) {
              cage_to_ref = d;
              cage_witness = v;
            }
          }
          JsonValue e = JsonValue::object();
          e.set("reference_to_cage_mm", number(ref_to_cage * k_mm));
          e.set("reference_node", JsonValue(ref_witness));
          e.set("reference_outside_cage_mm", number(outside * k_mm));
          e.set("reference_inside_cage_mm", number(inside * k_mm));
          e.set("cage_to_reference_mm", number(cage_to_ref * k_mm));
          e.set("cage_node", JsonValue(cage_witness));
          per_state.set(s.name, std::move(e));
          if (ref_to_cage > worst) {
            worst = ref_to_cage;
            worst_where = "state " + s.name + ": reference node " + str(ref_witness) + " at " +
                          mm_text(ref_to_cage) + " from the cage's boundary";
          }
          if (cage_to_ref > worst) {
            worst = cage_to_ref;
            worst_where = "state " + s.name + ": cage node " + str(cage_witness) + " at " +
                          mm_text(cage_to_ref) + " from the reference's boundary";
          }
        }
        r.value.set("representation",
                    JsonValue("the reference's same-node linear subdivision's boundary against the "
                              "cage's tetrahedra's, node to triangle both ways"));
        r.value.set("reference_boundary_nodes", JsonValue(static_cast<u64>(ref_nodes.size())));
        r.value.set("cage_boundary_nodes", JsonValue(static_cast<u64>(cage_nodes.size())));
        r.value.set("states", std::move(per_state));
        r.value.set("max_mm", number(worst * k_mm));
        r.witness = worst_where;
        if (worst > k_boundary_m) RowWriter::fail(r, worst_where);
      }
    }

    // ---- cage.state_displacement
    {
      const std::string threshold =
          "at every carried state: at the cage's nodes, the carried state within " +
          g4(k_node_error_m * 1.0e6) +
          " um of the reference's field there (its ten-node shape functions at the node's place "
          "in the construction); at the reference's nodes, the cage's linear field within " +
          mm_text(k_field_error_m) + " of the reference's own";
      ValidationRow& r =
          w.row("cage.state_displacement", subject, Severity::warning, threshold,
                "a state carried onto the cage is the reference's displacement field at the cage's "
                "nodes; what the cage cannot represent of it shows at the nodes it does not have");
      if (!both) {
        skipped(r);
      } else {
        CellLocator in_source;
        in_source.build(source);
        CellLocator in_cage;
        in_cage.build(cage);
        // Where each cage node, and each reference node, lies in the other's construction.
        Vector<CellLocator::Location> cage_in_source(cage.nodes.size());
        for (u32 i = 0; i < cage.nodes.size(); ++i)
          cage_in_source[i] = in_source.locate(d3(cage.nodes[i]));
        Vector<CellLocator::Location> source_in_cage(source.nodes.size());
        f64 outside_max = 0.0;
        for (u32 i = 0; i < source.nodes.size(); ++i) {
          source_in_cage[i] = in_cage.locate(d3(source.nodes[i]));
          outside_max = std::max(outside_max, source_in_cage[i].outside);
        }
        JsonValue per_state = JsonValue::object();
        f64 node_worst = 0.0;
        f64 field_worst = 0.0;
        std::string node_where;
        std::string field_where;
        for (const BodyState& s : cage.states) {
          const BodyState* ref = source.state(s.name);
          if (ref == nullptr) {
            RowWriter::fail(r, "state " + s.name + " is not a state of the reference");
            continue;
          }
          f64 at_nodes = 0.0;
          u32 at_node = 0;
          for (u32 i = 0; i < cage.nodes.size(); ++i) {
            const D3 expected = in_source.interpolate(cage_in_source[i], ref->nodes);
            const f64 e = query::length(expected - d3(s.nodes[i]));
            if (e > at_nodes) {
              at_nodes = e;
              at_node = i;
            }
          }
          Vector<f64> errors;
          f64 field_max = 0.0;
          u32 field_node = 0;
          for (u32 i = 0; i < source.nodes.size(); ++i) {
            // The cage's field at the reference node: its cell's linear map of the state.
            const D3 moved = in_cage.interpolate(source_in_cage[i], s.nodes);
            const f64 e = query::length(moved - d3(ref->nodes[i]));
            errors.push_back(e);
            if (e > field_max) {
              field_max = e;
              field_node = i;
            }
          }
          JsonValue e = JsonValue::object();
          e.set("at_cage_nodes_max_um", number(at_nodes * 1.0e6));
          e.set("at_cage_nodes_witness", JsonValue(at_node));
          e.set("at_reference_nodes_mm", mm_stats(std::move(errors)));
          e.set("at_reference_nodes_witness", JsonValue(field_node));
          per_state.set(s.name, std::move(e));
          if (at_nodes > node_worst) {
            node_worst = at_nodes;
            node_where = "state " + s.name + ": cage node " + str(at_node);
          }
          if (field_max > field_worst) {
            field_worst = field_max;
            field_where = "state " + s.name + ": reference node " + str(field_node) + ", " +
                          mm_text(field_max) + " from the cage's field";
          }
        }
        r.value.set("states", std::move(per_state));
        r.value.set("at_cage_nodes_max_um", number(node_worst * 1.0e6));
        r.value.set("at_reference_nodes_max_mm", number(field_worst * k_mm));
        r.value.set("reference_nodes_outside_cage_max_barycentric", number(outside_max));
        r.witness = field_where;
        if (node_worst > k_node_error_m) RowWriter::fail(r, node_where);
        if (field_worst > k_field_error_m) RowWriter::fail(r, field_where);
      }
    }

    // ---- cage.strain_energy
    {
      ValidationRow& r = w.row(
          "cage.strain_energy", subject, Severity::info,
          "reported: at every carried state, the declared energy (bulk-edge-v0) of the cage over "
          "its rest against the reference's over its rest (its linear subdivision), and the "
          "share",
          "a coarser cage is stiffer: the share says how much of the reference's strain energy "
          "the cage stores for the same displacement");
      r.value.set("law", JsonValue(energy::k_law));
      if (!both) {
        skipped(r);
      } else {
        const BodyState* cage_rest = cage.state_with_role(StateRole::Rest);
        const BodyState* ref_rest = source.state_with_role(StateRole::Rest);
        if (cage_rest == nullptr) cage_rest = &cage.states.front();
        if (ref_rest == nullptr) ref_rest = &source.states.front();
        energy::BulkEdgeEnergy e_cage;
        energy::BulkEdgeEnergy e_ref;
        std::string error;
        if (!body_energy(cage, *cage_rest, e_cage, error) ||
            !body_energy(source, *ref_rest, e_ref, error)) {
          r.verdict = Verdict::skipped;
          r.witness = error;
        } else {
          r.value.set("rest", JsonValue(cage_rest->name));
          JsonValue per_state = JsonValue::object();
          for (const BodyState& s : cage.states) {
            const BodyState* ref = source.state(s.name);
            if (ref == nullptr || &s == cage_rest) continue;
            const Vector<D3> xc = to_d3(s.nodes);
            const Vector<D3> xr = to_d3(ref->nodes);
            const energy::BulkEdgeEnergy::Terms tc =
                e_cage.energy(std::span<const D3>(xc.data(), xc.size()));
            const energy::BulkEdgeEnergy::Terms tr =
                e_ref.energy(std::span<const D3>(xr.data(), xr.size()));
            const f64 total_c = tc.bulk + tc.edge;
            const f64 total_r = tr.bulk + tr.edge;
            JsonValue e = JsonValue::object();
            e.set("cage_j", number(total_c));
            e.set("reference_j", number(total_r));
            e.set("share", total_r > 0.0 ? number(total_c / total_r) : JsonValue::null());
            per_state.set(s.name, std::move(e));
          }
          r.value.set("states", std::move(per_state));
        }
      }
    }
  }
}

}  // namespace engine::tissue::detail
