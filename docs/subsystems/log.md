# log (core)

**Purpose.** Structured logging for humans, tools, and agents (docs/plan/02-architecture.md §2.3, 09 §9.3). A record is a static message plus typed fields, never a formatted string, so the same call renders as a readable text line for a developer and as a JSON object an agent can query by key. Telemetry (counters, spans, frame reports) is a separate foundation-layer module that will emit through the same sinks.

**Model.**
- `Category`: a named, module-scoped source with its own run-time minimum level, registered at static initialization (`ENGINE_LOG_CATEGORY_DEFINE(ident, "dotted.name")`) and unregistered on destruction. A global floor applies on top of every category.
- `Field`: `key` plus one of bool, signed, unsigned, float, string, pointer, or 128-bit hex (anything shaped like `Id128`). 40 bytes, built on the caller's stack, valid for the duration of the emit call.
- `Record`: monotonic and wall time, category, message, fields, source location, thread index, level.
- `Sink`: receives records from every thread concurrently; has its own minimum level. `StreamSink` (text or JSON lines to a `FILE*`), `RingSink` (in-memory, copies strings, sequence numbers for since-queries). With no sink registered, records go to stderr as text.

**Invariants.**
- Field arguments are evaluated only when the record passes the category and global filters; records below `ENGINE_LOG_MIN_LEVEL` (0 trace in Debug, 1 debug otherwise; settable from CMake) compile to nothing.
- `Fatal` ignores level filters, flushes every sink, and terminates through the assertion path. `install_assert_hook()` routes `ENGINE_VERIFY` failures the other way, into the sinks as Fatal records in `engine.assert`.
- Level specs (`"info,jobs=debug,render.*=trace"`) apply to categories that exist and to categories registered later; malformed entries are reported, not ignored.
- `RingSink` sequences increase by exactly one per record; `first_sequence()`/`next_sequence()` bound what is retained.
- Formatting never allocates per record beyond a thread-local scratch string that reaches steady state.

**Formats.**
- Text: `HH:MM:SS.mmm level category [tN] message key=value ...`, UTC. Strings containing spaces, quotes, `=`, or control characters are JSON-quoted; integers and floats print shortest round-trip; pointers as `0x` + 16 hex digits; 128-bit ids as 32 hex digits.
- JSON lines, one object per record: `{"t":<monotonic ns>,"wall":<unix µs>,"level":"info","cat":"jobs","thread":3,"msg":"...","file":"...","line":12,"fields":{...}}`. `file`/`line` are omitted when unknown and `fields` when empty. Non-finite floats become `null`; pointers and ids are strings. Both formats are pinned by test.

**Public API.** `core/log/log.h`: `Level`, `level_name`, `parse_level`; `Field`, `field(...)`, `field_pointer`, `field_hex128`; `Category`, `first_category`, `find_category`, `global_min_level`, `set_global_min_level`, `apply_level_spec`, `reset_levels`; `Record`, `format_text`, `format_json_line`; `Sink`, `add_sink`, `remove_sink`, `sink_count`, `flush`, `emit`, `fatal_terminate`, `install_assert_hook`; `StreamSink`, `RingSink`; macros `ENGINE_LOG_CATEGORY_DECLARE/DEFINE`, `ENGINE_LOG_TRACE/DEBUG/INFO/WARN/ERROR/FATAL`, `ENGINE_LOG_AT`.

**Usage.**
```cpp
namespace engine::streaming {
ENGINE_LOG_CATEGORY_DEFINE(log_streaming, "streaming");
}
ENGINE_LOG_INFO(log_streaming, "tile loaded", log::field("tile", tile.id), log::field("ms", sw.elapsed_ms()));
```
Messages are short static phrases in lower case; data goes in fields, never interpolated into the message. Category names are dotted lower case, one per module or subsystem.

**Depends on.** `base`, `containers`, `time`, `platform`.

**Testing.** `tools/dev.ps1 test -Filter log`. Covers level parsing and specs (including late-registered categories), field typing through the ring sink, lazy evaluation of disabled records, per-sink filtering, ring eviction and since-queries, the pinned text and JSON formats, a JSON-lines file round trip, and eight threads emitting concurrently.

**Performance notes.** The enabled check is two relaxed atomic loads. An enabled record costs the field array on the stack, two clock reads, a shared lock on the sink table, and one formatted write per sink. Logging inside per-frame hot loops is a profiler finding, not a logging feature; trace-level records in such places are compiled out of release builds by `ENGINE_LOG_MIN_LEVEL`.
