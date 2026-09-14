// schemac: compiles .schema files into C++ types with reflection tables, JSON Schema, and
// Markdown documentation. See schemas/README.md for the language.
//
//   schemac --out <dir> [--schema-root <dir>] <file.schema>...
//
// Outputs, per input stem:
//   <out>/include/schemas/<stem>.h
//   <out>/src/<stem>.cpp
//   <out>/json/<stem>.schema.json
//   <out>/docs/<stem>.md
//
// Imported schemas not listed on the command line are loaded from --schema-root for type
// resolution only (no output is produced for them).

#include "model.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

bool read_file(const fs::path& path, std::string& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  std::ostringstream ss;
  ss << in.rdbuf();
  out = ss.str();
  return true;
}

// Writes only when the content changed, so downstream builds do not rebuild needlessly.
bool write_if_changed(const fs::path& path, const std::string& content) {
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  std::string existing;
  if (read_file(path, existing) && existing == content) return true;
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) return false;
  out << content;
  return static_cast<bool>(out);
}

int usage() {
  std::fprintf(stderr, "usage: schemac --out <dir> [--schema-root <dir>] <file.schema>...\n");
  return 2;
}

}  // namespace

int main(int argc, char** argv) {
  fs::path out_dir;
  fs::path schema_root;
  std::vector<fs::path> inputs;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--out" && i + 1 < argc) {
      out_dir = argv[++i];
    } else if (arg == "--schema-root" && i + 1 < argc) {
      schema_root = argv[++i];
    } else if (!arg.empty() && arg[0] == '-') {
      return usage();
    } else {
      inputs.emplace_back(arg);
    }
  }
  if (out_dir.empty() || inputs.empty()) return usage();

  schemac::Model model;
  std::set<std::string> loaded_stems;
  std::vector<std::string> output_stems;

  // Load inputs, then pull in imports from the schema root transitively.
  std::vector<fs::path> queue = inputs;
  size_t input_count = inputs.size();
  for (size_t qi = 0; qi < queue.size(); ++qi) {
    const fs::path& path = queue[qi];
    std::string text;
    if (!read_file(path, text)) {
      std::fprintf(stderr, "schemac: cannot read %s\n", path.string().c_str());
      return 1;
    }
    schemac::SchemaFile file;
    std::string error;
    if (!schemac::parse_schema(path.generic_string(), text, file, error)) {
      std::fprintf(stderr, "%s: error: %s\n", error.substr(0, error.find(": ")).c_str(),
                   error.substr(error.find(": ") + 2).c_str());
      return 1;
    }
    if (!loaded_stems.insert(file.stem).second) {
      std::fprintf(stderr, "%s: error: schema stem '%s' loaded twice\n", path.string().c_str(), file.stem.c_str());
      return 1;
    }
    if (qi < input_count) output_stems.push_back(file.stem);
    for (const std::string& imp : file.imports) {
      if (loaded_stems.count(imp) != 0) continue;
      bool queued = false;
      for (const fs::path& q : queue) {
        if (q.stem().string() == imp) queued = true;
      }
      if (queued) continue;
      if (schema_root.empty()) {
        std::fprintf(stderr, "%s: error: import '%s' needs --schema-root\n", path.string().c_str(), imp.c_str());
        return 1;
      }
      queue.push_back(schema_root / (imp + ".schema"));
    }
    model.files.push_back(std::move(file));
  }

  std::vector<std::string> errors;
  if (!schemac::resolve(model, errors)) {
    for (const std::string& e : errors) {
      const size_t sep = e.find(": ");
      std::fprintf(stderr, "%s: error: %s\n", e.substr(0, sep).c_str(), sep == std::string::npos ? e.c_str() : e.substr(sep + 2).c_str());
    }
    return 1;
  }

  for (const schemac::SchemaFile& file : model.files) {
    bool is_output = false;
    for (const std::string& s : output_stems) {
      if (s == file.stem) is_output = true;
    }
    if (!is_output) continue;
    const bool ok = write_if_changed(out_dir / "include" / "schemas" / (file.stem + ".h"), schemac::emit_cpp_header(model, file)) &&
                    write_if_changed(out_dir / "src" / (file.stem + ".cpp"), schemac::emit_cpp_source(model, file)) &&
                    write_if_changed(out_dir / "json" / (file.stem + ".schema.json"), schemac::emit_json_schema(model, file)) &&
                    write_if_changed(out_dir / "docs" / (file.stem + ".md"), schemac::emit_docs(model, file));
    if (!ok) {
      std::fprintf(stderr, "schemac: failed to write outputs for %s\n", file.stem.c_str());
      return 1;
    }
  }
  return 0;
}
