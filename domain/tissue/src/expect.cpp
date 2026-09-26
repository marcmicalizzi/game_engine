#include <core/json/json.h>
#include <core/schema/json_reflect.h>
#include <domain/tissue/expect.h>
#include <foundation/io/vfs.h>

#include <cmath>
#include <cstdio>
#include <string>
#include <utility>

namespace engine::tissue {

namespace {

bool fail(std::string* error, std::string message) {
  if (error != nullptr) *error = std::move(message);
  return false;
}

std::string number_text(f64 v) {
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "%.17g", v);
  return buffer;
}

std::string value_text(const JsonValue& v) {
  std::string out = write_json(v, JsonWriteOptions{.pretty = false});
  if (out.size() > 80) out = out.substr(0, 77) + "...";
  return out;
}

// Whether `actual` matches the pattern: every key of an object pattern present and matching,
// arrays of the same length element by element, numbers within the tolerance, everything else
// equal. `why` says where the first departure is.
bool matches(const JsonValue& pattern, const JsonValue& actual, f64 tolerance,
             const std::string& path, std::string& why) {
  const std::string where = path.empty() ? std::string("the value") : path;
  if (pattern.is_object()) {
    if (!actual.is_object()) {
      why = where + " is " + value_text(actual) + ", not an object";
      return false;
    }
    for (const auto& [key, value] : pattern.as_object()) {
      const std::string at = path.empty() ? key : path + "." + key;
      const JsonValue* found = actual.find(key);
      if (found == nullptr) {
        why = at + " is not in the row's value";
        return false;
      }
      if (!matches(value, *found, tolerance, at, why)) return false;
    }
    return true;
  }
  if (pattern.is_array()) {
    if (!actual.is_array() || actual.size() != pattern.size()) {
      why = where + " is " + value_text(actual) + ", declared an array of " +
            std::to_string(pattern.size());
      return false;
    }
    for (usize i = 0; i < pattern.size(); ++i)
      if (!matches(pattern[i], actual[i], tolerance, where + "[" + std::to_string(i) + "]", why))
        return false;
    return true;
  }
  if (pattern.is_number()) {
    f64 want = 0.0;
    f64 have = 0.0;
    pattern.get_f64(want);
    if (!actual.get_f64(have)) {
      why = where + " is " + value_text(actual) + ", declared " + number_text(want);
      return false;
    }
    if (!(std::fabs(have - want) <= tolerance)) {
      why = where + " is " + number_text(have) + ", declared " + number_text(want) + " within " +
            number_text(tolerance);
      return false;
    }
    return true;
  }
  if (!(pattern == actual)) {
    why = where + " is " + value_text(actual) + ", declared " + value_text(pattern);
    return false;
  }
  return true;
}

const char* declared_name(ExpectedVerdict v) noexcept {
  return v == ExpectedVerdict::Skipped ? "skipped" : "fail";
}

bool known_severity(std::string_view s) noexcept {
  return s == "error" || s == "warning" || s == "info";
}

}  // namespace

bool parse_expectation(std::string_view text, TissueExpectation& out, std::string* error) {
  JsonValue json;
  const JsonParseResult parsed = parse_json(text, json);
  if (!parsed.ok)
    return fail(error, std::string("the expectation is not JSON: ") + parsed.message + " at line " +
                           std::to_string(parsed.line) + ", column " +
                           std::to_string(parsed.column));
  schema::ReadContext ctx;
  ctx.options.ignore_unknown_fields = false;
  TissueExpectation expectation;
  if (!schema::from_json(expectation, json, ctx)) {
    const schema::Diagnostic& first = ctx.diagnostics.front();
    return fail(error,
                "the expectation is not a TissueExpectation: " + first.path + ": " + first.message);
  }
  if (expectation.format != k_expect_format)
    return fail(error, "the expectation's format is '" + expectation.format + "', not '" +
                           k_expect_format + "'");
  for (u32 i = 0; i < expectation.failures.size(); ++i) {
    const ExpectedFailure& f = expectation.failures[i];
    const std::string which = "failure " + std::to_string(i);
    if (f.id.empty() || f.subject.empty()) return fail(error, which + " names no id or no subject");
    if (!known_severity(f.severity))
      return fail(error, which + " (" + f.id + ") has severity '" + f.severity +
                             "', not error, warning or info");
    if (!(f.tolerance >= 0.0) || !std::isfinite(f.tolerance))
      return fail(error, which + " (" + f.id + ") has tolerance " + number_text(f.tolerance));
  }
  out = std::move(expectation);
  return true;
}

bool read_expectation(std::string_view path, TissueExpectation& out, std::string* error) {
  std::string text;
  const io::Status status = io::read_file(path, text);
  if (status != io::Status::Ok)
    return fail(error,
                std::string("cannot read ") + std::string(path) + ": " + io::status_name(status));
  return parse_expectation(text, out, error);
}

void compare_expectation(const TissueReport& report, const TissueExpectation& expectation,
                         ExpectationResult& out) {
  out = ExpectationResult{};
  out.declared = static_cast<u32>(expectation.failures.size());
  Vector<u8> used(static_cast<u32>(expectation.failures.size()), 0);
  for (const ValidationRow& r : report.rows) {
    if (r.verdict != Verdict::fail && r.verdict != Verdict::skipped) continue;
    ++out.outcomes;
    const ExpectedVerdict outcome =
        r.verdict == Verdict::fail ? ExpectedVerdict::Fail : ExpectedVerdict::Skipped;
    const char* severity = severity_name(r.severity);
    u32 match = ~0u;
    for (u32 i = 0; i < expectation.failures.size() && match == ~0u; ++i) {
      const ExpectedFailure& e = expectation.failures[i];
      if (used[i] == 0 && e.id == r.id && e.subject == r.subject && e.severity == severity &&
          e.verdict == outcome)
        match = i;
    }
    if (match == ~0u) {
      ExpectationDifference d;
      d.kind = ExpectationDifference::Kind::unexpected;
      d.id = r.id;
      d.subject = r.subject;
      d.severity = severity;
      d.actual = std::string(verdict_name(r.verdict)) + " as " + severity;
      d.detail = r.witness.empty() ? r.note : r.witness;
      out.differences.push_back(std::move(d));
      continue;
    }
    used[match] = 1;
    const ExpectedFailure& e = expectation.failures[match];
    std::string why;
    if (!e.value.is_null() && !matches(e.value, r.value, e.tolerance, std::string(), why)) {
      ExpectationDifference d;
      d.kind = ExpectationDifference::Kind::value;
      d.id = r.id;
      d.subject = r.subject;
      d.severity = severity;
      d.declared = declared_name(e.verdict);
      d.actual = std::string(verdict_name(r.verdict)) + " as " + severity;
      d.detail = std::move(why);
      out.differences.push_back(std::move(d));
    }
  }
  for (u32 i = 0; i < expectation.failures.size(); ++i) {
    if (used[i] != 0) continue;
    const ExpectedFailure& e = expectation.failures[i];
    ExpectationDifference d;
    d.kind = ExpectationDifference::Kind::missing;
    d.id = e.id;
    d.subject = e.subject;
    d.severity = e.severity;
    d.declared = declared_name(e.verdict);
    d.actual = "absent";
    for (const ValidationRow& r : report.rows)
      if (r.id == e.id && r.subject == e.subject) {
        d.actual = std::string(verdict_name(r.verdict)) + " as " + severity_name(r.severity);
        break;
      }
    std::string detail = d.actual == "absent" ? "the report has no row of this id and subject"
                         : d.actual.rfind("pass", 0) == 0 || d.actual.rfind("info", 0) == 0
                             ? "the row does not fail: fixed upstream, a role that reports it, or "
                               "a declaration out of date"
                             : "the row's outcome or severity is not the declared one";
    if (!e.witness.empty()) detail += "; declared with witness '" + e.witness + "'";
    d.detail = std::move(detail);
    out.differences.push_back(std::move(d));
  }
}

const char* difference_kind_name(ExpectationDifference::Kind kind) noexcept {
  switch (kind) {
    case ExpectationDifference::Kind::missing: return "missing";
    case ExpectationDifference::Kind::unexpected: return "unexpected";
    case ExpectationDifference::Kind::value: return "value";
  }
  return "unknown";
}

JsonValue expectation_json(const ExpectationResult& result) {
  JsonValue out = JsonValue::object();
  out.set("matched", JsonValue(result.matched()));
  out.set("declared", JsonValue(result.declared));
  out.set("outcomes", JsonValue(result.outcomes));
  JsonValue list = JsonValue::array();
  for (const ExpectationDifference& d : result.differences) {
    JsonValue e = JsonValue::object();
    e.set("kind", JsonValue(difference_kind_name(d.kind)));
    e.set("id", JsonValue(d.id));
    e.set("subject", JsonValue(d.subject));
    e.set("severity", JsonValue(d.severity));
    e.set("declared", JsonValue(d.declared));
    e.set("actual", JsonValue(d.actual));
    e.set("detail", JsonValue(d.detail));
    list.push_back(std::move(e));
  }
  out.set("differences", std::move(list));
  return out;
}

std::string expectation_text(const ExpectationResult& result) {
  std::string out;
  for (const ExpectationDifference& d : result.differences) {
    switch (d.kind) {
      case ExpectationDifference::Kind::missing:
        out +=
            "declared " + d.declared + " as " + d.severity + ", and the row is " + d.actual + ": ";
        break;
      case ExpectationDifference::Kind::unexpected:
        out += "not declared, and the row is " + d.actual + ": ";
        break;
      case ExpectationDifference::Kind::value:
        out += "declared " + d.declared + " and it is, but its value departs: ";
        break;
    }
    out += d.id + " [" + d.subject + "]";
    if (!d.detail.empty()) out += ": " + d.detail;
    out += "\n";
  }
  return out;
}

TissueExpectation expectation_from_report(const TissueReport& report, std::string note) {
  TissueExpectation out;
  out.format = k_expect_format;
  out.match = k_expect_rule;
  out.note = std::move(note);
  for (const ValidationRow& r : report.rows) {
    if (r.verdict != Verdict::fail && r.verdict != Verdict::skipped) continue;
    ExpectedFailure e;
    e.id = r.id;
    e.subject = r.subject;
    e.severity = severity_name(r.severity);
    e.witness = r.witness;
    if (r.verdict == Verdict::skipped) {
      e.verdict = ExpectedVerdict::Skipped;
      e.note = r.note;
    }
    out.failures.push_back(std::move(e));
  }
  return out;
}

std::string write_expectation(const TissueExpectation& expectation) {
  JsonValue json = schema::to_json(expectation);
  // The engine's additions only where they say something a v1 file cannot.
  if (JsonValue* failures = json.find("failures"); failures != nullptr && failures->is_array())
    for (usize i = 0; i < failures->size(); ++i) {
      const ExpectedFailure& f = expectation.failures[static_cast<u32>(i)];
      JsonValue::Object& fields = (*failures)[i].as_object();
      if (f.verdict == ExpectedVerdict::Fail) fields.erase(std::string_view("verdict"));
      if (f.value.is_null()) fields.erase(std::string_view("value"));
      if (f.tolerance == 0.0) fields.erase(std::string_view("tolerance"));
      if (f.note.empty()) fields.erase(std::string_view("note"));
    }
  std::string text = write_json(json, JsonWriteOptions{.pretty = true});
  text.push_back('\n');
  return text;
}

}  // namespace engine::tissue
