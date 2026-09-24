#pragma once

// Luau's type checker, driven directly: the analysis library behind `luau-analyze`, with a
// definition file loaded into its global scope first. Stock `luau-analyze` has no flag for a
// definition file (0.739), so this is how a script is checked against the typed API the scripting
// capability emits (type_definitions.h). Test-only: the engine library links the VM and the
// compiler, never the analyser.

#include <string>
#include <string_view>
#include <vector>

namespace engine::scripting::test {

struct Diagnostic {
  unsigned line = 0;  // 1-based
  std::string message;
};

struct CheckResult {
  bool definitions_loaded = true;  // false: the definition file itself did not check
  std::vector<Diagnostic> definition_errors;
  std::vector<Diagnostic> diagnostics;  // the script's type errors, in source order
};

// Checks `source` in strict mode (the analyser's --mode=strict) with `definitions` loaded as a
// definition file, or with only Luau's builtins when `definitions` is empty.
CheckResult check_luau(std::string_view definitions, std::string_view source);

// "line: message" per diagnostic, one per line, for test output and the E7 write-up.
std::string describe(const std::vector<Diagnostic>& diagnostics);

}  // namespace engine::scripting::test
