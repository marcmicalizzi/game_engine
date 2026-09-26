// engine-content tissue: the tissue definition on the command line (docs/subsystems/tissue.md,
// docs/subsystems/apps.md). Compiled against domain/tissue when this build has the capability
// (ENGINE_CONTENT_TISSUE); without it the command is refused with a sentence, as engine-view
// refuses
// `--animate` without animation, and `msvc-minimal` is the proof that the rest of the app builds.
//
//   tissue import <interchange.json> <out.tissue>   check every block's size and SHA-256, write the
//                                                   container
//   tissue info <file.tissue>                       the header, every section (known or not) and
//                                                   the definition's outline
//   tissue validate <file.tissue|interchange.json>  every validator row; exit 1 on a failing error
//       --expect <expected.json>                    the fixture mode: exit 0 exactly when the rows
//                                                   that fail or are skipped are the declared ones
//       --write-expect <expected.json>              write the declaration this run would match
//   tissue report <file.tissue|interchange.json>    the rows and every number they stand on
//   tissue example <directory>                      the synthetic definition, and its ten-node
//                                                   reference-body variant, as interchanges and
//                                                   containers: worked examples of the format
#include "content_commands.h"

#include <core/base/types.h>
#include <core/json/json.h>

#include <cstdio>
#include <string>
#include <string_view>

#if ENGINE_CONTENT_TISSUE
#include <core/containers/vector.h>
#include <domain/tissue/expect.h>
#include <domain/tissue/synthetic.h>
#include <domain/tissue/tissue_file.h>
#include <domain/tissue/validate.h>
#include <foundation/io/vfs.h>
#endif

namespace engine::content {

namespace {

constexpr int k_exit_ok = 0;
constexpr int k_exit_error = 1;
constexpr int k_exit_usage = 2;

const char* k_tissue_usage =
    "usage: engine-content tissue <command> ...\n"
    "  import <interchange.json> <out.tissue>   check sizes and SHA-256s, write the container\n"
    "  info <file.tissue>                       header, sections and the definition's outline\n"
    "  validate <file.tissue|interchange.json>  every validator row as one JSON line; exit 1 when\n"
    "                                           an error row fails\n"
    "      --no-modes   skip transferring each binding under the other two normal modes\n"
    "      --expect <expected.json>\n"
    "                   the fixture mode: exit 0 exactly when the rows that fail or are skipped\n"
    "                   are the ones the file declares, whatever their severity; every\n"
    "                   difference is printed to stderr and in the line's \"expect\"\n"
    "      --write-expect <expected.json>\n"
    "                   write the declaration this run would match, for review; exit 0\n"
    "  report <file.tissue|interchange.json>    the rows and the numbers they stand on\n"
    "      --no-modes   as above\n"
    "  example <directory>                      write the synthetic definition as\n"
    "                                           <directory>/synthetic.json with its blocks and\n"
    "                                           <directory>/synthetic.tissue, and its ten-node\n"
    "                                           reference body as synthetic-quadratic.*\n";

int usage(const char* message) {
  if (message != nullptr) std::fprintf(stderr, "engine-content tissue: %s\n", message);
  std::fputs(k_tissue_usage, stderr);
  return k_exit_usage;
}

int failed(const std::string& message) {
  std::fprintf(stderr, "engine-content tissue: %s\n", message.c_str());
  return k_exit_error;
}

[[maybe_unused]] void print_json(const JsonValue& value) {
  std::string text = write_json(value, JsonWriteOptions{.pretty = false});
  text.push_back('\n');
  std::fwrite(text.data(), 1, text.size(), stdout);
}

#if ENGINE_CONTENT_TISSUE

bool ends_with(std::string_view text, std::string_view tail) {
  return text.size() >= tail.size() && text.substr(text.size() - tail.size()) == tail;
}

// A container, or an interchange when the path ends in .json.
bool load(const std::string& path, tissue::TissueFile& out, std::string& error) {
  if (ends_with(path, ".json")) return tissue::import_interchange(path, out, &error);
  return tissue::read_tissue_file(path, out, &error);
}

JsonValue outline(const tissue::TissueDefinition& d) {
  JsonValue out = JsonValue::object();
  out.set("format", JsonValue(d.format));
  out.set("name", JsonValue(d.name));
  const auto names = [](const auto& list) {
    JsonValue array = JsonValue::array();
    for (const auto& item : list)
      array.push_back(JsonValue(item.name));
    return array;
  };
  out.set("regions", names(d.regions));
  out.set("frames", names(d.frames));
  out.set("surfaces", names(d.surfaces));
  out.set("attachments", names(d.attachments));
  out.set("bindings", names(d.bindings));
  out.set("states", names(d.states));
  out.set("blocks", JsonValue(static_cast<u64>(d.blocks.size())));
  out.set("base_id", JsonValue(d.observation.base_id));
  out.set("accepted", JsonValue(d.observation.acceptance.accepted));
  return out;
}

int import_command(const std::string& input, const std::string& output) {
  tissue::TissueFile file;
  std::string error;
  if (!tissue::import_interchange(input, file, &error)) return failed(input + ": " + error);
  if (!tissue::write_tissue_file(output, file, &error)) return failed(output + ": " + error);
  std::string bytes;
  io::read_file(output, bytes);
  JsonValue out = JsonValue::object();
  out.set("input", JsonValue(input));
  out.set("output", JsonValue(output));
  out.set("bytes", JsonValue(static_cast<u64>(bytes.size())));
  out.set("blocks", JsonValue(static_cast<u64>(file.blocks.size())));
  out.set("definition", outline(file.definition));
  print_json(out);
  return k_exit_ok;
}

int info_command(const std::string& path) {
  std::string bytes;
  const io::Status status = io::read_file(path, bytes);
  if (status != io::Status::Ok)
    return failed("cannot read '" + path + "': " + io::status_name(status));
  const std::span<const u8> view(reinterpret_cast<const u8*>(bytes.data()), bytes.size());
  tissue::TissueFileInfo info;
  std::string error;
  if (!tissue::read_tissue_file_info(view, info, &error)) return failed(path + ": " + error);
  tissue::TissueFile file;
  if (!tissue::read_tissue_file_memory(view, file, &error)) return failed(path + ": " + error);
  JsonValue out = JsonValue::object();
  out.set("file", JsonValue(path));
  out.set("version", JsonValue(info.header.version));
  out.set("bytes", JsonValue(info.header.total_bytes));
  char hash[32];
  std::snprintf(hash, sizeof(hash), "%016llx",
                static_cast<unsigned long long>(info.header.content_hash));
  out.set("content_hash", JsonValue(hash));
  JsonValue sections = JsonValue::array();
  for (const tissue::TissueFileSectionInfo& s : info.sections) {
    JsonValue e = JsonValue::object();
    e.set("kind", JsonValue(s.kind));
    e.set("kind_name", JsonValue(s.kind_name));
    e.set("name", JsonValue(s.name));
    e.set("element_size", JsonValue(s.element_size));
    e.set("count", JsonValue(s.element_count));
    e.set("offset", JsonValue(s.offset));
    e.set("known", JsonValue(s.known));
    sections.push_back(std::move(e));
  }
  out.set("sections", std::move(sections));
  out.set("definition", outline(file.definition));
  JsonValue warnings = JsonValue::array();
  for (const std::string& w : file.warnings)
    warnings.push_back(JsonValue(w));
  out.set("warnings", std::move(warnings));
  print_json(out);
  return k_exit_ok;
}

struct ValidateArgs {
  bool report = false;
  bool modes = true;
  std::string expect;        // --expect: the fixture mode's declaration
  std::string write_expect;  // --write-expect: where to write the declaration this run matches
};

int validate_command(const std::string& path, const ValidateArgs& args) {
  // The declaration is read before the definition is validated, so a broken one costs nothing.
  tissue::TissueExpectation expectation;
  std::string error;
  if (!args.expect.empty() && !tissue::read_expectation(args.expect, expectation, &error))
    return failed(args.expect + ": " + error);
  tissue::TissueFile file;
  if (!load(path, file, error)) return failed(path + ": " + error);
  tissue::ValidateOptions options;
  options.compare_modes = args.modes;
  tissue::TissueReport result;
  tissue::validate_tissue(file, options, result);
  JsonValue out = args.report ? tissue::report_json(result) : tissue::rows_json(result);
  out.set("file", JsonValue(path));
  if (!args.write_expect.empty()) {
    const tissue::TissueExpectation written = tissue::expectation_from_report(
        result, "written by engine-content tissue validate --write-expect from " + path +
                    ": review it before it is a fixture's declaration");
    const io::Status status =
        io::write_file_atomic(args.write_expect, tissue::write_expectation(written));
    if (status != io::Status::Ok)
      return failed("cannot write '" + args.write_expect + "': " + io::status_name(status));
    out.set("expect_written", JsonValue(args.write_expect));
    out.set("expect_failures", JsonValue(static_cast<u64>(written.failures.size())));
    print_json(out);
    return k_exit_ok;
  }
  if (!args.expect.empty()) {
    tissue::ExpectationResult compared;
    tissue::compare_expectation(result, expectation, compared);
    JsonValue e = tissue::expectation_json(compared);
    e.set("file", JsonValue(args.expect));
    out.set("expect", std::move(e));
    print_json(out);
    const std::string text = tissue::expectation_text(compared);
    if (!text.empty()) std::fputs(text.c_str(), stderr);
    return compared.matched() ? k_exit_ok : k_exit_error;
  }
  print_json(out);
  if (!args.report && result.errors > 0) return k_exit_error;
  return k_exit_ok;
}

int example_command(const std::string& directory) {
  std::string error;
  if (io::make_directories(directory) != io::Status::Ok)
    return failed("cannot create '" + directory + "'");
  JsonValue out = JsonValue::object();
  for (const bool quadratic : {false, true}) {
    tissue::SyntheticOptions options;
    options.quadratic = quadratic;
    const tissue::SyntheticTissue synthetic = tissue::make_synthetic_tissue(options);
    const std::string stem = quadratic ? "synthetic-quadratic" : "synthetic";
    if (!tissue::export_interchange(synthetic.file, directory, stem, &error)) return failed(error);
    const std::string container = io::join_path(directory, stem + ".tissue");
    if (!tissue::write_tissue_file(container, synthetic.file, &error)) return failed(error);
    JsonValue e = JsonValue::object();
    e.set("interchange", JsonValue(io::join_path(directory, stem + ".json")));
    e.set("container", JsonValue(container));
    e.set("blocks", JsonValue(static_cast<u64>(synthetic.file.blocks.size())));
    e.set("definition", outline(synthetic.file.definition));
    if (quadratic) {
      out.set("quadratic", std::move(e));
    } else {
      for (auto [key, value] : e.as_object())
        out.set(key, value);
    }
  }
  print_json(out);
  return k_exit_ok;
}

#endif

}  // namespace

int tissue_command(int argc, char** argv) {
  if (argc < 3) return usage(nullptr);
  const std::string_view sub = argv[2];
  if (sub == "--help" || sub == "-h" || sub == "help") {
    std::fputs(k_tissue_usage, stdout);
    return k_exit_ok;
  }
#if ENGINE_CONTENT_TISSUE
  std::string positional[2];
  u32 count = 0;
  ValidateArgs args;
  for (int i = 3; i < argc; ++i) {
    const std::string_view a = argv[i];
    if (a == "--no-modes") {
      args.modes = false;
    } else if (a == "--expect" || a == "--write-expect") {
      if (i + 1 >= argc) return usage("--expect and --write-expect take a path");
      (a == "--expect" ? args.expect : args.write_expect) = argv[++i];
    } else if (!a.empty() && a[0] == '-') {
      return usage("unknown option");
    } else if (count < 2) {
      positional[count++] = std::string(a);
    } else {
      return usage("too many arguments");
    }
  }
  if (sub != "validate" && (!args.expect.empty() || !args.write_expect.empty()))
    return usage("--expect and --write-expect belong to validate");
  if (!args.expect.empty() && !args.write_expect.empty())
    return usage("--expect compares with a declaration and --write-expect writes one: not both");
  if (sub == "import") {
    if (count != 2) return usage("import takes an interchange JSON and an output path");
    return import_command(positional[0], positional[1]);
  }
  if (count != 1) return usage("the command takes one path");
  if (sub == "info") return info_command(positional[0]);
  if (sub == "validate") return validate_command(positional[0], args);
  if (sub == "report") {
    args.report = true;
    return validate_command(positional[0], args);
  }
  if (sub == "example") return example_command(positional[0]);
  return usage("unknown tissue command");
#else
  (void)sub;
  return failed(
      "this engine-content was built without the tissue capability (ENGINE_WITH_TISSUE=OFF, "
      "ENGINE_WITH_PHYSICS=OFF, or ENGINE_MINIMAL=ON); configure with both on to read tissue "
      "definitions");
#endif
}

}  // namespace engine::content
