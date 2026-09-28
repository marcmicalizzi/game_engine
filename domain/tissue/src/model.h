#pragma once

// The resolved model the validators walk (docs/subsystems/tissue.md, "Validators"): a region's
// nodes, cells and states, a frame's triangles, and the small helpers every row is written with.
// Private to the module. `validate.cpp` resolves it and runs the rows of the definition as it was
// through version 1; `layered.cpp` runs the layered model's rows over the same resolution, so a
// region is read once and both halves see the same nodes.

#include "cells.h"
#include "mesh_query.h"

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/json/json_value.h>
#include <domain/geometry/limit_surface.h>
#include <domain/tissue/tissue_file.h>
#include <domain/tissue/validate.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace engine::tissue::detail {

using query::D3;
using query::d3;

inline constexpr f64 k_ml = 1.0e6;  // cubic metres to millilitres
inline constexpr f64 k_mm = 1.0e3;  // metres to millimetres

// ---- small helpers ------------------------------------------------------------------------------

inline std::string str(u64 v) { return std::to_string(v); }

inline std::string mm(f64 metres) {
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "%.4g mm", metres * k_mm);
  return buffer;
}

inline std::string xyz(D3 p) {
  char buffer[128];
  std::snprintf(buffer, sizeof(buffer), "(%.5f, %.5f, %.5f)", p.x, p.y, p.z);
  return buffer;
}

inline JsonValue number(f64 v) { return std::isfinite(v) ? JsonValue(v) : JsonValue::null(); }

// p50, p95, min and max of a distribution, scaled (1e3 for millimetres), and how many values.
inline JsonValue stats(Vector<f64> values, f64 scale) {
  JsonValue out = JsonValue::object();
  out.set("count", JsonValue(static_cast<u64>(values.size())));
  if (values.empty()) return out;
  std::sort(values.begin(), values.end());
  const auto at = [&](f64 q) {
    const u32 i = static_cast<u32>(std::floor(q * static_cast<f64>(values.size() - 1)));
    return values[i] * scale;
  };
  out.set("min", number(values.front() * scale));
  out.set("p50", number(at(0.50)));
  out.set("p95", number(at(0.95)));
  out.set("max", number(values.back() * scale));
  return out;
}

inline f64 percentile(Vector<f64> values, f64 q) {
  if (values.empty()) return 0.0;
  std::sort(values.begin(), values.end());
  return values[static_cast<u32>(std::floor(q * static_cast<f64>(values.size() - 1)))];
}

inline bool contains_sorted(const Vector<u32>& sorted, u32 value) {
  return std::binary_search(sorted.begin(), sorted.end(), value);
}

inline bool strictly_ascending(const Vector<u32>& ids) {
  for (u32 i = 1; i < ids.size(); ++i)
    if (!(ids[i - 1] < ids[i])) return false;
  return true;
}

// Typed views of blocks. A reference that does not resolve, or resolves to the wrong kind or an
// element count the caller did not expect, is a problem string, collected by the resolve row.
struct Blocks {
  const TissueFile& file;
  Vector<std::string>& problems;

  const TissueBlock* get(const std::string& name, BlockKind kind, const std::string& who) const {
    if (name.empty()) {
      problems.push_back(who + " names no block");
      return nullptr;
    }
    const TissueBlock* b = file.find(name);
    if (b == nullptr) {
      problems.push_back(who + " names block '" + name + "', which the file does not hold");
      return nullptr;
    }
    if (b->kind != kind) {
      problems.push_back(who + " names block '" + name + "', a " +
                         block_kind_name(static_cast<u32>(b->kind)) + " where a " +
                         block_kind_name(static_cast<u32>(kind)) + " is needed");
      return nullptr;
    }
    return b;
  }
  template <class T>
  bool read(const std::string& name, BlockKind kind, const std::string& who, Vector<T>& out,
            u32 per_element = 1) const {
    const TissueBlock* b = get(name, kind, who);
    if (b == nullptr) return false;
    out.resize(static_cast<u32>(b->count * per_element));
    if (!b->bytes.empty()) std::memcpy(out.data(), b->bytes.data(), b->bytes.size());
    return true;
  }
  bool optional(const std::string& name) const { return !name.empty(); }
};

// ---- the resolved model -------------------------------------------------------------------------

struct SheetModel {
  const Sheet* def = nullptr;
  Vector<u32> region_node;  // sheet-local control vertex -> region node
  Vector<u32> faces;        // sheet-local control triangles
  geometry::LoopLimitSurface surface;
  bool ok = false;
};

struct StateModel {
  std::string name;
  const RegionState* def = nullptr;  // null for the construction
  StateRole role = StateRole::Construction;
  Vector<Vec3> nodes;
  Vector<Vec3> expected_visible;
};

// How deep a ten-node cell is split when its Jacobian's Bernstein coefficients alone do not decide
// its sign: eight pieces a level, and only the undecided ones go on.
inline constexpr u32 k_jacobian_depth = 4;

struct RegionModel {
  const Region* def = nullptr;
  Vector<Vec3> nodes;
  // The cells as authored, `per_cell` node indices each: 4, or 10 for a quadratic cage.
  u32 per_cell = 4;
  Vector<u32> cell_nodes;
  // The four-node cells the rows whose meaning is linear walk: the cells themselves for a
  // four-node cage, the same-node linear subdivision (eight a cell, cells.h) for a ten-node one.
  // `parent` is each one's cell.
  Vector<u32> tets;
  Vector<u32> parent;
  Vector<SheetModel> sheets;
  i32 top = -1;
  i32 support = -1;
  // A region with no Loop sheet at all (TissueDefinition version 2: the layered model's regions,
  // whose skin is on the material boundary): the rows over the sheets are not run for it.
  bool sheetless = false;
  Vector<u32> stitch;
  Vector<u32> boundary;  // the tetrahedra's boundary faces, outward
  Vector<StateModel> states;
  i32 reference = -1;
  bool ok = false;

  Vector<Vec3> sheet_nodes(const SheetModel& sheet, const Vector<Vec3>& state) const {
    Vector<Vec3> out;
    for (const u32 n : sheet.region_node)
      out.push_back(state[n]);
    return out;
  }

  bool quadratic() const noexcept { return per_cell == 10; }
  u32 cell_count() const noexcept { return static_cast<u32>(cell_nodes.size() / per_cell); }
  const u32* cell(u32 c) const noexcept { return cell_nodes.data() + per_cell * c; }
  // A cell's nodes at `x`, as doubles: the first `per_cell` entries of `out`.
  void gather(u32 c, const Vector<Vec3>& x, query::D3 out[10]) const noexcept {
    for (u32 k = 0; k < per_cell; ++k)
      out[k] = query::d3(x[cell(c)[k]]);
  }
  // A cell's exact signed volume at `x`: a tetrahedron's, or a ten-node cell's integral of det J.
  f64 cell_volume(u32 c, const Vector<Vec3>& x) const noexcept {
    query::D3 p[10];
    gather(c, x, p);
    return quadratic() ? cells::quadratic_volume(p) : query::tet_volume(p[0], p[1], p[2], p[3]);
  }
  // What the rows whose meaning is linear walk, and the boundary they see, in words: every such
  // row names it, so a number measured on the subdivision is never read as the cells'.
  std::string linear_representation() const {
    if (!quadratic()) return "the tetrahedra";
    return "the same-node linear subdivision: " + str(tets.size() / 4) +
           " four-node cells, eight to each of the " + str(cell_count()) + " ten-node cells";
  }
  const char* boundary_representation() const noexcept {
    return quadratic() ? "the same-node linear subdivision's boundary: four triangles to each "
                         "ten-node face, through every boundary node"
                       : "the tetrahedra's boundary";
  }
  const char* cell_word() const noexcept { return quadratic() ? "cell" : "tetrahedron"; }
  // The state of this name ("construction" is the first), or null.
  const StateModel* state(std::string_view name) const noexcept {
    for (const StateModel& s : states)
      if (s.name == name) return &s;
    return nullptr;
  }
};

inline const char* role_name(RegionRole role) noexcept {
  return role == RegionRole::Reference ? "Reference" : "Runtime";
}

struct FrameModel {
  const Frame* def = nullptr;
  Vector<Vec3> vertices;
  Vector<D3> points;
  Vector<u32> triangles;
  Vector<f32> cover;
  Vector<u8> provenance;
  query::SignedSurface surface;
  bool ok = false;
};

// A frame's rigid transform at a state (FrameState), x -> R x + t, in double.
struct RigidTransform {
  f64 r[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
  f64 t[3] = {0, 0, 0};
  D3 apply(D3 p) const noexcept {
    return D3{r[0] * p.x + r[1] * p.y + r[2] * p.z + t[0],
              r[3] * p.x + r[4] * p.y + r[5] * p.z + t[1],
              r[6] * p.x + r[7] * p.y + r[8] * p.z + t[2]};
  }
  // The inverse of a rigid transform: R^T (p - t).
  D3 unapply(D3 p) const noexcept {
    const D3 q{p.x - t[0], p.y - t[1], p.z - t[2]};
    return D3{r[0] * q.x + r[3] * q.y + r[6] * q.z, r[1] * q.x + r[4] * q.y + r[7] * q.z,
              r[2] * q.x + r[5] * q.y + r[8] * q.z};
  }
};

// The FrameState of `frame` at `state`, or null: a frame with none there is where its vertices are.
inline const FrameState* frame_state(const TissueDefinition& def, std::string_view frame,
                                     std::string_view state) noexcept {
  for (const FrameState& s : def.frame_states)
    if (s.frame == frame && s.state == state && s.frame_kind == TargetKind::Frame) return &s;
  return nullptr;
}

inline RigidTransform rigid_transform(const FrameState* s) noexcept {
  RigidTransform out;
  if (s == nullptr) return out;
  for (u32 i = 0; i < 9; ++i)
    out.r[i] = s->rotation[i];
  for (u32 i = 0; i < 3; ++i)
    out.t[i] = s->translation_m[i];
  return out;
}

// Adds rows to a report: the one way every validator writes one.
struct RowWriter {
  TissueReport& report;

  ValidationRow& row(std::string id, std::string subject, Severity severity, std::string threshold,
                     std::string note) {
    ValidationRow r;
    r.id = std::move(id);
    r.subject = std::move(subject);
    r.severity = severity;
    r.verdict = severity == Severity::info ? Verdict::info : Verdict::pass;
    r.threshold = std::move(threshold);
    r.note = std::move(note);
    r.value = JsonValue::object();
    report.rows.push_back(std::move(r));
    return report.rows.back();
  }
  static void fail(ValidationRow& r, std::string witness) {
    r.verdict = Verdict::fail;
    if (r.witness.empty()) r.witness = std::move(witness);
  }
};

}  // namespace engine::tissue::detail
