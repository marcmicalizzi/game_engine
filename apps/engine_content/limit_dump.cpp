// engine-content limit-dump: the limit-surface conformance exchange with the authoring side
// (docs/subsystems/geometry.md, "The conformance exchange"; docs/subsystems/apps.md).
//
//   engine-content limit-dump <control.json> --level <n> --out <dump.json> [--mode <name>]
//
// **Why it exists.** The engine and the authoring tools must compute the same surface, the same
// normals and the same transfer, and "the same" is only a fact once both sides' numbers are laid
// side by side. The authoring side writes a control mesh — positions, triangles, optionally states
// and authored footpoints — and its own evaluation of it; this writes the engine's: the limit and
// tangent operators as the build composes them (compressed rows, f32 weights), the limit positions
// and vertex normals under the agreed rule at the reference and every state, and for the footpoints
// the footpoint normals, the fallbacks, and the bound positions both through the 10-byte records
// the runtime carries and without them, so that operator rounding, coordinate rounding and record
// packing can be told apart. It is a debugging exchange, not part of the `.tissue` format.
//
// The input is `astra.normal_conformance.input.v1` as the authoring side wrote it, and anything
// with the same fields: "positions" (required, [x, y, z] per control vertex), "faces" (required,
// three indices each, wound outward), "level" (overridden by --level), "states" (an object of named
// position lists), "corners" (control vertices Hoppe's corner rule pins), and "footpoints" (each
// {"triangle", "barycentric": [b0, b1, b2], "normal_offset" or "offset", "weight",
// "base_position"}; a missing offset is measured along the mode's normal, a missing weight is one,
// and a missing base position is the footpoint plus its offset along the reference normal). Every
// input number is rounded to f32 on the way in, which the output's "input_conversion" reports. The
// output's bytes are a function of the input's: the JSON writer is canonical and floating-point
// contraction is off tree-wide (ADR-0035).
#include "content_commands.h"

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/hash/hash.h>
#include <core/json/json.h>
#include <core/math/math.h>
#include <domain/geometry/limit_surface.h>
#include <domain/geometry/surface_binding.h>
#include <foundation/io/vfs.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <string_view>

namespace engine::content {

namespace {

constexpr int k_exit_ok = 0;
constexpr int k_exit_error = 1;
constexpr int k_exit_usage = 2;

const char* k_dump_usage =
    "usage: engine-content limit-dump <control.json> --level <n> --out <dump.json> [--mode <m>]\n"
    "  the limit and tangent operators, the limit positions and normals at the reference and at\n"
    "  every state the input names, and the footpoints' normals and bound positions, as one JSON\n"
    "  file; --mode is limit-interpolated (default), interpolated-vertex-area-weighted or\n"
    "  triangle\n";

int usage(const char* message) {
  if (message != nullptr) std::fprintf(stderr, "engine-content limit-dump: %s\n", message);
  std::fputs(k_dump_usage, stderr);
  return k_exit_usage;
}

int failed(const std::string& message) {
  std::fprintf(stderr, "engine-content limit-dump: %s\n", message.c_str());
  return k_exit_error;
}

struct Rounding {
  f64 worst = 0.0;
  bool exact = true;
  f32 take(f64 value) {
    const f32 rounded = static_cast<f32>(value);
    const f64 error = std::fabs(static_cast<f64>(rounded) - value);
    worst = std::max(worst, error);
    exact = exact && error == 0.0;
    return rounded;
  }
};

bool read_vec3s(const JsonValue* array, Vector<Vec3>& out, Rounding& rounding, std::string& error,
                const char* what) {
  out.clear();
  if (array == nullptr || !array->is_array()) {
    error = std::string(what) + " is not an array";
    return false;
  }
  for (usize i = 0; i < array->size(); ++i) {
    const JsonValue& p = (*array)[i];
    f64 xyz[3] = {0, 0, 0};
    if (!p.is_array() || p.size() != 3 || !p[0].get_f64(xyz[0]) || !p[1].get_f64(xyz[1]) ||
        !p[2].get_f64(xyz[2])) {
      error = std::string(what) + "[" + std::to_string(i) + "] is not three numbers";
      return false;
    }
    out.push_back(Vec3{rounding.take(xyz[0]), rounding.take(xyz[1]), rounding.take(xyz[2])});
  }
  return true;
}

JsonValue vec3_json(Vec3 v) {
  JsonValue out = JsonValue::array();
  out.push_back(JsonValue(v.x));
  out.push_back(JsonValue(v.y));
  out.push_back(JsonValue(v.z));
  return out;
}

JsonValue vec3s_json(const Vector<Vec3>& values) {
  JsonValue out = JsonValue::array();
  for (const Vec3& v : values)
    out.push_back(vec3_json(v));
  return out;
}

JsonValue indices_json(const Vector<u32>& values) {
  JsonValue out = JsonValue::array();
  for (const u32 v : values)
    out.push_back(JsonValue(v));
  return out;
}

JsonValue csr_json(const geometry::CsrMatrix& m) {
  JsonValue out = JsonValue::object();
  out.set("column_count", JsonValue(m.column_count));
  out.set("row_offsets", indices_json(m.row_offsets));
  out.set("column_indices", indices_json(m.column_index));
  JsonValue weights = JsonValue::array();
  for (const f32 w : m.weight)
    weights.push_back(JsonValue(w));
  out.set("weights", std::move(weights));
  return out;
}

struct Footpoints {
  Vector<geometry::AuthoredFootpoint> authored;
  Vector<Vec3> base;  // one per footpoint
  Vector<bool> base_given;
};

}  // namespace

int limit_dump_command(int argc, char** argv) {
  std::string input;
  std::string output;
  u32 level = 0;
  bool level_given = false;
  geometry::NormalMode mode = geometry::NormalMode::limit_interpolated;
  for (int i = 2; i < argc; ++i) {
    const std::string_view a = argv[i];
    if (a == "--help" || a == "-h") {
      std::fputs(k_dump_usage, stdout);
      return k_exit_ok;
    }
    if (a == "--level" || a == "--out" || a == "--mode") {
      if (i + 1 >= argc) return usage("an option needs a value");
      const std::string value = argv[++i];
      if (a == "--out") {
        output = value;
      } else if (a == "--mode") {
        if (!geometry::parse_normal_mode(value, mode))
          return usage(
              "--mode is limit-interpolated, interpolated-vertex-area-weighted or triangle");
      } else {
        char* end = nullptr;
        const unsigned long parsed = std::strtoul(value.c_str(), &end, 10);
        if (end == value.c_str() || *end != '\0' || parsed > geometry::k_loop_max_level)
          return usage("--level is 0 to 4");
        level = static_cast<u32>(parsed);
        level_given = true;
      }
    } else if (!a.empty() && a[0] == '-') {
      return usage("unknown option");
    } else if (input.empty()) {
      input = std::string(a);
    } else {
      return usage("one control mesh at a time");
    }
  }
  if (input.empty() || output.empty()) return usage("a control mesh and --out are required");

  std::string text;
  const io::Status status = io::read_file(input, text);
  if (status != io::Status::Ok)
    return failed("cannot read '" + input + "': " + io::status_name(status));
  JsonValue json;
  const JsonParseResult parsed = parse_json(text, json);
  if (!parsed.ok || !json.is_object())
    return failed(input + " is not a JSON object: " + parsed.message + " at line " +
                  std::to_string(parsed.line));

  // ---- the control mesh ------------------------------------------------------------------------
  Rounding positions_rounding;
  Vector<Vec3> control;
  std::string error;
  if (!read_vec3s(json.find("positions"), control, positions_rounding, error, "positions"))
    return failed(error);
  Vector<u32> faces;
  const JsonValue* face_list = json.find("faces");
  if (face_list == nullptr || !face_list->is_array()) return failed("faces is not an array");
  for (usize i = 0; i < face_list->size(); ++i) {
    const JsonValue& f = (*face_list)[i];
    for (usize k = 0; k < 3; ++k) {
      u64 v = 0;
      if (!f.is_array() || f.size() != 3 || !f[k].get_u64(v) || v >= control.size())
        return failed("faces[" + std::to_string(i) + "] is not three control vertex indices");
      faces.push_back(static_cast<u32>(v));
    }
  }
  if (!level_given) {
    u64 from_file = 0;
    const JsonValue* l = json.find("level");
    if (l == nullptr || !l->get_u64(from_file) || from_file > geometry::k_loop_max_level)
      return usage("--level is required when the input names none from 0 to 4");
    level = static_cast<u32>(from_file);
  }
  Vector<u32> corners;
  if (const JsonValue* c = json.find("corners"); c != nullptr && c->is_array())
    for (usize i = 0; i < c->size(); ++i) {
      u64 v = 0;
      if ((*c)[i].get_u64(v) && v < control.size()) corners.push_back(static_cast<u32>(v));
    }
  geometry::LoopSurfaceOptions options;
  options.level = level;
  options.tangents = true;
  options.corners = corners;
  geometry::LoopLimitSurface surface;
  if (!geometry::build_loop_limit_surface(faces, static_cast<u32>(control.size()), options, surface,
                                          &error))
    return failed("the control mesh is not a surface Loop's rules take: " + error);

  // ---- the states ------------------------------------------------------------------------------
  struct State {
    std::string name;
    Vector<Vec3> nodes;
  };
  Vector<State> states;
  states.push_back(State{"reference", control});
  Rounding state_rounding;
  if (const JsonValue* s = json.find("states"); s != nullptr && s->is_object()) {
    for (auto [name, value] : s->as_object()) {
      State state;
      state.name = std::string(name);
      if (!read_vec3s(&value, state.nodes, state_rounding, error, "a state")) return failed(error);
      if (state.nodes.size() != control.size())
        return failed("state '" + state.name + "' is not one position per control vertex");
      states.push_back(std::move(state));
    }
  }

  // ---- the footpoints --------------------------------------------------------------------------
  Footpoints feet;
  Rounding barycentric_rounding;
  Rounding offset_rounding;
  Rounding base_rounding;
  const JsonValue* footpoints = json.find("footpoints");
  const bool have_feet = footpoints != nullptr && footpoints->is_array() && footpoints->size() > 0;
  if (have_feet) {
    for (usize i = 0; i < footpoints->size(); ++i) {
      const JsonValue& f = (*footpoints)[i];
      const std::string who = "footpoints[" + std::to_string(i) + "]";
      u64 triangle = 0;
      const JsonValue* t = f.find("triangle");
      const JsonValue* b = f.find("barycentric");
      if (t == nullptr || !t->get_u64(triangle) || b == nullptr || !b->is_array() || b->size() != 3)
        return failed(who + " needs a triangle and three barycentrics");
      f64 bary[3] = {0, 0, 0};
      for (usize k = 0; k < 3; ++k)
        if (!(*b)[k].get_f64(bary[k])) return failed(who + "'s barycentrics are not numbers");
      geometry::AuthoredFootpoint a;
      a.triangle = static_cast<u32>(triangle);
      a.barycentric = Vec3{barycentric_rounding.take(bary[0]), barycentric_rounding.take(bary[1]),
                           barycentric_rounding.take(bary[2])};
      f64 value = 0.0;
      const JsonValue* h = f.find("normal_offset");
      if (h == nullptr) h = f.find("offset");
      if (h != nullptr && h->get_f64(value)) a.offset = offset_rounding.take(value);
      a.weight = 1.0f;
      if (const JsonValue* w = f.find("weight"); w != nullptr && w->get_f64(value))
        a.weight = static_cast<f32>(value);
      feet.authored.push_back(a);
      Vector<Vec3> base;
      JsonValue wrapped = JsonValue::array();
      bool given = false;
      if (const JsonValue* p = f.find("base_position"); p != nullptr && p->is_array()) {
        wrapped.push_back(*p);
        given = read_vec3s(&wrapped, base, base_rounding, error, "base_position");
        if (!given) return failed(who + ": " + error);
      }
      feet.base.push_back(given ? base.front() : Vec3{});
      feet.base_given.push_back(given);
    }
  }

  // ---- the reference ----------------------------------------------------------------------------
  geometry::BindingFrame rest;
  if (!geometry::evaluate_binding_frame(mode, surface, control, control, rest, &error))
    return failed(error);
  const geometry::BindingSurface rest_view = rest.view(surface.faces);
  // The limit rule's vertex normals, whatever the mode, since they are what the exchange compares.
  const u32 rows = surface.limit.rows();
  Vector<Vec3> tu(rows);
  Vector<Vec3> tv(rows);
  geometry::apply(surface.tangent_u, control, std::span<Vec3>(tu.data(), rows));
  geometry::apply(surface.tangent_v, control, std::span<Vec3>(tv.data(), rows));
  Vector<Vec3> limit_normals(rows);
  Vector<u32> invalid_vertices;
  geometry::limit_reference_normals(tu, tv, std::span<Vec3>(limit_normals.data(), rows),
                                    &invalid_vertices);

  Vector<geometry::SurfaceBinding> records;
  geometry::SurfaceBindReport bind_report;
  if (have_feet) {
    const u32 triangles = surface.triangle_count();
    for (u32 i = 0; i < feet.authored.size(); ++i) {
      if (feet.authored[i].triangle >= triangles)
        return failed("footpoints[" + std::to_string(i) + "] names triangle " +
                      std::to_string(feet.authored[i].triangle) + " of " +
                      std::to_string(triangles));
      if (feet.base_given[i]) continue;
      // No base: the footpoint plus its offset along the reference footpoint normal.
      const geometry::AuthoredFootpoint& a = feet.authored[i];
      const u32* c = surface.faces.data() + 3 * a.triangle;
      const Vec3 q = rest.reference[c[0]] * a.barycentric.x +
                     rest.reference[c[1]] * a.barycentric.y +
                     rest.reference[c[2]] * a.barycentric.z;
      const Vec3 n = geometry::footpoint_normal(mode, rest_view, a.triangle, a.barycentric);
      feet.base[i] = q + n * (std::isfinite(a.offset) ? a.offset : 0.0f);
    }
    if (!geometry::bind_from_records(mode, feet.base, feet.authored, rest_view,
                                     geometry::SurfaceBindOptions{}, records, &bind_report, &error))
      return failed(error);
  }

  // ---- every state ------------------------------------------------------------------------------
  JsonValue states_json = JsonValue::object();
  for (const State& state : states) {
    geometry::BindingFrame frame;
    if (!geometry::evaluate_binding_frame(mode, surface, control, state.nodes, frame, &error))
      return failed(error);
    const geometry::BindingSurface view = frame.view(surface.faces);
    JsonValue e = JsonValue::object();
    Vector<Vec3> limit_positions(rows);
    geometry::apply(surface.limit, state.nodes, std::span<Vec3>(limit_positions.data(), rows));
    e.set("limit_positions", vec3s_json(limit_positions));
    // Vertex normals at the state, under the limit rule: the reference normal plus its change.
    Vector<Vec3> dtu(rows);
    Vector<Vec3> dtv(rows);
    Vector<Vec3> node_displacement(static_cast<u32>(control.size()));
    for (u32 k = 0; k < control.size(); ++k)
      node_displacement[k] = state.nodes[k] - control[k];
    geometry::apply(surface.tangent_u, node_displacement, std::span<Vec3>(dtu.data(), rows));
    geometry::apply(surface.tangent_v, node_displacement, std::span<Vec3>(dtv.data(), rows));
    Vector<Vec3> change(rows);
    Vector<u32> fallback_vertices;
    geometry::limit_normal_changes(tu, tv, dtu, dtv, std::span<Vec3>(change.data(), rows),
                                   &fallback_vertices);
    Vector<Vec3> vertex_normals(rows);
    for (u32 k = 0; k < rows; ++k)
      vertex_normals[k] = limit_normals[k] + change[k];
    e.set("vertex_normals", vec3s_json(vertex_normals));
    e.set("invalid_vertices", indices_json(invalid_vertices));
    e.set("fallback_vertices", indices_json(fallback_vertices));
    if (have_feet) {
      Vector<Vec3> foot_normals;
      Vector<u32> fallback_feet;
      Vector<u32> invalid_feet;
      Vector<Vec3> unquantized;
      for (u32 i = 0; i < feet.authored.size(); ++i) {
        const geometry::AuthoredFootpoint& a = feet.authored[i];
        const Vec3 n = geometry::footpoint_normal(mode, rest_view, a.triangle, a.barycentric);
        bool fell_back = false;
        const Vec3 dn =
            geometry::footpoint_normal_change(mode, view, a.triangle, a.barycentric, &fell_back);
        foot_normals.push_back(n + dn);
        if (n == Vec3{}) invalid_feet.push_back(i);
        if (fell_back) fallback_feet.push_back(i);
        // The transfer with the input's own barycentrics, offset and weight: no record packing.
        const u32* c = surface.faces.data() + 3 * a.triangle;
        const Vec3 moved = frame.displacement[c[0]] * a.barycentric.x +
                           frame.displacement[c[1]] * a.barycentric.y +
                           frame.displacement[c[2]] * a.barycentric.z;
        const f32 h = std::isfinite(a.offset) ? a.offset : 0.0f;
        const Vec3 delta = h == 0.0f ? moved : moved + dn * h;
        const Vec3 step = delta * a.weight;
        unquantized.push_back(a.weight == 0.0f ? feet.base[i] : feet.base[i] + step);
      }
      Vector<Vec3> bound(static_cast<u32>(feet.base.size()));
      geometry::apply_binding(mode, records, feet.base, view,
                              std::span<Vec3>(bound.data(), bound.size()));
      e.set("footpoint_normals", vec3s_json(foot_normals));
      e.set("invalid_footpoints", indices_json(invalid_feet));
      e.set("fallback_footpoints", indices_json(fallback_feet));
      e.set("bound_positions", vec3s_json(bound));
      e.set("bound_positions_unquantized", vec3s_json(unquantized));
    }
    states_json.set(state.name, std::move(e));
  }

  // ---- the file ---------------------------------------------------------------------------------
  JsonValue out = JsonValue::object();
  out.set("schema", JsonValue("engine.limit_dump.v1"));
  JsonValue source = JsonValue::object();
  source.set("path", JsonValue(input));
  char hash[32];
  std::snprintf(hash, sizeof(hash), "%016llx",
                static_cast<unsigned long long>(hash_bytes(text.data(), text.size())));
  source.set("hash_bytes", JsonValue(hash));
  if (const JsonValue* s = json.find("schema"); s != nullptr) source.set("schema", *s);
  out.set("input", std::move(source));
  out.set("level", JsonValue(level));
  out.set("mode", JsonValue(geometry::normal_mode_name(mode)));
  out.set("rule", JsonValue("loop-hoppe-1994-v1"));
  JsonValue thresholds = JsonValue::object();
  thresholds.set("sine", JsonValue(geometry::k_normal_degenerate_sine));
  thresholds.set("interpolated_length", JsonValue(geometry::k_normal_interpolated_length));
  thresholds.set("degenerate_ratio", JsonValue(geometry::k_normal_degenerate_ratio));
  out.set("thresholds", std::move(thresholds));
  JsonValue control_json = JsonValue::object();
  control_json.set("vertices", JsonValue(static_cast<u64>(control.size())));
  control_json.set("triangles", JsonValue(static_cast<u64>(faces.size() / 3)));
  control_json.set("max_boundary_faces", JsonValue(surface.max_boundary_faces));
  control_json.set("refined_vertices", JsonValue(surface.vertex_count()));
  control_json.set("refined_triangles", JsonValue(surface.triangle_count()));
  out.set("control", std::move(control_json));
  JsonValue conversion = JsonValue::object();
  conversion.set("positions_f32_exact",
                 JsonValue(positions_rounding.exact && state_rounding.exact));
  conversion.set("max_position_rounding_m",
                 JsonValue(std::max(positions_rounding.worst, state_rounding.worst)));
  conversion.set("max_barycentric_rounding", JsonValue(barycentric_rounding.worst));
  conversion.set("max_offset_rounding_m", JsonValue(offset_rounding.worst));
  conversion.set("max_base_rounding_m", JsonValue(base_rounding.worst));
  out.set("input_conversion", std::move(conversion));
  if (have_feet) {
    JsonValue r = JsonValue::object();
    r.set("count", JsonValue(static_cast<u64>(records.size())));
    r.set("max_quantization_m", JsonValue(bind_report.max_quantization_error));
    r.set("max_tangential_residual_m", JsonValue(bind_report.max_tangential_residual));
    r.set("max_offset_disagreement_m", JsonValue(bind_report.max_offset_disagreement));
    r.set("boundary_footpoints",
          JsonValue(static_cast<u64>(bind_report.boundary_footpoints.size())));
    r.set("invalid_normals", JsonValue(static_cast<u64>(bind_report.invalid_normals.size())));
    out.set("records", std::move(r));
  }
  JsonValue refined = JsonValue::array();
  for (u32 t = 0; t < surface.triangle_count(); ++t) {
    JsonValue f = JsonValue::array();
    for (u32 k = 0; k < 3; ++k)
      f.push_back(JsonValue(surface.faces[3 * t + k]));
    refined.push_back(std::move(f));
  }
  out.set("faces", std::move(refined));
  JsonValue operators = JsonValue::object();
  operators.set("L", csr_json(surface.limit));
  operators.set("Tu", csr_json(surface.tangent_u));
  operators.set("Tv", csr_json(surface.tangent_v));
  out.set("operators", std::move(operators));
  out.set("states", std::move(states_json));

  std::string written;
  if (!write_json(out, written, JsonWriteOptions{.pretty = false}))
    return failed("an evaluation produced a number JSON cannot hold");
  written.push_back('\n');
  const io::Status wrote = io::write_file_atomic(output, written);
  if (wrote != io::Status::Ok)
    return failed("cannot write '" + output + "': " + io::status_name(wrote));
  JsonValue summary = JsonValue::object();
  summary.set("input", JsonValue(input));
  summary.set("output", JsonValue(output));
  summary.set("bytes", JsonValue(static_cast<u64>(written.size())));
  summary.set("level", JsonValue(level));
  summary.set("mode", JsonValue(geometry::normal_mode_name(mode)));
  summary.set("states", JsonValue(static_cast<u64>(states.size())));
  summary.set("footpoints", JsonValue(static_cast<u64>(feet.authored.size())));
  summary.set("invalid_vertices", JsonValue(static_cast<u64>(invalid_vertices.size())));
  std::string line = write_json(summary, JsonWriteOptions{.pretty = false});
  line.push_back('\n');
  std::fwrite(line.data(), 1, line.size(), stdout);
  return k_exit_ok;
}

}  // namespace engine::content
