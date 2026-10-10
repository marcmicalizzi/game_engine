#pragma once

// engine-host --console (docs/subsystems/apps.md, "The console"; plan 06 §6.3): the host runs one
// Luau program — a coding agent's script — against its own dispatcher, instead of serving JSON-RPC
// on stdin. The program calls methods with `engine.call(method, params)` or `engine.doc.apply{...}`
// and every call is dispatched exactly as a request on the wire would be: the same attribution
// from --actor, --role and --task, the same role gate and read-only rule, the same journal. It
// mutates only by calling methods, which is the sanctioned path; it is a client, not a gameplay
// script (docs/subsystems/scripting.md, "Programs: the console").
//
// The VM is foundation/scripting's (ScriptContext::run_program), linked when the configuration has
// the scripting capability (ADR-0027); without it --console says so and exits 2.

#include <core/base/types.h>
#include <core/containers/vector.h>

#include <string>

namespace engine::protocol {
class Dispatcher;
}

namespace engine::host {

// The program's step budget, counted over the whole run: Luau's safepoints (loop back edges,
// calls, returns), so it stops a runaway loop deterministically and never counts the time a method
// takes. A hundred million is about a second of pure Luau in a debug build — a guard against
// `while true do end`, not a quota on an honest script — and --step-budget changes it (0: none).
inline constexpr u64 k_console_step_budget = 100'000'000;
// The VM heap, MiB: room for a few hundred thousand objects' worth of results.
inline constexpr u64 k_console_memory_mb = 512;

// Exit codes: the program ran to its end; it failed (a compile error, an uncaught error — a
// method's included — or a limit), with the chunk and line on stderr; the console could not start
// (a script that does not read, or a build without the scripting capability).
inline constexpr int k_console_exit_ok = 0;
inline constexpr int k_console_exit_failed = 1;
inline constexpr int k_console_exit_usage = 2;

struct ConsoleOptions {
  std::string script;        // the program's file; empty reads it from stdin until EOF
  Vector<std::string> args;  // the program's `...`, everything after `--`
  u64 step_budget = k_console_step_budget;
  u64 memory_mb = k_console_memory_mb;
};

// Runs the program and returns the process's exit code. stdout is the program's `print`; the
// host's log stays where it is (warnings and up on stderr), and a failure is one line on stderr.
int run_console(protocol::Dispatcher& dispatcher, const ConsoleOptions& options);

}  // namespace engine::host
