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
//   tissue report <file.tissue|interchange.json>    the rows and every number they stand on
//   tissue example <directory>                      the synthetic definition as an interchange and
//                                                   a container: a worked example of the format
#include "content_commands.h"

#include <core/base/types.h>
#include <core/json/json.h>

#include <cstdio>
#include <string>
#include <string_view>

#if ENGINE_CONTENT_TISSUE
#include <core/containers/vector.h>
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
    "  report <file.tissue|interchange.json>    the rows and the numbers they stand on\n"
    "      --no-modes   as above\n"
    "  example <directory>                      write the synthetic definition as\n"
    "                                           <directory>/synthetic.json with its blocks, and\n"
    "                                           <directory>/synthetic.tissue\n";

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

int validate_command(const std::string& path, bool report, bool modes) {
  tissue::TissueFile file;
  std::string error;
  if (!load(path, file, error)) return failed(path + ": " + error);
  tissue::ValidateOptions options;
  options.compare_modes = modes;
  tissue::TissueReport result;
  tissue::validate_tissue(file, options, result);
  JsonValue out = report ? tissue::report_json(result) : tissue::rows_json(result);
  out.set("file", JsonValue(path));
  print_json(out);
  if (!report && result.errors > 0) return k_exit_error;
  return k_exit_ok;
}

int example_command(const std::string& directory) {
  const tissue::SyntheticTissue synthetic = tissue::make_synthetic_tissue();
  std::string error;
  if (io::make_directories(directory) != io::Status::Ok)
    return failed("cannot create '" + directory + "'");
  if (!tissue::export_interchange(synthetic.file, directory, "synthetic", &error))
    return failed(error);
  const std::string container = io::join_path(directory, "synthetic.tissue");
  if (!tissue::write_tissue_file(container, synthetic.file, &error)) return failed(error);
  JsonValue out = JsonValue::object();
  out.set("interchange", JsonValue(io::join_path(directory, "synthetic.json")));
  out.set("container", JsonValue(container));
  out.set("blocks", JsonValue(static_cast<u64>(synthetic.file.blocks.size())));
  out.set("definition", outline(synthetic.file.definition));
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
  bool modes = true;
  for (int i = 3; i < argc; ++i) {
    const std::string_view a = argv[i];
    if (a == "--no-modes") {
      modes = false;
    } else if (!a.empty() && a[0] == '-') {
      return usage("unknown option");
    } else if (count < 2) {
      positional[count++] = std::string(a);
    } else {
      return usage("too many arguments");
    }
  }
  if (sub == "import") {
    if (count != 2) return usage("import takes an interchange JSON and an output path");
    return import_command(positional[0], positional[1]);
  }
  if (count != 1) return usage("the command takes one path");
  if (sub == "info") return info_command(positional[0]);
  if (sub == "validate") return validate_command(positional[0], false, modes);
  if (sub == "report") return validate_command(positional[0], true, modes);
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
