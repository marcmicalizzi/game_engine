#include "layered.h"

#include "cells.h"
#include "mesh_query.h"
#include "p2_face.h"

#include <core/json/json.h>
#include <core/schema/json_reflect.h>
#include <domain/tissue/sha256.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <string>

namespace engine::tissue::detail {

namespace {

// Positions are float32 in every block, and a law or a rigid transform evaluated in double lands
// within half an ulp of what an authoring tool rounded the same double to: at a fixture's scale (a
// tenth of a metre) that is under 4 nm, so 0.1 µm is a few ulps of room and still two orders of
// magnitude below anything a mechanism would move a node by.
constexpr f64 k_position_m = 1.0e-7;
// A rotation's orthonormality and determinant, from JSON's doubles.
constexpr f64 k_rotation = 1.0e-9;
// Barycentrics and shares, float32 in their blocks.
constexpr f64 k_unit = 1.0e-6;

constexpr const char* k_chordal =
    "chordal: each six-node face is tested by its four chords, the same-node subdivision's "
    "boundary; a pass here is not the certificate's curved separation, which is the authoring "
    "side's (reference.certificate_provenance)";

i32 find_region(const LayeredContext& c, std::string_view name) {
  for (u32 i = 0; i < c.regions.size(); ++i)
    if (c.regions[i].def->name == name) return static_cast<i32>(i);
  return -1;
}

const FrameModel* find_frame(const LayeredContext& c, std::string_view name) {
  for (const FrameModel& f : c.frames)
    if (f.def->name == name) return &f;
  return nullptr;
}

i32 find_surface(const LayeredModel& m, std::string_view name) {
  for (u32 i = 0; i < m.surfaces.size(); ++i)
    if (m.surfaces[i].def->name == name) return static_cast<i32>(i);
  return -1;
}

void face_points(const SurfaceModel& s, u32 f, const Vector<Vec3>& x, D3 out[6]) {
  for (u32 k = 0; k < 6; ++k)
    out[k] = d3(x[s.nodes[6 * f + k]]);
}

std::string relative(f64 a, f64 b) {
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "%.3g", b != 0.0 ? (a - b) / b : a);
  return buffer;
}

// The nodes a region's node set names, or none.
Vector<u32> node_set(const LayeredContext& c, const RegionModel& region, std::string_view name) {
  Vector<u32> out;
  Vector<std::string> ignored;
  const Blocks blocks{c.file, ignored};
  for (const NodeSetRef& s : region.def->node_sets)
    if (s.name == name) blocks.read(s.nodes, BlockKind::NodeSet, "node set", out);
  return out;
}

// The chords of a surface at a configuration: four triangles to each face, over the region's nodes.
Vector<u32> chords(const SurfaceModel& s) {
  Vector<u32> out;
  for (u32 f = 0; f < s.face_count(); ++f)
    for (const auto& chord : p2::k_chords)
      for (const u32 k : chord)
        out.push_back(s.nodes[6 * f + k]);
  return out;
}

// The rotation angle of a rotation matrix, degrees.
f64 rotation_angle_deg(const f64 r[9]) {
  const f64 c = std::clamp((r[0] + r[4] + r[8] - 1.0) * 0.5, -1.0, 1.0);
  return std::acos(c) * 180.0 / std::numbers::pi;
}

// ---- the rows -----------------------------------------------------------------------------------

class Rows {
 public:
  Rows(const LayeredContext& context, const LayeredModel& model, TissueReport& report)
      : c_(context), m_(model), def_(context.file.definition), w_{report} {}

  void run();

 private:
  ValidationRow& row(std::string id, std::string subject, Severity severity, std::string threshold,
                     std::string note) {
    return w_.row(std::move(id), std::move(subject), severity, std::move(threshold),
                  std::move(note));
  }
  static void fail(ValidationRow& r, std::string witness) {
    RowWriter::fail(r, std::move(witness));
  }

  void parameters();
  void muscles();
  void frames();
  void attachments();
  void surfaces();
  void fat();
  void skins();
  void contact();
  void separated_contact(u32 attachment);
  void target_gaps();
  void certificates();

  // The transform of a frame at a state of a region: its FrameState there, or where it is.
  RigidTransform frame_at(std::string_view frame, std::string_view state) const {
    return rigid_transform(frame_state(def_, frame, state));
  }
  bool frame_moves(std::string_view frame) const {
    for (const FrameState& s : def_.frame_states)
      if (s.frame_kind == TargetKind::Frame && s.frame == frame) return true;
    return false;
  }
  // The nodes an essential attachment holds, across the definition, for a region.
  Vector<u8> held_nodes(const RegionModel& region) const {
    Vector<u8> out(static_cast<u32>(region.nodes.size()), 0);
    for (const Attachment& a : def_.attachments) {
      if (a.region != region.def->name || a.enforcement != AttachmentEnforcement::Essential)
        continue;
      for (const u32 v : node_set(c_, region, a.nodes))
        if (v < out.size()) out[v] = 1;
    }
    return out;
  }

  const LayeredContext& c_;
  const LayeredModel& m_;
  const TissueDefinition& def_;
  RowWriter w_;
};

void Rows::run() {
  parameters();
  muscles();
  frames();
  attachments();
  surfaces();
  fat();
  skins();
  contact();
  certificates();
}

// ---- parameters ---------------------------------------------------------------------------------

bool known_unit(std::string_view unit) {
  for (const char* u : {"1", "m", "rad", "Pa", "kg/m^3", "N/m", "s", "m/s^2", "kg"})
    if (unit == u) return true;
  return false;
}

void Rows::parameters() {
  for (u32 di = 0; di < def_.parameter_domains.size(); ++di) {
    const ParameterDomain& d = def_.parameter_domains[di];
    ValidationRow& r =
        row("parameters.domain", "parameter domain " + d.name, Severity::error,
            "a unique name; every parameter a unique name, an SI unit of the table ('1', 'm', "
            "'rad', 'Pa', 'kg/m^3', 'N/m', 's', 'm/s^2', 'kg'), finite min <= reference <= max, "
            "and an owner '<kind>:<name>' naming a rest driver, a thickness field or a contact "
            "policy of this definition, or a morphology law it does not carry",
            "a parameter is a stable name, a unit and a range, owned by the law it drives");
    for (u32 dj = 0; dj < di; ++dj)
      if (def_.parameter_domains[dj].name == d.name) fail(r, "the name is used twice");
    if (d.parameters.empty()) fail(r, "the domain has no parameters");
    JsonValue list = JsonValue::array();
    for (u32 i = 0; i < d.parameters.size(); ++i) {
      const Parameter& p = d.parameters[i];
      JsonValue e = JsonValue::object();
      e.set("name", JsonValue(p.name));
      e.set("unit", JsonValue(p.unit));
      e.set("min", number(p.min));
      e.set("max", number(p.max));
      e.set("reference", number(p.reference));
      e.set("owner", JsonValue(p.owner));
      list.push_back(std::move(e));
      if (p.name.empty()) fail(r, "parameter " + str(i) + " has no name");
      for (u32 j = 0; j < i; ++j)
        if (d.parameters[j].name == p.name) fail(r, "parameter " + p.name + " is named twice");
      if (!known_unit(p.unit)) fail(r, "parameter " + p.name + " has unit '" + p.unit + "'");
      if (!std::isfinite(p.min) || !std::isfinite(p.max) || !(p.min <= p.max))
        fail(r, "parameter " + p.name + " has no finite range");
      if (!(p.reference >= p.min && p.reference <= p.max))
        fail(r, "parameter " + p.name + "'s reference is outside its range");
      const usize colon = p.owner.find(':');
      const std::string kind = colon == std::string::npos ? p.owner : p.owner.substr(0, colon);
      const std::string name = colon == std::string::npos ? "" : p.owner.substr(colon + 1);
      bool owned = false;
      if (kind == "rest_driver") {
        for (const Region& region : def_.regions)
          owned = owned || (region.name == name && region.rest_driver.has_value() &&
                            region.rest_driver->domain == d.name);
      } else if (kind == "thickness_field") {
        for (const ThicknessField& f : def_.thickness_fields)
          owned = owned || f.name == name;
      } else if (kind == "contact_policy") {
        for (const ContactPolicy& policy : def_.contact_policies)
          owned = owned || policy.name == name;
      } else if (kind == "morphology") {
        owned = !name.empty();
      }
      if (!owned)
        fail(r, "parameter " + p.name + " names owner '" + p.owner +
                    "', which is not a law of this definition that reads this domain");
    }
    r.value.set("parameters", std::move(list));
  }

  for (const ParameterSample& s : def_.parameter_samples) {
    ValidationRow& r =
        row("state.parameters", "sample " + s.name, Severity::error,
            "names a domain and a region of this definition; one value for every parameter of the "
            "domain and none other, each finite and inside its range; its snapshot one position "
            "per node of the region, whose SHA-256 is the recorded one; the same values never "
            "regenerating two different snapshots",
            "a snapshot is reproducible from its parameters, and its identity is its bytes");
    const ParameterDomain* domain = nullptr;
    for (const ParameterDomain& d : def_.parameter_domains)
      if (d.name == s.domain) domain = &d;
    const SampleModel* model = nullptr;
    for (const SampleModel& sm : m_.samples)
      if (sm.def == &s) model = &sm;
    if (domain == nullptr) fail(r, "the sample names domain '" + s.domain + "'");
    if (find_region(c_, s.region) < 0) fail(r, "the sample names region '" + s.region + "'");
    JsonValue values = JsonValue::object();
    for (const ParameterValue& v : s.values)
      values.set(v.name, number(v.value));
    r.value.set("values", std::move(values));
    if (domain != nullptr) {
      for (const Parameter& p : domain->parameters) {
        u32 given = 0;
        for (const ParameterValue& v : s.values)
          if (v.name == p.name) {
            ++given;
            if (!std::isfinite(v.value) || v.value < p.min || v.value > p.max)
              fail(r, p.name + " = " + std::to_string(v.value) + " is outside [" +
                          std::to_string(p.min) + ", " + std::to_string(p.max) + "]");
          }
        if (given != 1)
          fail(r, "parameter " + p.name + " is given " + str(given) + " times, not once");
      }
      for (const ParameterValue& v : s.values) {
        bool known = false;
        for (const Parameter& p : domain->parameters)
          known = known || p.name == v.name;
        if (!known) fail(r, "value '" + v.name + "' is no parameter of the domain");
      }
    }
    if (model == nullptr || !model->ok || model->block == nullptr) {
      fail(r, "the snapshot '" + s.snapshot + "' does not resolve to one position per node");
    } else {
      const std::string hash = sha256_hex(model->block->bytes);
      r.value.set("snapshot_sha256", JsonValue(hash));
      if (hash != s.snapshot_sha256)
        fail(r, "the snapshot's SHA-256 is " + hash + ", recorded " + s.snapshot_sha256);
      // The same values twice must be the same snapshot.
      for (const SampleModel& other : m_.samples) {
        if (&other == model || other.def == nullptr || other.block == nullptr) continue;
        if (other.def->domain != s.domain || other.def->region != s.region) continue;
        bool same = other.def->values.size() == s.values.size();
        for (const ParameterValue& v : s.values) {
          bool found = false;
          for (const ParameterValue& o : other.def->values)
            found = found || (o.name == v.name && o.value == v.value);
          same = same && found;
        }
        if (same && other.block->bytes != model->block->bytes)
          fail(r, "sample " + other.def->name +
                      " has the same values and a different snapshot: the snapshot is not "
                      "reproducible from its parameters");
      }
    }
    JsonValue sources = JsonValue::array();
    for (const SourceHash& h : s.sources) {
      JsonValue e = JsonValue::object();
      e.set("path", JsonValue(h.path));
      e.set("sha256", JsonValue(h.sha256));
      sources.push_back(std::move(e));
    }
    r.value.set("sources", std::move(sources));
    r.value.set("sources_note", JsonValue("reported: their bytes are not in the file"));
  }
}

// ---- the active rest shape ----------------------------------------------------------------------

struct CanonicalFrame {
  D3 origin;
  D3 x;
  D3 y;
  D3 z;
};

// fusiform-arch-v1 (DESIGN's synthetic law): lambda = 1 - c a; x' = lambda x, y' = y /
// sqrt(lambda), z' = z / sqrt(lambda) + b sin(theta) (1 - (2 x / L)^2), in the driver's canonical
// frame. Its Jacobian is triangular with diagonal (lambda, lambda^-1/2, lambda^-1/2): det G = 1.
D3 fusiform_arch(D3 p, const CanonicalFrame& f, f64 c, f64 b, f64 length, f64 a, f64 theta) {
  const D3 q = p - f.origin;
  const f64 x = query::dot(q, f.x);
  const f64 y = query::dot(q, f.y);
  const f64 z = query::dot(q, f.z);
  const f64 lambda = 1.0 - c * a;
  const f64 root = std::sqrt(lambda);
  const f64 s = 2.0 * x / length;
  const f64 x1 = lambda * x;
  const f64 y1 = y / root;
  const f64 z1 = z / root + b * std::sin(theta) * (1.0 - s * s);
  return f.origin + f.x * x1 + f.y * y1 + f.z * z1;
}

void Rows::muscles() {
  for (const RegionModel& region : c_.regions) {
    if (!region.def->rest_driver.has_value()) continue;
    const ActiveRestShape& driver = *region.def->rest_driver;
    const std::string subject = "region " + region.def->name;
    const StateModel* reference = region.state(driver.reference_state);
    // The driver's samples, resolved.
    Vector<const SampleModel*> samples;
    std::string sample_problem;
    for (const std::string& name : driver.samples) {
      const SampleModel* found = nullptr;
      for (const SampleModel& sm : m_.samples)
        if (sm.def->name == name) found = &sm;
      if (found == nullptr || !found->ok)
        sample_problem = "sample '" + name + "' does not resolve";
      else if (found->def->region != region.def->name || found->def->domain != driver.domain)
        sample_problem = "sample '" + name + "' is not of this region and domain";
      else
        samples.push_back(found);
    }
    const ParameterDomain* domain = nullptr;
    for (const ParameterDomain& d : def_.parameter_domains)
      if (d.name == driver.domain) domain = &d;
    const auto value = [&](const SampleModel& s, const std::string& name, f64& out) {
      for (const ParameterValue& v : s.def->values)
        if (v.name == name) {
          out = v.value;
          return true;
        }
      return false;
    };
    const auto at_identity = [&](const SampleModel& s) {
      if (domain == nullptr) return false;
      for (const Parameter& p : domain->parameters) {
        f64 v = 0.0;
        if (!value(s, p.name, v) || v != p.reference) return false;
      }
      return true;
    };

    // ---- muscle.rest_identity
    {
      ValidationRow& r =
          row("muscle.rest_identity", subject, Severity::error,
              "the driver's canonical reference state exists, its samples are the region's and its "
              "domain's, and the sample at every parameter's reference value (no activation, no "
              "pose) has a snapshot equal to the reference state's nodes bit for bit: the law is "
              "the identity there",
              "a rest driver maps the canonical reference, and at zero activation and pose it maps "
              "it to itself; the identity is sampled, not assumed");
      r.value.set("law", JsonValue(driver.law));
      r.value.set("reference_state", JsonValue(driver.reference_state));
      r.value.set("samples", JsonValue(static_cast<u64>(driver.samples.size())));
      if (reference == nullptr)
        fail(r,
             "the reference state '" + driver.reference_state + "' is not a state of the region");
      if (domain == nullptr) fail(r, "the driver names domain '" + driver.domain + "'");
      if (!sample_problem.empty()) fail(r, sample_problem);
      const SampleModel* identity = nullptr;
      for (const SampleModel* s : samples)
        if (identity == nullptr && at_identity(*s)) identity = s;
      if (identity == nullptr) {
        fail(r, "no sample is at every parameter's reference value: the identity is not sampled");
      } else if (reference != nullptr) {
        r.value.set("identity_sample", JsonValue(identity->def->name));
        u32 differ = 0;
        for (u32 i = 0; i < reference->nodes.size() && i < identity->snapshot.size(); ++i)
          if (std::memcmp(&reference->nodes[i], &identity->snapshot[i], sizeof(Vec3)) != 0) {
            if (differ == 0)
              fail(r, "sample " + identity->def->name + ", node " + str(i) +
                          " differs from the reference state");
            ++differ;
          }
        r.value.set("nodes_differing", JsonValue(differ));
      }
    }

    // ---- muscle.target_volume
    {
      ValidationRow& r = row(
          "muscle.target_volume", subject, Severity::error,
          "every target snapshot's exact volume within 1e-5 of the reference state's, relative, "
          "and every cell's within 1e-4 (the law's det G = 1; float32 storage is all that is "
          "allowed for), each target cell's Jacobian proven positive, and the mass the "
          "reference's (density times reference volume, whatever the activation)",
          "activation preserves reference volume and mass; the natural target is itself a "
          "valid configuration; analytic det G = 1 is not claimed for rounded data, so it is "
          "measured");
      JsonValue per_sample = JsonValue::object();
      if (reference == nullptr) {
        r.verdict = Verdict::skipped;
        r.note = "the driver's reference state does not resolve";
      } else {
        f64 v0 = 0.0;
        for (u32 cell = 0; cell < region.cell_count(); ++cell)
          v0 += region.cell_volume(cell, reference->nodes);
        r.value.set("reference_volume_ml", number(v0 * k_ml));
        for (const SampleModel* s : samples) {
          f64 v = 0.0;
          f64 worst_cell = 0.0;
          u32 worst_index = 0;
          u32 not_positive = 0;
          f64 least_ratio = std::numeric_limits<f64>::infinity();
          for (u32 cell = 0; cell < region.cell_count(); ++cell) {
            const f64 a = region.cell_volume(cell, reference->nodes);
            const f64 b = region.cell_volume(cell, s->snapshot);
            v += b;
            const f64 dev = std::fabs(b / a - 1.0);
            if (dev > worst_cell) {
              worst_cell = dev;
              worst_index = cell;
            }
            if (region.quadratic()) {
              D3 x[10];
              region.gather(cell, s->snapshot, x);
              const cells::JacobianCheck j = cells::quadratic_jacobian(x, k_jacobian_depth);
              if (j.upper > 0.0) least_ratio = std::min(least_ratio, j.lower / j.upper);
              if (j.result != cells::JacobianCheck::Result::positive) {
                if (not_positive == 0)
                  fail(r, "sample " + s->def->name + ", cell " + str(cell) +
                              ": the target's det J is not proven positive");
                ++not_positive;
              }
            } else if (!(b > 0.0)) {
              if (not_positive == 0)
                fail(r, "sample " + s->def->name + ", tetrahedron " + str(cell) + " inverted");
              ++not_positive;
            }
          }
          JsonValue e = JsonValue::object();
          e.set("volume_ml", number(v * k_ml));
          e.set("relative_change", number(v / v0 - 1.0));
          e.set("worst_cell_relative_change", number(worst_cell));
          e.set("worst_cell", JsonValue(worst_index));
          e.set("cells_not_proven_positive", JsonValue(not_positive));
          if (region.quadratic()) e.set("jacobian_ratio_min", number(least_ratio));
          per_sample.set(s->def->name, std::move(e));
          if (!(std::fabs(v / v0 - 1.0) <= 1.0e-5))
            fail(r,
                 "sample " + s->def->name + ": the target's volume changes by " + relative(v, v0));
          if (!(worst_cell <= 1.0e-4))
            fail(r, "sample " + s->def->name + ", cell " + str(worst_index) +
                        ": its volume "
                        "changes by " +
                        relative(worst_cell + 1.0, 1.0));
        }
      }
      r.value.set("samples", std::move(per_sample));
    }

    // ---- muscle.passive_reference
    {
      ValidationRow& r =
          row("muscle.passive_reference", subject, Severity::error,
              "the canonical reference is the construction, the reference or a rest state, never a "
              "response; no target snapshot is a solved (response) state bit for bit; the natural "
              "length positive and the driver's own",
              "the observed pose is not its own stress-free state: a target reset to the current "
              "solution is the current-pose reset the layered model refuses");
      r.value.set("natural_length_m", number(driver.natural_length_m));
      if (reference != nullptr && reference->role == StateRole::Response)
        fail(r, "the canonical reference is response state " + reference->name);
      if (!(driver.natural_length_m > 0.0) || !std::isfinite(driver.natural_length_m))
        fail(r, "the natural length is " + std::to_string(driver.natural_length_m));
      for (const SampleModel* s : samples)
        for (const StateModel& state : region.states) {
          if (state.role != StateRole::Response || state.nodes.size() != s->snapshot.size())
            continue;
          if (std::memcmp(state.nodes.data(), s->snapshot.data(),
                          state.nodes.size() * sizeof(Vec3)) == 0)
            fail(r, "sample " + s->def->name + "'s target is response state " + state.name +
                        " bit for bit");
        }
    }

    // ---- muscle.objectivity
    {
      ValidationRow& r =
          row("muscle.objectivity", subject, Severity::error,
              "the target is a function of the material reference and the parameters, in the "
              "driver's canonical frame, and never of the current pose: every target snapshot is "
              "the law evaluated on the canonical reference state within 0.1 um, the frame's axes "
              "unit and orthogonal to 1e-9; each snapshot's best-fit rotation against the "
              "reference reported beside it. The energy's invariance under a rigid motion of "
              "F G^-1 is the certificate's",
              "a snapshot alone is not a parametric law, and a target carrying a frame's motion or "
              "a solved state is not the law's");
      r.value.set("law", JsonValue(driver.law));
      if (driver.law != "fusiform-arch-v1") {
        r.verdict = Verdict::skipped;
        r.note = "law '" + driver.law + "' is not one this build evaluates (fusiform-arch-v1)";
      } else if (reference == nullptr) {
        r.verdict = Verdict::skipped;
        r.note = "the driver's reference state does not resolve";
      } else {
        FusiformArchCoefficients coefficients;
        schema::ReadContext ctx;
        ctx.options.ignore_unknown_fields = false;
        const bool parsed = driver.coefficients.is_null() ||
                            schema::from_json(coefficients, driver.coefficients, ctx);
        if (!parsed)
          fail(r, "the coefficients are not fusiform-arch-v1's: " + ctx.diagnostics.front().path +
                      ": " + ctx.diagnostics.front().message);
        CanonicalFrame f;
        f.origin = D3{driver.origin_m[0], driver.origin_m[1], driver.origin_m[2]};
        f.x = D3{driver.axis[0], driver.axis[1], driver.axis[2]};
        f.y = D3{driver.transverse[0], driver.transverse[1], driver.transverse[2]};
        f.z = query::cross(f.x, f.y);
        if (!(std::fabs(query::length(f.x) - 1.0) <= k_rotation) ||
            !(std::fabs(query::length(f.y) - 1.0) <= k_rotation) ||
            !(std::fabs(query::dot(f.x, f.y)) <= k_rotation))
          fail(r, "the canonical frame's axis and transverse are not unit and orthogonal");
        r.value.set("activation_contraction", number(coefficients.activation_contraction));
        r.value.set("arch_m", number(coefficients.arch_m));
        JsonValue per_sample = JsonValue::object();
        for (const SampleModel* s : samples) {
          f64 a = 0.0;
          f64 theta = 0.0;
          const bool has = value(*s, driver.activation, a) && value(*s, driver.pose, theta);
          JsonValue e = JsonValue::object();
          if (!has) {
            fail(r, "sample " + s->def->name + " gives no '" + driver.activation + "' or '" +
                        driver.pose + "'");
            per_sample.set(s->def->name, std::move(e));
            continue;
          }
          f64 worst = 0.0;
          u32 worst_node = 0;
          // The best-fit rotation of the target against the reference: the polar factor of the
          // centred cross-covariance, by Higham's iteration.
          D3 centre_a;
          D3 centre_b;
          const u32 n = static_cast<u32>(reference->nodes.size());
          for (u32 i = 0; i < n; ++i) {
            const D3 target =
                fusiform_arch(d3(reference->nodes[i]), f, coefficients.activation_contraction,
                              coefficients.arch_m, driver.natural_length_m, a, theta);
            const f64 gap = query::length(target - d3(s->snapshot[i]));
            if (gap > worst) {
              worst = gap;
              worst_node = i;
            }
            centre_a = centre_a + d3(reference->nodes[i]) * (1.0 / n);
            centre_b = centre_b + d3(s->snapshot[i]) * (1.0 / n);
          }
          f64 h[9] = {};
          for (u32 i = 0; i < n; ++i) {
            const D3 p = d3(reference->nodes[i]) - centre_a;
            const D3 q = d3(s->snapshot[i]) - centre_b;
            const f64 pv[3] = {p.x, p.y, p.z};
            const f64 qv[3] = {q.x, q.y, q.z};
            for (u32 row_index = 0; row_index < 3; ++row_index)
              for (u32 col = 0; col < 3; ++col)
                h[3 * row_index + col] += qv[row_index] * pv[col];
          }
          f64 rot[9];
          std::memcpy(rot, h, sizeof(rot));
          for (u32 iteration = 0; iteration < 50; ++iteration) {
            // R <- (R + R^-T) / 2.
            const f64 det = rot[0] * (rot[4] * rot[8] - rot[5] * rot[7]) -
                            rot[1] * (rot[3] * rot[8] - rot[5] * rot[6]) +
                            rot[2] * (rot[3] * rot[7] - rot[4] * rot[6]);
            if (!(std::fabs(det) > 0.0)) break;
            const f64 inv_t[9] = {(rot[4] * rot[8] - rot[5] * rot[7]) / det,
                                  -(rot[3] * rot[8] - rot[5] * rot[6]) / det,
                                  (rot[3] * rot[7] - rot[4] * rot[6]) / det,
                                  -(rot[1] * rot[8] - rot[2] * rot[7]) / det,
                                  (rot[0] * rot[8] - rot[2] * rot[6]) / det,
                                  -(rot[0] * rot[7] - rot[1] * rot[6]) / det,
                                  (rot[1] * rot[5] - rot[2] * rot[4]) / det,
                                  -(rot[0] * rot[5] - rot[2] * rot[3]) / det,
                                  (rot[0] * rot[4] - rot[1] * rot[3]) / det};
            for (u32 k = 0; k < 9; ++k)
              rot[k] = 0.5 * (rot[k] + inv_t[k]);
          }
          e.set("activation", number(a));
          e.set("pose", number(theta));
          e.set("max_departure_from_law_m", number(worst));
          e.set("worst_node", JsonValue(worst_node));
          e.set("best_fit_rotation_deg", number(rotation_angle_deg(rot)));
          per_sample.set(s->def->name, std::move(e));
          if (!(worst <= k_position_m))
            fail(r, "sample " + s->def->name + ", node " + str(worst_node) + ": " + mm(worst) +
                        " from the law's target");
        }
        r.value.set("samples", std::move(per_sample));
      }
    }
  }
}

// ---- frames -------------------------------------------------------------------------------------

void Rows::frames() {
  // Every frame (or bone) a FrameState names, in the order first named.
  Vector<std::string> names;
  Vector<TargetKind> kinds;
  for (const FrameState& s : def_.frame_states) {
    bool seen = false;
    for (u32 i = 0; i < names.size(); ++i)
      seen = seen || (names[i] == s.frame && kinds[i] == s.frame_kind);
    if (!seen) {
      names.push_back(s.frame);
      kinds.push_back(s.frame_kind);
    }
  }
  const auto state_exists = [&](std::string_view state) {
    if (state == "construction") return true;
    for (const RegionState& s : def_.states)
      if (s.name == state) return true;
    return false;
  };
  for (u32 fi = 0; fi < names.size(); ++fi) {
    const bool bone = kinds[fi] == TargetKind::Bone;
    const std::string subject = (bone ? "bone " : "frame ") + names[fi];
    ValidationRow& r = row(
        "frame.rigidity", subject, Severity::error,
        "every FrameState names a frame of this definition (or a bone by name) and a state that "
        "exists, at most one per state; its rotation orthonormal to 1e-9 with determinant +1 to "
        "1e-9, its translation finite, its interpolation named",
        "a frame is rigid: nothing scales, shears or softens it to make tissue fit, and its "
        "motion between states is a declared rule");
    if (kinds[fi] != TargetKind::Frame && kinds[fi] != TargetKind::Bone)
      fail(r, "a FrameState's frame kind is neither Frame nor Bone");
    if (!bone && find_frame(c_, names[fi]) == nullptr)
      fail(r, "no frame '" + names[fi] + "' in the definition");
    if (bone && names[fi].empty()) fail(r, "a bone FrameState names no bone");
    JsonValue path = JsonValue::array();
    Vector<std::string> states;
    for (const FrameState& s : def_.frame_states) {
      if (s.frame != names[fi] || s.frame_kind != kinds[fi]) continue;
      for (const std::string& seen : states)
        if (seen == s.state) fail(r, "state " + s.state + " has two FrameStates");
      states.push_back(s.state);
      if (!state_exists(s.state)) fail(r, "state '" + s.state + "' does not exist");
      const f64* m = s.rotation.data();
      f64 worst = 0.0;
      for (u32 i = 0; i < 3; ++i)
        for (u32 j = 0; j < 3; ++j) {
          f64 dot = 0.0;
          for (u32 k = 0; k < 3; ++k)
            dot += m[3 * k + i] * m[3 * k + j];
          worst = std::max(worst, std::fabs(dot - (i == j ? 1.0 : 0.0)));
        }
      const f64 det = m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) +
                      m[2] * (m[3] * m[7] - m[4] * m[6]);
      JsonValue e = JsonValue::object();
      e.set("state", JsonValue(s.state));
      e.set("orthonormality_error", number(worst));
      e.set("determinant", number(det));
      e.set("interpolation", JsonValue(s.interpolation));
      path.push_back(std::move(e));
      if (!(worst <= k_rotation) || !(std::fabs(det - 1.0) <= k_rotation))
        fail(r, "state " + s.state + ": the rotation is not a proper rotation (orthonormality " +
                    std::to_string(worst) + ", determinant " + std::to_string(det) + ")");
      for (const f64 t : s.translation_m)
        if (!std::isfinite(t)) fail(r, "state " + s.state + ": the translation is not finite");
      if (s.interpolation.empty()) fail(r, "state " + s.state + " names no interpolation rule");
    }
    r.value.set("states", std::move(path));
  }

  for (u32 fi = 0; fi < names.size(); ++fi) {
    const std::string subject = (kinds[fi] == TargetKind::Bone ? "bone " : "frame ") + names[fi];
    ValidationRow& r =
        row("contact.frame_trajectory", subject, Severity::info,
            "reported: the frame's load path, its FrameStates in the definition's order, with the "
            "rotation angle and the translation of each step and its interpolation rule",
            "a rigid rotation is not a linear vertex trajectory: continuous collision along it is "
            "the certificate's (rigid-motion bounds), and this build never interpolates a frame");
    JsonValue steps = JsonValue::array();
    const FrameState* previous = nullptr;
    for (const FrameState& s : def_.frame_states) {
      if (s.frame != names[fi] || s.frame_kind != kinds[fi]) continue;
      JsonValue e = JsonValue::object();
      e.set("state", JsonValue(s.state));
      e.set("interpolation", JsonValue(s.interpolation));
      if (previous != nullptr) {
        // R_prev^T R.
        f64 rel[9] = {};
        for (u32 i = 0; i < 3; ++i)
          for (u32 j = 0; j < 3; ++j)
            for (u32 k = 0; k < 3; ++k)
              rel[3 * i + j] += previous->rotation[3 * k + i] * s.rotation[3 * k + j];
        const D3 step{s.translation_m[0] - previous->translation_m[0],
                      s.translation_m[1] - previous->translation_m[1],
                      s.translation_m[2] - previous->translation_m[2]};
        e.set("rotation_deg", number(rotation_angle_deg(rel)));
        e.set("translation_mm", number(query::length(step) * k_mm));
      }
      previous = &s;
      steps.push_back(std::move(e));
    }
    r.value.set("load_path", std::move(steps));
  }
}

// ---- attachments --------------------------------------------------------------------------------

void Rows::attachments() {
  for (u32 ai = 0; ai < def_.attachments.size(); ++ai) {
    const Attachment& a = def_.attachments[ai];
    const bool essential = a.enforcement == AttachmentEnforcement::Essential;
    // `gap_m != 0` is true for a NaN too: a gap that is not a number is declared, not absent.
    const bool declares_interface =
        a.interface != AttachmentInterface::Coincident || a.gap_m != 0.0;
    const bool uses_new = essential || !a.patch.empty() || !a.patch_surface.empty() ||
                          !a.transition_band.empty() || declares_interface;
    if (!uses_new) continue;
    const i32 ri = find_region(c_, a.region);
    const std::string subject = "attachment " + a.name;
    ValidationRow& r = row(
        "attachment.enforcement", subject, Severity::error,
        "an essential attachment is kind Fixed, to a frame or the world, with no stiffness; "
        "its patch a set of faces of a material surface of its region, every node of which it "
        "holds; a spring carries no patch, no Separated interface and no gap; a transition band a "
        "node set of the region, disjoint "
        "from the held nodes and owned by one attachment; no node held by two essential "
        "attachments",
        "an essential attachment is an exact constraint and never a stiff spring; exact "
        "attachment and soft transition are different data");
    r.value.set("enforcement", JsonValue(essential ? "Essential" : "Spring"));
    if (ri < 0) {
      fail(r, "the attachment names region '" + a.region + "'");
      continue;
    }
    const RegionModel& region = c_.regions[ri];
    const Vector<u32> held = node_set(c_, region, a.nodes);
    Vector<u32> held_sorted = held;
    std::sort(held_sorted.begin(), held_sorted.end());
    r.value.set("nodes", JsonValue(static_cast<u64>(held.size())));
    if (essential) {
      if (a.kind != AttachmentKind::Fixed)
        fail(r, "an essential attachment of a kind other than Fixed is not one this build reads");
      if (a.target_kind != TargetKind::Frame && a.target_kind != TargetKind::World)
        fail(r, "an essential attachment's target is a frame or the world");
      if (a.stiffness_pa.has_value())
        fail(r, "an essential attachment declares a stiffness (" + std::to_string(*a.stiffness_pa) +
                    " Pa): that is a spring, and it is never read as one");
      // No node held exactly by two essential attachments.
      for (u32 bi = 0; bi < ai; ++bi) {
        const Attachment& b = def_.attachments[bi];
        if (b.enforcement != AttachmentEnforcement::Essential || b.region != a.region) continue;
        for (const u32 v : node_set(c_, region, b.nodes))
          if (contains_sorted(held_sorted, v)) {
            fail(r, "node " + str(v) + " is also held by essential attachment " + b.name);
            break;
          }
      }
    } else {
      if (!a.patch.empty() || !a.patch_surface.empty())
        fail(
            r,
            "a spring attachment names a patch: a patch is an essential attachment's material map");
      // A spring's stretch is a force whatever it starts at, so where its nodes lie against the
      // target says nothing a spring reads; an interface or a gap on one is data it would ignore.
      if (a.interface != AttachmentInterface::Coincident)
        fail(
            r,
            "a spring attachment declares interface Separated: the interface says how an essential "
            "attachment holds its patch against its frame, and a spring, whose stretch is a "
            "force, reads none");
      if (a.gap_m != 0.0)
        fail(r, "a spring attachment declares a gap of " + mm(a.gap_m) +
                    ": the gap is a separated essential tie's, and a spring, whose stretch is a "
                    "force, reads none");
    }
    const PatchModel& patch = m_.patches[ai];
    if (!a.patch.empty()) {
      const i32 si = find_surface(m_, a.patch_surface);
      if (si < 0) {
        fail(r, "the patch's surface '" + a.patch_surface + "' is not a material surface");
      } else if (m_.surfaces[si].def->region != a.region) {
        fail(r, "the patch's surface is not of the attachment's region");
      } else if (!patch.ok || !m_.surfaces[si].ok) {
        fail(r, "the patch does not resolve");
      } else {
        const SurfaceModel& s = m_.surfaces[si];
        u32 outside = 0;
        u32 unheld = 0;
        for (u32 f = 0; f + 1 < patch.refs.size(); f += 2) {
          const u32 face = s.find(patch.refs[f], patch.refs[f + 1]);
          if (face == ~0u) {
            if (outside == 0)
              fail(r, "patch face (" + str(patch.refs[f]) + ", " + str(patch.refs[f + 1]) +
                          ") is not a face of surface " + s.def->name);
            ++outside;
            continue;
          }
          for (u32 k = 0; k < 6; ++k)
            if (!contains_sorted(held_sorted, s.nodes[6 * face + k])) {
              if (unheld == 0)
                fail(r, "node " + str(s.nodes[6 * face + k]) + " of patch face " + str(face) +
                            " is not held");
              ++unheld;
            }
        }
        r.value.set("patch_faces", JsonValue(static_cast<u64>(patch.refs.size() / 2)));
        r.value.set("patch_nodes_not_held", JsonValue(unheld));
      }
    } else if (!a.patch_surface.empty()) {
      fail(r, "the attachment names a patch surface and no patch");
    }
    if (!a.transition_band.empty()) {
      bool exists = false;
      for (const NodeSetRef& s : region.def->node_sets)
        exists = exists || s.name == a.transition_band;
      const Vector<u32> band = node_set(c_, region, a.transition_band);
      r.value.set("band_nodes", JsonValue(static_cast<u64>(band.size())));
      if (!exists) fail(r, "the transition band '" + a.transition_band + "' is no node set");
      for (const u32 v : band)
        if (contains_sorted(held_sorted, v)) {
          fail(r, "band node " + str(v) + " is also held: exact and soft are different data");
          break;
        }
      for (u32 bi = 0; bi < def_.attachments.size(); ++bi)
        if (bi != ai && def_.attachments[bi].region == a.region &&
            def_.attachments[bi].transition_band == a.transition_band)
          fail(r, "the band is owned by attachment " + def_.attachments[bi].name + " too");
    }
  }

  // ---- attachment.pose_binding: every essential attachment, and every spring to a moving frame.
  for (const Attachment& a : def_.attachments) {
    const bool essential = a.enforcement == AttachmentEnforcement::Essential;
    const bool to_moving_frame = a.target_kind == TargetKind::Frame && frame_moves(a.target);
    if (!essential && !to_moving_frame) continue;
    const i32 ri = find_region(c_, a.region);
    if (ri < 0) continue;
    const RegionModel& region = c_.regions[ri];
    if (!region.ok) continue;
    ValidationRow& r =
        row("attachment.pose_binding", "attachment " + a.name, Severity::error,
            essential
                ? "at every state, every held node where the target frame's transform there "
                  "carries its construction position (the construction's transform undone), within "
                  "0.1 um; the world holds it where it was built"
                : "reported, never failed: a spring's nodes against where its moving frame carries "
                  "their construction positions, at every state",
            essential ? "an essential attachment is prescribed: its nodes are where the frame puts "
                        "them, bit for bit up to float32"
                      : "a spring's stretch is a force, not a violation");
    const Vector<u32> held = node_set(c_, region, a.nodes);
    const bool framed = a.target_kind == TargetKind::Frame;
    const RigidTransform at_construction =
        framed ? frame_at(a.target, "construction") : RigidTransform{};
    JsonValue per_state = JsonValue::object();
    for (const StateModel& s : region.states) {
      const RigidTransform at = framed ? frame_at(a.target, s.name) : RigidTransform{};
      f64 worst = 0.0;
      u32 worst_node = 0;
      for (const u32 v : held) {
        if (v >= s.nodes.size()) continue;
        const D3 target = at.apply(at_construction.unapply(d3(region.nodes[v])));
        const f64 gap = query::length(d3(s.nodes[v]) - target);
        if (gap > worst) {
          worst = gap;
          worst_node = v;
        }
      }
      JsonValue e = JsonValue::object();
      e.set("max_departure_mm", number(worst * k_mm));
      e.set("worst_node", JsonValue(worst_node));
      per_state.set(s.name, std::move(e));
      if (essential && !(worst <= k_position_m))
        fail(r, "state " + s.name + ", node " + str(worst_node) + " is " + mm(worst) +
                    " from where the frame holds it");
    }
    r.value.set("states", std::move(per_state));
  }
}

// ---- the material boundary ----------------------------------------------------------------------

void Rows::surfaces() {
  for (const SurfaceModel& s : m_.surfaces) {
    const std::string subject = "surface " + s.def->name;
    const RegionModel* region = s.region >= 0 ? &c_.regions[s.region] : nullptr;
    // ---- surface.material_boundary
    {
      ValidationRow& r =
          row("surface.material_boundary", subject, Severity::error,
              "names a ten-node region of this definition; its faces (cell, local face) in range, "
              "none twice, each a boundary face of the region (no other cell has it); evaluated "
              "'p2-six-node-v1'",
              "the material boundary is the quadratic cells' own outer faces, the surface the "
              "membrane, the contact and the skin binding share; not a Loop sheet");
      r.value.set("faces", JsonValue(static_cast<u64>(s.face_count())));
      if (region == nullptr) {
        fail(r, "the surface names region '" + s.def->region + "'");
      } else if (!region->quadratic()) {
        fail(r, "region " + region->def->name +
                    " is a four-node cage: a material boundary surface is six-node faces");
      } else if (s.def->evaluation != "p2-six-node-v1") {
        r.verdict = Verdict::skipped;
        r.note = "evaluation '" + s.def->evaluation + "' is not one this build reads";
      } else if (!s.ok) {
        fail(r, "the faces do not resolve");
      } else {
        // Every face of every cell, by its sorted corners, and how many cells have it.
        struct Key {
          u32 v[3];
          bool operator<(const Key& o) const noexcept {
            return std::lexicographical_compare(v, v + 3, o.v, o.v + 3);
          }
          bool operator==(const Key& o) const noexcept {
            return v[0] == o.v[0] && v[1] == o.v[1] && v[2] == o.v[2];
          }
        };
        const auto key_of = [&](u32 cell, u32 local) {
          Key k;
          for (u32 i = 0; i < 3; ++i)
            k.v[i] = region->cell(cell)[p2::k_face_corners[local][i]];
          std::sort(k.v, k.v + 3);
          return k;
        };
        Vector<Key> all;
        for (u32 cell = 0; cell < region->cell_count(); ++cell)
          for (u32 local = 0; local < 4; ++local)
            all.push_back(key_of(cell, local));
        std::sort(all.begin(), all.end());
        Vector<Key> mine;
        u32 interior = 0;
        for (u32 f = 0; f < s.face_count(); ++f) {
          const Key k = key_of(s.refs[2 * f], s.refs[2 * f + 1]);
          const auto lo = std::lower_bound(all.begin(), all.end(), k);
          const auto hi = std::upper_bound(all.begin(), all.end(), k);
          if (hi - lo != 1) {
            if (interior == 0)
              fail(r, "face " + str(f) + " (" + str(s.refs[2 * f]) + ", " + str(s.refs[2 * f + 1]) +
                          ") is shared by " + str(static_cast<u64>(hi - lo)) +
                          " cells: not on the boundary");
            ++interior;
          }
          mine.push_back(k);
        }
        std::sort(mine.begin(), mine.end());
        u32 twice = 0;
        for (u32 i = 1; i < mine.size(); ++i)
          if (mine[i] == mine[i - 1]) ++twice;
        if (twice > 0) fail(r, str(twice) + " faces are listed twice");
        Vector<u32> nodes = s.nodes;
        std::sort(nodes.begin(), nodes.end());
        nodes.erase(std::unique(nodes.begin(), nodes.end()), nodes.end());
        f64 area = 0.0;
        for (u32 f = 0; f < s.face_count(); ++f) {
          D3 x[6];
          face_points(s, f, region->nodes, x);
          area += p2::area(x);
        }
        r.value.set("nodes", JsonValue(static_cast<u64>(nodes.size())));
        r.value.set("interior_faces", JsonValue(interior));
        r.value.set("area_at_construction_mm2", number(area * k_mm * k_mm));
      }
    }
    // ---- surface.orientation
    {
      ValidationRow& r =
          row("surface.orientation", subject, Severity::error,
              "every face faces out of its cell at the construction (its corners' normal points "
              "away from the corner it does not have), and the faces are consistently wound: an "
              "edge two faces share is used in opposite directions, no edge by more than two",
              "an oriented surface is one the membrane, the contact's sign and the skin's normal "
              "agree on");
      if (!s.ok || region == nullptr) {
        r.verdict = Verdict::skipped;
        r.note = "the surface does not resolve";
      } else {
        u32 inward = 0;
        Vector<std::pair<u64, i32>> edges;
        for (u32 f = 0; f < s.face_count(); ++f) {
          const u32 cell = s.refs[2 * f];
          const u32 local = s.refs[2 * f + 1];
          const D3 a = d3(region->nodes[s.nodes[6 * f]]);
          const D3 b = d3(region->nodes[s.nodes[6 * f + 1]]);
          const D3 cc = d3(region->nodes[s.nodes[6 * f + 2]]);
          const D3 opposite = d3(region->nodes[region->cell(cell)[p2::k_face_opposite[local]]]);
          const D3 n = query::cross(b - a, cc - a);
          const D3 centre = (a + b + cc) * (1.0 / 3.0);
          if (!(query::dot(n, centre - opposite) > 0.0)) {
            if (inward == 0) fail(r, "face " + str(f) + " faces into its cell");
            ++inward;
          }
          for (u32 k = 0; k < 3; ++k) {
            const u32 p = s.nodes[6 * f + k];
            const u32 q = s.nodes[6 * f + (k + 1) % 3];
            edges.push_back({(u64{std::min(p, q)} << 32) | u64{std::max(p, q)}, p < q ? 1 : -1});
          }
        }
        std::sort(edges.begin(), edges.end());
        u32 boundary = 0;
        u32 bad = 0;
        for (u32 i = 0; i < edges.size();) {
          u32 j = i;
          i32 balance = 0;
          while (j < edges.size() && edges[j].first == edges[i].first) {
            balance += edges[j].second;
            ++j;
          }
          if (j - i == 1) {
            ++boundary;
          } else if (j - i != 2 || balance != 0) {
            if (bad == 0)
              fail(r, "edge (" + str(edges[i].first >> 32) + ", " +
                          str(edges[i].first & 0xffffffffu) + ") is used " + str(j - i) +
                          (j - i == 2 ? " times in the same direction" : " times"));
            ++bad;
          }
          i = j;
        }
        r.value.set("faces_facing_in", JsonValue(inward));
        r.value.set("boundary_edges", JsonValue(boundary));
        r.value.set("closed", JsonValue(boundary == 0));
      }
    }
    // ---- surface.embedding
    {
      ValidationRow& r =
          row("surface.embedding", subject, Severity::error,
              "at every state, no two chords of the surface that share no node intersect, no chord "
              "without area, and no two chords that share an edge folded onto each other (their "
              "normals' dot product above -0.99); " +
                  std::string(k_chordal),
              "a positive det J does not prove the surface does not pass through itself; incident "
              "chords meet legally along their shared nodes and are held to not folding");
      if (!s.ok || region == nullptr) {
        r.verdict = Verdict::skipped;
        r.note = "the surface does not resolve";
      } else {
        const Vector<u32> tri = chords(s);
        JsonValue per_state = JsonValue::object();
        for (const StateModel& state : region->states) {
          const Vector<D3> x = [&] {
            Vector<D3> out;
            for (const Vec3& p : state.nodes)
              out.push_back(d3(p));
            return out;
          }();
          query::TriangleBvh bvh;
          bvh.build(std::span<const D3>(x.data(), x.size()),
                    std::span<const u32>(tri.data(), tri.size()));
          Vector<std::pair<u32, u32>> candidates;
          bvh.candidate_pairs(bvh, 0.0, candidates);
          u32 crossings = 0;
          u32 folds = 0;
          u32 flat = 0;
          const auto shared = [&](u32 i, u32 j) {
            u32 n = 0;
            for (u32 a = 0; a < 3; ++a)
              for (u32 b = 0; b < 3; ++b)
                n += tri[3 * i + a] == tri[3 * j + b] ? 1u : 0u;
            return n;
          };
          for (u32 i = 0; i < tri.size() / 3; ++i) {
            const D3 n =
                query::cross(x[tri[3 * i + 1]] - x[tri[3 * i]], x[tri[3 * i + 2]] - x[tri[3 * i]]);
            if (!(query::length(n) > 0.0)) {
              if (flat == 0) fail(r, "state " + state.name + ", chord " + str(i) + " has no area");
              ++flat;
            }
          }
          for (const auto& [i, j] : candidates) {
            if (i >= j) continue;
            const u32 common = shared(i, j);
            const D3 a0 = x[tri[3 * i]], a1 = x[tri[3 * i + 1]], a2 = x[tri[3 * i + 2]];
            const D3 b0 = x[tri[3 * j]], b1 = x[tri[3 * j + 1]], b2 = x[tri[3 * j + 2]];
            if (common == 0) {
              if (query::triangles_cross(a0, a1, a2, b0, b1, b2)) {
                if (crossings == 0)
                  fail(r, "state " + state.name + ", chords " + str(i) + " and " + str(j) +
                              " (faces " + str(i / 4) + " and " + str(j / 4) + ") cross");
                ++crossings;
              }
            } else if (common == 2) {
              const D3 na = query::unit(query::cross(a1 - a0, a2 - a0));
              const D3 nb = query::unit(query::cross(b1 - b0, b2 - b0));
              if (query::dot(na, nb) < -0.99) {
                if (folds == 0)
                  fail(r, "state " + state.name + ", chords " + str(i) + " and " + str(j) +
                              " fold onto each other");
                ++folds;
              }
            }
          }
          JsonValue e = JsonValue::object();
          e.set("crossing_pairs", JsonValue(crossings));
          e.set("folded_pairs", JsonValue(folds));
          e.set("chords_without_area", JsonValue(flat));
          per_state.set(state.name, std::move(e));
        }
        r.value.set("chords", JsonValue(static_cast<u64>(tri.size() / 3)));
        r.value.set("states", std::move(per_state));
        r.value.set("representation", JsonValue(k_chordal));
      }
    }
  }
}

// ---- fat ----------------------------------------------------------------------------------------

void Rows::fat() {
  for (const ThicknessField& field : def_.thickness_fields) {
    const std::string subject = "thickness field " + field.name;
    const i32 ri = find_region(c_, field.region);
    const RegionModel* region = ri >= 0 ? &c_.regions[ri] : nullptr;
    // ---- fat.thickness_positive
    {
      ValidationRow& r =
          row("fat.thickness_positive", subject, Severity::error,
              "names a Volume region and a material surface of it; its thickness positive "
              "everywhere its law reaches: uniform-v1's mean above 0; cosine-modulation-v1's "
              "h_mean (1 - |d|) above 0 with |d| < 1 and L, W above 0",
              "thickness is a positive material-coordinate field; it is not softness");
      r.value.set("law", JsonValue(field.law));
      r.value.set("mean_thickness_mm", number(field.mean_thickness_m * k_mm));
      r.value.set("normal_convention", JsonValue(field.normal_convention));
      if (region == nullptr) {
        fail(r, "the field names region '" + field.region + "'");
      } else if (region->def->kind != RegionKind::Volume) {
        fail(r, "region " + field.region + " is not a Volume region");
      }
      const i32 si = find_surface(m_, field.surface);
      if (si < 0)
        fail(r, "the field names surface '" + field.surface + "'");
      else if (m_.surfaces[si].def->region != field.region)
        fail(r, "surface " + field.surface + " is not of region " + field.region);
      const f64 mean = field.mean_thickness_m;
      if (field.law == "uniform-v1") {
        if (!field.coefficients.is_null())
          fail(r, "uniform-v1 reads no coefficients and some are given");
        if (!(mean > 0.0) || !std::isfinite(mean))
          fail(r, "the mean thickness is " + std::to_string(mean));
        r.value.set("min_thickness_mm", number(mean * k_mm));
        r.value.set("max_thickness_mm", number(mean * k_mm));
      } else if (field.law == "cosine-modulation-v1") {
        CosineModulation modulation;
        schema::ReadContext ctx;
        ctx.options.ignore_unknown_fields = false;
        if (!schema::from_json(modulation, field.coefficients, ctx)) {
          fail(r, "the coefficients are not cosine-modulation-v1's: " +
                      ctx.diagnostics.front().path + ": " + ctx.diagnostics.front().message);
        } else {
          const f64 d = std::fabs(modulation.modulation);
          r.value.set("modulation", number(modulation.modulation));
          r.value.set("length_mm", number(modulation.length_m * k_mm));
          r.value.set("width_mm", number(modulation.width_m * k_mm));
          r.value.set("min_thickness_mm", number(mean * (1.0 - d) * k_mm));
          r.value.set("max_thickness_mm", number(mean * (1.0 + d) * k_mm));
          if (!(d < 1.0)) fail(r, "|d| is " + std::to_string(d) + ", not below 1");
          if (!(modulation.length_m > 0.0) || !(modulation.width_m > 0.0))
            fail(r, "L and W must be positive");
          if (!(mean * (1.0 - d) > 0.0)) fail(r, "the least thickness is not positive");
        }
      } else {
        r.verdict = Verdict::skipped;
        r.note = "law '" + field.law + "' is not one this build evaluates";
      }
    }
    // ---- fat.field_jacobian
    {
      ValidationRow& r =
          row("fat.field_jacobian", subject, Severity::error,
              "every cell of the field's region positive over the whole cell at the construction "
              "(a ten-node cell's det J proven by its Bernstein coefficients to depth 4): the "
              "offset as realized does not fold",
              "offsetting a curved surface can fold where h is positive, so a positive thickness "
              "proves nothing about the realized volume; the cells are what is realized");
      if (region == nullptr || !region->ok) {
        r.verdict = Verdict::skipped;
        r.note = "the field's region does not resolve";
      } else {
        u32 bad = 0;
        f64 least = std::numeric_limits<f64>::infinity();
        for (u32 cell = 0; cell < region->cell_count(); ++cell) {
          if (region->quadratic()) {
            D3 x[10];
            region->gather(cell, region->nodes, x);
            const cells::JacobianCheck j = cells::quadratic_jacobian(x, k_jacobian_depth);
            if (j.upper > 0.0) least = std::min(least, j.lower / j.upper);
            if (j.result != cells::JacobianCheck::Result::positive) {
              if (bad == 0) fail(r, "cell " + str(cell) + " is not proven positive");
              ++bad;
            }
          } else if (!(region->cell_volume(cell, region->nodes) > 0.0)) {
            if (bad == 0) fail(r, "tetrahedron " + str(cell) + " is not positive");
            ++bad;
          }
        }
        r.value.set("cells", JsonValue(region->cell_count()));
        r.value.set("not_positive", JsonValue(bad));
        if (region->quadratic()) r.value.set("jacobian_ratio_min", number(least));
      }
    }
    // ---- fat.mass_ledger
    {
      ValidationRow& r =
          row("fat.mass_ledger", subject, Severity::error,
              "the region's reference volume and mass (every cell's exact volume at the "
              "construction, density times that volume) within 1e-6 of the field's declared "
              "ledger, relative, where it declares one; each depot partition's depots summing to "
              "the region's volume and mass within 1e-9, and each depot within 1e-6 of what it "
              "declares; the analytic identity h_mean times the reference surface's area reported "
              "beside it, never substituted",
              "a depot partitions or redistributes the field's volume and never adds to it; mass "
              "is stored density times reference volume, never a deformed cell's");
      if (region == nullptr || !region->ok) {
        r.verdict = Verdict::skipped;
        r.note = "the field's region does not resolve";
      } else {
        // Each cell's density, the phases mixed by fraction as region.materials mixes them.
        const Region& rd = *region->def;
        Vector<Vector<f32>> fractions(static_cast<u32>(rd.phases.size()));
        Vector<std::string> ignored;
        const Blocks blocks{c_.file, ignored};
        for (u32 p = 1; p < rd.phases.size(); ++p)
          blocks.read(rd.phases[p].fraction, BlockKind::PhaseFraction, "phase", fractions[p]);
        Vector<f64> volume(region->cell_count());
        Vector<f64> density(region->cell_count(), 0.0);
        f64 total_volume = 0.0;
        f64 total_mass = 0.0;
        for (u32 cell = 0; cell < region->cell_count(); ++cell) {
          volume[cell] = region->cell_volume(cell, region->nodes);
          f64 rest = 1.0;
          for (u32 p = 1; p < rd.phases.size(); ++p) {
            const f64 f = fractions[p].size() > cell ? static_cast<f64>(fractions[p][cell]) : 0.0;
            density[cell] += f * rd.phases[p].material.density_kg_m3;
            rest -= f;
          }
          if (!rd.phases.empty()) density[cell] += rest * rd.phases[0].material.density_kg_m3;
          total_volume += volume[cell];
          total_mass += density[cell] * volume[cell];
        }
        r.value.set("volume_ml", number(total_volume * k_ml));
        r.value.set("mass_kg", number(total_mass));
        const auto agree = [](f64 a, f64 b, f64 tolerance) {
          return std::isfinite(a) && std::fabs(a - b) <= tolerance * std::fabs(b);
        };
        if (field.declared_volume_m3.has_value()) {
          r.value.set("declared_volume_ml", number(*field.declared_volume_m3 * k_ml));
          if (!agree(*field.declared_volume_m3, total_volume, 1.0e-6))
            fail(r, "the declared volume departs from the cells' by " +
                        relative(*field.declared_volume_m3, total_volume));
        }
        if (field.declared_mass_kg.has_value()) {
          r.value.set("declared_mass_kg", number(*field.declared_mass_kg));
          if (!agree(*field.declared_mass_kg, total_mass, 1.0e-6))
            fail(r, "the declared mass departs from the cells' by " +
                        relative(*field.declared_mass_kg, total_mass));
        }
        // The analytic identity, on the reference surface's area.
        const i32 si = find_surface(m_, field.surface);
        if (si >= 0 && m_.surfaces[si].ok) {
          f64 area = 0.0;
          for (u32 f = 0; f < m_.surfaces[si].face_count(); ++f) {
            D3 x[6];
            face_points(m_.surfaces[si], f, region->nodes, x);
            area += p2::area(x);
          }
          const f64 analytic = field.mean_thickness_m * area;
          r.value.set("analytic_volume_ml", number(analytic * k_ml));
          r.value.set("analytic_note",
                      JsonValue("h_mean times the reference surface's area: the field's own "
                                "identity, exact on a flat reference; reported, never substituted "
                                "for the cells' volume"));
          r.value.set("cells_minus_analytic_ml", number((total_volume - analytic) * k_ml));
        }
        JsonValue partitions = JsonValue::object();
        for (const PartitionModel& pm : m_.partitions) {
          if (pm.def->field != field.name) continue;
          JsonValue depots = JsonValue::object();
          f64 sum_volume = 0.0;
          f64 sum_mass = 0.0;
          for (u32 di = 0; di < pm.def->depots.size(); ++di) {
            const Depot& depot = pm.def->depots[di];
            const DepotModel& dm = pm.depots[di];
            f64 v = 0.0;
            f64 mass = 0.0;
            if (dm.ok)
              for (u32 cell = 0; cell < region->cell_count(); ++cell) {
                v += static_cast<f64>(dm.shares[cell]) * volume[cell];
                mass += static_cast<f64>(dm.shares[cell]) * density[cell] * volume[cell];
              }
            sum_volume += v;
            sum_mass += mass;
            JsonValue e = JsonValue::object();
            e.set("volume_ml", number(v * k_ml));
            e.set("mass_kg", number(mass));
            if (depot.declared_volume_m3.has_value() &&
                !agree(*depot.declared_volume_m3, v, 1.0e-6))
              fail(r, "depot " + depot.name + " declares a volume that departs by " +
                          relative(*depot.declared_volume_m3, v));
            if (depot.declared_mass_kg.has_value() && !agree(*depot.declared_mass_kg, mass, 1.0e-6))
              fail(r, "depot " + depot.name + " declares a mass that departs by " +
                          relative(*depot.declared_mass_kg, mass));
            depots.set(depot.name, std::move(e));
          }
          JsonValue e = JsonValue::object();
          e.set("depots", std::move(depots));
          e.set("sum_volume_ml", number(sum_volume * k_ml));
          e.set("sum_mass_kg", number(sum_mass));
          partitions.set(pm.def->name, std::move(e));
          if (!agree(sum_volume, total_volume, 1.0e-9))
            fail(r, "partition " + pm.def->name + "'s depots sum to " +
                        relative(sum_volume, total_volume) +
                        " of the region's volume, relative: "
                        "a depot adds or loses volume");
          if (!agree(sum_mass, total_mass, 1.0e-9))
            fail(r, "partition " + pm.def->name + "'s depots' mass departs from the region's by " +
                        relative(sum_mass, total_mass));
        }
        r.value.set("partitions", std::move(partitions));
      }
    }
  }

  // ---- fat.depot_partition
  for (u32 pi = 0; pi < m_.partitions.size(); ++pi) {
    const PartitionModel& pm = m_.partitions[pi];
    const DepotPartition& p = *pm.def;
    ValidationRow& r =
        row("fat.depot_partition", "depot partition " + p.name, Severity::error,
            "names a thickness field, which no other partition does; one or more depots, named "
            "once each, each a share of every cell of the field's region in [0, 1]; in every cell "
            "the shares summing to one within 1e-6",
            "depots divide one field's volume: a share that leaves a cell short or over is volume "
            "lost or added");
    const ThicknessField* field = nullptr;
    for (const ThicknessField& f : def_.thickness_fields)
      if (f.name == p.field) field = &f;
    if (field == nullptr) {
      fail(r, "the partition names field '" + p.field + "'");
      continue;
    }
    for (u32 qi = 0; qi < pi; ++qi)
      if (m_.partitions[qi].def->field == p.field)
        fail(r, "field " + p.field + " is partitioned by " + m_.partitions[qi].def->name + " too");
    if (p.depots.empty()) fail(r, "the partition has no depots");
    const i32 ri = find_region(c_, field->region);
    const u32 cells = ri >= 0 ? c_.regions[ri].cell_count() : 0;
    for (u32 di = 0; di < p.depots.size(); ++di) {
      for (u32 dj = 0; dj < di; ++dj)
        if (p.depots[dj].name == p.depots[di].name)
          fail(r, "depot " + p.depots[di].name + " is named twice");
      if (!pm.depots[di].ok) fail(r, "depot " + p.depots[di].name + "'s shares do not resolve");
    }
    Vector<f64> sum(cells, 0.0);
    u32 out_of_range = 0;
    for (u32 di = 0; di < p.depots.size(); ++di) {
      if (!pm.depots[di].ok) continue;
      for (u32 cell = 0; cell < cells; ++cell) {
        const f32 share = pm.depots[di].shares[cell];
        if (!(share >= 0.0f && share <= 1.0f)) {
          if (out_of_range == 0)
            fail(r, "depot " + p.depots[di].name + ", cell " + str(cell) + " has share " +
                        std::to_string(share));
          ++out_of_range;
        }
        sum[cell] += static_cast<f64>(share);
      }
    }
    u32 unbalanced = 0;
    f64 worst = 0.0;
    for (u32 cell = 0; cell < cells; ++cell) {
      const f64 d = std::fabs(sum[cell] - 1.0);
      worst = std::max(worst, d);
      if (!(d <= k_unit)) {
        if (unbalanced == 0)
          fail(r, "cell " + str(cell) + "'s shares sum to " + std::to_string(sum[cell]));
        ++unbalanced;
      }
    }
    r.value.set("depots", JsonValue(static_cast<u64>(p.depots.size())));
    r.value.set("cells", JsonValue(cells));
    r.value.set("cells_not_summing_to_one", JsonValue(unbalanced));
    r.value.set("largest_departure", number(worst));
  }
}

// ---- the mechanical skin ------------------------------------------------------------------------

void Rows::skins() {
  for (const SkinModel& skin : m_.skins) {
    const MaterialSkin& s = *skin.def;
    const std::string subject = "skin " + s.name;
    const SurfaceModel* surface = skin.surface >= 0 ? &m_.surfaces[skin.surface] : nullptr;
    const RegionModel* region =
        surface != nullptr && surface->region >= 0 ? &c_.regions[surface->region] : nullptr;
    // ---- skin.material_binding
    {
      ValidationRow& r =
          row("skin.material_binding", subject, Severity::error,
              "names a material surface; declares its zero-offset coincidence with it; its sampled "
              "ids strictly ascending, one point of the surface each, the face in range, "
              "barycentrics in [0, 1] summing to one within 1e-6; with an observation, every id "
              "one of its vertices and at the region's reference state the surface at the point "
              "within 0.1 um of the observation there",
              "the mechanical skin is the material boundary itself, bound by material "
              "coordinates; the Loop render binding is another representation, checked by its own "
              "rows");
      r.value.set("samples", JsonValue(static_cast<u64>(skin.ids.size())));
      if (surface == nullptr) fail(r, "the skin names surface '" + s.surface + "'");
      if (!s.coincident)
        fail(r,
             "the skin does not declare its coincidence with the surface: version 1 reads only "
             "the boundary itself, at zero offset, and never assumes it");
      if (!skin.ok || surface == nullptr || !surface->ok || region == nullptr) {
        if (surface != nullptr) fail(r, "the samples or the surface do not resolve");
      } else {
        if (!strictly_ascending(skin.ids)) fail(r, "the sampled ids are not strictly ascending");
        u32 bad = 0;
        for (u32 i = 0; i < skin.points.size(); ++i) {
          const SurfacePoint& p = skin.points[i];
          const f64 sum =
              static_cast<f64>(p.b[0]) + static_cast<f64>(p.b[1]) + static_cast<f64>(p.b[2]);
          bool ok = p.face < surface->face_count() && std::fabs(sum - 1.0) <= k_unit;
          for (const f32 b : p.b)
            ok = ok && std::isfinite(b) && b >= -static_cast<f32>(k_unit) &&
                 b <= 1.0f + static_cast<f32>(k_unit);
          if (!ok) {
            if (bad == 0) fail(r, "id " + str(skin.ids[i]) + " has no valid point of the surface");
            ++bad;
          }
        }
        r.value.set("invalid_points", JsonValue(bad));
        if (c_.observation != nullptr && bad == 0) {
          const StateModel& reference = region->states[region->reference];
          f64 worst = 0.0;
          u32 worst_id = 0;
          u32 outside = 0;
          for (u32 i = 0; i < skin.ids.size(); ++i) {
            if (skin.ids[i] >= c_.observation->size()) {
              if (outside == 0) fail(r, "id " + str(skin.ids[i]) + " is no observation vertex");
              ++outside;
              continue;
            }
            D3 x[6];
            face_points(*surface, skin.points[i].face, reference.nodes, x);
            const f64 l[3] = {static_cast<f64>(skin.points[i].b[0]),
                              static_cast<f64>(skin.points[i].b[1]),
                              static_cast<f64>(skin.points[i].b[2])};
            const f64 gap = query::length(p2::evaluate(x, l) - d3((*c_.observation)[skin.ids[i]]));
            if (gap > worst) {
              worst = gap;
              worst_id = skin.ids[i];
            }
          }
          r.value.set("reference_state", JsonValue(reference.name));
          r.value.set("identity_gap_max_um", number(worst * 1.0e6));
          if (!(worst <= k_position_m))
            fail(r, "id " + str(worst_id) + " is " + mm(worst) +
                        " from the observation at the reference");
        } else {
          r.value.set("observation", JsonValue("none: the sampled ids are the skin's own"));
        }
      }
    }
    // ---- skin.rest_metric
    {
      ValidationRow& r =
          row("skin.rest_metric", subject, Severity::error,
              "a membrane law named, E.t above 0 N/m, -1 < nu < 0.5, a declared thickness above 0; "
              "its natural metric (the rest configuration named, or the construction) regular on "
              "every face: the area element above 0 at each face's corners, edge midpoints and "
              "seven interior points",
              "the skin's rest area, its stiffness and the fat's thickness are independent inputs: "
              "a larger envelope is a different metric, not a lower modulus");
      r.value.set("law", JsonValue(s.law));
      r.value.set("stiffness_n_per_m", number(s.stiffness_n_per_m));
      r.value.set("poisson", number(s.poisson));
      if (s.law.empty()) fail(r, "no membrane law is named");
      if (!(s.stiffness_n_per_m > 0.0) || !std::isfinite(s.stiffness_n_per_m))
        fail(r, "E.t is " + std::to_string(s.stiffness_n_per_m) + " N/m");
      if (!(s.poisson > -1.0 && s.poisson < 0.5))
        fail(r, "Poisson's ratio is " + std::to_string(s.poisson));
      if (s.thickness_m.has_value() && !(*s.thickness_m > 0.0))
        fail(r, "the declared thickness is not positive");
      if (!skin.ok || surface == nullptr || !surface->ok || region == nullptr) {
        fail(r, "the skin's surface or rest configuration does not resolve");
      } else {
        f64 natural = 0.0;
        f64 built = 0.0;
        f64 least = std::numeric_limits<f64>::infinity();
        u32 least_face = 0;
        for (u32 f = 0; f < surface->face_count(); ++f) {
          D3 x[6];
          face_points(*surface, f, skin.rest, x);
          natural += p2::area(x);
          const f64 metric = p2::least_metric(x);
          if (metric < least) {
            least = metric;
            least_face = f;
          }
          face_points(*surface, f, region->nodes, x);
          built += p2::area(x);
        }
        r.value.set("rest", JsonValue(s.rest_nodes.empty() ? "the construction" : s.rest_nodes));
        r.value.set("natural_area_mm2", number(natural * k_mm * k_mm));
        r.value.set("construction_area_mm2", number(built * k_mm * k_mm));
        r.value.set("natural_over_construction", number(natural / built));
        r.value.set("least_area_element", number(least));
        if (!(least > 0.0)) fail(r, "face " + str(least_face) + "'s natural metric is degenerate");
      }
    }
    // ---- skin.energy_transfer
    {
      ValidationRow& r =
          row("skin.energy_transfer", subject, Severity::info,
              "reported: the membrane law, its parameters, E.t, nu, the faces and the natural "
              "area; not evaluated here",
              "the membrane energy on the six-node faces, its compression relaxation and the "
              "transfer of its gradient through the shape functions are the certificate's: this "
              "build carries the law and evaluates none of it, and never fabricates a result");
      r.value.set("law", JsonValue(s.law));
      r.value.set("law_parameters", s.law_parameters);
      r.value.set("stiffness_n_per_m", number(s.stiffness_n_per_m));
      r.value.set("poisson", number(s.poisson));
      r.value.set("faces",
                  JsonValue(static_cast<u64>(surface != nullptr ? surface->face_count() : 0u)));
      r.value.set("authority", JsonValue("the authoring certificate"));
    }
  }
}

// ---- contact ------------------------------------------------------------------------------------

// One pair evaluated at one state, on chords.
struct PairState {
  std::string state;
  u32 eligible_within_reach = 0;
  f64 least_nonlocal = std::numeric_limits<f64>::infinity();
  f64 least_local = std::numeric_limits<f64>::infinity();
  u32 violations = 0;
  u32 prescribed_violations = 0;  // both sides prescribed: infeasible
  std::string witness;
  std::string prescribed_witness;
  f64 deviation_a = 0.0;
  f64 deviation_b = 0.0;
};

struct PairResult {
  bool evaluated = false;
  std::string why_not;
  Vector<PairState> states;
};

void Rows::contact() {
  // ---- contact.pairs and contact.exclusions, and the owned interfaces
  const auto policy_of = [&](const ContactPair& p) -> const ContactPolicy* {
    for (const ContactPolicy& policy : def_.contact_policies)
      if (policy.name == p.policy) return &policy;
    return nullptr;
  };
  const auto side_ok = [&](const ContactSide& side, std::string& why) {
    if (side.kind == ContactSurfaceKind::MaterialSurface) {
      const i32 si = find_surface(m_, side.name);
      if (si < 0) {
        why = "surface '" + side.name + "' is not a material surface";
        return false;
      }
      if (m_.surfaces[si].def->region != side.owner) {
        why = "surface " + side.name + " is owned by region " + m_.surfaces[si].def->region +
              ", not '" + side.owner + "'";
        return false;
      }
      if (!m_.surfaces[si].ok) {
        why = "surface " + side.name + " does not resolve";
        return false;
      }
      return true;
    }
    const FrameModel* frame = find_frame(c_, side.name);
    if (frame == nullptr || !frame->ok) {
      why = "frame '" + side.name + "' does not resolve";
      return false;
    }
    if (side.owner != side.name) {
      why = "frame " + side.name + " is its own owner, not '" + side.owner + "'";
      return false;
    }
    return true;
  };

  for (const PairModel& pm : m_.pairs) {
    const ContactPair& p = *pm.def;
    const ContactPolicy* policy = policy_of(p);
    ValidationRow& r = row(
        "contact.pairs", "contact pair " + p.name, Severity::error,
        "a unique name; both sides resolve (a material surface owned by the region named, or a "
        "frame, its own owner), at least one a surface; self contact exactly when both sides "
        "are one surface; an offset of 0 or more; a policy naming a potential and a proxy rule, "
        "an activation distance, a proxy deviation target, a depth and work caps above 0, and a "
        "locality relation when a self-contact pair's offset is positive",
        "one contact contract for self, region and frame contact: owners, eligibility, "
        "offsets, the potential and the certificate's tolerances are declared, never chosen "
        "during a run");
    r.value.set("offset_mm", number(p.offset_m * k_mm));
    r.value.set("self_contact", JsonValue(p.self_contact));
    r.value.set(
        "eligibility",
        JsonValue(p.eligibility == ContactEligibility::Unilateral ? "Unilateral" : "Attached"));
    for (const PairModel& other : m_.pairs)
      if (&other != &pm && other.def->name == p.name) {
        fail(r, "the name is used twice");
        break;
      }
    std::string why;
    if (!side_ok(p.a, why)) fail(r, "side a: " + why);
    if (!side_ok(p.b, why)) fail(r, "side b: " + why);
    if (p.a.kind != ContactSurfaceKind::MaterialSurface &&
        p.b.kind != ContactSurfaceKind::MaterialSurface)
      fail(r, "both sides are frames: two rigid bodies are not a tissue contact");
    const bool same = p.a.kind == ContactSurfaceKind::MaterialSurface &&
                      p.b.kind == ContactSurfaceKind::MaterialSurface && p.a.name == p.b.name;
    if (same != p.self_contact)
      fail(r, p.self_contact ? "a self-contact pair names two surfaces"
                             : "one surface paired with itself is self contact, and the pair does "
                               "not say so");
    if (!(p.offset_m >= 0.0) || !std::isfinite(p.offset_m))
      fail(r, "the offset is " + std::to_string(p.offset_m));
    if (policy == nullptr) {
      fail(r, "the pair names policy '" + p.policy + "'");
    } else {
      r.value.set("policy", JsonValue(policy->name));
      r.value.set("potential", JsonValue(policy->potential));
      r.value.set("activation_mm", number(policy->activation_m * k_mm));
      r.value.set("proxy_rule", JsonValue(policy->proxy_rule));
      if (policy->potential.empty()) fail(r, "the policy names no potential");
      if (policy->proxy_rule.empty()) fail(r, "the policy names no proxy rule");
      if (!(policy->activation_m > 0.0) || !std::isfinite(policy->activation_m))
        fail(r, "the policy's activation distance is not positive");
      if (!(policy->proxy_epsilon_m > 0.0) || policy->max_depth == 0 ||
          policy->max_subtriangles == 0 || policy->max_feature_tests == 0)
        fail(r, "the policy's certificate tolerances and work caps are not all declared");
      if (p.self_contact && p.offset_m > 0.0 &&
          !(policy->locality_m.has_value() && *policy->locality_m > 0.0))
        fail(r,
             "a self-contact pair with a positive offset and no locality relation: arbitrarily "
             "close points of one surface cannot all be the offset apart, and CONTACT.md makes "
             "the relation an execution blocker");
      r.value.set("locality_mm", policy->locality_m.has_value() ? number(*policy->locality_m * k_mm)
                                                                : JsonValue::null());
    }
  }

  for (const PairModel& pm : m_.pairs) {
    const ContactPair& p = *pm.def;
    ValidationRow& r =
        row("contact.exclusions", "contact pair " + p.name, Severity::error,
            "every exclusion names exact faces: its a faces faces of side a's surface, its b faces "
            "as many faces of side b's (none against a frame); an incident one pairs faces of one "
            "region that share a node; an attachment one names an essential attachment whose "
            "patch is on side a's surface and whose target is side b's frame, and only faces of "
            "that patch",
            "an exclusion that masks a nonincident feature because it is near a fixed node or in "
            "a neighbouring region hides the overlap it exempts");
    const i32 sa =
        p.a.kind == ContactSurfaceKind::MaterialSurface ? find_surface(m_, p.a.name) : -1;
    const i32 sb =
        p.b.kind == ContactSurfaceKind::MaterialSurface ? find_surface(m_, p.b.name) : -1;
    u32 excluded = 0;
    for (u32 ei = 0; ei < p.exclusions.size(); ++ei) {
      const ContactExclusion& e = p.exclusions[ei];
      const ExclusionModel& em = pm.exclusions[ei];
      const std::string which = "exclusion " + str(ei);
      if (!em.ok || sa < 0 || !m_.surfaces[sa].ok) {
        fail(r, which + " does not resolve");
        continue;
      }
      const SurfaceModel& a = m_.surfaces[sa];
      const bool against_frame = p.b.kind == ContactSurfaceKind::Frame;
      if (against_frame && !em.b.empty()) fail(r, which + " names b faces against a frame");
      if (!against_frame && em.b.size() != em.a.size())
        fail(r, which + " pairs " + str(em.a.size() / 2) + " a faces with " + str(em.b.size() / 2) +
                    " b faces");
      excluded += static_cast<u32>(em.a.size() / 2);
      for (u32 f = 0; f + 1 < em.a.size(); f += 2) {
        const u32 fa = a.find(em.a[f], em.a[f + 1]);
        if (fa == ~0u) {
          fail(r, which + ": face (" + str(em.a[f]) + ", " + str(em.a[f + 1]) +
                      ") is not a face of surface " + a.def->name);
          continue;
        }
        if (e.reason == ExclusionReason::Incident) {
          if (against_frame || sb < 0 || f + 1 >= em.b.size()) {
            fail(r, which + " is incident and names no face of a surface on side b");
            continue;
          }
          const SurfaceModel& b = m_.surfaces[sb];
          const u32 fb = b.find(em.b[f], em.b[f + 1]);
          if (fb == ~0u) {
            fail(r, which + ": face (" + str(em.b[f]) + ", " + str(em.b[f + 1]) +
                        ") is not a face of surface " + b.def->name);
            continue;
          }
          bool share = a.region == b.region;
          if (share) {
            share = false;
            for (u32 i = 0; i < 6; ++i)
              for (u32 j = 0; j < 6; ++j)
                share = share || a.nodes[6 * fa + i] == b.nodes[6 * fb + j];
          }
          if (!share)
            fail(r, which + ": faces " + str(fa) + " and " + str(fb) +
                        " share no node, so they are not incident: the exclusion masks a "
                        "nonincident pair");
        } else if (e.reason == ExclusionReason::Attachment) {
          const Attachment* owner = nullptr;
          u32 owner_index = 0;
          for (u32 ai = 0; ai < def_.attachments.size(); ++ai)
            if (def_.attachments[ai].name == e.attachment) {
              owner = &def_.attachments[ai];
              owner_index = ai;
            }
          if (owner == nullptr || owner->enforcement != AttachmentEnforcement::Essential ||
              owner->patch_surface != a.def->name || !against_frame ||
              owner->target_kind != TargetKind::Frame || owner->target != p.b.name) {
            fail(r, which + " names attachment '" + e.attachment +
                        "', which is not an essential attachment of side a's surface to side b's "
                        "frame");
            continue;
          }
          const PatchModel& patch = m_.patches[owner_index];
          bool in_patch = false;
          for (u32 k = 0; k + 1 < patch.refs.size(); k += 2)
            in_patch = in_patch || (patch.refs[k] == em.a[f] && patch.refs[k + 1] == em.a[f + 1]);
          if (!in_patch)
            fail(r, which + ": face " + str(fa) + " is not in attachment " + owner->name +
                        "'s patch: an owned interface is the patch alone");
        } else {
          fail(r, which + " gives a reason this build does not know");
        }
      }
    }
    r.value.set("exclusions", JsonValue(static_cast<u64>(p.exclusions.size())));
    r.value.set("excluded_faces", JsonValue(excluded));
  }

  // ---- attachment.contact_compatibility
  for (u32 ai = 0; ai < def_.attachments.size(); ++ai) {
    const Attachment& a = def_.attachments[ai];
    if (a.enforcement != AttachmentEnforcement::Essential || a.target_kind != TargetKind::Frame)
      continue;
    if (a.interface == AttachmentInterface::Separated) {
      separated_contact(ai);
      continue;
    }
    if (a.patch.empty()) continue;
    ValidationRow& r =
        row("attachment.contact_compatibility", "attachment " + a.name, Severity::error,
            "every unilateral contact pair between the patch's surface and the attachment's frame "
            "excludes the whole patch against that frame by an exclusion naming this attachment",
            "an owned coincidence is not a unilateral barrier: the patch against its own frame is "
            "the attachment's, and every other pairing stays active");
    const PatchModel& patch = m_.patches[ai];
    u32 pairs = 0;
    for (const PairModel& pm : m_.pairs) {
      const ContactPair& p = *pm.def;
      if (p.eligibility != ContactEligibility::Unilateral) continue;
      if (!(p.a.kind == ContactSurfaceKind::MaterialSurface && p.a.name == a.patch_surface &&
            p.b.kind == ContactSurfaceKind::Frame && p.b.name == a.target))
        continue;
      ++pairs;
      u32 uncovered = 0;
      for (u32 k = 0; k + 1 < patch.refs.size(); k += 2) {
        bool covered = false;
        for (u32 ei = 0; ei < p.exclusions.size(); ++ei) {
          if (p.exclusions[ei].reason != ExclusionReason::Attachment ||
              p.exclusions[ei].attachment != a.name)
            continue;
          const ExclusionModel& em = pm.exclusions[ei];
          for (u32 f = 0; f + 1 < em.a.size(); f += 2)
            covered = covered || (em.a[f] == patch.refs[k] && em.a[f + 1] == patch.refs[k + 1]);
        }
        if (!covered) ++uncovered;
      }
      if (uncovered > 0)
        fail(r, "pair " + p.name + " leaves " + str(uncovered) +
                    " patch faces active against their own frame");
    }
    r.value.set("pairs_between_patch_and_frame", JsonValue(pairs));
    r.value.set("patch_faces", JsonValue(static_cast<u64>(patch.refs.size() / 2)));
  }

  target_gaps();

  // ---- the chordal clearance, per unilateral pair and state
  for (const PairModel& pm : m_.pairs) {
    const ContactPair& p = *pm.def;
    if (p.eligibility != ContactEligibility::Unilateral) continue;
    const ContactPolicy* policy = policy_of(p);
    PairResult result;
    const i32 sa =
        p.a.kind == ContactSurfaceKind::MaterialSurface ? find_surface(m_, p.a.name) : -1;
    const i32 sb =
        p.b.kind == ContactSurfaceKind::MaterialSurface ? find_surface(m_, p.b.name) : -1;
    const FrameModel* frame =
        p.b.kind == ContactSurfaceKind::Frame ? find_frame(c_, p.b.name) : nullptr;
    const bool resolvable = sa >= 0 && m_.surfaces[sa].ok && policy != nullptr &&
                            (sb >= 0 ? m_.surfaces[sb].ok : frame != nullptr && frame->ok);
    if (!resolvable) {
      result.why_not = "the pair's sides or its policy do not resolve (contact.pairs)";
    } else {
      result.evaluated = true;
      const SurfaceModel& a = m_.surfaces[sa];
      const RegionModel& ra = c_.regions[a.region];
      const SurfaceModel* b = sb >= 0 ? &m_.surfaces[sb] : nullptr;
      const RegionModel* rb = b != nullptr ? &c_.regions[b->region] : nullptr;
      const bool same_region = rb == &ra;
      const f64 offset = p.offset_m;
      const f64 reach = offset + policy->activation_m + 1.0e-9;
      const f64 locality = policy->locality_m.has_value() ? *policy->locality_m : 0.0;
      // Excluded face pairs, as (a face, b face) with b face ~0u for the whole frame.
      Vector<std::pair<u32, u32>> excluded;
      for (u32 ei = 0; ei < p.exclusions.size(); ++ei) {
        const ExclusionModel& em = pm.exclusions[ei];
        if (!em.ok) continue;
        for (u32 f = 0; f + 1 < em.a.size(); f += 2) {
          const u32 fa = a.find(em.a[f], em.a[f + 1]);
          u32 fb = ~0u;
          if (b != nullptr && f + 1 < em.b.size()) fb = b->find(em.b[f], em.b[f + 1]);
          if (fa == ~0u) continue;
          excluded.push_back({fa, fb});
          if (p.self_contact && fb != ~0u) excluded.push_back({fb, fa});
        }
      }
      std::sort(excluded.begin(), excluded.end());
      const auto is_excluded = [&](u32 fa, u32 fb) {
        return std::binary_search(excluded.begin(), excluded.end(), std::pair<u32, u32>{fa, fb}) ||
               std::binary_search(excluded.begin(), excluded.end(), std::pair<u32, u32>{fa, ~0u});
      };
      const Vector<u32> tri_a = chords(a);
      const Vector<u32> tri_b = b != nullptr ? chords(*b) : Vector<u32>{};
      const Vector<u8> held_a = held_nodes(ra);
      const Vector<u8> held_b = rb != nullptr ? held_nodes(*rb) : Vector<u8>{};
      const Vector<D3> reference_a = [&] {
        Vector<D3> out;
        for (const Vec3& v : ra.nodes)
          out.push_back(d3(v));
        return out;
      }();
      for (const StateModel& state : ra.states) {
        const StateModel* other = rb != nullptr ? rb->state(state.name) : nullptr;
        if (rb != nullptr && other == nullptr) continue;  // a state only one region has
        PairState ps;
        ps.state = state.name;
        Vector<D3> xa;
        for (const Vec3& v : state.nodes)
          xa.push_back(d3(v));
        Vector<D3> xb;
        const Vector<u32>* tb = &tri_b;
        if (other != nullptr) {
          for (const Vec3& v : other->nodes)
            xb.push_back(d3(v));
        } else {
          const RigidTransform at = frame_at(frame->def->name, state.name);
          for (const D3& v : frame->points)
            xb.push_back(at.apply(v));
          tb = &frame->triangles;
        }
        for (u32 f = 0; f < a.face_count(); ++f) {
          D3 x[6];
          face_points(a, f, state.nodes, x);
          ps.deviation_a = std::max(ps.deviation_a, p2::chord_deviation(x));
        }
        if (b != nullptr && other != nullptr)
          for (u32 f = 0; f < b->face_count(); ++f) {
            D3 x[6];
            face_points(*b, f, other->nodes, x);
            ps.deviation_b = std::max(ps.deviation_b, p2::chord_deviation(x));
          }
        query::TriangleBvh bvh_a;
        bvh_a.build(std::span<const D3>(xa.data(), xa.size()),
                    std::span<const u32>(tri_a.data(), tri_a.size()));
        query::TriangleBvh bvh_b;
        bvh_b.build(std::span<const D3>(xb.data(), xb.size()),
                    std::span<const u32>(tb->data(), tb->size()));
        Vector<std::pair<u32, u32>> candidates;
        bvh_a.candidate_pairs(bvh_b, reach, candidates);
        for (const auto& [i, j] : candidates) {
          if (p.self_contact && i >= j) continue;
          const u32 fa = i / 4;
          const u32 fb = b != nullptr ? j / 4 : ~0u;
          const u32* va = tri_a.data() + 3 * i;
          const u32* vb = tb->data() + 3 * j;
          // Incident chords of one region meet legally along their shared nodes.
          if (same_region) {
            bool incident = false;
            for (u32 k = 0; k < 3; ++k)
              for (u32 l = 0; l < 3; ++l)
                incident = incident || va[k] == vb[l];
            if (incident) continue;
          }
          if (is_excluded(fa, fb)) continue;
          const D3 a0 = xa[va[0]], a1 = xa[va[1]], a2 = xa[va[2]];
          const D3 b0 = xb[vb[0]], b1 = xb[vb[1]], b2 = xb[vb[2]];
          const f64 d = query::triangle_distance(a0, a1, a2, b0, b1, b2);
          // Within one region, features closer than the locality relation in the canonical
          // reference are local: held to nonintersection only.
          bool local = false;
          if (same_region && locality > 0.0) {
            const f64 at_reference = query::triangle_distance(
                reference_a[va[0]], reference_a[va[1]], reference_a[va[2]], reference_a[vb[0]],
                reference_a[vb[1]], reference_a[vb[2]]);
            local = at_reference < locality;
          }
          ++ps.eligible_within_reach;
          bool violated = false;
          if (local || offset == 0.0) {
            if (local)
              ps.least_local = std::min(ps.least_local, d);
            else
              ps.least_nonlocal = std::min(ps.least_nonlocal, d);
            violated = query::triangles_cross(a0, a1, a2, b0, b1, b2);
          } else {
            ps.least_nonlocal = std::min(ps.least_nonlocal, d);
            violated = !(d > offset);
          }
          if (!violated) continue;
          char buffer[200];
          std::snprintf(buffer, sizeof(buffer), "state %s, faces %u and %u%s at %.4g mm",
                        state.name.c_str(), fa, fb == ~0u ? j : fb,
                        fb == ~0u ? " (frame triangle)" : "", d * k_mm);
          if (ps.violations == 0) ps.witness = buffer;
          ++ps.violations;
          // Prescribed on both sides: every node of the a chord held, and a frame or held b chord.
          bool prescribed = held_a[va[0]] != 0 && held_a[va[1]] != 0 && held_a[va[2]] != 0;
          if (b != nullptr)
            prescribed =
                prescribed && held_b[vb[0]] != 0 && held_b[vb[1]] != 0 && held_b[vb[2]] != 0;
          if (prescribed) {
            if (ps.prescribed_violations == 0) ps.prescribed_witness = buffer;
            ++ps.prescribed_violations;
          }
        }
        result.states.push_back(std::move(ps));
      }
    }

    // ---- contact.curved_clearance
    {
      ValidationRow& r = row(
          "contact.curved_clearance", "contact pair " + p.name, Severity::error,
          "at every state, every eligible feature pair strictly above its required separation: "
          "the pair's offset (" +
              mm(p.offset_m) +
              "), or, within one region, nonintersection "
              "for features closer than the locality relation in the canonical reference; incident "
              "chords and exact exclusions exempt; the chords' deviation from the curved faces "
              "reported apart, never added; " +
              std::string(k_chordal),
          "every eligible pair starts and stays strictly above its offset, equality being the "
          "barrier's boundary; the engine tests the proxy it can and names the certificate as "
          "the authority for the curved surfaces");
      if (!result.evaluated) {
        r.verdict = Verdict::skipped;
        r.note = result.why_not;
      } else {
        JsonValue per_state = JsonValue::object();
        // Only chords whose boxes come within the offset and the activation band are measured:
        // a least separation is the least of those, and null when there were none.
        r.value.set("reach_mm",
                    number((p.offset_m + (policy != nullptr ? policy->activation_m : 0.0)) * k_mm));
        for (const PairState& ps : result.states) {
          JsonValue e = JsonValue::object();
          e.set("pairs_measured", JsonValue(ps.eligible_within_reach));
          e.set("least_nonlocal_mm", std::isfinite(ps.least_nonlocal)
                                         ? number(ps.least_nonlocal * k_mm)
                                         : JsonValue::null());
          e.set("least_local_mm",
                std::isfinite(ps.least_local) ? number(ps.least_local * k_mm) : JsonValue::null());
          e.set("violations", JsonValue(ps.violations));
          e.set("proxy_deviation_a_mm", number(ps.deviation_a * k_mm));
          e.set("proxy_deviation_b_mm", number(ps.deviation_b * k_mm));
          per_state.set(ps.state, std::move(e));
          if (ps.violations > 0) fail(r, ps.witness);
        }
        r.value.set("states", std::move(per_state));
        r.value.set("representation", JsonValue(k_chordal));
      }
    }
    // ---- contact.constraint_compatibility
    {
      ValidationRow& r =
          row("contact.constraint_compatibility", "contact pair " + p.name, Severity::error,
              "no eligible feature pair within its required separation at any state whose sides "
              "are both prescribed (every node of the chord held by an essential attachment, "
              "against a frame or another held chord): such a pair is infeasible, not a violation "
              "a solve can remove; " +
                  std::string(k_chordal),
              "an incompatibility between essential attachments and the contact floor is a witness "
              "to preserve, never a reason to free a node or widen an exclusion");
      if (!result.evaluated) {
        r.verdict = Verdict::skipped;
        r.note = result.why_not;
      } else {
        u32 total = 0;
        for (const PairState& ps : result.states) {
          total += ps.prescribed_violations;
          if (ps.prescribed_violations > 0) fail(r, ps.prescribed_witness);
        }
        r.value.set("infeasible_pairs", JsonValue(total));
      }
    }
    // ---- contact.trajectory
    {
      ValidationRow& r =
          row("contact.trajectory", "contact pair " + p.name, Severity::info,
              "reported: the pair's states in order, with the chordal least separation at each; "
              "not evaluated between them",
              "continuous separation between states — straight node paths, a frame's rotation — is "
              "the certificate's continuous collision detection; this build checks the endpoints "
              "(contact.curved_clearance) and never interpolates");
      JsonValue path = JsonValue::array();
      for (const PairState& ps : result.states) {
        JsonValue e = JsonValue::object();
        e.set("state", JsonValue(ps.state));
        e.set("least_nonlocal_mm", std::isfinite(ps.least_nonlocal)
                                       ? number(ps.least_nonlocal * k_mm)
                                       : JsonValue::null());
        path.push_back(std::move(e));
      }
      r.value.set("states", std::move(path));
      r.value.set("authority", JsonValue("the authoring certificate"));
    }
  }
}

// ---- attachment.contact_compatibility, a separated tie's
//
// A patch held flush on its frame owns that coincidence, and the tissue just outside the patch
// approaches zero distance from the frame, so no uniform positive clearance can hold at its edge:
// the patch is excluded against its frame. A tie across a declared gap has no such edge, and its
// contact against its own frame is what keeps the tissue beside the patch off the frame; excluding
// it would switch off the barrier the mode exists to keep.
void Rows::separated_contact(u32 ai) {
  const Attachment& a = def_.attachments[ai];
  ValidationRow& r = row(
      "attachment.contact_compatibility", "attachment " + a.name, Severity::error,
      "a separated tie keeps its contact: a unilateral contact pair between the patch's surface "
      "and "
      "the attachment's frame exists, no pair between them is an attached interface, and no "
      "exclusion of such a pair exempts a face of the patch or names this attachment",
      "a tie across a declared gap is not an owned coincidence: the patch against its own frame "
      "stays an active unilateral pair, and every other pairing stays active too");
  r.value.set("interface", JsonValue("Separated"));
  const PatchModel& patch = m_.patches[ai];
  if (a.patch_surface.empty())
    fail(r,
         "the separated tie names no patch surface, so no contact pair can hold its patch apart "
         "from its frame");
  u32 unilateral = 0;
  u32 excluded = 0;
  for (const PairModel& pm : m_.pairs) {
    const ContactPair& p = *pm.def;
    if (a.patch_surface.empty() ||
        !(p.a.kind == ContactSurfaceKind::MaterialSurface && p.a.name == a.patch_surface &&
          p.b.kind == ContactSurfaceKind::Frame && p.b.name == a.target))
      continue;
    if (p.eligibility != ContactEligibility::Unilateral) {
      fail(r, "pair " + p.name +
                  " between the patch's surface and its frame is an attached interface, which "
                  "nothing checks: a separated tie's contact is unilateral");
      continue;
    }
    ++unilateral;
    for (u32 ei = 0; ei < p.exclusions.size(); ++ei) {
      const ContactExclusion& e = p.exclusions[ei];
      const ExclusionModel& em = pm.exclusions[ei];
      u32 in_patch = 0;
      for (u32 f = 0; f + 1 < em.a.size(); f += 2)
        for (u32 k = 0; k + 1 < patch.refs.size(); k += 2)
          if (em.a[f] == patch.refs[k] && em.a[f + 1] == patch.refs[k + 1]) {
            ++in_patch;
            break;
          }
      const bool names = e.reason == ExclusionReason::Attachment && e.attachment == a.name;
      if (in_patch == 0 && !names) continue;
      excluded += in_patch;
      fail(r, "pair " + p.name + " exclusion " + str(ei) + " exempts " + str(in_patch) +
                  " patch faces against their own frame" +
                  (names ? std::string(" and names this attachment") : std::string()) +
                  ": a separated tie's contact stays active");
    }
  }
  if (unilateral == 0 && !a.patch_surface.empty())
    fail(r, "no unilateral contact pair between surface " + a.patch_surface + " and frame " +
                a.target + ": nothing keeps the held patch apart from its frame");
  r.value.set("unilateral_pairs", JsonValue(unilateral));
  r.value.set("patch_faces", JsonValue(static_cast<u64>(patch.refs.size() / 2)));
  r.value.set("excluded_patch_faces", JsonValue(excluded));
}

// ---- attachment.target_gap
//
// Emitted for an essential attachment that declares anything of the interface: a separated tie, or
// a coincident one with a gap (which fails). A coincident attachment at its zero gap is version 2's
// meaning and gets no row, so a definition written before version 3 reports as it did. A spring
// reads neither field, and one that declares either fails attachment.enforcement instead.
void Rows::target_gaps() {
  for (u32 ai = 0; ai < def_.attachments.size(); ++ai) {
    const Attachment& a = def_.attachments[ai];
    if (a.enforcement != AttachmentEnforcement::Essential) continue;
    const bool separated = a.interface == AttachmentInterface::Separated;
    if (!separated && a.gap_m == 0.0) continue;
    ValidationRow& r = row(
        "attachment.target_gap", "attachment " + a.name, Severity::error,
        "a separated tie's gap is strictly positive; every held node's frame-local target (its "
        "construction position in the frame's own coordinates, inverse(T_construction) "
        "X_construction, which each state's T_s carries to that state) lies at the declared gap "
        "from the frame's surface, signed outward, within 0.1 um; the gap is not below the offset "
        "of any unilateral pair between the patch's surface and the frame; the policy's activation "
        "distance is reported beside it and not judged; a coincident attachment's gap is zero. "
        "Chordal: the held nodes against the frame's triangles, not the curved six-node faces "
        "between them; it certifies no trajectory between states and no clearance of the tissue "
        "beside the patch",
        "a separated tie is prescribed across a gap it declares, and where its targets lie is "
        "geometry to check; whether a tie inside the contact's activation band is wanted is the "
        "authoring side's mechanics, reported here and never judged");
    r.value.set("interface", JsonValue(separated ? "Separated" : "Coincident"));
    r.value.set("gap_mm", number(a.gap_m * k_mm));
    if (!separated) {
      fail(r, "a coincident attachment declares a gap of " + mm(a.gap_m) +
                  ": coincident is flush on its frame, and a patch held across a gap is a "
                  "separated tie that says so");
      continue;
    }
    r.value.set("tolerance_mm", number(k_position_m * k_mm));
    if (!(a.gap_m > 0.0) || !std::isfinite(a.gap_m))
      fail(r,
           "the declared gap is " + mm(a.gap_m) + ": a separated tie's gap is strictly positive");
    if (a.target_kind != TargetKind::Frame) {
      fail(r,
           "a separated tie is to a frame: the world has no surface to hold the patch apart from");
      continue;
    }
    const FrameModel* frame = find_frame(c_, a.target);
    const i32 ri = find_region(c_, a.region);
    if (frame == nullptr || !frame->ok || ri < 0 || !c_.regions[ri].ok) {
      fail(r, "the attachment's region or frame does not resolve (attachments.resolve)");
      continue;
    }
    const RegionModel& region = c_.regions[ri];
    const RigidTransform at_construction = frame_at(a.target, "construction");
    f64 least = std::numeric_limits<f64>::infinity();
    f64 greatest = -std::numeric_limits<f64>::infinity();
    f64 worst = 0.0;
    f64 worst_distance = 0.0;
    u32 worst_node = 0;
    u32 held = 0;
    u32 off = 0;
    for (const u32 v : node_set(c_, region, a.nodes)) {
      if (v >= region.nodes.size()) continue;
      ++held;
      // Rigid transforms keep distances, so the target's distance from the frame at a state is its
      // frame-local one: measured once, against the frame's own triangles.
      const D3 local = at_construction.unapply(d3(region.nodes[v]));
      const f64 d = frame->surface.query(local).distance;
      least = std::min(least, d);
      greatest = std::max(greatest, d);
      const f64 departure = std::fabs(d - a.gap_m);
      if (!(departure <= k_position_m)) ++off;
      if (!(departure <= worst)) {
        worst = departure;
        worst_distance = d;
        worst_node = v;
      }
    }
    r.value.set("held_nodes", JsonValue(held));
    r.value.set("least_distance_mm", number(least * k_mm));
    r.value.set("greatest_distance_mm", number(greatest * k_mm));
    r.value.set("max_departure_mm", number(worst * k_mm));
    r.value.set("worst_node", JsonValue(worst_node));
    if (off > 0)
      fail(r, str(off) + " held nodes' frame-local targets are off the declared gap: node " +
                  str(worst_node) + " at " + mm(worst_distance) +
                  " from the frame's surface, not " + mm(a.gap_m));
    JsonValue pairs = JsonValue::array();
    for (const PairModel& pm : m_.pairs) {
      const ContactPair& p = *pm.def;
      if (!(p.a.kind == ContactSurfaceKind::MaterialSurface && p.a.name == a.patch_surface &&
            p.b.kind == ContactSurfaceKind::Frame && p.b.name == a.target))
        continue;
      const bool unilateral = p.eligibility == ContactEligibility::Unilateral;
      const ContactPolicy* policy = nullptr;
      for (const ContactPolicy& candidate : def_.contact_policies)
        if (candidate.name == p.policy) policy = &candidate;
      JsonValue e = JsonValue::object();
      e.set("pair", JsonValue(p.name));
      e.set("eligibility", JsonValue(unilateral ? "Unilateral" : "Attached"));
      e.set("offset_mm", number(p.offset_m * k_mm));
      e.set("activation_mm",
            policy != nullptr ? number(policy->activation_m * k_mm) : JsonValue::null());
      pairs.push_back(std::move(e));
      if (unilateral && a.gap_m < p.offset_m)
        fail(r, "the declared gap of " + mm(a.gap_m) + " is below pair " + p.name +
                    "'s offset of " + mm(p.offset_m) +
                    ": the held patch would sit inside the barrier it keeps");
    }
    r.value.set("pairs", std::move(pairs));
  }
}

// ---- certificates -------------------------------------------------------------------------------

void Rows::certificates() {
  for (const ReferenceCertificate& cert : def_.certificates) {
    ValidationRow& r =
        row("reference.certificate_provenance", "certificate " + cert.name, Severity::info,
            "reported, never a pass: the certified state's SHA-256 verified against its block "
            "(a mismatch fails the row), and everything else the certificate carries reported as "
            "given — its inputs, protocol and witness by hash, its authority, implementation, "
            "outcome and bounds",
            "curved separation, continuous steps and the mechanics are the authoring certificate's "
            "to prove and this build's to name: a supplied pass never becomes the engine's own");
    r.value.set("authority", JsonValue(cert.authority));
    r.value.set("implementation", JsonValue(cert.implementation));
    r.value.set("outcome", JsonValue(cert.outcome));
    r.value.set("bounds", cert.bounds);
    r.value.set("recomputed", JsonValue(cert.recomputed));
    r.value.set("input_sha256", JsonValue(cert.input_sha256));
    r.value.set("protocol_sha256", JsonValue(cert.protocol_sha256));
    r.value.set("witness_sha256", JsonValue(cert.witness_sha256));
    r.value.set("witness_path", JsonValue(cert.witness_path));
    r.value.set("not_verified_here",
                JsonValue("the inputs, the protocol and the witness: their bytes are not in the "
                          "file"));
    const i32 ri = find_region(c_, cert.region);
    const TissueBlock* block = nullptr;
    if (ri < 0) {
      fail(r, "the certificate names region '" + cert.region + "'");
    } else if (cert.state == "construction") {
      block = c_.file.find(c_.regions[ri].def->nodes);
    } else {
      for (const RegionState& s : def_.states)
        if (s.region == cert.region && s.name == cert.state) block = c_.file.find(s.nodes);
      if (block == nullptr) fail(r, "the certificate names state '" + cert.state + "'");
    }
    if (block != nullptr) {
      const std::string hash = sha256_hex(block->bytes);
      r.value.set("state_sha256", JsonValue(hash));
      r.value.set("state_verified", JsonValue(hash == cert.state_sha256));
      if (hash != cert.state_sha256)
        fail(r, "the state's bytes have SHA-256 " + hash + " and the certificate names " +
                    cert.state_sha256 + ": it certifies another state");
    }
  }
}

}  // namespace

// ---- resolution ---------------------------------------------------------------------------------

void resolve_layered(const LayeredContext& c, Vector<std::string>& problems, LayeredModel& out) {
  out = LayeredModel{};
  const TissueDefinition& def = c.file.definition;
  const Blocks blocks{c.file, problems};

  for (const MaterialBoundarySurface& s : def.material_surfaces) {
    SurfaceModel m;
    m.def = &s;
    const std::string who = "material surface " + s.name;
    m.region = find_region(c, s.region);
    bool ok = blocks.read(s.faces, BlockKind::BoundaryFaceRefs, who + ".faces", m.refs, 2);
    const RegionModel* region = m.region >= 0 ? &c.regions[m.region] : nullptr;
    if (ok && region != nullptr && region->quadratic() && !region->cell_nodes.empty()) {
      for (u32 f = 0; f + 1 < m.refs.size(); f += 2)
        if (m.refs[f] >= region->cell_count() || m.refs[f + 1] >= 4) {
          problems.push_back(who + " names face (" + str(m.refs[f]) + ", " + str(m.refs[f + 1]) +
                             "), past its region's cells or a cell's four faces");
          ok = false;
          break;
        }
      if (ok)
        for (u32 f = 0; f + 1 < m.refs.size(); f += 2)
          for (u32 k = 0; k < 6; ++k)
            m.nodes.push_back(region->cell(m.refs[f])[p2::k_face_nodes[m.refs[f + 1]][k]]);
    } else {
      ok = false;
    }
    m.ok = ok && region != nullptr && region->ok;
    out.surfaces.push_back(std::move(m));
  }

  for (const MaterialSkin& s : def.material_skins) {
    SkinModel m;
    m.def = &s;
    const std::string who = "material skin " + s.name;
    m.surface = find_surface(out, s.surface);
    bool ok = blocks.read(s.sample_ids, BlockKind::CanonicalIds, who + ".sample_ids", m.ids);
    ok = blocks.read(s.sample_coordinates, BlockKind::MaterialSurfaceCoordinates,
                     who + ".sample_coordinates", m.points) &&
         ok;
    if (ok && m.points.size() != m.ids.size()) {
      problems.push_back(who + " holds " + str(m.points.size()) + " points for " +
                         str(m.ids.size()) + " sampled ids");
      ok = false;
    }
    const RegionModel* region = m.surface >= 0 && out.surfaces[m.surface].region >= 0
                                    ? &c.regions[out.surfaces[m.surface].region]
                                    : nullptr;
    if (region != nullptr) {
      if (s.rest_nodes.empty()) {
        m.rest = region->nodes;
      } else if (blocks.read(s.rest_nodes, BlockKind::StateNodes, who + ".rest_nodes", m.rest) &&
                 m.rest.size() != region->nodes.size()) {
        problems.push_back(who + ".rest_nodes holds " + str(m.rest.size()) + " nodes for " +
                           str(region->nodes.size()));
        ok = false;
      }
      ok = ok && m.rest.size() == region->nodes.size();
    }
    m.ok = ok && region != nullptr;
    out.skins.push_back(std::move(m));
  }

  for (const ParameterSample& s : def.parameter_samples) {
    SampleModel m;
    m.def = &s;
    const std::string who = "sample " + s.name;
    m.region = find_region(c, s.region);
    m.block = blocks.get(s.snapshot, BlockKind::StateNodes, who + ".snapshot");
    bool ok = m.block != nullptr &&
              blocks.read(s.snapshot, BlockKind::StateNodes, who + ".snapshot", m.snapshot);
    if (ok && m.region >= 0 && m.snapshot.size() != c.regions[m.region].nodes.size()) {
      problems.push_back(who + ".snapshot holds " + str(m.snapshot.size()) + " nodes for " +
                         str(c.regions[m.region].nodes.size()));
      ok = false;
    }
    m.ok = ok && m.region >= 0;
    out.samples.push_back(std::move(m));
  }

  for (const DepotPartition& p : def.depot_partitions) {
    PartitionModel m;
    m.def = &p;
    const ThicknessField* field = nullptr;
    for (const ThicknessField& f : def.thickness_fields)
      if (f.name == p.field) field = &f;
    const i32 ri = field != nullptr ? find_region(c, field->region) : -1;
    for (const Depot& d : p.depots) {
      DepotModel dm;
      const std::string who = "depot " + p.name + "/" + d.name;
      dm.ok = blocks.read(d.shares, BlockKind::PhaseFraction, who + ".shares", dm.shares);
      if (dm.ok && ri >= 0 && dm.shares.size() != c.regions[ri].cell_count()) {
        problems.push_back(who + ".shares holds " + str(dm.shares.size()) + " values for " +
                           str(c.regions[ri].cell_count()) + " cells");
        dm.ok = false;
      }
      dm.ok = dm.ok && ri >= 0;
      m.depots.push_back(std::move(dm));
    }
    out.partitions.push_back(std::move(m));
  }

  for (const ContactPair& p : def.contact_pairs) {
    PairModel m;
    m.def = &p;
    for (u32 ei = 0; ei < p.exclusions.size(); ++ei) {
      const ContactExclusion& e = p.exclusions[ei];
      const std::string who = "contact pair " + p.name + " exclusion " + str(ei);
      ExclusionModel em;
      em.ok = blocks.read(e.a_faces, BlockKind::BoundaryFaceRefs, who + ".a_faces", em.a, 2);
      if (!e.b_faces.empty())
        em.ok =
            blocks.read(e.b_faces, BlockKind::BoundaryFaceRefs, who + ".b_faces", em.b, 2) && em.ok;
      m.exclusions.push_back(std::move(em));
    }
    out.pairs.push_back(std::move(m));
  }

  for (const Attachment& a : def.attachments) {
    PatchModel m;
    if (!a.patch.empty())
      m.ok = blocks.read(a.patch, BlockKind::BoundaryFaceRefs, "attachment " + a.name + ".patch",
                         m.refs, 2);
    out.patches.push_back(std::move(m));
  }
}

void check_layered(const LayeredContext& context, const LayeredModel& model, TissueReport& report) {
  Rows rows(context, model, report);
  rows.run();
}

}  // namespace engine::tissue::detail
