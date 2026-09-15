# tunables (foundation)

**Purpose.** The registry of named run-time parameters (ADR-0011; docs/plan/08-toolchain.md §8.7; 11 §11.8): every hardware-selected knob a kernel or system reads (batch size, prefetch distance, spin count, thread count, workgroup size, budget) is a tunable with a default, a range or choice list, and documentation. Kernels read once outside hot loops; config files, command lines, the bench harness, and later the engine protocol set values by name. It is Stage 2 of hardware characterization; the calibration runner (Stage 3) is built only when experiment E3 fires.

**Model.**
- `Int`, `Float`, `Bool`, `Enum<E>` are static objects defined at namespace scope and registered in their constructor; they unregister on destruction. Names are dotted lower case and unique (verified).
- `get()` is one relaxed atomic load. `set()` validates (range, finite, known choice) and returns false without changing anything when the value is rejected; an accepted change bumps the tunable's `version()` and the registry `generation()`.
- Text forms (`set_from_text`, `append_value`) serve command lines and tables; JSON forms (`set_from_json`, `value_json`, `describe()`) serve files and the protocol. `describe()` yields `{name, kind, value, default, doc, modified, version}` plus `min`/`max` or `choices`.
- Registry: `find`, `first`/`next` chain, `count`, `generation`; `apply_overrides("a.b=4, c=on")`; `load_json`/`save_json` (name → value objects, only modified values by default); `describe_all`; `load_file`/`save_file`.

**Invariants (tested).**
- A rejected set leaves the value and version untouched; setting the current value again bumps nothing.
- Overrides and JSON loads apply every valid entry and report every problem; they never stop at the first.
- `save_json()` then `load_json()` restores exactly the modified values; `save_json(false)` lists every tunable.
- Enum choices are dense from zero and the default is one of them (verified at construction).

**Rules for users.**
- Hardware-selected dimensions are tunables. Data-selected dimensions (formats, modes, feature flags, SIMD width via ISPC) are compile-time variants behind dispatch tables, never tunables (11 §11.4).
- Read a tunable once per job or per frame, into a local, and pass it down. Reading inside an inner loop is a review finding.
- Core-layer modules do not depend on this module; they take config structs (`JobSystemConfig`) that the application layer fills from tunables.

**Public API.** `foundation/tunables/tunables.h`.

**Depends on.** `base`, `containers`, `json`, `platform`.

**Testing.** `tools/dev.ps1 test -Filter tunables`.

**Performance notes.** Cold except for `get()`. Lookup by name is a linear scan over the registry chain under a spinlock; it happens on configuration paths only.
