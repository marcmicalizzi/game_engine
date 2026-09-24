#include "luau_check.h"

#include <Luau/BuiltinDefinitions.h>
#include <Luau/Error.h>
#include <Luau/Frontend.h>
#include <algorithm>
#include <optional>
#include <string>

namespace engine::scripting::test {

namespace {

// One module, "main", whose source is the script under test.
struct SingleFileResolver : Luau::FileResolver {
  std::string source;

  std::optional<Luau::SourceCode> readSource(const Luau::ModuleName& name) override {
    if (name != "main") return std::nullopt;
    return Luau::SourceCode{source, Luau::SourceCode::Module};
  }

  std::optional<Luau::ModuleInfo> resolveModule(const Luau::ModuleInfo*, Luau::AstExpr*,
                                                const Luau::TypeCheckLimits&) override {
    return std::nullopt;  // scripts have no require
  }

  std::string getHumanReadableModuleName(const Luau::ModuleName& name) const override {
    return name;
  }

  std::optional<std::string> getEnvironmentForModule(const Luau::ModuleName&) const override {
    return std::nullopt;
  }
};

struct StrictConfigResolver : Luau::ConfigResolver {
  Luau::Config config;

  StrictConfigResolver() { config.mode = Luau::Mode::Strict; }

  const Luau::Config& getConfig(const Luau::ModuleName&,
                                const Luau::TypeCheckLimits&) const override {
    return config;
  }
};

Diagnostic diagnostic_of(const Luau::TypeError& error) {
  Diagnostic d;
  d.line = static_cast<unsigned>(error.location.begin.line + 1);
  d.message = Luau::toString(error);
  return d;
}

}  // namespace

CheckResult check_luau(std::string_view definitions, std::string_view source) {
  CheckResult result;
  SingleFileResolver files;
  files.source.assign(source.data(), source.size());
  StrictConfigResolver config;
  Luau::FrontendOptions options;
  Luau::Frontend frontend(Luau::SolverMode::New, &files, &config, options);

  Luau::registerBuiltinGlobals(frontend, frontend.globals);
  if (!definitions.empty()) {
    const Luau::LoadDefinitionFileResult loaded = frontend.loadDefinitionFile(
        frontend.globals, frontend.globals.globalScope, definitions, "@engine",
        /* captureComments */ false, /* typeCheckForAutocomplete */ false);
    result.definitions_loaded = loaded.success;
    for (const Luau::ParseError& error : loaded.parseResult.errors) {
      result.definition_errors.push_back(
          Diagnostic{static_cast<unsigned>(error.getLocation().begin.line + 1), error.what()});
    }
    if (loaded.module) {
      for (const Luau::TypeError& error : loaded.module->errors)
        result.definition_errors.push_back(diagnostic_of(error));
    }
  }
  Luau::freeze(frontend.globals.globalTypes);

  const Luau::CheckResult checked = frontend.check("main");
  for (const Luau::TypeError& error : checked.errors)
    result.diagnostics.push_back(diagnostic_of(error));
  std::stable_sort(result.diagnostics.begin(), result.diagnostics.end(),
                   [](const Diagnostic& a, const Diagnostic& b) { return a.line < b.line; });
  return result;
}

std::string describe(const std::vector<Diagnostic>& diagnostics) {
  std::string out;
  for (const Diagnostic& d : diagnostics) {
    out += std::to_string(d.line);
    out += ": ";
    out += d.message;
    out += '\n';
  }
  return out;
}

}  // namespace engine::scripting::test
