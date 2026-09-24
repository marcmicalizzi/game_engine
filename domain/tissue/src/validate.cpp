#include "energy.h"
#include "mesh_query.h"

#include <core/json/json.h>
#include <domain/geometry/limit_surface.h>
#include <domain/geometry/surface_binding.h>
#include <domain/physics/deformable.h>
#include <domain/tissue/validate.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <numbers>
#include <optional>
#include <string>

namespace engine::tissue {

namespace {

using query::D3;
using query::d3;

constexpr f64 k_ml = 1.0e6;  // cubic metres to millilitres
constexpr f64 k_mm = 1.0e3;  // metres to millimetres
constexpr const char* k_loop_rule = "loop-hoppe-1994-v1";

// ---- small helpers ------------------------------------------------------------------------------

std::string str(u64 v) { return std::to_string(v); }

std::string mm(f64 metres) {
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "%.4g mm", metres * k_mm);
  return buffer;
}

std::string xyz(D3 p) {
  char buffer[128];
  std::snprintf(buffer, sizeof(buffer), "(%.5f, %.5f, %.5f)", p.x, p.y, p.z);
  return buffer;
}

JsonValue number(f64 v) { return std::isfinite(v) ? JsonValue(v) : JsonValue::null(); }

// p50, p95, min and max of a distribution, scaled (1e3 for millimetres), and how many values.
JsonValue stats(Vector<f64> values, f64 scale) {
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

f64 percentile(Vector<f64> values, f64 q) {
  if (values.empty()) return 0.0;
  std::sort(values.begin(), values.end());
  return values[static_cast<u32>(std::floor(q * static_cast<f64>(values.size() - 1)))];
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

struct RegionModel {
  const Region* def = nullptr;
  Vector<Vec3> nodes;
  Vector<u32> tets;
  Vector<SheetModel> sheets;
  i32 top = -1;
  i32 support = -1;
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
};

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

struct ObservationModel {
  Vector<Vec3> positions;
  Vector<u32> triangles;  // quads split (0, 1, 2), (0, 2, 3)
  Vector<u32> domain;
  Vector<Vec3> base_outside;
  Vector<f32> deviation;
  bool ok = false;
};

struct BindingModel {
  const SkinBinding* def = nullptr;
  i32 region = -1;
  i32 sheet = -1;
  geometry::NormalMode mode = geometry::NormalMode::limit_interpolated;
  bool mode_known = false;
  Vector<u32> domain;
  Vector<u32> footprint;
  Vector<u32> band;
  Vector<f32> band_weight;
  Vector<f32> weight;  // per domain entry
  Vector<geometry::AuthoredFootpoint> footpoints;
  Vector<Vec3> authored_normals;
  Vector<geometry::SurfaceBinding> records;  // per domain entry, under `mode`
  geometry::SurfaceBindReport bind_report;
  bool ok = false;
};

bool contains_sorted(const Vector<u32>& sorted, u32 value) {
  return std::binary_search(sorted.begin(), sorted.end(), value);
}

bool strictly_ascending(const Vector<u32>& ids) {
  for (u32 i = 1; i < ids.size(); ++i)
    if (!(ids[i - 1] < ids[i])) return false;
  return true;
}

// The boundary faces of a tetrahedral mesh: every face used by one tetrahedron, wound outward.
Vector<u32> tet_boundary(const Vector<u32>& tets, const Vector<Vec3>& nodes) {
  struct Face {
    u32 key[3];
    u32 wound[3];
  };
  Vector<Face> faces;
  for (u32 t = 0; t + 3 < tets.size(); t += 4) {
    const u32 v[4] = {tets[t], tets[t + 1], tets[t + 2], tets[t + 3]};
    // Faces opposite each corner, wound so that the opposite corner is behind them for a positive
    // tetrahedron: (1,2,3) faces away from 0, and so on.
    const u32 local[4][3] = {{1, 2, 3}, {0, 3, 2}, {0, 1, 3}, {0, 2, 1}};
    const bool positive =
        query::tet_volume(d3(nodes[v[0]]), d3(nodes[v[1]]), d3(nodes[v[2]]), d3(nodes[v[3]])) > 0.0;
    for (const auto& f : local) {
      Face face;
      for (u32 k = 0; k < 3; ++k)
        face.wound[k] = v[f[k]];
      if (!positive) std::swap(face.wound[1], face.wound[2]);
      face.key[0] = face.wound[0];
      face.key[1] = face.wound[1];
      face.key[2] = face.wound[2];
      std::sort(face.key, face.key + 3);
      faces.push_back(face);
    }
  }
  std::sort(faces.begin(), faces.end(), [](const Face& a, const Face& b) {
    return std::lexicographical_compare(a.key, a.key + 3, b.key, b.key + 3);
  });
  Vector<u32> out;
  for (u32 i = 0; i < faces.size();) {
    u32 j = i + 1;
    while (j < faces.size() && std::equal(faces[i].key, faces[i].key + 3, faces[j].key))
      ++j;
    if (j - i == 1)
      for (u32 k = 0; k < 3; ++k)
        out.push_back(faces[i].wound[k]);
    i = j;
  }
  return out;
}

// A closed, consistently wound 2-manifold: every edge used exactly twice, in opposite directions.
struct ManifoldCheck {
  bool closed_manifold = true;
  std::string witness;
  u32 degenerate = 0;
};
ManifoldCheck check_manifold(const Vector<u32>& triangles, const Vector<D3>& points) {
  ManifoldCheck out;
  Vector<std::pair<u64, i32>> edges;  // undirected key, +1 for (min, max) direction, -1 opposite
  for (u32 t = 0; t + 2 < triangles.size(); t += 3) {
    for (u32 k = 0; k < 3; ++k) {
      const u32 a = triangles[t + k];
      const u32 b = triangles[t + (k + 1) % 3];
      edges.push_back({(u64{std::min(a, b)} << 32) | u64{std::max(a, b)}, a < b ? 1 : -1});
    }
    const D3 n = query::cross(points[triangles[t + 1]] - points[triangles[t]],
                              points[triangles[t + 2]] - points[triangles[t]]);
    if (!(query::length(n) > 0.0)) {
      if (out.degenerate == 0) out.witness = "triangle " + str(t / 3) + " has no area";
      ++out.degenerate;
    }
  }
  std::sort(edges.begin(), edges.end());
  for (u32 i = 0; i < edges.size();) {
    u32 j = i;
    i32 balance = 0;
    while (j < edges.size() && edges[j].first == edges[i].first) {
      balance += edges[j].second;
      ++j;
    }
    if (j - i != 2 || balance != 0) {
      if (out.closed_manifold) {
        out.witness = "edge (" + str(edges[i].first >> 32) + ", " +
                      str(edges[i].first & 0xffffffffu) + ") is used " + str(j - i) +
                      (j - i == 2 ? " times in the same direction" : " times");
      }
      out.closed_manifold = false;
    }
    i = j;
  }
  if (out.degenerate > 0) out.closed_manifold = false;
  return out;
}

// Per-vertex area of a triangle mesh: a third of each incident triangle's.
Vector<f64> vertex_areas(const Vector<D3>& points, const Vector<u32>& triangles) {
  Vector<f64> out(static_cast<u32>(points.size()), 0.0);
  for (u32 t = 0; t + 2 < triangles.size(); t += 3) {
    const f64 area =
        0.5 * query::length(query::cross(points[triangles[t + 1]] - points[triangles[t]],
                                         points[triangles[t + 2]] - points[triangles[t]]));
    for (u32 k = 0; k < 3; ++k)
      out[triangles[t + k]] += area / 3.0;
  }
  return out;
}

Vector<D3> to_d3(const Vector<Vec3>& v) {
  Vector<D3> out(static_cast<u32>(v.size()));
  for (u32 i = 0; i < v.size(); ++i)
    out[i] = d3(v[i]);
  return out;
}

Vector<Vec3> evaluate(const geometry::CsrMatrix& matrix, const Vector<Vec3>& nodes) {
  Vector<Vec3> out(matrix.rows());
  geometry::apply(matrix, nodes, std::span<Vec3>(out.data(), out.size()));
  return out;
}

f64 swept(const Vector<D3>& from, const Vector<D3>& to, const Vector<u32>& triangles) {
  f64 sum = 0.0;
  for (u32 t = 0; t + 2 < triangles.size(); t += 3) {
    const u32 a = triangles[t];
    const u32 b = triangles[t + 1];
    const u32 c = triangles[t + 2];
    sum += query::swept_volume(from[a], from[b], from[c], to[a], to[b], to[c]);
  }
  return sum;
}

f64 cage_volume(const Vector<u32>& tets, const Vector<Vec3>& nodes) {
  f64 sum = 0.0;
  for (u32 t = 0; t + 3 < tets.size(); t += 4)
    sum += query::tet_volume(d3(nodes[tets[t]]), d3(nodes[tets[t + 1]]), d3(nodes[tets[t + 2]]),
                             d3(nodes[tets[t + 3]]));
  return sum;
}

// ---- the validator ------------------------------------------------------------------------------

class Validator {
 public:
  Validator(const TissueFile& file, const ValidateOptions& options, TissueReport& report)
      : file_(file),
        def_(file.definition),
        options_(options),
        report_(report),
        blocks_{file, problems_} {}

  void run();

 private:
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
    report_.rows.push_back(std::move(r));
    return report_.rows.back();
  }
  static void fail(ValidationRow& r, std::string witness) {
    r.verdict = Verdict::fail;
    if (r.witness.empty()) r.witness = std::move(witness);
  }

  void resolve();
  void resolve_observation();
  void resolve_regions();
  void resolve_frames();
  void resolve_bindings();

  void check_observation();
  void check_regions();
  void check_attachments();
  void check_mechanics();
  void check_frames();
  void check_skin_and_depth();
  void check_bindings();
  void check_volumes();

  // The observation's positions with a binding's transfer applied at a state, and the transfer
  // under explicit terms and mode, into `out` (domain entries only).
  bool transfer(const BindingModel& b, const StateModel& state, geometry::NormalMode mode,
                geometry::BindingTerms terms, Vector<Vec3>& domain_out,
                geometry::BindingFrame* frame_out = nullptr) const;
  Vector<Vec3> body_with(const BindingModel& b, const Vector<Vec3>& domain_positions) const;
  // The skin at a state: the observation, moved by every binding of the region at that state.
  Vector<Vec3> skin_at(i32 region, const StateModel& state) const;
  i32 binding_of(i32 region) const {
    for (u32 i = 0; i < bindings_.size(); ++i)
      if (bindings_[i].region == region && bindings_[i].ok) return static_cast<i32>(i);
    return -1;
  }
  JsonValue& region_numbers(const RegionModel& r) {
    JsonValue& regions = report_.numbers["regions"];
    if (!regions.is_object()) regions = JsonValue::object();
    JsonValue& out = regions[r.def->name];
    if (!out.is_object()) out = JsonValue::object();
    return out;
  }
  JsonValue& state_numbers(const RegionModel& r, const StateModel& s) {
    JsonValue& states = region_numbers(r)["states"];
    if (!states.is_object()) states = JsonValue::object();
    JsonValue& out = states[s.name];
    if (!out.is_object()) out = JsonValue::object();
    return out;
  }

  const TissueFile& file_;
  const TissueDefinition& def_;
  ValidateOptions options_;
  TissueReport& report_;
  Vector<std::string> problems_;
  Blocks blocks_;

  ObservationModel observation_;
  Vector<RegionModel> regions_;
  Vector<FrameModel> frames_;
  Vector<BindingModel> bindings_;
  query::SignedSurface skin_;  // the observation, for the construction and the reference
};

void Validator::run() {
  report_.numbers = JsonValue::object();
  if (!file_.warnings.empty()) {
    ValidationRow& r = row("file.read", "the file", Severity::warning, "nothing skipped",
                           "a section, block or field this build does not know was skipped: what "
                           "names it falls back to its rest shape");
    JsonValue list = JsonValue::array();
    for (const std::string& w : file_.warnings)
      list.push_back(JsonValue(w));
    r.value.set("skipped", std::move(list));
    fail(r, file_.warnings.front());
  }
  {
    ValidationRow& r = row("definition.format", "the definition", Severity::error,
                           std::string("format '") + k_tissue_format + "', units 'm'",
                           "the interchange's version and its one unit");
    r.value.set("format", JsonValue(def_.format));
    r.value.set("units", JsonValue(def_.units));
    if (def_.format != k_tissue_format) fail(r, "format is '" + def_.format + "'");
    if (def_.units != "m") fail(r, "units are '" + def_.units + "'");
  }
  resolve();
  check_observation();
  check_regions();
  check_attachments();
  check_mechanics();
  check_frames();
  check_skin_and_depth();
  check_bindings();
  check_volumes();
  for (const ValidationRow& r : report_.rows) {
    if (r.verdict != Verdict::fail) continue;
    if (r.severity == Severity::error) ++report_.errors;
    if (r.severity == Severity::warning) ++report_.warnings;
  }
}

// ---- resolution
// -----------------------------------------------------------------------------------

void Validator::resolve() {
  resolve_observation();
  resolve_regions();
  resolve_frames();
  resolve_bindings();
  ValidationRow& r = row("blocks.resolve", "the definition", Severity::error,
                         "every reference names a block of the right kind and count, every index "
                         "in range",
                         "nothing below can be checked on data that does not resolve");
  r.value.set("blocks", JsonValue(static_cast<u64>(file_.blocks.size())));
  r.value.set("problems", JsonValue(static_cast<u64>(problems_.size())));
  if (!problems_.empty()) {
    JsonValue list = JsonValue::array();
    for (u32 i = 0; i < problems_.size() && i < 32; ++i)
      list.push_back(JsonValue(problems_[i]));
    r.value.set("first_problems", std::move(list));
    fail(r, problems_.front());
  }
}

void Validator::resolve_observation() {
  const Observation& o = def_.observation;
  ObservationModel& m = observation_;
  const u32 n = o.vertex_count;
  bool ok = n > 0;
  if (n == 0) problems_.push_back("the observation declares no vertices");
  ok = blocks_.read(o.positions, BlockKind::ObservedPositions, "observation.positions",
                    m.positions) &&
       ok;
  if (ok && m.positions.size() != n) {
    problems_.push_back("observation.positions holds " + str(m.positions.size()) +
                        " positions for " + str(n) + " vertices");
    ok = false;
  }
  // The faces: quads or triangles.
  const TissueBlock* faces = file_.find(o.base_faces);
  if (faces == nullptr) {
    problems_.push_back("observation.base_faces names block '" + o.base_faces +
                        "', which the file does not hold");
    ok = false;
  } else if (faces->kind == BlockKind::BaseQuads) {
    Vector<u32> quads;
    blocks_.read(o.base_faces, BlockKind::BaseQuads, "observation.base_faces", quads, 4);
    for (u32 q = 0; q + 3 < quads.size(); q += 4) {
      const u32 split[6] = {quads[q], quads[q + 1], quads[q + 2],
                            quads[q], quads[q + 2], quads[q + 3]};
      for (const u32 v : split)
        m.triangles.push_back(v);
    }
  } else if (faces->kind == BlockKind::BaseTriangles) {
    blocks_.read(o.base_faces, BlockKind::BaseTriangles, "observation.base_faces", m.triangles, 3);
  } else {
    problems_.push_back("observation.base_faces is neither BaseQuads nor BaseTriangles");
    ok = false;
  }
  for (const u32 v : m.triangles)
    if (v >= n) {
      problems_.push_back("a base face names vertex " + str(v) + ", past " + str(n));
      ok = false;
      break;
    }
  if (blocks_.read(o.domain, BlockKind::CanonicalIds, "observation.domain", m.domain)) {
    if (!strictly_ascending(m.domain) || (!m.domain.empty() && m.domain.back() >= n)) {
      problems_.push_back("observation.domain is not strictly ascending canonical ids below " +
                          str(n));
      ok = false;
    }
  } else {
    ok = false;
  }
  if (blocks_.read(o.base_outside, BlockKind::BaseOutsidePositions, "observation.base_outside",
                   m.base_outside)) {
    if (m.base_outside.size() + m.domain.size() != n) {
      problems_.push_back("observation.base_outside holds " + str(m.base_outside.size()) +
                          " positions for " + str(n - m.domain.size()) + " ids outside the domain");
      ok = false;
    }
  } else {
    ok = false;
  }
  if (blocks_.read(o.signed_deviation, BlockKind::IdValues, "observation.signed_deviation",
                   m.deviation) &&
      m.deviation.size() != m.domain.size()) {
    problems_.push_back("observation.signed_deviation is not one value per domain id");
    ok = false;
  }
  m.ok = ok;
  if (m.ok && !m.triangles.empty()) skin_.build(m.positions, m.triangles);
}

void Validator::resolve_regions() {
  for (const Region& region : def_.regions) {
    RegionModel m;
    m.def = &region;
    const std::string who = "region " + region.name;
    bool ok = region.cage == CageKind::Tetrahedral;
    if (!ok) problems_.push_back(who + " is not a tetrahedral cage, the only kind v0 reads");
    ok = blocks_.read(region.nodes, BlockKind::RegionNodes, who + ".nodes", m.nodes) && ok;
    ok = blocks_.read(region.tetrahedra, BlockKind::Tetrahedra, who + ".tetrahedra", m.tets, 4) &&
         ok;
    const u32 count = m.nodes.size();
    for (const u32 v : m.tets)
      if (v >= count) {
        problems_.push_back(who + " has a tetrahedron naming node " + str(v) + ", past " +
                            str(count));
        ok = false;
        break;
      }
    for (u32 s = 0; s < region.sheets.size(); ++s) {
      const Sheet& sheet = region.sheets[s];
      SheetModel sm;
      sm.def = &sheet;
      Vector<u32> faces;
      if (blocks_.read(sheet.faces, BlockKind::SheetFaces, who + " sheet " + sheet.name, faces,
                       3)) {
        Vector<u32> used = faces;
        std::sort(used.begin(), used.end());
        used.erase(std::unique(used.begin(), used.end()), used.end());
        bool in_range = used.empty() || used.back() < count;
        if (!in_range)
          problems_.push_back(who + " sheet " + sheet.name + " names a node past " + str(count));
        if (in_range && !faces.empty()) {
          sm.region_node = used;
          for (const u32 v : faces)
            sm.faces.push_back(
                static_cast<u32>(std::lower_bound(used.begin(), used.end(), v) - used.begin()));
          geometry::LoopSurfaceOptions options;
          options.level = sheet.level;
          options.tangents = true;
          std::string error;
          sm.ok = sheet.level <= geometry::k_loop_max_level &&
                  geometry::build_loop_limit_surface(sm.faces, used.size(), options, sm.surface,
                                                     &error);
          if (!sm.ok)
            problems_.push_back(who + " sheet " + sheet.name +
                                " is not a surface Loop's rules take" +
                                (error.empty() ? std::string() : ": " + error));
        }
      }
      if (sheet.name == region.top_sheet) m.top = static_cast<i32>(s);
      if (sheet.name == region.support_sheet) m.support = static_cast<i32>(s);
      ok = ok && sm.ok;
      m.sheets.push_back(std::move(sm));
    }
    if (m.top < 0)
      problems_.push_back(who + " names no top sheet it has ('" + region.top_sheet + "')");
    if (!region.shell_stitch.empty())
      blocks_.read(region.shell_stitch, BlockKind::ShellStitch, who + ".shell_stitch", m.stitch, 3);
    if (ok) m.boundary = tet_boundary(m.tets, m.nodes);
    // States: the construction first, then the definition's for this region.
    StateModel construction;
    construction.name = "construction";
    construction.nodes = m.nodes;
    m.states.push_back(std::move(construction));
    for (const RegionState& state : def_.states) {
      if (state.region != region.name) continue;
      StateModel sm;
      sm.name = state.name;
      sm.def = &state;
      sm.role = state.role;
      const std::string swho = who + " state " + state.name;
      if (!blocks_.read(state.nodes, BlockKind::StateNodes, swho, sm.nodes)) continue;
      if (sm.nodes.size() != count) {
        problems_.push_back(swho + " holds " + str(sm.nodes.size()) + " nodes for " + str(count));
        continue;
      }
      if (!state.expected_visible.empty())
        blocks_.read(state.expected_visible, BlockKind::ExpectedVisible, swho + ".expected_visible",
                     sm.expected_visible);
      if (state.role == StateRole::Reference && m.reference < 0)
        m.reference = static_cast<i32>(m.states.size());
      m.states.push_back(std::move(sm));
    }
    if (m.reference < 0) m.reference = 0;  // no reference: the construction stands in
    m.ok = ok && m.top >= 0;
    regions_.push_back(std::move(m));
  }
}

void Validator::resolve_frames() {
  for (const Frame& frame : def_.frames) {
    FrameModel m;
    m.def = &frame;
    const std::string who = "frame " + frame.name;
    bool ok = blocks_.read(frame.vertices, BlockKind::FrameVertices, who + ".vertices", m.vertices);
    ok = blocks_.read(frame.triangles, BlockKind::FrameTriangles, who + ".triangles", m.triangles,
                      3) &&
         ok;
    for (const u32 v : m.triangles)
      if (v >= m.vertices.size()) {
        problems_.push_back(who + " has a triangle naming vertex " + str(v));
        ok = false;
        break;
      }
    if (!frame.cover.empty() &&
        blocks_.read(frame.cover, BlockKind::FrameCover, who + ".cover", m.cover) &&
        m.cover.size() != m.vertices.size())
      problems_.push_back(who + ".cover is not one value per vertex");
    if (!frame.cover_provenance.empty() &&
        blocks_.read(frame.cover_provenance, BlockKind::FrameCoverProvenance,
                     who + ".cover_provenance", m.provenance) &&
        m.provenance.size() != m.vertices.size())
      problems_.push_back(who + ".cover_provenance is not one value per vertex");
    m.ok = ok;
    if (ok) {
      m.points = to_d3(m.vertices);
      m.surface.build(m.vertices, m.triangles);
    }
    frames_.push_back(std::move(m));
  }
}

void Validator::resolve_bindings() {
  for (const SkinBinding& binding : def_.bindings) {
    BindingModel m;
    m.def = &binding;
    const std::string who = "binding " + binding.name;
    for (u32 r = 0; r < regions_.size(); ++r)
      if (regions_[r].def->name == binding.region) m.region = static_cast<i32>(r);
    m.mode_known = geometry::parse_normal_mode(binding.normal_mode, m.mode);
    bool ok = m.region >= 0 && m.mode_known && binding.rule == k_loop_rule;
    if (m.region < 0)
      problems_.push_back(who + " names region '" + binding.region + "', which does not exist");
    if (!m.mode_known)
      problems_.push_back(who + " has normal mode '" + binding.normal_mode +
                          "', which this build does not know");
    if (binding.rule != k_loop_rule)
      problems_.push_back(who + " uses refinement rule '" + binding.rule +
                          "', which this build does not compute");
    if (m.region >= 0) {
      const RegionModel& region = regions_[m.region];
      for (u32 s = 0; s < region.sheets.size(); ++s)
        if (region.sheets[s].def->name == binding.sheet) m.sheet = static_cast<i32>(s);
      if (m.sheet < 0) {
        problems_.push_back(who + " names sheet '" + binding.sheet + "', which region " +
                            binding.region + " does not have");
        ok = false;
      } else if (region.sheets[m.sheet].def->level != binding.level) {
        problems_.push_back(who + " is at level " + str(binding.level) + " and its sheet at " +
                            str(region.sheets[m.sheet].def->level));
        ok = false;
      }
      ok = ok && region.ok;
    }
    ok = blocks_.read(binding.domain, BlockKind::CanonicalIds, who + ".domain", m.domain) && ok;
    ok = blocks_.read(binding.footprint.ids, BlockKind::CanonicalIds, who + ".footprint",
                      m.footprint) &&
         ok;
    ok = blocks_.read(binding.band.ids, BlockKind::CanonicalIds, who + ".band", m.band) && ok;
    ok = blocks_.read(binding.band.weights, BlockKind::IdValues, who + ".band.weights",
                      m.band_weight) &&
         ok;
    Vector<u32> triangles;
    Vector<Vec3> barycentrics;
    Vector<f32> offsets;
    ok = blocks_.read(binding.records.triangles, BlockKind::FootpointTriangles,
                      who + ".records.triangles", triangles) &&
         ok;
    ok = blocks_.read(binding.records.barycentrics, BlockKind::FootpointBarycentrics,
                      who + ".records.barycentrics", barycentrics) &&
         ok;
    if (!binding.records.offsets.empty())
      ok = blocks_.read(binding.records.offsets, BlockKind::FootpointOffsets,
                        who + ".records.offsets", offsets) &&
           ok;
    if (!binding.records.authored_normals.empty())
      blocks_.read(binding.records.authored_normals, BlockKind::AuthoredNormals,
                   who + ".records.authored_normals", m.authored_normals);
    const u32 n = m.domain.size();
    if (ok && (triangles.size() != n || barycentrics.size() != n ||
               (!offsets.empty() && offsets.size() != n) ||
               (!m.authored_normals.empty() && m.authored_normals.size() != n) ||
               m.band_weight.size() != m.band.size())) {
      problems_.push_back(
          who + "'s records, offsets, normals or band weights are not one per id of their set");
      ok = false;
    }
    if (ok && (!strictly_ascending(m.domain) || !strictly_ascending(m.footprint) ||
               !strictly_ascending(m.band))) {
      problems_.push_back(who + "'s domain, footprint or band is not strictly ascending");
      ok = false;
    }
    if (ok && observation_.ok && !m.domain.empty() &&
        m.domain.back() >= observation_.positions.size()) {
      problems_.push_back(who + "'s domain names an id past the observation's vertices");
      ok = false;
    }
    if (ok) {
      m.weight.assign(n, 0.0f);
      for (u32 i = 0; i < n; ++i) {
        const u32 id = m.domain[i];
        if (contains_sorted(m.footprint, id)) m.weight[i] = 1.0f;
        const auto it = std::lower_bound(m.band.begin(), m.band.end(), id);
        if (it != m.band.end() && *it == id)
          m.weight[i] = m.band_weight[static_cast<u32>(it - m.band.begin())];
      }
      for (u32 i = 0; i < n; ++i)
        m.footpoints.push_back(geometry::AuthoredFootpoint{
            triangles[i], barycentrics[i],
            offsets.empty() ? std::numeric_limits<f32>::quiet_NaN() : offsets[i], m.weight[i]});
    }
    // The records, under the binding's own mode, at the region's reference.
    if (ok && observation_.ok) {
      const RegionModel& region = regions_[m.region];
      const SheetModel& sheet = region.sheets[m.sheet];
      const Vector<Vec3> ref = region.sheet_nodes(sheet, region.states[region.reference].nodes);
      geometry::BindingFrame frame;
      std::string error;
      if (!geometry::evaluate_binding_frame(m.mode, sheet.surface, ref, ref, frame, &error)) {
        problems_.push_back(who + ": " + error);
        ok = false;
      } else {
        Vector<Vec3> base;
        for (const u32 id : m.domain)
          base.push_back(observation_.positions[id]);
        if (!geometry::bind_from_records(
                m.mode, base, m.footpoints, frame.view(sheet.surface.faces),
                geometry::SurfaceBindOptions{}, m.records, &m.bind_report, &error)) {
          problems_.push_back(who + ": " + error);
          ok = false;
        }
      }
    }
    m.ok = ok;
    bindings_.push_back(std::move(m));
  }
}

bool Validator::transfer(const BindingModel& b, const StateModel& state, geometry::NormalMode mode,
                         geometry::BindingTerms terms, Vector<Vec3>& domain_out,
                         geometry::BindingFrame* frame_out) const {
  const RegionModel& region = regions_[b.region];
  const SheetModel& sheet = region.sheets[b.sheet];
  const Vector<Vec3> ref = region.sheet_nodes(sheet, region.states[region.reference].nodes);
  const Vector<Vec3> now = region.sheet_nodes(sheet, state.nodes);
  geometry::BindingFrame frame;
  if (!geometry::evaluate_binding_frame(mode, sheet.surface, ref, now, frame)) return false;
  Vector<Vec3> base;
  for (const u32 id : b.domain)
    base.push_back(observation_.positions[id]);
  domain_out.resize(base.size());
  geometry::apply_binding(mode, b.records, base, frame.view(sheet.surface.faces),
                          std::span<Vec3>(domain_out.data(), domain_out.size()), terms);
  if (frame_out != nullptr) *frame_out = std::move(frame);
  return true;
}

Vector<Vec3> Validator::body_with(const BindingModel& b,
                                  const Vector<Vec3>& domain_positions) const {
  Vector<Vec3> body = observation_.positions;
  for (u32 i = 0; i < b.domain.size(); ++i)
    body[b.domain[i]] = domain_positions[i];
  return body;
}

Vector<Vec3> Validator::skin_at(i32 region, const StateModel& state) const {
  Vector<Vec3> body = observation_.positions;
  for (const BindingModel& b : bindings_) {
    if (b.region != region || !b.ok) continue;
    Vector<Vec3> moved;
    if (!transfer(b, state, b.mode, geometry::BindingTerms::full, moved)) continue;
    for (u32 i = 0; i < b.domain.size(); ++i)
      body[b.domain[i]] = moved[i];
  }
  return body;
}

// ---- the observation contract -------------------------------------------------------------------

void Validator::check_observation() {
  const Observation& o = def_.observation;
  const std::string subject = "observation of " + o.base_id;
  ObservationModel& m = observation_;
  {
    ValidationRow& r = row("observation.topology", subject, Severity::error,
                           "SHA-256 of the base's face indices as little-endian u64 equals the "
                           "declared topology_sha256",
                           "an observation is accepted against one declared base, and the topology "
                           "is what every canonical id names a vertex of");
    const TissueBlock* faces = file_.find(o.base_faces);
    if (faces == nullptr) {
      r.verdict = Verdict::skipped;
    } else {
      Vector<u32> indices(static_cast<u32>(faces->bytes.size() / 4));
      std::memcpy(indices.data(), faces->bytes.data(), indices.size() * 4);
      const std::string hash = topology_sha256(indices);
      r.value.set("computed", JsonValue(hash));
      r.value.set("declared", JsonValue(o.topology_sha256));
      if (hash != o.topology_sha256) fail(r, "the computed hash is " + hash);
    }
  }
  {
    ValidationRow& r = row("observation.outside_bitwise", subject, Severity::error,
                           "the observation equals the base bit for bit at every id outside the "
                           "declared domain",
                           "outside an explicitly declared id set the accepted shape is the base, "
                           "and the transfer then leaves it so");
    if (!m.ok) {
      r.verdict = Verdict::skipped;
    } else {
      u32 outside = 0;
      u32 differ = 0;
      u32 domain_at = 0;
      for (u32 id = 0; id < m.positions.size(); ++id) {
        if (domain_at < m.domain.size() && m.domain[domain_at] == id) {
          ++domain_at;
          continue;
        }
        const Vec3 base = m.base_outside[outside++];
        if (std::memcmp(&base, &m.positions[id], sizeof(Vec3)) != 0) {
          if (differ == 0) r.witness = "id " + str(id);
          ++differ;
        }
      }
      r.value.set("outside", JsonValue(static_cast<u64>(outside)));
      r.value.set("domain", JsonValue(static_cast<u64>(m.domain.size())));
      r.value.set("differing", JsonValue(static_cast<u64>(differ)));
      if (differ > 0) r.verdict = Verdict::fail;
    }
  }
  {
    ValidationRow& r = row("observation.deviation", subject, Severity::info,
                           "reported: the signed deviation from the base along its normals, inside "
                           "the domain",
                           "where the accepted shape lies inside or outside the base");
    Vector<f64> values;
    for (const f32 v : m.deviation)
      values.push_back(static_cast<f64>(v));
    r.value.set("signed_deviation_mm", stats(values, k_mm));
  }
  {
    ValidationRow& r = row("observation.landmarks", subject, Severity::error,
                           "each landmark names an id of the base, and its stated position is the "
                           "observation's there, bit for bit",
                           "landmarks are named by canonical id, never by position");
    JsonValue list = JsonValue::array();
    for (const Landmark& l : o.landmarks) {
      JsonValue entry = JsonValue::object();
      entry.set("name", JsonValue(l.name));
      entry.set("id", JsonValue(l.id));
      if (!m.ok || l.id >= m.positions.size()) {
        fail(r, "landmark " + l.name + " names id " + str(l.id));
      } else if (l.position.has_value()) {
        const Vec3 p = *l.position;
        const bool same = std::memcmp(&p, &m.positions[l.id], sizeof(Vec3)) == 0;
        entry.set("position_bitwise", JsonValue(same));
        if (!same)
          fail(r, "landmark " + l.name + " is not the observation's position at id " + str(l.id));
      }
      list.push_back(std::move(entry));
    }
    r.value.set("landmarks", std::move(list));
  }
  {
    ValidationRow& r = row("observation.load", subject, Severity::warning,
                           "a finite, nonzero gravity vector and a named medium",
                           "a shape is read under a load, and the load is part of the contract");
    const Vec3 g = o.load.gravity;
    r.value.set("gravity", JsonValue(std::string(xyz(d3(g)))));
    r.value.set("medium", JsonValue(o.load.medium));
    r.value.set("pose", JsonValue(o.load.pose));
    r.value.set("fill", JsonValue(o.load.fill));
    const f64 magnitude = query::length(d3(g));
    if (!(magnitude > 0.0) || !std::isfinite(magnitude) || o.load.medium.empty())
      fail(r, "the load is not declared");
  }
  {
    ValidationRow& r = row("observation.acceptance", subject, Severity::info,
                           "reported, never gated: acceptance is a record beside the geometry",
                           "every geometric row may pass while this reads false");
    r.value.set("accepted", JsonValue(o.acceptance.accepted));
    r.value.set("who", JsonValue(o.acceptance.who));
    r.value.set("when", JsonValue(o.acceptance.when));
    r.value.set("sheet", JsonValue(o.acceptance.sheet));
  }
  JsonValue& numbers = report_.numbers["observation"];
  numbers = JsonValue::object();
  numbers.set("base_id", JsonValue(o.base_id));
  numbers.set("vertices", JsonValue(o.vertex_count));
  numbers.set("domain", JsonValue(static_cast<u64>(m.domain.size())));
  numbers.set("accepted", JsonValue(o.acceptance.accepted));
}

// ---- regions
// ----------------------------------------------------------------------------------------

void Validator::check_regions() {
  for (RegionModel& region : regions_) {
    const std::string subject = "region " + region.def->name;
    JsonValue& numbers = region_numbers(region);
    numbers.set("nodes", JsonValue(static_cast<u64>(region.nodes.size())));
    numbers.set("tetrahedra", JsonValue(static_cast<u64>(region.tets.size() / 4)));
    {
      ValidationRow& r =
          row("region.cage_size", subject, Severity::warning,
              "at most 256 nodes unless hero, at most 800 for any volume (ADR-0029, "
              "physics::cage_size_verdict)",
              "a cage wider than one solve group does not scale; past 800 it is refused");
      const u32 nodes = region.nodes.size();
      const physics::CageSizeVerdict v = physics::cage_size_verdict(nodes, region.def->hero);
      const char* names[] = {"Ok", "Wide", "Refused"};
      r.value.set("nodes", JsonValue(nodes));
      r.value.set("hero", JsonValue(region.def->hero));
      r.value.set("verdict", JsonValue(names[static_cast<u32>(v)]));
      r.value.set("verdict_as_hero",
                  JsonValue(names[static_cast<u32>(physics::cage_size_verdict(nodes, true))]));
      if (v == physics::CageSizeVerdict::Wide)
        fail(r, str(nodes) + " nodes, " + str(nodes - physics::k_cage_elements_default) +
                    " over the ambient default");
      if (v == physics::CageSizeVerdict::Refused) {
        r.severity = Severity::error;
        fail(r, str(nodes) + " nodes, past the 800 limit");
      }
      numbers.set("cage_verdict", JsonValue(names[static_cast<u32>(v)]));
    }
    if (!region.ok) continue;
    {
      ValidationRow& r = row("region.cell_quality", subject, Severity::warning,
                             "every tetrahedron's SICN (Knupp's signed inverse condition number, "
                             "as Gmsh reports it) above 0.1; at or below 0 is an inverted cell",
                             "the imported tetrahedral cage kind needs a quality row: no engine "
                             "tetrahedralizer stands behind it");
      f64 worst = std::numeric_limits<f64>::infinity();
      u32 worst_tet = 0;
      f64 smallest = std::numeric_limits<f64>::infinity();
      f64 largest = 0.0;
      Vector<f64> all;
      for (u32 t = 0; t + 3 < region.tets.size(); t += 4) {
        const D3 a = d3(region.nodes[region.tets[t]]);
        const D3 b = d3(region.nodes[region.tets[t + 1]]);
        const D3 c = d3(region.nodes[region.tets[t + 2]]);
        const D3 d = d3(region.nodes[region.tets[t + 3]]);
        const f64 q = query::tet_sicn(a, b, c, d);
        const f64 v = std::fabs(query::tet_volume(a, b, c, d));
        all.push_back(q);
        smallest = std::min(smallest, v);
        largest = std::max(largest, v);
        if (q < worst) {
          worst = q;
          worst_tet = t / 4;
        }
      }
      r.value.set("sicn", stats(all, 1.0));
      r.value.set("volume_ratio", number(largest / smallest));
      r.witness = "tetrahedron " + str(worst_tet);
      if (worst <= 0.0) {
        r.severity = Severity::error;
        fail(r, "tetrahedron " + str(worst_tet) + " is inverted or flat");
      } else if (worst < 0.1) {
        fail(r, "tetrahedron " + str(worst_tet));
      }
      numbers.set("sicn_min", number(worst));
    }
    {
      ValidationRow& r = row("region.cell_orientation", subject, Severity::error,
                             "every tetrahedron positive in every state",
                             "a cell that turns inside "
                             "out has a volume constraint pushing it further out");
      JsonValue per_state = JsonValue::object();
      for (const StateModel& s : region.states) {
        u32 inverted = 0;
        f64 smallest = std::numeric_limits<f64>::infinity();
        for (u32 t = 0; t + 3 < region.tets.size(); t += 4) {
          const f64 v =
              query::tet_volume(d3(s.nodes[region.tets[t]]), d3(s.nodes[region.tets[t + 1]]),
                                d3(s.nodes[region.tets[t + 2]]), d3(s.nodes[region.tets[t + 3]]));
          smallest = std::min(smallest, v);
          if (!(v > 0.0)) {
            if (inverted == 0) fail(r, "state " + s.name + ", tetrahedron " + str(t / 4));
            ++inverted;
          }
        }
        JsonValue e = JsonValue::object();
        e.set("inverted", JsonValue(inverted));
        e.set("smallest_m3", number(smallest));
        per_state.set(s.name, std::move(e));
      }
      r.value.set("states", std::move(per_state));
    }
    {
      ValidationRow& r =
          row("region.materials", subject, Severity::error,
              "moduli and densities positive, phase fractions in [0, 1] summing to at "
              "most 1 per cell, membranes 0 <= E.t and -1 < nu < 0.5, cables with "
              "nonnegative stiffness, slack and recruitment",
              "physical quantities in SI units only; the build derives compliances");
      const Region& def = *region.def;
      if (def.phases.empty()) fail(r, "the region has no material");
      Vector<f64> sum(static_cast<u32>(region.tets.size() / 4), 0.0);
      JsonValue phases = JsonValue::array();
      for (u32 p = 0; p < def.phases.size(); ++p) {
        const Material& mat = def.phases[p].material;
        JsonValue e = JsonValue::object();
        e.set("name", JsonValue(mat.name));
        e.set("bulk_modulus_pa", JsonValue(mat.bulk_modulus_pa));
        e.set("shear_modulus_pa", JsonValue(mat.shear_modulus_pa));
        e.set("density_kg_m3", JsonValue(mat.density_kg_m3));
        phases.push_back(std::move(e));
        if (!(mat.bulk_modulus_pa > 0.0) || !(mat.shear_modulus_pa > 0.0) ||
            !(mat.density_kg_m3 > 0.0))
          fail(r, "material " + mat.name + " has a modulus or density that is not positive");
        if (p == 0) continue;
        Vector<f32> fraction;
        if (!blocks_.read(def.phases[p].fraction, BlockKind::PhaseFraction,
                          subject + " phase " + mat.name, fraction) ||
            fraction.size() != sum.size()) {
          fail(r, "phase " + mat.name + " has no fraction per tetrahedron");
          continue;
        }
        for (u32 t = 0; t < sum.size(); ++t) {
          if (!(fraction[t] >= 0.0f && fraction[t] <= 1.0f))
            fail(r, "phase " + mat.name + ", tetrahedron " + str(t));
          sum[t] += static_cast<f64>(fraction[t]);
        }
      }
      for (u32 t = 0; t < sum.size(); ++t)
        if (sum[t] > 1.0 + 1e-6) fail(r, "tetrahedron " + str(t) + "'s phases sum past 1");
      r.value.set("phases", std::move(phases));
      for (const Membrane& mem : def.membranes) {
        Vector<u32> tris;
        if (!blocks_.read(mem.triangles, BlockKind::MembraneTriangles,
                          subject + " membrane " + mem.name, tris, 3))
          fail(r, "membrane " + mem.name + " has no triangles");
        for (const u32 v : tris)
          if (v >= region.nodes.size())
            fail(r, "membrane " + mem.name + " names a node past the region's");
        Vector<f32> et;
        if (!mem.stiffness.empty() && blocks_.read(mem.stiffness, BlockKind::MembraneStiffness,
                                                   subject + " membrane " + mem.name, et))
          for (const f32 v : et)
            if (!(v >= 0.0f)) fail(r, "membrane " + mem.name + " has a negative E.t");
        if (!(mem.poisson > -1.0 && mem.poisson < 0.5))
          fail(r, "membrane " + mem.name + " has Poisson's ratio " + std::to_string(mem.poisson));
      }
      for (const CableSet& cab : def.cables) {
        Vector<u32> edges;
        if (!blocks_.read(cab.edges, BlockKind::CableEdges, subject + " cables " + cab.name, edges,
                          2))
          fail(r, "cable set " + cab.name + " has no edges");
        for (const u32 v : edges)
          if (v >= region.nodes.size())
            fail(r, "cable set " + cab.name + " names a node past the region's");
        if (!(cab.slack >= 0.0) || !(cab.recruitment >= 0.0) || !(cab.stiffness_n_per_m >= 0.0))
          fail(r, "cable set " + cab.name + " has a negative slack, recruitment or stiffness");
      }
      r.value.set("membranes", JsonValue(static_cast<u64>(def.membranes.size())));
      r.value.set("cable_sets", JsonValue(static_cast<u64>(def.cables.size())));
      // Mass, from the phases' densities.
      f64 mass = 0.0;
      if (!def.phases.empty()) {
        Vector<Vector<f32>> fractions(def.phases.size());
        for (u32 p = 1; p < def.phases.size(); ++p)
          blocks_.read(def.phases[p].fraction, BlockKind::PhaseFraction, subject, fractions[p]);
        for (u32 t = 0; t + 3 < region.tets.size(); t += 4) {
          const f64 v = query::tet_volume(
              d3(region.nodes[region.tets[t]]), d3(region.nodes[region.tets[t + 1]]),
              d3(region.nodes[region.tets[t + 2]]), d3(region.nodes[region.tets[t + 3]]));
          f64 rest = 1.0;
          f64 density = 0.0;
          for (u32 p = 1; p < def.phases.size(); ++p) {
            const f64 f = fractions[p].size() > t / 4 ? static_cast<f64>(fractions[p][t / 4]) : 0.0;
            density += f * def.phases[p].material.density_kg_m3;
            rest -= f;
          }
          density += rest * def.phases[0].material.density_kg_m3;
          mass += density * v;
        }
      }
      numbers.set("mass_kg", number(mass));
    }
    {
      ValidationRow& r = row("region.sheets", subject, Severity::warning,
                             "every sheet a Loop surface of at most five faces at any control "
                             "boundary vertex (Hoppe's rules are G1 only there), and its refined "
                             "triangles within the binding record's u16",
                             "the skin follows the top sheet's limit surface");
      JsonValue list = JsonValue::object();
      for (const SheetModel& s : region.sheets) {
        JsonValue e = JsonValue::object();
        e.set("control_vertices", JsonValue(static_cast<u64>(s.region_node.size())));
        e.set("control_triangles", JsonValue(static_cast<u64>(s.faces.size() / 3)));
        e.set("level", JsonValue(s.def->level));
        e.set("refined_triangles", JsonValue(s.surface.triangle_count()));
        e.set("max_boundary_faces", JsonValue(s.surface.max_boundary_faces));
        if (s.surface.max_boundary_faces > 5)
          fail(r, "sheet " + s.def->name + " has a boundary vertex with " +
                      str(s.surface.max_boundary_faces) + " faces");
        if (!geometry::surface_binding_can_represent(s.surface.triangle_count())) {
          r.severity = Severity::error;
          fail(r, "sheet " + s.def->name + " refines to " + str(s.surface.triangle_count()) +
                      " triangles, past 65,536");
        }
        list.set(s.def->name, std::move(e));
      }
      r.value.set("sheets", std::move(list));
    }
    {
      ValidationRow& r = row("state.inverse", subject, Severity::error,
                             "every state that claims inverse statics declares its method and its "
                             "interior extension, and the node sets it held exist",
                             "one exterior observation does not identify the interior rest, so a "
                             "fit declares the prior it put there");
      JsonValue per_state = JsonValue::object();
      for (const StateModel& s : region.states) {
        if (s.def == nullptr || s.def->provenance != StateProvenance::InverseStatics) continue;
        JsonValue e = JsonValue::object();
        const std::optional<InverseFit>& fit = s.def->inverse;
        if (!fit.has_value() || fit->method.empty() || fit->interior_extension.empty()) {
          fail(r, "state " + s.name +
                      " claims inverse statics and declares no method or interior extension");
          e.set("declared", JsonValue(false));
          per_state.set(s.name, std::move(e));
          continue;
        }
        e.set("declared", JsonValue(true));
        e.set("method", JsonValue(fit->method));
        e.set("interior_extension", JsonValue(fit->interior_extension));
        e.set("step_alpha", JsonValue(fit->step_alpha));
        JsonValue held = JsonValue::array();
        for (const std::string& set : fit->held_node_sets) {
          held.push_back(JsonValue(set));
          bool exists = false;
          for (const NodeSetRef& n : region.def->node_sets)
            exists = exists || n.name == set;
          if (!exists)
            fail(r, "state " + s.name + " held node set '" + set + "', which the region lacks");
        }
        e.set("held_node_sets", std::move(held));
        e.set("fit_residual_p95_mm", fit->fit_residual_p95_mm.has_value()
                                         ? number(*fit->fit_residual_p95_mm)
                                         : JsonValue::null());
        e.set("min_cell_ratio",
              fit->min_cell_ratio.has_value() ? number(*fit->min_cell_ratio) : JsonValue::null());
        const bool known_method =
            fit->method == "surface-targeted" || fit->method == "legacy-all-nodes";
        const bool known_extension = fit->interior_extension == "harmonic" ||
                                     fit->interior_extension == "elastic" ||
                                     fit->interior_extension == "none";
        e.set("known_spellings", JsonValue(known_method && known_extension));
        per_state.set(s.name, std::move(e));
      }
      r.value.set("states", std::move(per_state));
    }
    {
      ValidationRow& r =
          row("state.inverse_solver", subject, Severity::warning,
              "every state that claims inverse statics names the solver its rest was recovered "
              "against; an xpbd one its iterations and step, a static minimization its gradient "
              "tolerance",
              "the engine-facing reference is the fixed point of the runtime solver, and a rest "
              "reproduces its observation only under the solver, iterations and step it was "
              "recovered against");
      JsonValue per_state = JsonValue::object();
      for (const StateModel& s : region.states) {
        if (s.def == nullptr || s.def->provenance != StateProvenance::InverseStatics) continue;
        JsonValue e = JsonValue::object();
        e.set("reference", JsonValue(s.role == StateRole::Reference));
        const std::optional<InverseFit>& fit = s.def->inverse;
        if (!fit.has_value() || !fit->solver.has_value() || fit->solver->kind.empty()) {
          fail(r, "state " + s.name + " names no solver its rest was recovered against");
          e.set("declared", JsonValue(false));
          per_state.set(s.name, std::move(e));
          continue;
        }
        const InverseSolver& solver = *fit->solver;
        e.set("declared", JsonValue(true));
        e.set("kind", JsonValue(solver.kind));
        e.set("iterations", JsonValue(solver.iterations));
        e.set("step_s", number(solver.step_s));
        e.set("duration_s",
              solver.duration_s.has_value() ? number(*solver.duration_s) : JsonValue::null());
        e.set("damping_per_s",
              solver.damping_per_s.has_value() ? number(*solver.damping_per_s) : JsonValue::null());
        e.set("gradient_tolerance_n", solver.gradient_tolerance_n.has_value()
                                          ? number(*solver.gradient_tolerance_n)
                                          : JsonValue::null());
        e.set("implementation", JsonValue(solver.implementation));
        const bool xpbd = solver.kind == "xpbd";
        const bool minimization = solver.kind == "static-minimization";
        e.set("known_kind", JsonValue(xpbd || minimization));
        const bool pinned = xpbd ? solver.iterations > 0 && solver.step_s > 0.0
                                 : !minimization || (solver.gradient_tolerance_n.has_value() &&
                                                     *solver.gradient_tolerance_n > 0.0);
        if (!pinned) {
          r.severity = Severity::error;
          r.witness.clear();
          fail(r, "state " + s.name + "'s " + solver.kind + " solver names no " +
                      (xpbd ? "iterations or no step" : "gradient tolerance") +
                      ", so it pins no fixed point");
        }
        per_state.set(s.name, std::move(e));
      }
      r.value.set("states", std::move(per_state));
    }
    {
      ValidationRow& r =
          row("region.shell_closed", subject, Severity::error,
              "the sheets' refined triangles and the stitch close a consistently "
              "wound 2-manifold of positive volume",
              "the closed smooth shell is one of the four volumes, and a volume needs "
              "a closed surface");
      if (region.stitch.empty()) {
        r.verdict = Verdict::skipped;
        r.note = "the region declares no shell stitch";
      } else {
        Vector<D3> points;
        Vector<u32> faces;
        for (const SheetModel& s : region.sheets) {
          const u32 offset = points.size();
          const Vector<Vec3> dense = evaluate(s.surface.limit, region.sheet_nodes(s, region.nodes));
          for (const Vec3& p : dense)
            points.push_back(d3(p));
          for (const u32 v : s.surface.faces)
            faces.push_back(v + offset);
        }
        bool in_range = true;
        for (const u32 v : region.stitch) {
          in_range = in_range && v < points.size();
          faces.push_back(v);
        }
        if (!in_range) {
          fail(r, "the stitch names a refined vertex past the sheets'");
        } else {
          const ManifoldCheck check = check_manifold(faces, points);
          const f64 volume = query::enclosed_volume(points, faces);
          r.value.set("triangles", JsonValue(static_cast<u64>(faces.size() / 3)));
          r.value.set("volume_ml", number(volume * k_ml));
          if (!check.closed_manifold) fail(r, check.witness);
          if (!(volume > 0.0))
            fail(r, "the shell encloses a volume of " + std::to_string(volume * k_ml) + " ml");
        }
      }
    }
  }
}

void Validator::check_attachments() {
  ValidationRow& r = row("attachments.resolve", "the attachments", Severity::error,
                         "every attachment names a region, a node set of it, and a target that "
                         "exists: a frame, a surface, another region's sheet, the world or a bone",
                         "an attachment to nothing is a region that falls");
  JsonValue list = JsonValue::array();
  for (const Attachment& a : def_.attachments) {
    JsonValue e = JsonValue::object();
    e.set("name", JsonValue(a.name));
    const char* kinds[] = {"fixed", "sliding-bilateral", "unilateral"};
    e.set("kind", JsonValue(kinds[static_cast<u32>(a.kind) % 3]));
    e.set("target", JsonValue(a.target));
    const RegionModel* region = nullptr;
    for (const RegionModel& m : regions_)
      if (m.def->name == a.region) region = &m;
    if (region == nullptr) {
      fail(r, "attachment " + a.name + " names region '" + a.region + "'");
      list.push_back(std::move(e));
      continue;
    }
    const NodeSetRef* set = nullptr;
    for (const NodeSetRef& s : region->def->node_sets)
      if (s.name == a.nodes) set = &s;
    Vector<u32> nodes;
    if (set == nullptr ||
        !blocks_.read(set->nodes, BlockKind::NodeSet, "attachment " + a.name, nodes)) {
      fail(r, "attachment " + a.name + " names node set '" + a.nodes + "'");
    } else {
      e.set("nodes", JsonValue(static_cast<u64>(nodes.size())));
      for (const u32 v : nodes)
        if (v >= region->nodes.size()) fail(r, "attachment " + a.name + " names node " + str(v));
    }
    bool target = false;
    bool proxy = false;
    switch (a.target_kind) {
      case TargetKind::World:
      case TargetKind::Bone:
        target = !a.target.empty() || a.target_kind == TargetKind::World;
        break;
      case TargetKind::Frame:
        for (const Frame& f : def_.frames)
          if (f.name == a.target) target = true, proxy = f.proxy;
        break;
      case TargetKind::Surface:
        for (const Surface& s : def_.surfaces)
          if (s.name == a.target) target = true, proxy = s.proxy;
        break;
      case TargetKind::RegionSheet: {
        const usize slash = a.target.find('/');
        if (slash != std::string::npos)
          for (const Region& reg : def_.regions)
            if (reg.name == a.target.substr(0, slash))
              for (const Sheet& sh : reg.sheets)
                if (sh.name == a.target.substr(slash + 1)) target = true;
        break;
      }
    }
    e.set("target_proxy", JsonValue(proxy));
    if (!target) fail(r, "attachment " + a.name + " names target '" + a.target + "'");
    list.push_back(std::move(e));
  }
  r.value.set("attachments", std::move(list));
}

// ---- mechanics
// ------------------------------------------------------------------------------------

// What the declared materials make of the cage, from the declared energy (energy.h, law
// `bulk-edge-v0`): how many cells span the region's thickness, what an affine deformation of the
// whole region reads as moduli against the declared ones (the fabric of its edges), how far the
// reference's cells are strained against the rest (a pre-stress the rest carries), and the
// equilibrium gap — the energy's exact gradient at the free nodes of the rest, the reference and
// every response. The gap is a diagnostic and never a failure: a rest recovered against an XPBD
// relaxation reproduces its observation at the relaxation's fixed point, which is not the energy's
// stationary point, and the definition says the engine-facing reference is the fixed point
// (InverseSolver). What the energy does not yet include (membranes, cables) the gap row lists.
void Validator::check_mechanics() {
  // A unilateral attachment's node reacts along the target's normal only while it touches it.
  constexpr f64 k_contact_m = 1.0e-4;
  enum : u8 { k_free, k_sliding, k_contact, k_held, k_no_geometry };
  const char* class_names[] = {"free", "sliding", "contact", "held", "no_target_geometry"};
  // Surfaces a sliding or unilateral attachment slides on, built once.
  Vector<query::SignedSurface> surfaces(static_cast<u32>(def_.surfaces.size()));
  Vector<u8> surface_built(static_cast<u32>(def_.surfaces.size()), 0);
  const auto target_surface = [&](const Attachment& a) -> const query::SignedSurface* {
    if (a.target_kind == TargetKind::Frame) {
      for (const FrameModel& f : frames_)
        if (f.def->name == a.target && f.ok) return &f.surface;
      return nullptr;
    }
    if (a.target_kind != TargetKind::Surface) return nullptr;
    for (u32 i = 0; i < def_.surfaces.size(); ++i) {
      const Surface& s = def_.surfaces[i];
      if (s.name != a.target) continue;
      if (surface_built[i] == 0) {
        surface_built[i] = 2;
        Vector<Vec3> vertices;
        Vector<u32> triangles;
        bool ok =
            blocks_.read(s.vertices, BlockKind::SurfaceVertices, "surface " + s.name, vertices) &&
            blocks_.read(s.triangles, BlockKind::SurfaceTriangles, "surface " + s.name, triangles,
                         3);
        for (const u32 v : triangles)
          ok = ok && v < vertices.size();
        if (ok && !triangles.empty()) {
          surfaces[i].build(vertices, triangles);
          surface_built[i] = 1;
        }
      }
      return surface_built[i] == 1 ? &surfaces[i] : nullptr;
    }
    return nullptr;
  };

  for (RegionModel& region : regions_) {
    if (!region.ok) continue;
    const Region& def = *region.def;
    const std::string subject = "region " + def.name;
    const u32 cells = static_cast<u32>(region.tets.size() / 4);
    const u32 n = static_cast<u32>(region.nodes.size());

    // The nodes' attachments: held (fixed, or a target the gap cannot see), sliding (the target's
    // normal reaction is removed), unilateral (removed while in contact). Fixed wins.
    Vector<u8> kind(n, k_free);
    Vector<const query::SignedSurface*> target(n, nullptr);
    Vector<u8> unilateral(n, 0);
    for (const Attachment& a : def_.attachments) {
      if (a.region != def.name) continue;
      const NodeSetRef* set = nullptr;
      for (const NodeSetRef& s : def.node_sets)
        if (s.name == a.nodes) set = &s;
      Vector<u32> nodes;
      if (set == nullptr || !blocks_.read(set->nodes, BlockKind::NodeSet, subject, nodes)) continue;
      const query::SignedSurface* surface =
          a.kind == AttachmentKind::Fixed ? nullptr : target_surface(a);
      for (const u32 v : nodes) {
        if (v >= n || kind[v] == k_held) continue;
        if (a.kind == AttachmentKind::Fixed) {
          kind[v] = k_held;
        } else if (surface == nullptr) {
          kind[v] = k_no_geometry;
        } else if (kind[v] != k_no_geometry) {
          kind[v] = k_sliding;
          target[v] = surface;
          unilateral[v] = a.kind == AttachmentKind::Unilateral ? 1 : 0;
        }
      }
    }

    // ---- through-thickness resolution
    {
      ValidationRow& r =
          row("region.through_thickness", subject, Severity::info,
              "reported: the cells that span both sheets, and the nodes inside the cage",
              "a slab one cell thick has no through-thickness freedom, and refining it changes "
              "its response for that reason alone");
      Vector<u8> on_top(n, 0);
      Vector<u8> on_support(n, 0);
      Vector<u8> on_boundary(n, 0);
      for (const u32 v : region.sheets[region.top].region_node)
        on_top[v] = 1;
      if (region.support >= 0)
        for (const u32 v : region.sheets[region.support].region_node)
          on_support[v] = 1;
      for (const u32 v : region.boundary)
        on_boundary[v] = 1;
      u32 spanning = 0;
      for (u32 t = 0; t < cells; ++t) {
        bool top = false;
        bool support = false;
        for (u32 k = 0; k < 4; ++k) {
          top = top || on_top[region.tets[4 * t + k]] != 0;
          support = support || on_support[region.tets[4 * t + k]] != 0;
        }
        if (top && support) ++spanning;
      }
      u32 interior = 0;
      u32 on_neither = 0;
      for (u32 i = 0; i < n; ++i) {
        if (on_boundary[i] == 0) ++interior;
        if (on_top[i] == 0 && on_support[i] == 0) ++on_neither;
      }
      r.value.set("cells", JsonValue(cells));
      r.value.set("cells_spanning_both_sheets", JsonValue(spanning));
      r.value.set("nodes", JsonValue(n));
      r.value.set("interior_nodes", JsonValue(interior));
      r.value.set("nodes_on_neither_sheet", JsonValue(on_neither));
      r.value.set("support_sheet", JsonValue(region.support >= 0));
      region_numbers(region).set("cells_spanning_both_sheets", JsonValue(spanning));
      region_numbers(region).set("interior_nodes", JsonValue(interior));
    }

    // Each cell's material and density: the phases mixed by fraction, as region.materials mixes
    // them for the mass.
    Vector<energy::CellMaterial> materials(cells);
    Vector<f64> density(cells, 0.0);
    if (!def.phases.empty()) {
      Vector<Vector<f32>> fractions(static_cast<u32>(def.phases.size()));
      for (u32 p = 1; p < def.phases.size(); ++p)
        blocks_.read(def.phases[p].fraction, BlockKind::PhaseFraction, subject, fractions[p]);
      for (u32 t = 0; t < cells; ++t) {
        f64 first = 1.0;
        for (u32 p = 1; p < def.phases.size(); ++p) {
          const f64 f = fractions[p].size() > t ? static_cast<f64>(fractions[p][t]) : 0.0;
          const Material& m = def.phases[p].material;
          materials[t].bulk_modulus_pa += f * m.bulk_modulus_pa;
          materials[t].shear_modulus_pa += f * m.shear_modulus_pa;
          density[t] += f * m.density_kg_m3;
          first -= f;
        }
        const Material& m = def.phases[0].material;
        materials[t].bulk_modulus_pa += first * m.bulk_modulus_pa;
        materials[t].shear_modulus_pa += first * m.shear_modulus_pa;
        density[t] += first * m.density_kg_m3;
      }
    }
    const StateModel* rest = nullptr;
    for (const StateModel& s : region.states)
      if (rest == nullptr && s.def != nullptr && s.role == StateRole::Rest) rest = &s;
    energy::BulkEdgeEnergy model;
    std::string model_error;
    bool model_ok = false;
    if (rest == nullptr)
      model_error =
          "the region declares no rest state, so its energy has no rest to be measured "
          "from";
    else if (def.phases.empty())
      model_error = "the region has no material";
    else
      model_ok = model.build(to_d3(rest->nodes), region.tets, materials, &model_error);

    // ---- the affine patch test
    {
      ValidationRow& r =
          row("region.affine_patch", subject, Severity::info,
              "reported: the effective shear modulus of each plane, globally and in the region's "
              "frame, the isotropic shear and the bulk modulus, from an affine deformation of the "
              "whole region at its rest, against the declared moduli",
              "a network of edges carries its mesh's fabric: the law calibrates its isotropic "
              "part to the declared shear modulus exactly, and the planes are the mesh's");
      r.value.set("law", JsonValue(energy::k_law));
      if (!model_ok) {
        r.verdict = Verdict::skipped;
        r.note = model_error;
      } else {
        const Vector<D3> x0 = to_d3(rest->nodes);
        const std::span<const f64> v0 = model.rest_volumes();
        f64 volume = 0.0;
        f64 mu = 0.0;
        f64 kappa = 0.0;
        for (u32 t = 0; t < cells; ++t) {
          volume += v0[t];
          mu += materials[t].shear_modulus_pa * v0[t];
          kappa += materials[t].bulk_modulus_pa * v0[t];
        }
        mu /= volume;
        kappa /= volume;
        constexpr f64 k_gamma = 1.0e-4;
        struct M3 {
          f64 m[3][3];
        };
        // The energy of x' = x + gamma S x, averaged over +gamma and -gamma so the odd orders
        // cancel and what is left is the quadratic form to a relative gamma^2.
        const auto strained = [&](const M3& s) {
          f64 sum = 0.0;
          for (const f64 sign : {1.0, -1.0}) {
            const f64 g = sign * k_gamma;
            Vector<D3> x(n);
            for (u32 i = 0; i < n; ++i) {
              const D3 p = x0[i];
              x[i] = D3{p.x + g * (s.m[0][0] * p.x + s.m[0][1] * p.y + s.m[0][2] * p.z),
                        p.y + g * (s.m[1][0] * p.x + s.m[1][1] * p.y + s.m[1][2] * p.z),
                        p.z + g * (s.m[2][0] * p.x + s.m[2][1] * p.y + s.m[2][2] * p.z)};
            }
            const energy::BulkEdgeEnergy::Terms t = model.energy(x);
            sum += t.bulk + t.edge;
          }
          return 0.5 * sum;
        };
        // Pure shear in the plane of orthonormal a and b, S = (a b^T + b a^T) / 2, |S|^2 = 1/2:
        // an isotropic material stores mu gamma^2 V / 2, and a fabric its C_abab in place of mu.
        const auto plane = [&](D3 a, D3 b) {
          const f64 av[3] = {a.x, a.y, a.z};
          const f64 bv[3] = {b.x, b.y, b.z};
          M3 s{};
          for (u32 i = 0; i < 3; ++i)
            for (u32 j = 0; j < 3; ++j)
              s.m[i][j] = 0.5 * (av[i] * bv[j] + bv[i] * av[j]);
          return strained(s) / (0.5 * k_gamma * k_gamma * volume);
        };
        // The region's frame: the top sheet's mean normal at the rest, and two directions in its
        // plane, the first as close to x as the plane allows.
        const SheetModel& top = region.sheets[region.top];
        D3 normal;
        for (u32 f = 0; f + 2 < top.faces.size(); f += 3) {
          const D3 a = x0[top.region_node[top.faces[f]]];
          const D3 b = x0[top.region_node[top.faces[f + 1]]];
          const D3 c = x0[top.region_node[top.faces[f + 2]]];
          normal = normal + query::cross(b - a, c - a);
        }
        normal = query::unit(normal);
        D3 t1 = D3{1.0, 0.0, 0.0} - normal * normal.x;
        if (query::length(t1) < 0.1) t1 = D3{0.0, 1.0, 0.0} - normal * normal.y;
        t1 = query::unit(t1);
        const D3 t2 = query::cross(normal, t1);
        struct Plane {
          const char* name;
          D3 a;
          D3 b;
        };
        const Plane planes[] = {{"xy", D3{1, 0, 0}, D3{0, 1, 0}}, {"xz", D3{1, 0, 0}, D3{0, 0, 1}},
                                {"yz", D3{0, 1, 0}, D3{0, 0, 1}}, {"in_plane", t1, t2},
                                {"transverse_1", t1, normal},     {"transverse_2", t2, normal}};
        JsonValue per_plane = JsonValue::object();
        f64 lowest = std::numeric_limits<f64>::infinity();
        f64 highest = 0.0;
        for (const Plane& p : planes) {
          const f64 shear = plane(p.a, p.b);
          JsonValue e = JsonValue::object();
          e.set("shear_pa", number(shear));
          e.set("ratio", number(shear / mu));
          lowest = std::min(lowest, shear / mu);
          highest = std::max(highest, shear / mu);
          per_plane.set(p.name, std::move(e));
        }
        // The two other independent shears of each frame, the normal-stress differences
        // (a a^T - b b^T) / sqrt 2 and (a a^T + b b^T - 2 c c^T) / sqrt 6, unit norm, which an
        // isotropic material stores as mu gamma^2 V: a cubic fabric (a lattice, a slab of Kuhn
        // cells) is soft in the three plane shears and stiff in these, so neither set alone shows
        // the anisotropy.
        struct Difference {
          const char* name;
          D3 a;
          D3 b;
          D3 c;
          bool second;
        };
        const Difference differences[] = {
            {"xx-yy", D3{1, 0, 0}, D3{0, 1, 0}, D3{0, 0, 1}, false},
            {"xx+yy-2zz", D3{1, 0, 0}, D3{0, 1, 0}, D3{0, 0, 1}, true},
            {"in_plane_difference", t1, t2, normal, false},
            {"in_plane_vs_normal", t1, t2, normal, true}};
        for (const Difference& d : differences) {
          const f64 av[3] = {d.a.x, d.a.y, d.a.z};
          const f64 bv[3] = {d.b.x, d.b.y, d.b.z};
          const f64 cv[3] = {d.c.x, d.c.y, d.c.z};
          M3 s{};
          for (u32 i = 0; i < 3; ++i)
            for (u32 j = 0; j < 3; ++j)
              s.m[i][j] =
                  d.second ? (av[i] * av[j] + bv[i] * bv[j] - 2.0 * cv[i] * cv[j]) / std::sqrt(6.0)
                           : (av[i] * av[j] - bv[i] * bv[j]) / std::sqrt(2.0);
          const f64 shear = strained(s) / (k_gamma * k_gamma * volume);
          JsonValue e = JsonValue::object();
          e.set("shear_pa", number(shear));
          e.set("ratio", number(shear / mu));
          lowest = std::min(lowest, shear / mu);
          highest = std::max(highest, shear / mu);
          per_plane.set(d.name, std::move(e));
        }
        // The isotropic shear: the mean over an orthonormal basis of traceless strains, each of
        // which an isotropic material stores as mu gamma^2 V.
        const f64 r2 = 1.0 / std::sqrt(2.0);
        const f64 r6 = 1.0 / std::sqrt(6.0);
        const M3 basis[5] = {
            M3{{{0, r2, 0}, {r2, 0, 0}, {0, 0, 0}}}, M3{{{0, 0, r2}, {0, 0, 0}, {r2, 0, 0}}},
            M3{{{0, 0, 0}, {0, 0, r2}, {0, r2, 0}}}, M3{{{r2, 0, 0}, {0, -r2, 0}, {0, 0, 0}}},
            M3{{{r6, 0, 0}, {0, r6, 0}, {0, 0, -2.0 * r6}}}};
        f64 isotropic = 0.0;
        for (const M3& b : basis)
          isotropic += strained(b) / (k_gamma * k_gamma * volume) / 5.0;
        // A dilation stores (K / 2) (3 gamma)^2 V.
        const f64 bulk = 2.0 * strained(M3{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}}) /
                         (9.0 * k_gamma * k_gamma * volume);
        r.value.set("configuration", JsonValue(rest->name));
        r.value.set("declared_shear_pa", number(mu));
        r.value.set("declared_bulk_pa", number(kappa));
        r.value.set("isotropic_shear_pa", number(isotropic));
        r.value.set("isotropic_ratio", number(isotropic / mu));
        r.value.set("bulk_pa", number(bulk));
        r.value.set("bulk_ratio", number(bulk / kappa));
        r.value.set("shears", std::move(per_plane));
        r.value.set("anisotropy_min", number(lowest));
        r.value.set("anisotropy_max", number(highest));
        JsonValue frame = JsonValue::array();
        for (const f64 c : {normal.x, normal.y, normal.z})
          frame.push_back(number(c));
        r.value.set("region_normal", std::move(frame));
        JsonValue& numbers = region_numbers(region);
        numbers.set("affine_isotropic_ratio", number(isotropic / mu));
        numbers.set("affine_anisotropy_min", number(lowest));
        numbers.set("affine_anisotropy_max", number(highest));
      }
    }

    // ---- the reference's volumetric strain against the rest
    u32 strain_index = ~0u;
    {
      ValidationRow& r =
          row("region.volumetric_strain", subject, Severity::info,
              "a plausibility diagnostic, read with the load, the prior and the equilibrium gap; "
              "never a verdict: the reference's volumetric strain against the rest, |V / V0 - 1|, "
              "per cell, node-averaged, and the deviation between them, p50, p99 and max, beside "
              "the declared load, the declared interior prior and the reference's equilibrium gap",
              "stress at a loaded reference is physical (gravity and the support demand it), so a "
              "strain alone does not say whether the load or the fit's interior prior put it "
              "there; a fit can place an interior pre-stress an order of magnitude above its "
              "load while the surface fits to a fraction of a millimetre");
      strain_index = report_.rows.size() - 1u;
      const StateModel& reference = region.states[region.reference];
      if (rest == nullptr || reference.def == nullptr || reference.role != StateRole::Reference) {
        r.verdict = Verdict::skipped;
        r.note = rest == nullptr ? "the region declares no rest state"
                                 : "the region declares no reference state";
      } else {
        Vector<f64> magnitude;
        Vector<f64> pressure;
        f64 lowest = std::numeric_limits<f64>::infinity();
        f64 highest = -std::numeric_limits<f64>::infinity();
        u32 worst = 0;
        f64 worst_value = -1.0;
        Vector<std::pair<f64, u32>> order;
        for (u32 t = 0; t < cells; ++t) {
          const u32* v = region.tets.data() + 4 * t;
          const f64 v0 = query::tet_volume(d3(rest->nodes[v[0]]), d3(rest->nodes[v[1]]),
                                           d3(rest->nodes[v[2]]), d3(rest->nodes[v[3]]));
          const f64 v1 = query::tet_volume(d3(reference.nodes[v[0]]), d3(reference.nodes[v[1]]),
                                           d3(reference.nodes[v[2]]), d3(reference.nodes[v[3]]));
          const f64 j = v1 / v0 - 1.0;
          magnitude.push_back(std::fabs(j));
          pressure.push_back(std::fabs(j) * materials[t].bulk_modulus_pa);
          lowest = std::min(lowest, j);
          highest = std::max(highest, j);
          order.push_back({-std::fabs(j), t});
          if (std::fabs(j) > worst_value) {
            worst_value = std::fabs(j);
            worst = t;
          }
        }
        // Node-averaged: each node's rest-volume-weighted mean of its cells' strains, and each
        // cell's deviation from the mean of its corners' averages. A smooth field reads the same
        // both ways; a checkerboard of alternating cells averages away at the nodes and shows in
        // the deviation.
        Vector<f64> node_sum(static_cast<u32>(region.nodes.size()), 0.0);
        Vector<f64> node_weight(static_cast<u32>(region.nodes.size()), 0.0);
        Vector<f64> signed_strain;
        for (u32 t = 0; t < cells; ++t) {
          const u32* v = region.tets.data() + 4 * t;
          const f64 v0 = query::tet_volume(d3(rest->nodes[v[0]]), d3(rest->nodes[v[1]]),
                                           d3(rest->nodes[v[2]]), d3(rest->nodes[v[3]]));
          const f64 v1 = query::tet_volume(d3(reference.nodes[v[0]]), d3(reference.nodes[v[1]]),
                                           d3(reference.nodes[v[2]]), d3(reference.nodes[v[3]]));
          signed_strain.push_back(v1 / v0 - 1.0);
          for (u32 k = 0; k < 4; ++k) {
            node_sum[v[k]] += std::fabs(v0) * (v1 / v0 - 1.0);
            node_weight[v[k]] += std::fabs(v0);
          }
        }
        Vector<f64> node_magnitude;
        Vector<f64> node_average(static_cast<u32>(region.nodes.size()), 0.0);
        for (u32 i = 0; i < region.nodes.size(); ++i) {
          if (!(node_weight[i] > 0.0)) continue;
          node_average[i] = node_sum[i] / node_weight[i];
          node_magnitude.push_back(std::fabs(node_average[i]));
        }
        Vector<f64> deviation;
        u32 worst_deviation_cell = 0;
        f64 worst_deviation = -1.0;
        for (u32 t = 0; t < cells; ++t) {
          f64 corners = 0.0;
          for (u32 k = 0; k < 4; ++k)
            corners += node_average[region.tets[4 * t + k]] / 4.0;
          const f64 d = std::fabs(signed_strain[t] - corners);
          deviation.push_back(d);
          if (d > worst_deviation) {
            worst_deviation = d;
            worst_deviation_cell = t;
          }
        }
        r.value.set("node_averaged_percent", stats(node_magnitude, 100.0));
        r.value.set("cell_minus_node_average_percent", stats(deviation, 100.0));
        r.value.set("worst_deviation_cell", JsonValue(worst_deviation_cell));
        std::sort(order.begin(), order.end());
        u32 held_among_worst = 0;
        const u32 worst_count = std::min<u32>(10, cells);
        for (u32 w = 0; w < worst_count; ++w) {
          bool touches = false;
          for (u32 k = 0; k < 4; ++k)
            touches = touches || kind[region.tets[4 * order[w].second + k]] == k_held;
          if (touches) ++held_among_worst;
        }
        const f64 p99 = percentile(magnitude, 0.99);
        r.value.set("reference", JsonValue(reference.name));
        r.value.set("rest", JsonValue(rest->name));
        r.value.set("strain_percent", stats(magnitude, 100.0));
        r.value.set("p99_percent", number(p99 * 100.0));
        r.value.set("signed_min_percent", number(lowest * 100.0));
        r.value.set("signed_max_percent", number(highest * 100.0));
        r.value.set("pressure_p99_kpa", number(percentile(pressure, 0.99) * 1.0e-3));
        r.value.set("pressure_max_kpa",
                    number(*std::max_element(pressure.begin(), pressure.end()) * 1.0e-3));
        r.value.set("worst_cell", JsonValue(worst));
        r.value.set("worst_ten_touching_held_nodes", JsonValue(held_among_worst));
        // The two companions it is read with: the load the reference is under, and the prior
        // the fit put in the interior.
        const Load& load = reference.def->load;
        JsonValue declared_load = JsonValue::object();
        JsonValue gravity = JsonValue::array();
        for (const f32 c : {load.gravity.x, load.gravity.y, load.gravity.z})
          gravity.push_back(number(static_cast<f64>(c)));
        declared_load.set("gravity_m_s2", std::move(gravity));
        declared_load.set("medium", JsonValue(load.medium));
        declared_load.set("pose", JsonValue(load.pose));
        r.value.set("load", std::move(declared_load));
        const std::optional<InverseFit>& fit = reference.def->inverse;
        JsonValue prior = JsonValue::object();
        prior.set("provenance",
                  JsonValue(reference.def->provenance == StateProvenance::InverseStatics
                                ? "inverse statics"
                                : "not inverse statics"));
        prior.set("method", fit.has_value() ? JsonValue(fit->method) : JsonValue::null());
        prior.set("interior_extension",
                  fit.has_value() ? JsonValue(fit->interior_extension) : JsonValue::null());
        r.value.set("prior", std::move(prior));
        char buffer[160];
        std::snprintf(buffer, sizeof(buffer), "p99 %.3g%%, cell %u at %.3g%%", p99 * 100.0, worst,
                      worst_value * 100.0);
        r.witness = buffer;
        region_numbers(region).set("volumetric_strain_p99_percent", number(p99 * 100.0));
      }
    }

    // ---- the equilibrium gap
    ValidationRow& r =
        row("state.equilibrium_gap", subject, Severity::info,
            "a diagnostic, never a failure: the norm of the declared energy's exact gradient at "
            "the free nodes of the rest, the reference and each response, max and rms, newtons",
            "a rest recovered against an XPBD relaxation reproduces its observation at the "
            "relaxation's fixed point, not at the energy's stationary point; this is the distance "
            "between the two, and the static-equilibrium residual of every stored state");
    r.value.set("law", JsonValue(energy::k_law));
    if (!model_ok) {
      r.verdict = Verdict::skipped;
      r.note = model_error;
      continue;
    }
    r.value.set("rest", JsonValue(rest->name));
    r.value.set("cells", JsonValue(model.cell_count()));
    r.value.set("edges", JsonValue(model.edge_count()));
    // The solver whose fixed point the reference is: the gap is the distance from it to the
    // energy's stationary point, so it is read with the solver named.
    {
      const StateModel& reference = region.states[region.reference];
      JsonValue solver = JsonValue::null();
      if (reference.def != nullptr && reference.def->inverse.has_value() &&
          reference.def->inverse->solver.has_value()) {
        const InverseSolver& s = *reference.def->inverse->solver;
        solver = JsonValue::object();
        solver.set("kind", JsonValue(s.kind));
        solver.set("iterations", JsonValue(s.iterations));
        solver.set("step_s", number(s.step_s));
        solver.set("implementation", JsonValue(s.implementation));
      }
      r.value.set("solver", std::move(solver));
    }
    JsonValue included = JsonValue::array();
    included.push_back(JsonValue("tetrahedral bulk"));
    included.push_back(JsonValue("tetrahedral edges"));
    included.push_back(JsonValue("gravity less the medium's buoyancy"));
    r.value.set("included", std::move(included));
    // What is not included, and the nodes it touches: at every other node the included terms are
    // all the terms, and the gap there is complete.
    JsonValue not_included = JsonValue::array();
    Vector<u8> partial(n, 0);
    for (const Membrane& m : def.membranes) {
      Vector<u32> tris;
      blocks_.read(m.triangles, BlockKind::MembraneTriangles, subject, tris, 3);
      for (const u32 v : tris)
        if (v < n) partial[v] = 1;
      not_included.push_back(
          JsonValue("membrane '" + m.name + "', " + str(tris.size() / 3) + " triangles"));
    }
    for (const CableSet& c : def.cables) {
      Vector<u32> edges;
      blocks_.read(c.edges, BlockKind::CableEdges, subject, edges, 2);
      for (const u32 v : edges)
        if (v < n) partial[v] = 1;
      not_included.push_back(
          JsonValue("cable set '" + c.name + "', " + str(edges.size() / 2) + " cables"));
    }
    const bool complete = not_included.size() == 0;
    r.value.set("complete", JsonValue(complete));
    r.value.set("not_included", std::move(not_included));

    // Weight per node, lumped from the construction's cells (the configuration the densities are
    // declared at) and less the medium's density, a quarter to each corner.
    Vector<f64> lumped_volume_density(n, 0.0);  // sum of rho V / 4
    Vector<f64> lumped_volume(n, 0.0);          // sum of V / 4
    for (u32 t = 0; t < cells; ++t) {
      const u32* v = region.tets.data() + 4 * t;
      const f64 volume = query::tet_volume(d3(region.nodes[v[0]]), d3(region.nodes[v[1]]),
                                           d3(region.nodes[v[2]]), d3(region.nodes[v[3]]));
      for (u32 k = 0; k < 4; ++k) {
        lumped_volume_density[v[k]] += density[t] * volume / 4.0;
        lumped_volume[v[k]] += volume / 4.0;
      }
    }

    JsonValue per_state = JsonValue::object();
    for (const StateModel& s : region.states) {
      if (s.def == nullptr) continue;  // the construction carries no load
      const Load& load = s.def->load;
      const D3 gravity = d3(load.gravity);
      const Vector<D3> x = to_d3(s.nodes);
      Vector<D3> bulk(n);
      Vector<D3> edge(n);
      model.add_gradient(x, std::span<D3>(bulk.data(), bulk.size()),
                         std::span<D3>(edge.data(), edge.size()));
      const energy::BulkEdgeEnergy::Terms energies = model.energy(x);
      struct Tally {
        u32 count = 0;
        f64 max = 0.0;
        f64 sum_squares = 0.0;
        u32 witness = query::k_none;
        u8 witness_class = 0;
        void add(f64 value, u32 node, u8 node_class) {
          ++count;
          sum_squares += value * value;
          if (value > max || witness == query::k_none) {
            max = value;
            witness = node;
            witness_class = node_class;
          }
        }
      };
      Tally all;
      Tally by_class[3];
      Tally untouched;  // nodes no excluded term reaches: the gap there is complete
      u32 held = 0;
      u32 no_geometry = 0;
      f64 bulk_max = 0.0;
      f64 edge_max = 0.0;
      f64 weight_max = 0.0;
      f64 reaction_max = 0.0;
      f64 total_weight = 0.0;
      for (u32 i = 0; i < n; ++i) {
        const f64 mass = lumped_volume_density[i] - load.medium_density_kg_m3 * lumped_volume[i];
        total_weight += mass * query::length(gravity);
        if (kind[i] == k_held) {
          ++held;
          continue;
        }
        if (kind[i] == k_no_geometry) {
          ++no_geometry;
          continue;
        }
        const D3 weight = gravity * mass;
        D3 g = bulk[i] + edge[i] - weight;
        u8 c = k_free;
        if (kind[i] == k_sliding) {
          const query::SignedSurface::Result q = target[i]->query(x[i]);
          if (unilateral[i] == 0 || std::fabs(q.distance) <= k_contact_m) {
            const f64 reaction = query::dot(g, q.normal);
            g = g - q.normal * reaction;
            reaction_max = std::max(reaction_max, std::fabs(reaction));
            c = unilateral[i] == 0 ? k_sliding : k_contact;
          }
        }
        const f64 norm = query::length(g);
        all.add(norm, i, c);
        by_class[c].add(norm, i, c);
        if (partial[i] == 0) untouched.add(norm, i, c);
        bulk_max = std::max(bulk_max, query::length(bulk[i]));
        edge_max = std::max(edge_max, query::length(edge[i]));
        weight_max = std::max(weight_max, query::length(weight));
      }
      const auto rms = [](const Tally& t) {
        return t.count == 0 ? 0.0 : std::sqrt(t.sum_squares / static_cast<f64>(t.count));
      };
      const char* role = s.role == StateRole::Reference  ? "reference"
                         : s.role == StateRole::Rest     ? "rest"
                         : s.role == StateRole::Response ? "response"
                                                         : "construction";
      JsonValue e = JsonValue::object();
      e.set("role", JsonValue(role));
      e.set("max_n", number(all.max));
      e.set("rms_n", number(rms(all)));
      e.set("witness_node",
            all.witness == query::k_none ? JsonValue::null() : JsonValue(all.witness));
      e.set("witness_class", all.witness == query::k_none
                                 ? JsonValue::null()
                                 : JsonValue(class_names[all.witness_class]));
      JsonValue classes = JsonValue::object();
      for (u32 c = 0; c < 3; ++c) {
        JsonValue ce = JsonValue::object();
        ce.set("nodes", JsonValue(by_class[c].count));
        ce.set("max_n", number(by_class[c].max));
        ce.set("rms_n", number(rms(by_class[c])));
        classes.set(class_names[c], std::move(ce));
      }
      e.set("classes", std::move(classes));
      JsonValue where_complete = JsonValue::object();
      where_complete.set("nodes", JsonValue(untouched.count));
      where_complete.set("max_n", number(untouched.max));
      where_complete.set("rms_n", number(rms(untouched)));
      where_complete.set("witness_node", untouched.witness == query::k_none
                                             ? JsonValue::null()
                                             : JsonValue(untouched.witness));
      e.set("where_complete", std::move(where_complete));
      e.set("held_nodes", JsonValue(held));
      e.set("nodes_without_target_geometry", JsonValue(no_geometry));
      JsonValue terms = JsonValue::object();
      terms.set("bulk_max_n", number(bulk_max));
      terms.set("edge_max_n", number(edge_max));
      terms.set("weight_max_n", number(weight_max));
      terms.set("sliding_reaction_max_n", number(reaction_max));
      e.set("terms", std::move(terms));
      e.set("weight_n", number(total_weight));
      e.set("energy_bulk_j", number(energies.bulk));
      e.set("energy_edge_j", number(energies.edge));
      JsonValue summary = JsonValue::object();
      summary.set("max_n", number(all.max));
      summary.set("rms_n", number(rms(all)));
      summary.set("complete", JsonValue(complete));
      summary.set("where_complete_max_n", number(untouched.max));
      state_numbers(region, s).set("equilibrium_gap", std::move(summary));
      if (s.role == StateRole::Reference && strain_index != ~0u &&
          report_.rows[strain_index].verdict != Verdict::skipped) {
        // The strain diagnostic's third companion.
        JsonValue gap = JsonValue::object();
        gap.set("max_n", number(all.max));
        gap.set("rms_n", number(rms(all)));
        gap.set("where_complete_max_n", number(untouched.max));
        gap.set("complete", JsonValue(complete));
        report_.rows[strain_index].value.set("equilibrium_gap", std::move(gap));
      }
      if (s.role == StateRole::Reference && all.witness != query::k_none) {
        char buffer[160];
        std::snprintf(buffer, sizeof(buffer), "state %s: node %u, %.4g N (rms %.4g N)",
                      s.name.c_str(), all.witness, all.max, rms(all));
        r.witness = buffer;
      }
      per_state.set(s.name, std::move(e));
    }
    r.value.set("states", std::move(per_state));
  }
}

// ---- frames
// -----------------------------------------------------------------------------------------

void Validator::check_frames() {
  for (FrameModel& frame : frames_) {
    const std::string subject = "frame " + frame.def->name;
    if (!frame.ok) continue;
    JsonValue& numbers = report_.numbers["frames"];
    if (!numbers.is_object()) numbers = JsonValue::object();
    JsonValue& mine = numbers[frame.def->name];
    mine = JsonValue::object();
    {
      ValidationRow& r =
          row("frame.closed_manifold", subject, Severity::error,
              "every edge used by exactly two triangles in opposite directions, no "
              "triangle without area, positive enclosed volume",
              "containment by winding number needs a closed, consistently wound frame");
      const ManifoldCheck check = check_manifold(frame.triangles, frame.points);
      const f64 volume = query::enclosed_volume(frame.points, frame.triangles);
      r.value.set("vertices", JsonValue(static_cast<u64>(frame.vertices.size())));
      r.value.set("triangles", JsonValue(static_cast<u64>(frame.triangles.size() / 3)));
      r.value.set("volume_ml", number(volume * k_ml));
      if (!check.closed_manifold) fail(r, check.witness);
      if (!(volume > 0.0)) fail(r, "the frame encloses a negative volume: it is wound inward");
    }
    {
      ValidationRow& r =
          row("frame.cover", subject, Severity::info,
              "reported: the declared cover per vertex with its provenance, beside the "
              "measured nearest skin distance over all frame vertices",
              "a frame contracted to fit is a size changed, not discovered");
      const char* names[] = {"undeclared", "fitted", "authored_transform", "authored"};
      u32 counts[4] = {0, 0, 0, 0};
      for (u32 i = 0; i < frame.vertices.size(); ++i) {
        const u8 p = i < frame.provenance.size() ? frame.provenance[i] : u8{0};
        counts[p < 4 ? p : 0]++;
      }
      JsonValue provenance = JsonValue::object();
      for (u32 k = 0; k < 4; ++k)
        provenance.set(names[k], JsonValue(counts[k]));
      r.value.set("provenance", std::move(provenance));
      r.value.set("provenance_note", JsonValue(frame.def->provenance));
      if (observation_.ok) {
        Vector<f64> gap;
        f64 worst = std::numeric_limits<f64>::infinity();
        u32 worst_vertex = 0;
        Vector<f64> declared_difference;
        for (u32 i = 0; i < frame.vertices.size(); ++i) {
          const query::SignedSurface::Result q = skin_.query(frame.points[i]);
          const f64 cover = -q.distance;  // inside the skin is positive cover
          gap.push_back(cover);
          if (cover < worst) {
            worst = cover;
            worst_vertex = i;
          }
          if (i < frame.cover.size() && std::isfinite(frame.cover[i]))
            declared_difference.push_back(std::fabs(cover - static_cast<f64>(frame.cover[i])));
        }
        r.value.set("measured_cover_mm", stats(gap, k_mm));
        r.value.set("declared_minus_measured_mm", stats(declared_difference, k_mm));
        r.witness = "vertex " + str(worst_vertex) + " at " + xyz(frame.points[worst_vertex]);
        mine.set("measured_cover_mm", stats(gap, k_mm));
      }
    }
    for (const std::string& outer : frame.def->inside) {
      ValidationRow& r =
          row("frame.layering", subject + " inside " + outer, Severity::error,
              "every vertex of the frame inside the frame it declares it lies in, by "
              "winding number",
              "frames declare their layering, and the declaration is checked");
      const FrameModel* other = nullptr;
      for (const FrameModel& f : frames_)
        if (f.def->name == outer && f.ok) other = &f;
      if (other == nullptr) {
        fail(r, "no frame '" + outer + "'");
        continue;
      }
      u32 outside = 0;
      for (u32 i = 0; i < frame.points.size(); ++i)
        if (query::winding_number(frame.points[i], other->points, other->triangles) < 0.5) {
          if (outside == 0) r.witness = "vertex " + str(i);
          ++outside;
        }
      r.value.set("outside", JsonValue(outside));
      if (outside > 0) r.verdict = Verdict::fail;
    }
    // Containment and intersection against every region, in every state.
    for (u32 ri = 0; ri < regions_.size(); ++ri) {
      RegionModel& region = regions_[ri];
      if (!region.ok) continue;
      row("frame.containment", subject + ", region " + region.def->name, Severity::error,
          "no region node inside the frame by generalized winding number "
          "(> 0.5), all nodes, every state",
          "a region node inside its frame is tissue inside bone; the "
          "winding number does not care what a ray would have hit");
      row("frame.intersections", subject + ", region " + region.def->name, Severity::error,
          "no triangle of the region's piecewise-linear boundary or of its "
          "closed smooth shell intersects the frame, every state",
          "surfaces can cross between vertices that all pass");
      const u32 contain_index = report_.rows.size() - 2u;
      const u32 cross_index = report_.rows.size() - 1u;
      JsonValue contain_states = JsonValue::object();
      JsonValue cross_states = JsonValue::object();
      query::TriangleBvh frame_bvh;
      frame_bvh.build(frame.vertices, frame.triangles);
      for (const StateModel& s : region.states) {
        u32 inside = 0;
        f64 nearest = std::numeric_limits<f64>::infinity();
        u32 nearest_node = 0;
        for (u32 n = 0; n < s.nodes.size(); ++n) {
          const D3 p = d3(s.nodes[n]);
          const f64 w = query::winding_number(p, frame.points, frame.triangles);
          if (w > 0.5) {
            if (inside == 0)
              fail(report_.rows[contain_index], "state " + s.name + ", node " + str(n));
            ++inside;
          }
          const query::SignedSurface::Result q = frame.surface.query(p);
          if (std::fabs(q.distance) < nearest) {
            nearest = std::fabs(q.distance);
            nearest_node = n;
          }
        }
        JsonValue e = JsonValue::object();
        e.set("nodes_inside", JsonValue(inside));
        e.set("min_node_distance_mm", number(nearest * k_mm));
        e.set("nearest_node", JsonValue(nearest_node));
        contain_states.set(s.name, std::move(e));
        JsonValue& sn = state_numbers(region, s);
        sn.set("nodes_inside_frame", JsonValue(inside));
        sn.set("frame_node_min_distance_mm", number(nearest * k_mm));
        // Intersections: the piecewise-linear boundary and the smooth shell.
        query::TriangleBvh pl;
        pl.build(s.nodes, region.boundary);
        Vector<std::pair<u32, u32>> witnesses;
        const u64 pl_count = pl.intersections(frame_bvh, 1, &witnesses);
        Vector<D3> shell_points;
        Vector<u32> shell_faces;
        for (const SheetModel& sheet : region.sheets) {
          const u32 offset = shell_points.size();
          for (const Vec3& p : evaluate(sheet.surface.limit, region.sheet_nodes(sheet, s.nodes)))
            shell_points.push_back(d3(p));
          for (const u32 v : sheet.surface.faces)
            shell_faces.push_back(v + offset);
        }
        for (const u32 v : region.stitch)
          if (v < shell_points.size()) shell_faces.push_back(v);
        query::TriangleBvh smooth;
        smooth.build(shell_points, shell_faces);
        const u64 smooth_count = smooth.intersections(frame_bvh, 1, &witnesses);
        JsonValue c = JsonValue::object();
        c.set("piecewise_linear", JsonValue(pl_count));
        c.set("smooth_shell", JsonValue(smooth_count));
        cross_states.set(s.name, std::move(c));
        if (pl_count + smooth_count > 0)
          fail(report_.rows[cross_index], "state " + s.name + ", triangles (" +
                                              str(witnesses.front().first) + ", " +
                                              str(witnesses.front().second) + ")");
      }
      report_.rows[contain_index].value.set("states", std::move(contain_states));
      report_.rows[cross_index].value.set("states", std::move(cross_states));
    }
  }
}

// ---- the skin, the depth budget and the cover
// -----------------------------------------------------

void Validator::check_skin_and_depth() {
  if (!observation_.ok) return;
  for (u32 ri = 0; ri < regions_.size(); ++ri) {
    RegionModel& region = regions_[ri];
    if (!region.ok) continue;
    const std::string subject = "region " + region.def->name;
    const SheetModel& top = region.sheets[region.top];

    // Containment inside the skin, and clearance to it, on both representations, every state: an
    // exact triangle-pair intersection test of the simulated surface (the tetrahedra's boundary)
    // and of the smooth shell against the skin as transferred to that state, and the clearance
    // distribution against the declared floor on the simulated surface as well as the dense cap —
    // never on the cap alone, because the cap is what the skin follows and the tetrahedra are what
    // is simulated, and the two clear the skin by different amounts.
    f64 floor = 0.0;
    for (const DepthBudget& b : def_.budgets)
      if (b.region == region.def->name) floor = b.skin_clearance_floor_m;
    row("region.skin_containment", subject, Severity::error,
        "every region node, every node of the tetrahedra's boundary, every dense point of the top "
        "sheet's limit surface at least the declared floor (" +
            mm(floor) +
            ") inside the skin (signed distance by pseudonormal), the skin's own vertices outside "
            "the tetrahedra's boundary by the same floor, and no triangle of either "
            "representation crossing it, every state",
        "the region lies inside the base's skin; an open crop's sign is "
        "local, so the intersection count stands beside it");
    const u32 contain_index = report_.rows.size() - 1u;
    report_.rows[contain_index].value.set("floor_mm", number(floor * k_mm));
    Vector<u8> on_boundary(region.nodes.size(), 0);
    for (const u32 v : region.boundary)
      on_boundary[v] = 1;
    JsonValue states = JsonValue::object();
    for (const StateModel& s : region.states) {
      // The skin at a state is the observation moved by the region's bindings, except at the
      // construction and the reference, where the observation is the skin by definition.
      const bool observed = &s == &region.states[0] || &s == &region.states[region.reference];
      const Vector<Vec3> skin_positions =
          observed ? observation_.positions : skin_at(static_cast<i32>(ri), s);
      query::SignedSurface skin;
      skin.build(skin_positions, observation_.triangles);
      // Nodes: the piecewise-linear representation, the simulated surface's corners.
      Vector<f64> node_clearance;
      Vector<f64> boundary_clearance;
      u32 nodes_outside = 0;
      u32 worst_node = 0;
      f64 worst_node_clearance = std::numeric_limits<f64>::infinity();
      for (u32 n = 0; n < s.nodes.size(); ++n) {
        const f64 c = -skin.query(d3(s.nodes[n])).distance;
        node_clearance.push_back(c);
        if (on_boundary[n] != 0) boundary_clearance.push_back(c);
        if (c < worst_node_clearance) {
          worst_node_clearance = c;
          worst_node = n;
        }
        if (c < floor) {
          if (nodes_outside == 0)
            fail(report_.rows[contain_index],
                 "state " + s.name + ", node " + str(n) + " at " + mm(c));
          ++nodes_outside;
        }
      }
      // The other direction of the vertex-to-face distance between the two surfaces: every skin
      // vertex of the declared domain against the tetrahedra's boundary, positive outside it.
      query::SignedSurface simulated;
      simulated.build(s.nodes, region.boundary);
      Vector<f64> skin_clearance;
      f64 worst_skin = std::numeric_limits<f64>::infinity();
      u32 worst_skin_id = 0;
      for (const u32 id : observation_.domain) {
        if (id >= skin_positions.size()) continue;
        const f64 c = simulated.query(d3(skin_positions[id])).distance;
        skin_clearance.push_back(c);
        if (c < worst_skin) {
          worst_skin = c;
          worst_skin_id = id;
        }
      }
      if (worst_skin < floor)
        fail(report_.rows[contain_index],
             "state " + s.name + ", skin id " + str(worst_skin_id) + " at " + mm(worst_skin));
      // The dense cap: the top sheet's limit surface, which the skin follows.
      const Vector<Vec3> cap = evaluate(top.surface.limit, region.sheet_nodes(top, s.nodes));
      Vector<f64> cap_clearance;
      u32 cap_outside = 0;
      f64 worst_cap = std::numeric_limits<f64>::infinity();
      u32 worst_cap_vertex = 0;
      for (u32 v = 0; v < cap.size(); ++v) {
        const f64 c = -skin.query(d3(cap[v])).distance;
        cap_clearance.push_back(c);
        if (c < worst_cap) {
          worst_cap = c;
          worst_cap_vertex = v;
        }
        if (c < floor) {
          if (cap_outside == 0)
            fail(report_.rows[contain_index],
                 "state " + s.name + ", dense cap vertex " + str(v) + " at " + mm(c));
          ++cap_outside;
        }
      }
      // Intersections with the skin: the boundary and the shell.
      query::TriangleBvh pl;
      pl.build(s.nodes, region.boundary);
      Vector<D3> shell_points;
      Vector<u32> shell_faces;
      for (const SheetModel& sheet : region.sheets) {
        const u32 offset = shell_points.size();
        for (const Vec3& p : evaluate(sheet.surface.limit, region.sheet_nodes(sheet, s.nodes)))
          shell_points.push_back(d3(p));
        for (const u32 v : sheet.surface.faces)
          shell_faces.push_back(v + offset);
      }
      for (const u32 v : region.stitch)
        if (v < shell_points.size()) shell_faces.push_back(v);
      query::TriangleBvh smooth;
      smooth.build(shell_points, shell_faces);
      Vector<std::pair<u32, u32>> witnesses;
      const u64 pl_cross = pl.intersections(skin.bvh(), 1, &witnesses);
      const u64 smooth_cross = smooth.intersections(skin.bvh(), 1, &witnesses);
      if (pl_cross + smooth_cross > 0)
        fail(report_.rows[contain_index], "state " + s.name + ", triangles (" +
                                              str(witnesses.front().first) + ", " +
                                              str(witnesses.front().second) + ")");
      JsonValue e = JsonValue::object();
      e.set("node_clearance_mm", stats(node_clearance, k_mm));
      e.set("boundary_node_clearance_mm", stats(boundary_clearance, k_mm));
      e.set("worst_node", JsonValue(worst_node));
      e.set("skin_to_simulated_surface_mm", stats(skin_clearance, k_mm));
      e.set("worst_skin_id", JsonValue(worst_skin_id));
      e.set("dense_cap_clearance_mm", stats(cap_clearance, k_mm));
      e.set("worst_dense_vertex", JsonValue(worst_cap_vertex));
      e.set("piecewise_linear_skin_intersections", JsonValue(pl_cross));
      e.set("smooth_shell_skin_intersections", JsonValue(smooth_cross));
      states.set(s.name, std::move(e));
      JsonValue& sn = state_numbers(region, s);
      sn.set("skin_node_min_clearance_mm", number(worst_node_clearance * k_mm));
      sn.set("skin_dense_cap_min_clearance_mm", number(worst_cap * k_mm));
      sn.set("skin_to_simulated_surface_min_mm", number(worst_skin * k_mm));
    }
    report_.rows[contain_index].value.set("states", std::move(states));

    // The depth budget and the cover, at the construction (the geometry the authoring side fitted
    // against the observation).
    const DepthBudget* budget = nullptr;
    for (const DepthBudget& b : def_.budgets)
      if (b.region == region.def->name) budget = &b;
    if (budget == nullptr) continue;
    const FrameModel* frame = nullptr;
    for (const FrameModel& f : frames_)
      if (f.def->name == budget->frame && f.ok) frame = &f;
    const StateModel& construction = region.states[0];
    const Vector<Vec3> top_nodes = region.sheet_nodes(top, construction.nodes);
    const Vector<Vec3> cap = evaluate(top.surface.limit, top_nodes);
    Vector<Vec3> tu(top.surface.tangent_u.rows());
    Vector<Vec3> tv(top.surface.tangent_v.rows());
    geometry::apply(top.surface.tangent_u, top_nodes, std::span<Vec3>(tu.data(), tu.size()));
    geometry::apply(top.surface.tangent_v, top_nodes, std::span<Vec3>(tv.data(), tv.size()));
    Vector<Vec3> normals(cap.size());
    geometry::limit_reference_normals(tu, tv, std::span<Vec3>(normals.data(), normals.size()));
    const Vector<D3> cap_d = to_d3(cap);
    const Vector<f64> area = vertex_areas(cap_d, top.surface.faces);
    query::TriangleBvh support;
    if (region.support >= 0) {
      const SheetModel& sheet = region.sheets[region.support];
      support.build(evaluate(sheet.surface.limit, region.sheet_nodes(sheet, construction.nodes)),
                    sheet.surface.faces);
    }
    query::TriangleBvh frame_bvh;
    if (frame != nullptr) frame_bvh.build(frame->vertices, frame->triangles);

    Vector<f64> cover_along;
    Vector<f64> tissue_along;
    Vector<f64> clearance_along;
    Vector<f64> cover_nearest;
    Vector<f64> ratio;
    Vector<f64> residual;
    u32 order_violations = 0;
    u32 support_misses = 0;
    u32 frame_misses = 0;
    u32 skin_misses = 0;
    std::string order_witness;
    f64 area_total = 0.0;
    f64 area_in_range = 0.0;
    f64 area_above = 0.0;
    f64 area_below = 0.0;
    f64 area_above_5mm = 0.0;
    f64 cover_volume = 0.0;
    f64 excess = 0.0;
    f64 deficit = 0.0;
    f64 excess_over_max = 0.0;
    f64 worst_residual = 0.0;
    u32 worst_residual_vertex = 0;
    for (u32 v = 0; v < cap.size(); ++v) {
      const D3 p = cap_d[v];
      const D3 n = d3(normals[v]);
      if (query::length(n) == 0.0) continue;
      const f64 a = area[v];
      area_total += a;
      // Cover: the nearest distance to the skin, positive inside it, and the ray along the normal.
      const f64 c = -skin_.query(p).distance;
      cover_nearest.push_back(c);
      const query::TriangleBvh::Hit out_hit = skin_.bvh().first_hit(p, n, 0.0, 1.0);
      if (out_hit.triangle == query::k_none) {
        ++skin_misses;
      } else {
        cover_along.push_back(out_hit.t);
      }
      const f64 lo = budget->cover_min_m;
      const f64 hi = budget->cover_max_m;
      if (c >= lo && c <= hi) area_in_range += a;
      if (c > hi) area_above += a;
      if (c < lo) area_below += a;
      if (c > 0.005) area_above_5mm += a;
      cover_volume += c * a;
      excess += std::max(c - budget->cover_target_m, 0.0) * a;
      deficit += std::max(budget->cover_target_m - c, 0.0) * a;
      excess_over_max += std::max(c - hi, 0.0) * a;
      residual.push_back(c - budget->cover_target_m);
      if (std::fabs(c - budget->cover_target_m) > worst_residual) {
        worst_residual = std::fabs(c - budget->cover_target_m);
        worst_residual_vertex = v;
      }
      // Tissue and clearance along the inward normal: roof -> support -> frame.
      const D3 inward = n * -1.0;
      const query::TriangleBvh::Hit s_hit =
          support.empty() ? query::TriangleBvh::Hit{} : support.first_hit(p, inward, 1e-7, 1.0);
      const query::TriangleBvh::Hit f_hit =
          frame_bvh.empty() ? query::TriangleBvh::Hit{} : frame_bvh.first_hit(p, inward, 1e-7, 1.0);
      if (s_hit.triangle == query::k_none) ++support_misses;
      if (f_hit.triangle == query::k_none) ++frame_misses;
      if (s_hit.triangle != query::k_none) {
        tissue_along.push_back(s_hit.t);
        if (c > 0.0) ratio.push_back(c / s_hit.t);
      }
      if (s_hit.triangle != query::k_none && f_hit.triangle != query::k_none) {
        clearance_along.push_back(f_hit.t - s_hit.t);
        if (!(f_hit.t > s_hit.t)) {
          if (order_violations == 0)
            order_witness = "dense roof vertex " + str(v) + " at " + xyz(p) + ": the frame at " +
                            mm(f_hit.t) + ", the support at " + mm(s_hit.t);
          ++order_violations;
        }
      }
      if (c < 0.0) {
        if (order_violations == 0)
          order_witness = "dense roof vertex " + str(v) + " is outside the skin";
        ++order_violations;
      }
    }
    // Nearest-point clearance to the frame, on both representations: the dense support and whole
    // shell, and the piecewise-linear boundary (its vertices to the frame, and the frame's to it).
    JsonValue clearance = JsonValue::object();
    f64 clearance_min = std::numeric_limits<f64>::infinity();
    std::string clearance_witness;
    if (frame != nullptr) {
      const auto signed_to_frame = [&](D3 p) {
        const query::SignedSurface::Result q = frame->surface.query(p);
        return q.distance;  // positive outside the frame
      };
      Vector<f64> nodes_clear;
      for (u32 n = 0; n < construction.nodes.size(); ++n) {
        const f64 d = signed_to_frame(d3(construction.nodes[n]));
        nodes_clear.push_back(d);
        if (d < clearance_min) {
          clearance_min = d;
          clearance_witness = "node " + str(n);
        }
      }
      Vector<f64> dense_clear;
      for (const SheetModel& sheet : region.sheets)
        for (const Vec3& p :
             evaluate(sheet.surface.limit, region.sheet_nodes(sheet, construction.nodes)))
          dense_clear.push_back(signed_to_frame(d3(p)));
      query::SignedSurface boundary;
      boundary.build(construction.nodes, region.boundary);
      Vector<f64> frame_to_boundary;
      for (const D3& p : frame->points)
        frame_to_boundary.push_back(boundary.query(p).distance);  // positive: outside the region
      clearance.set("nodes_mm", stats(nodes_clear, k_mm));
      clearance.set("dense_shell_mm", stats(dense_clear, k_mm));
      clearance.set("frame_vertices_to_boundary_mm", stats(frame_to_boundary, k_mm));
      clearance.set("along_normal_mm", stats(clearance_along, k_mm));
    }
    JsonValue depth = JsonValue::object();
    depth.set("cover_nearest_mm", stats(cover_nearest, k_mm));
    depth.set("cover_along_normal_mm", stats(cover_along, k_mm));
    depth.set("tissue_along_normal_mm", stats(tissue_along, k_mm));
    depth.set("clearance", clearance);
    depth.set("dense_points", JsonValue(static_cast<u64>(cap.size())));
    depth.set("support_ray_misses", JsonValue(support_misses));
    depth.set("frame_ray_misses", JsonValue(frame_misses));
    depth.set("skin_ray_misses", JsonValue(skin_misses));
    depth.set("order_violations", JsonValue(order_violations));
    region_numbers(region).set("depth", depth);
    {
      ValidationRow& r = row("depth.layer_order", subject, Severity::error,
                             "along the roof's local outward normal at every dense point: the skin "
                             "outside, the support inside, the frame beyond the support; never a "
                             "world axis",
                             "the depth budget is a layer order, and a sloping region inverts any "
                             "axis-aligned order");
      r.value = depth;
      if (order_violations > 0) fail(r, order_witness);
    }
    {
      ValidationRow& r =
          row("depth.clearance", subject + ", frame " + budget->frame, Severity::warning,
              "support to frame at least " + mm(budget->clearance_min_m) +
                  " on both representations; above " + mm(budget->clearance_slot_m) +
                  " at the median it is a named slot or an undeclared depth",
              "a clearance wider than a few millimetres is a region to be, not a margin");
      r.value = clearance;
      if (frame == nullptr) {
        fail(r, "the budget names frame '" + budget->frame + "'");
      } else {
        const f64 median = percentile(clearance_along, 0.5);
        r.value.set("slot", JsonValue(budget->slot));
        if (clearance_min < budget->clearance_min_m)
          fail(r, clearance_witness + ": " + mm(clearance_min));
        if (median > budget->clearance_slot_m && budget->slot.empty())
          fail(r, "the median clearance along the normal is " + mm(median) +
                      " and no slot is declared: undeclared depth");
        if (median > budget->clearance_slot_m && !budget->slot.empty())
          r.note += "; the median " + mm(median) + " is the declared slot '" + budget->slot + "'";
      }
    }
    {
      ValidationRow& r =
          row("cover.range", subject, Severity::warning,
              "the roof's cover (nearest distance to the skin) inside [" + mm(budget->cover_min_m) +
                  ", " + mm(budget->cover_max_m) + "] on at least " +
                  std::to_string(static_cast<i32>(budget->cover_area_fraction_min * 100.0)) +
                  "% of the roof's area",
              "the cover is a thickness distribution and an area fraction, never a "
              "percentage of the shell");
      const f64 fraction = area_total > 0.0 ? area_in_range / area_total : 0.0;
      r.value.set("area_fraction_in_range", number(fraction));
      r.value.set("area_fraction_above", number(area_total > 0.0 ? area_above / area_total : 0.0));
      r.value.set("area_fraction_below", number(area_total > 0.0 ? area_below / area_total : 0.0));
      r.value.set("area_fraction_above_5mm",
                  number(area_total > 0.0 ? area_above_5mm / area_total : 0.0));
      r.value.set("cover_mm", stats(cover_nearest, k_mm));
      if (fraction < budget->cover_area_fraction_min)
        fail(r, "dense roof vertex " + str(worst_residual_vertex) + " at " +
                    xyz(cap_d[worst_residual_vertex]));
    }
    {
      ValidationRow& r =
          row("cover.report", subject, Severity::info,
              "reported: total cover = declared layer + excess - deficit, all on the "
              "roof's area; cover over local tissue thickness; the roof-fit residual",
              "one sampling measure throughout: the roof's own dense area");
      JsonValue cover = JsonValue::object();
      cover.set("roof_area_m2", number(area_total));
      cover.set("target_mm", number(budget->cover_target_m * k_mm));
      cover.set("purpose", JsonValue(budget->cover_purpose));
      cover.set("total_ml", number(cover_volume * k_ml));
      cover.set("declared_layer_ml", number(budget->cover_target_m * area_total * k_ml));
      cover.set("excess_over_target_ml", number(excess * k_ml));
      cover.set("deficit_below_target_ml", number(deficit * k_ml));
      cover.set("excess_over_max_ml", number(excess_over_max * k_ml));
      cover.set(
          "split_residual_ml",
          number((cover_volume - (budget->cover_target_m * area_total + excess - deficit)) * k_ml));
      cover.set("cover_over_tissue", stats(ratio, 1.0));
      cover.set("roof_fit_residual_mm", stats(residual, k_mm));
      cover.set("worst_residual_at", JsonValue(xyz(cap_d[worst_residual_vertex])));
      r.value = cover;
      r.witness = "dense roof vertex " + str(worst_residual_vertex) + " at " +
                  xyz(cap_d[worst_residual_vertex]);
      region_numbers(region).set("cover", cover);
    }
  }
}

// ---- bindings
// -------------------------------------------------------------------------------------

void Validator::check_bindings() {
  // Seam identity across bindings: no two write one vertex.
  {
    ValidationRow& r = row("binding.seam_identity", "the bindings", Severity::error,
                           "no vertex written (weight above zero) by two bindings; every vertex a "
                           "binding does not write is the observation bit for bit in every state",
                           "a bilateral region is two modules with a seam neither writes");
    Vector<std::pair<u32, u32>> written;  // (id, binding)
    for (u32 b = 0; b < bindings_.size(); ++b) {
      const BindingModel& m = bindings_[b];
      if (!m.ok) continue;
      for (u32 i = 0; i < m.domain.size(); ++i)
        if (m.weight[i] > 0.0f) written.push_back({m.domain[i], b});
    }
    std::sort(written.begin(), written.end());
    u32 shared = 0;
    for (u32 i = 1; i < written.size(); ++i)
      if (written[i].first == written[i - 1].first) {
        if (shared == 0) fail(r, "id " + str(written[i].first));
        ++shared;
      }
    r.value.set("written", JsonValue(static_cast<u64>(written.size())));
    r.value.set("written_by_two", JsonValue(shared));
    // Unwritten vertices stay the observation in every state.
    u32 moved = 0;
    for (const BindingModel& m : bindings_) {
      if (!m.ok) continue;
      const RegionModel& region = regions_[m.region];
      for (const StateModel& s : region.states) {
        Vector<Vec3> out;
        if (!transfer(m, s, m.mode, geometry::BindingTerms::full, out)) continue;
        for (u32 i = 0; i < m.domain.size(); ++i) {
          if (m.weight[i] > 0.0f) continue;
          if (std::memcmp(&out[i], &observation_.positions[m.domain[i]], sizeof(Vec3)) != 0) {
            if (moved == 0)
              fail(r, "binding " + m.def->name + ", state " + s.name + ", id " + str(m.domain[i]));
            ++moved;
          }
        }
      }
    }
    r.value.set("unwritten_moved", JsonValue(moved));
  }

  for (BindingModel& m : bindings_) {
    const std::string subject = "binding " + m.def->name;
    JsonValue& numbers = report_.numbers["bindings"];
    if (!numbers.is_object()) numbers = JsonValue::object();
    JsonValue& mine = numbers[m.def->name];
    mine = JsonValue::object();
    mine.set("normal_mode", JsonValue(m.def->normal_mode));
    {
      ValidationRow& r =
          row("binding.domain", subject, Severity::error,
              "the footprint and the band are disjoint subsets of the declared domain, "
              "band weights in (0, 1), one record per domain id",
              "the footprint and the band are two authored objects, and the domain is "
              "an explicit id set");
      r.value.set("domain", JsonValue(static_cast<u64>(m.domain.size())));
      r.value.set("footprint", JsonValue(static_cast<u64>(m.footprint.size())));
      r.value.set("band", JsonValue(static_cast<u64>(m.band.size())));
      if (!m.ok) {
        r.verdict = Verdict::skipped;
      } else {
        for (const u32 id : m.footprint)
          if (!contains_sorted(m.domain, id))
            fail(r, "footprint id " + str(id) + " is outside the domain");
        for (u32 i = 0; i < m.band.size(); ++i) {
          if (!contains_sorted(m.domain, m.band[i]))
            fail(r, "band id " + str(m.band[i]) + " is outside the domain");
          if (contains_sorted(m.footprint, m.band[i]))
            fail(r, "id " + str(m.band[i]) + " is in both the footprint and the band");
          if (!(m.band_weight[i] > 0.0f && m.band_weight[i] < 1.0f))
            fail(r,
                 "band id " + str(m.band[i]) + " has weight " + std::to_string(m.band_weight[i]));
        }
      }
    }
    if (!m.ok) continue;
    const RegionModel& region = regions_[m.region];
    const SheetModel& sheet = region.sheets[m.sheet];
    {
      ValidationRow& r =
          row("binding.written_in_observation_domain", subject, Severity::warning,
              "every vertex the binding writes lies in the observation's declared domain",
              "the observation is bitwise the base outside the regions and their bands; "
              "a binding that writes outside that set moves a vertex the contract calls "
              "base");
      u32 outside = 0;
      JsonValue ids = JsonValue::array();
      for (u32 i = 0; i < m.domain.size(); ++i)
        if (m.weight[i] > 0.0f && !contains_sorted(observation_.domain, m.domain[i])) {
          if (outside < options_.max_witnesses) ids.push_back(JsonValue(m.domain[i]));
          ++outside;
        }
      r.value.set("written_outside", JsonValue(outside));
      r.value.set("ids", std::move(ids));
      if (outside > 0) fail(r, str(outside) + " written ids outside the declared domain");
    }
    {
      ValidationRow& r = row("binding.records", subject, Severity::error,
                             "every footpoint a triangle of the sheet's refined surface within the "
                             "u16 cap, barycentrics a point of it, a finite offset, and a valid "
                             "reference footpoint normal under the record set's mode",
                             "invalid reference normals are rejected, never invented");
      u32 max_triangle = 0;
      u32 active = 0;
      Vector<f64> offsets;
      for (u32 i = 0; i < m.records.size(); ++i) {
        max_triangle = std::max(max_triangle, u32{m.records[i].triangle});
        if (m.weight[i] > 0.0f) {
          ++active;
          offsets.push_back(static_cast<f64>(geometry::binding_normal_offset(m.records[i])));
        }
      }
      std::string error;
      if (!geometry::validate_surface_bindings(m.records, sheet.surface.triangle_count(), &error))
        fail(r, error);
      r.value.set("normal_mode", JsonValue(m.def->normal_mode));
      r.value.set("records", JsonValue(static_cast<u64>(m.records.size())));
      r.value.set("active", JsonValue(active));
      r.value.set("max_triangle", JsonValue(max_triangle));
      r.value.set("surface_triangles", JsonValue(sheet.surface.triangle_count()));
      r.value.set("u16_cap", JsonValue(geometry::k_surface_binding_max_triangles));
      r.value.set("active_offset_mm", stats(offsets, k_mm));
      r.value.set("max_quantization_um",
                  number(static_cast<f64>(m.bind_report.max_quantization_error) * 1e6));
      r.value.set("max_tangential_residual_mm",
                  number(static_cast<f64>(m.bind_report.max_tangential_residual) * k_mm));
      r.value.set("max_offset_disagreement_mm",
                  number(static_cast<f64>(m.bind_report.max_offset_disagreement) * k_mm));
      r.value.set("boundary_footpoints",
                  JsonValue(static_cast<u64>(m.bind_report.boundary_footpoints.size())));
      r.value.set("invalid_normals",
                  JsonValue(static_cast<u64>(m.bind_report.invalid_normals.size())));
      if (!m.bind_report.invalid_normals.empty())
        fail(r, "domain id " + str(m.domain[m.bind_report.invalid_normals.front()]) +
                    " has an invalid reference footpoint normal");
      mine.set("max_triangle", JsonValue(max_triangle));
      mine.set("surface_triangles", JsonValue(sheet.surface.triangle_count()));
    }
    // The reference: bit for bit.
    const StateModel& reference = region.states[region.reference];
    {
      ValidationRow& r =
          row("binding.reference_identity", subject, Severity::error,
              "at the reference state every vertex of the domain is the observation, "
              "bit for bit",
              "the transfer moves the observation by the surface's change, which is "
              "exactly zero at the reference on every compiler");
      Vector<Vec3> out;
      u32 changed = 0;
      if (transfer(m, reference, m.mode, geometry::BindingTerms::full, out)) {
        for (u32 i = 0; i < m.domain.size(); ++i)
          if (std::memcmp(&out[i], &observation_.positions[m.domain[i]], sizeof(Vec3)) != 0) {
            if (changed == 0) fail(r, "id " + str(m.domain[i]));
            ++changed;
          }
      }
      r.value.set("reference_state", JsonValue(reference.name));
      r.value.set("changed", JsonValue(changed));
    }
    // Image orientation at the reference, and triangles turning over in every response state.
    {
      ValidationRow& r =
          row("binding.image_orientation", subject, Severity::error,
              "no bound render triangle whose footpoint image is reversed, and none "
              "that turns over in any state",
              "a nearest-point binding on a fold reverses images before anything moves; "
              "study019's first binding had eight");
      Vector<geometry::SurfaceBinding> full(static_cast<u32>(observation_.positions.size()));
      for (u32 i = 0; i < m.domain.size(); ++i)
        full[m.domain[i]] = m.records[i];
      const Vector<Vec3> ref_nodes = region.sheet_nodes(sheet, reference.nodes);
      const Vector<Vec3> surface = evaluate(sheet.surface.limit, ref_nodes);
      JsonValue sets = JsonValue::object();
      for (const bool fully : {true, false}) {
        geometry::BindingImageReport images;
        geometry::check_binding_images(full, observation_.positions, observation_.triangles,
                                       surface, sheet.surface.faces, fully, images);
        JsonValue e = JsonValue::object();
        e.set("triangles", JsonValue(images.triangles));
        e.set("reversed", JsonValue(static_cast<u64>(images.reversed.size())));
        e.set("deviation_deg_p50", number(static_cast<f64>(images.deviation_p50_deg)));
        e.set("deviation_deg_p95", number(static_cast<f64>(images.deviation_p95_deg)));
        e.set("deviation_deg_max", number(static_cast<f64>(images.deviation_max_deg)));
        if (!images.reversed.empty()) fail(r, "render triangle " + str(images.reversed.front()));
        sets.set(fully ? "fully_weighted" : "active", std::move(e));
      }
      r.value.set("reference", std::move(sets));
      JsonValue turned = JsonValue::object();
      for (const StateModel& s : region.states) {
        if (s.role != StateRole::Response) continue;
        Vector<Vec3> out;
        if (!transfer(m, s, m.mode, geometry::BindingTerms::full, out)) continue;
        const Vector<Vec3> body = body_with(m, out);
        u32 flipped = 0;
        for (u32 t = 0; t + 2 < observation_.triangles.size(); t += 3) {
          const u32 a = observation_.triangles[t];
          const u32 b = observation_.triangles[t + 1];
          const u32 c = observation_.triangles[t + 2];
          const D3 n0 = query::cross(d3(observation_.positions[b]) - d3(observation_.positions[a]),
                                     d3(observation_.positions[c]) - d3(observation_.positions[a]));
          const D3 n1 = query::cross(d3(body[b]) - d3(body[a]), d3(body[c]) - d3(body[a]));
          if (!(query::dot(n0, n1) > 0.0)) {
            if (flipped == 0) fail(r, "state " + s.name + ", render triangle " + str(t / 3));
            ++flipped;
          }
        }
        turned.set(s.name, JsonValue(flipped));
      }
      r.value.set("turned_over", std::move(turned));
    }
    // The footpoint normal against the authoring side's, and the modes against each other.
    {
      ValidationRow& r = row("binding.footpoint_normals", subject, Severity::info,
                             "reported: the angle between the engine's reference footpoint normal "
                             "and the authored one, under the record set's mode and under the "
                             "plan's limit-interpolated rule",
                             "one definition of the normal, measured rather than assumed");
      const Vector<Vec3> ref_nodes = region.sheet_nodes(sheet, reference.nodes);
      for (u32 mode_index = 0; mode_index < geometry::k_normal_mode_count; ++mode_index) {
        const geometry::NormalMode mode = static_cast<geometry::NormalMode>(mode_index);
        geometry::BindingFrame frame;
        if (!geometry::evaluate_binding_frame(mode, sheet.surface, ref_nodes, ref_nodes, frame))
          continue;
        Vector<f64> against_authored;
        Vector<f64> against_own;
        const geometry::BindingSurface view = frame.view(sheet.surface.faces);
        geometry::BindingFrame own;
        geometry::evaluate_binding_frame(m.mode, sheet.surface, ref_nodes, ref_nodes, own);
        const geometry::BindingSurface own_view = own.view(sheet.surface.faces);
        for (u32 i = 0; i < m.records.size(); ++i) {
          if (m.weight[i] <= 0.0f) continue;
          const Vec3 w = geometry::binding_barycentrics(m.records[i]);
          const Vec3 n = geometry::footpoint_normal(mode, view, m.records[i].triangle, w);
          const Vec3 o = geometry::footpoint_normal(m.mode, own_view, m.records[i].triangle, w);
          against_own.push_back(query::angle_deg(d3(n), d3(o)));
          if (!m.authored_normals.empty()) {
            against_authored.push_back(query::angle_deg(d3(n), d3(m.authored_normals[i])));
          }
        }
        JsonValue e = JsonValue::object();
        e.set("against_authored_deg", stats(against_authored, 1.0));
        e.set("against_record_mode_deg", stats(against_own, 1.0));
        r.value.set(geometry::normal_mode_name(mode), std::move(e));
      }
      mine.set("footpoint_normals", r.value);
    }
    // Every response state: the transfer against the authoring side's, and the modes compared.
    for (const StateModel& s : region.states) {
      if (s.role != StateRole::Response) continue;
      Vector<Vec3> ours;
      if (!transfer(m, s, m.mode, geometry::BindingTerms::full, ours)) continue;
      if (!s.expected_visible.empty() && s.def != nullptr &&
          (s.def->binding.empty() || s.def->binding == m.def->name)) {
        ValidationRow& r =
            row("binding.transfer_agreement", subject + ", state " + s.name, Severity::warning,
                "the engine's transfer within 0.01 mm of the authoring side's at every "
                "domain vertex, the same records and mode",
                "two implementations of one transfer are kept honest by comparing them");
        Vector<f64> gap;
        u32 worst = 0;
        f64 worst_gap = 0.0;
        for (u32 i = 0; i < ours.size() && i < s.expected_visible.size(); ++i) {
          const f64 g = query::length(d3(ours[i]) - d3(s.expected_visible[i]));
          gap.push_back(g);
          if (g > worst_gap) {
            worst_gap = g;
            worst = i;
          }
        }
        r.value.set("difference_mm", stats(gap, k_mm));
        if (s.expected_visible.size() != ours.size())
          fail(r, "the expected visible block is not one per domain id");
        if (worst_gap > 1.0e-5) fail(r, "id " + str(m.domain[worst]) + ": " + mm(worst_gap));
      }
      if (options_.compare_modes) {
        ValidationRow& r =
            row("binding.mode_conformance", subject + ", state " + s.name, Severity::info,
                "reported: the same records transferred under each normal mode, and "
                "how far the modes' positions and visible volumes are apart",
                "the round-five closure's conformance comparison: fixed records, the "
                "mode the only difference");
        Vector<Vector<Vec3>> by_mode(geometry::k_normal_mode_count);
        JsonValue volumes = JsonValue::object();
        for (u32 k = 0; k < geometry::k_normal_mode_count; ++k) {
          const geometry::NormalMode mode = static_cast<geometry::NormalMode>(k);
          transfer(m, s, mode, geometry::BindingTerms::full, by_mode[k]);
          const Vector<D3> body = to_d3(body_with(m, by_mode[k]));
          volumes.set(
              geometry::normal_mode_name(mode),
              number(swept(to_d3(observation_.positions), body, observation_.triangles) * k_ml));
        }
        r.value.set("visible_swept_ml", std::move(volumes));
        for (u32 a = 0; a < geometry::k_normal_mode_count; ++a)
          for (u32 b = a + 1; b < geometry::k_normal_mode_count; ++b) {
            Vector<f64> gap;
            for (u32 i = 0; i < m.domain.size(); ++i)
              if (m.weight[i] > 0.0f)
                gap.push_back(query::length(d3(by_mode[a][i]) - d3(by_mode[b][i])));
            r.value.set(
                std::string(geometry::normal_mode_name(static_cast<geometry::NormalMode>(a))) +
                    " vs " + geometry::normal_mode_name(static_cast<geometry::NormalMode>(b)) +
                    " position_mm",
                stats(gap, k_mm));
          }
        mine.set("mode_conformance_" + s.name, r.value);
      }
    }
  }
}

// ---- volumes
// ----------------------------------------------------------------------------------------

void Validator::check_volumes() {
  for (u32 ri = 0; ri < regions_.size(); ++ri) {
    RegionModel& region = regions_[ri];
    if (!region.ok) continue;
    const std::string subject = "region " + region.def->name;
    const SheetModel& top = region.sheets[region.top];
    const StateModel& reference = region.states[region.reference];
    const Vector<D3> top_ref =
        to_d3(evaluate(top.surface.limit, region.sheet_nodes(top, reference.nodes)));
    const i32 bi = binding_of(static_cast<i32>(ri));
    const DepthBudget* budget = nullptr;
    for (const DepthBudget& b : def_.budgets)
      if (b.region == region.def->name) budget = &b;

    // Coverage. The binding follows the top sheet, so the visible body and the smooth quantities
    // represent the cage only when the sheets span every simulated surface node that can move: a
    // node of the cage's boundary on its exposed side (every boundary face not wholly the support
    // sheet's) that is not held and not the support's own. One that is on no sheet is a node the
    // bound surface does not carry; the volume its motion sweeps beyond what the bound surface
    // would have carried it through is the omitted volume, and the smooth entries are restricted.
    const u32 node_count = region.nodes.size();
    Vector<u8> on_top(node_count, 0);
    Vector<u8> on_support(node_count, 0);
    Vector<u8> held(node_count, 0);
    for (const u32 v : top.region_node)
      on_top[v] = 1;
    if (region.support >= 0)
      for (const u32 v : region.sheets[region.support].region_node)
        on_support[v] = 1;
    for (const Attachment& a : def_.attachments) {
      if (a.region != region.def->name || a.kind != AttachmentKind::Fixed) continue;
      for (const NodeSetRef& s : region.def->node_sets) {
        Vector<u32> ids;
        if (s.name == a.nodes && blocks_.read(s.nodes, BlockKind::NodeSet, subject, ids))
          for (const u32 v : ids)
            if (v < node_count) held[v] = 1;
      }
    }
    Vector<u32> exposed_faces;
    for (u32 f = 0; f + 2 < region.boundary.size(); f += 3) {
      const u32* v = region.boundary.data() + f;
      if (on_support[v[0]] != 0 && on_support[v[1]] != 0 && on_support[v[2]] != 0) continue;
      for (u32 k = 0; k < 3; ++k)
        exposed_faces.push_back(v[k]);
    }
    Vector<u8> required(node_count, 0);
    for (const u32 v : exposed_faces)
      if (held[v] == 0 && on_support[v] == 0) required[v] = 1;
    Vector<u32> uncovered;
    u32 required_count = 0;
    for (u32 i = 0; i < node_count; ++i) {
      if (required[i] == 0) continue;
      ++required_count;
      if (on_top[i] == 0) uncovered.push_back(i);
    }
    const bool coverage_complete = uncovered.empty();
    // Where the bound surface would carry each uncovered node: its nearest point on the top
    // sheet's limit surface at the reference, moved as that point moves.
    struct Foot {
      u32 node = 0;
      u32 triangle = 0;
      f64 b[3] = {1.0, 0.0, 0.0};
    };
    Vector<Foot> feet;
    const Vector<D3> reference_nodes = to_d3(reference.nodes);
    if (!coverage_complete) {
      query::TriangleBvh bvh;
      bvh.build(std::span<const D3>(top_ref.data(), top_ref.size()),
                std::span<const u32>(top.surface.faces.data(), top.surface.faces.size()));
      for (const u32 u : uncovered) {
        const query::TriangleBvh::Nearest q = bvh.nearest(reference_nodes[u]);
        feet.push_back(Foot{u, q.triangle, {q.b[0], q.b[1], q.b[2]}});
      }
    }
    JsonValue omitted_states = JsonValue::object();
    Vector<D3> support_ref;
    if (region.support >= 0) {
      const SheetModel& support = region.sheets[region.support];
      support_ref =
          to_d3(evaluate(support.surface.limit, region.sheet_nodes(support, reference.nodes)));
    }

    row("volume.report", subject, Severity::info,
        "reported together, never alone: the cage's piecewise-linear volume, "
        "the closed smooth shell, the top surface's swept volume and the "
        "visible body's, with the exact transfer decomposition and the "
        "projection diagnostic, which is never subtracted",
        "a response is compared with the top surface's sweep, never with the "
        "cage, and the marginals cancel");
    const u32 report_index = report_.rows.size() - 1u;
    JsonValue states = JsonValue::object();
    f64 shell_at_reference = 0.0;
    f64 cage_at_reference = 0.0;
    for (const StateModel& s : region.states) {
      JsonValue e = JsonValue::object();
      const f64 cage = cage_volume(region.tets, s.nodes);
      e.set("cage_ml", number(cage * k_ml));
      if (!region.stitch.empty()) {
        Vector<D3> points;
        Vector<u32> faces;
        for (const SheetModel& sheet : region.sheets) {
          const u32 offset = points.size();
          for (const Vec3& p : evaluate(sheet.surface.limit, region.sheet_nodes(sheet, s.nodes)))
            points.push_back(d3(p));
          for (const u32 v : sheet.surface.faces)
            faces.push_back(v + offset);
        }
        bool in_range = true;
        for (const u32 v : region.stitch) {
          in_range = in_range && v < points.size();
          faces.push_back(v);
        }
        if (in_range) {
          const f64 shell = query::enclosed_volume(points, faces);
          e.set("shell_ml", number(shell * k_ml));
          if (&s == &reference) {
            shell_at_reference = shell;
            cage_at_reference = cage;
          }
        }
      }
      const Vector<D3> top_now =
          to_d3(evaluate(top.surface.limit, region.sheet_nodes(top, s.nodes)));
      e.set("top_swept_ml", number(swept(top_ref, top_now, top.surface.faces) * k_ml));
      // The posterior sweep: the support side's own line, on its limit surface and on its control
      // triangles, signed along the sheet's outward winding.
      if (region.support >= 0) {
        const SheetModel& support = region.sheets[region.support];
        const Vector<D3> support_now =
            to_d3(evaluate(support.surface.limit, region.sheet_nodes(support, s.nodes)));
        e.set("support_swept_ml",
              number(swept(support_ref, support_now, support.surface.faces) * k_ml));
        e.set("support_swept_pl_ml",
              number(swept(to_d3(region.sheet_nodes(support, reference.nodes)),
                           to_d3(region.sheet_nodes(support, s.nodes)), support.faces) *
                     k_ml));
      }
      if (!coverage_complete && &s != &reference) {
        const Vector<D3> actual = to_d3(s.nodes);
        Vector<D3> carried = actual;
        Vector<f64> apart;
        for (const Foot& f : feet) {
          D3 move;
          for (u32 k = 0; k < 3; ++k) {
            const u32 c = top.surface.faces[3 * f.triangle + k];
            move = move + (top_now[c] - top_ref[c]) * f.b[k];
          }
          carried[f.node] = reference_nodes[f.node] + move;
          apart.push_back(query::length(actual[f.node] - carried[f.node]));
        }
        const f64 omitted = swept(reference_nodes, actual, exposed_faces) -
                            swept(reference_nodes, carried, exposed_faces);
        e.set("omitted_ml", number(omitted * k_ml));
        JsonValue o = JsonValue::object();
        o.set("omitted_ml", number(omitted * k_ml));
        o.set("apart_from_bound_surface_mm", stats(apart, k_mm));
        omitted_states.set(s.name, std::move(o));
      }
      if (bi >= 0 && s.role == StateRole::Response) {
        const BindingModel& m = bindings_[bi];
        const Vector<D3> base = to_d3(observation_.positions);
        Vector<Vec3> full;
        Vector<Vec3> op;
        Vector<Vec3> off;
        geometry::BindingFrame frame;
        transfer(m, s, m.mode, geometry::BindingTerms::full, full, &frame);
        transfer(m, s, m.mode, geometry::BindingTerms::displacement, op);
        transfer(m, s, m.mode, geometry::BindingTerms::offset, off);
        const f64 v_full = swept(base, to_d3(body_with(m, full)), observation_.triangles);
        const f64 v_op = swept(base, to_d3(body_with(m, op)), observation_.triangles);
        const f64 v_off = swept(base, to_d3(body_with(m, off)), observation_.triangles);
        e.set("visible_swept_ml", number(v_full * k_ml));
        JsonValue d = JsonValue::object();
        d.set("operator_only_ml", number(v_op * k_ml));
        d.set("offset_only_ml", number(v_off * k_ml));
        d.set("full_ml", number(v_full * k_ml));
        d.set("interaction_ml", number((v_full - v_op - v_off) * k_ml));
        d.set("normal_mode", JsonValue(m.def->normal_mode));
        e.set("decomposition", std::move(d));
        // The projection diagnostic: sum of w h (cos theta - 1) A over the written vertices, theta
        // the footpoint normal's turn, A the skin's vertex area. Reported; never subtracted.
        const Vector<f64> area = vertex_areas(base, observation_.triangles);
        const geometry::BindingSurface view = frame.view(region.sheets[m.sheet].surface.faces);
        f64 projection = 0.0;
        f64 bound_offset = 0.0;
        f64 weighted_area = 0.0;
        Vector<f64> turn;
        for (u32 i = 0; i < m.records.size(); ++i) {
          if (m.weight[i] <= 0.0f) continue;
          const Vec3 w = geometry::binding_barycentrics(m.records[i]);
          const Vec3 n = geometry::footpoint_normal(m.mode, view, m.records[i].triangle, w);
          const Vec3 dn = geometry::footpoint_normal_change(m.mode, view, m.records[i].triangle, w);
          // The turn from the directions themselves, and cos - 1 as -2 sin^2(theta / 2), which
          // keeps its digits where cos is near one.
          const f64 theta_deg = query::angle_deg(d3(n), d3(n) + d3(dn));
          turn.push_back(theta_deg);
          const f64 half = 0.5 * theta_deg * std::numbers::pi / 180.0;
          const f64 cosine_minus_one = -2.0 * std::sin(half) * std::sin(half);
          const f64 h = static_cast<f64>(geometry::binding_normal_offset(m.records[i]));
          const f64 weight = static_cast<f64>(geometry::binding_weight(m.records[i]));
          projection += weight * h * cosine_minus_one * area[m.domain[i]];
          bound_offset += weight * h * area[m.domain[i]];
          weighted_area += weight * area[m.domain[i]];
        }
        e.set("projection_diagnostic_ml", number(projection * k_ml));
        e.set("footpoint_turn_deg", stats(turn, 1.0));
        e.set("bound_offset_volume_ml", number(bound_offset * k_ml));
        e.set("bound_offset_weighted_skin_area_m2", number(weighted_area));
        if (budget != nullptr) {
          ValidationRow& r =
              row("volume.visible_vs_top", subject + ", state " + s.name, Severity::warning,
                  "|visible swept - top-surface swept| at most " +
                      std::to_string(budget->visible_minus_top_ml_max) + " ml",
                  "an absolute target beside the region's volume, never a ratio: what "
                  "is left is the binding's, not the mechanics'");
          const f64 top_swept = swept(top_ref, top_now, top.surface.faces);
          const f64 difference = (v_full - top_swept) * k_ml;
          r.value.set("visible_ml", number(v_full * k_ml));
          r.value.set("top_ml", number(top_swept * k_ml));
          r.value.set("difference_ml", number(difference));
          if (std::fabs(difference) > budget->visible_minus_top_ml_max)
            fail(r, std::to_string(difference) + " ml");
        }
      }
      JsonValue& sn = state_numbers(region, s);
      sn.set("role", JsonValue(static_cast<u64>(s.role)));
      for (auto [key, value] : e.as_object())
        sn.set(key, value);
      states.set(s.name, std::move(e));
    }
    report_.rows[report_index].value.set("reference_state", JsonValue(reference.name));
    report_.rows[report_index].value.set("states", std::move(states));
    report_.rows[report_index].value.set("shell_bias_at_reference_ml",
                                         number((shell_at_reference - cage_at_reference) * k_ml));
    region_numbers(region).set("shell_bias_at_reference_ml",
                               number((shell_at_reference - cage_at_reference) * k_ml));
    {
      // Which representation each quantity is measured on.
      const char* smooth = coverage_complete ? "complete: the sheets span every surface node"
                                             : "restricted: the sheets' control nodes only";
      JsonValue representations = JsonValue::object();
      representations.set("cage_ml", JsonValue("complete: the tetrahedra, every node"));
      representations.set("shell_ml", JsonValue(smooth));
      representations.set("top_swept_ml", JsonValue(smooth));
      representations.set("visible_swept_ml", JsonValue(smooth));
      representations.set("support_swept_ml",
                          JsonValue("the support sheet's limit surface, from the reference"));
      representations.set("support_swept_pl_ml",
                          JsonValue("the support sheet's control triangles, every support node"));
      report_.rows[report_index].value.set("representations", std::move(representations));
      report_.rows[report_index].value.set("coverage_complete", JsonValue(coverage_complete));
      region_numbers(region).set("coverage_complete", JsonValue(coverage_complete));
    }
    ValidationRow& r =
        row("volume.omitted_volume", subject, Severity::warning,
            "the sheets span every simulated surface node that can move; otherwise the volume "
            "those nodes sweep beyond what the bound surface carries is reported and the smooth "
            "shell, the top sweep and the visible body are restricted",
            "a binding over a subset of a cage's surface nodes is not a representation of that "
            "cage: what it omits is invisible to every smooth quantity and can exceed the "
            "response it is compared with");
    r.value.set("required_nodes", JsonValue(required_count));
    r.value.set("uncovered_nodes", JsonValue(static_cast<u64>(uncovered.size())));
    JsonValue first = JsonValue::array();
    for (u32 i = 0; i < uncovered.size() && i < options_.max_witnesses; ++i)
      first.push_back(JsonValue(uncovered[i]));
    r.value.set("uncovered_first", std::move(first));
    r.value.set("states", std::move(omitted_states));
    // With the smooth quantities restricted, containment is the only check that covers the
    // omitted nodes, so it is mandatory: region.skin_containment runs on every node and on the
    // tetrahedra's boundary whatever the coverage, and a restricted binding without it is refused.
    bool containment = false;
    for (const ValidationRow& other : report_.rows)
      containment = containment || (other.id == "region.skin_containment" &&
                                    other.subject == subject && other.verdict != Verdict::skipped);
    r.value.set("containment_evaluated", JsonValue(containment));
    if (!coverage_complete)
      fail(r, str(uncovered.size()) + " moving surface nodes on no sheet, first node " +
                  str(uncovered.front()));
    if (!coverage_complete && !containment) {
      r.severity = Severity::error;
      r.witness = "the binding is restricted and containment could not be evaluated";
    }
  }
}

}  // namespace

const char* severity_name(Severity severity) noexcept {
  switch (severity) {
    case Severity::error: return "error";
    case Severity::warning: return "warning";
    case Severity::info: return "info";
  }
  return "unknown";
}

const char* verdict_name(Verdict verdict) noexcept {
  switch (verdict) {
    case Verdict::pass: return "pass";
    case Verdict::fail: return "fail";
    case Verdict::info: return "info";
    case Verdict::skipped: return "skipped";
  }
  return "unknown";
}

void validate_tissue(const TissueFile& file, const ValidateOptions& options, TissueReport& out) {
  out = TissueReport{};
  Validator validator(file, options, out);
  validator.run();
}

JsonValue rows_json(const TissueReport& report) {
  JsonValue out = JsonValue::object();
  out.set("errors", JsonValue(report.errors));
  out.set("warnings", JsonValue(report.warnings));
  JsonValue rows = JsonValue::array();
  for (const ValidationRow& r : report.rows) {
    JsonValue e = JsonValue::object();
    e.set("id", JsonValue(r.id));
    e.set("subject", JsonValue(r.subject));
    e.set("severity", JsonValue(severity_name(r.severity)));
    e.set("verdict", JsonValue(verdict_name(r.verdict)));
    e.set("threshold", JsonValue(r.threshold));
    e.set("value", r.value);
    e.set("witness", JsonValue(r.witness));
    e.set("note", JsonValue(r.note));
    rows.push_back(std::move(e));
  }
  out.set("rows", std::move(rows));
  return out;
}

JsonValue report_json(const TissueReport& report) {
  JsonValue out = rows_json(report);
  out.set("numbers", report.numbers);
  return out;
}

}  // namespace engine::tissue
