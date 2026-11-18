# The hostile-input harness: adding a target

Roadmap T13, stage 0. The design, the inventory of every reader of bytes the engine did not write and the order they are taken in is [the fuzzing design](../experiments/fuzzing-design.md); this page is how the harness is used. What it costs the gate is in [what to run](what-to-run.md#the-hostile-input-cases).

**A target is one function, run two ways.** `Verdict one(std::span<const u8>)` hands the bytes to a reader's memory entry and checks the reader's contract. Under CTest it is a doctest case, `fuzz: <target>`, in the reader's own test executable, fed by deterministic mutators a counted number of times, so `tools/dev.ps1 test -Affected` selects it with its module and nothing new is scheduled. Under the `linux-clang-fuzz` preset the same file is compiled again as `LLVMFuzzerTestOneInput`, into one libFuzzer executable a target. A finding in either is a case in both.

The harness is test support, not a module: three headers in `tests/support/`, on every test target's include path the way `test_temp_dir.h` is.

| File | What |
|---|---|
| `fuzz_harness.h` | What both drivers share: `TargetSpec`, `Verdict`, the generator, the byte and JSON mutators, `generate()`. No doctest. |
| `fuzz_runner.h` | The in-tree driver: the corpus, the oracles, the kept inputs, the replay, the child runs. |
| `fuzz_target.h` | `ENGINE_FUZZ_TARGET(name, spec)`: the doctest case, or libFuzzer's entry point. |
| `cmake/EngineFuzz.cmake` | `engine_module_fuzz()` and the `ENGINE_FUZZ` switch. |

## Adding a target

1. Write `tests/fuzz_<target>.cpp` in the reader's module. It defines the function and a `constexpr TargetSpec` naming it, and ends with `ENGINE_FUZZ_TARGET(<target>, <spec>)`. `core/json/tests/fuzz_parse_json.cpp` is the model, short enough to copy.
2. Make the function's verdict the reader's contract, not "did not crash" (the crash is the harness's to see): **a refusal leaves the output empty and the message non-empty; an accepted input that has a writer reads back from its own writing to the same value.** A reader with no writer checks what it can of the first half.
3. In the module's `CMakeLists.txt`, after its `engine_module_tests()`:
   ```cmake
   engine_module_fuzz(NAME <module> TARGETS <target>=tests/fuzz_<target>.cpp [DEPS <module>...])
   ```
4. Add `content/fuzz/<target>/` with `seeds/` (text only, a few small files that between them reach every branch of the format you know of), `regress/NOTES.txt` and `known-failures.txt`, both of which may start with comments only. A binary format commits no seed: its seeds are made at test time by the engine's own writers and handed over as `builtin_seeds`, or the target function builds them (the design's "Corpora").
5. Set `byte_iterations` and `json_iterations` so the case stays under **10 s in a serial `msvc-debug` run**, measured, and say the measurement in the commit. Counts are what is committed, never a time.

## The oracles

Every input is held to all of these:

- **A crash or fault.** The platform's fault report and doctest's crash report end the run; the case's INFO context names the input, and a listener keeps its bytes, best effort, since it runs where the fault left the process.
- **An `ENGINE_ASSERT`.** A reader never asserts on external input, so in a debug build an assert reached from bytes is a finding, not a test bug. The harness's assert hook keeps the input, prints one line naming it and ends the process with exit code 86.
- **An allocation past the cap**, `cap_floor + cap_per_byte × size` (1 MiB and 64 by default), against the engine allocator's counters. The counters only grow, so the watch makes the peak exact by holding a ballast through the call when an earlier case left more headroom than the cap; when the ballast would pass 8 MiB the input's check is counted as blind and the case's summary line says how many were. The standard library's `new` and a third-party `malloc` are not seen here; under libFuzzer they are `-malloc_limit_mb`'s.
- **A hang.** A watchdog thread names an input that has run for 30 s, keeps it, and ends the process with exit code 87. Its only job is to name the input: counted mutation keeps an honest run far below it, and nothing waits for it.
- **A broken contract**: the target's own `Verdict`.

A broken contract or an allocation past the cap fails the case and the run goes on, to at most eight findings. The other three end the process.

## Inputs are named, kept and replayed

An input is `<seed file>#<iteration>` (iteration 0 is the seed itself, then the byte layer, then the JSON layer) or `regress/<file>`, and is a function of the target's name, the seed's bytes and the iteration and of nothing else: the generator is SplitMix64, written in the harness, and every choice is integer arithmetic on it, because the standard leaves its distributions' algorithms to each library. A failing input is written into the case's `TempDir`, which is then kept and named on stderr, as `<target>-<id>.input`. To run one input alone, anywhere:

```powershell
$env:ENGINE_FUZZ_REPLAY = 'parse_json|numbers.seed#517'
build/msvc-debug/bin/engine_json_tests --test-case="fuzz: parse_json"
```

`ENGINE_FUZZ_KEEP_DIR` names where a finding is written instead of the case's own scratch; it is what a parent case gives the child it starts, and nothing else should set it.

## Regressions

A finding becomes a regression in the commit that mends it, failing before the mend: its text input under `content/fuzz/<target>/regress/`, with a line in `regress/NOTES.txt`, `<file> <roadmap id>: what it found`. Every regression is replayed before any mutation, on every build. The case refuses an input with no line and a line with no input. A binary format's regression is a doctest case that builds the input from the writer's fixture and the minimized edit, since no binary file is committed.

## Known failures

`content/fuzz/<target>/known-failures.txt` lists inputs known to fail while another row mends the reader: `<input id> <roadmap id> <what fails>`. The case leaves them out and says how many it left out. It also replays each one alone in a child process of the same executable, and **fails while an entry's input no longer fails**, so the line has to go in the commit that mends it and the list cannot outlive what it lists. An entry naming an input the target does not have (a seed renamed, an iteration past the counts) is refused. A change to a mutator renumbers what every iteration is, so an entry by iteration survives only until the next such change: copy a known failure's input into `regress/` and list it by that name when it has to last.

## What the harness's own tests pin

They are in `core/json/tests/fuzz_harness_tests.cpp`, beside the first target, since `core/json` is the lowest module the harness needs.

- **A planted target that asserts on one input fails its case.** `planted_assert` asserts on the input `planted`; the test runs it in a child and holds the child to exit code 86, the line naming `planted_assert builtin-0#0`, the kept bytes, and the scratch root holding nothing of the child's: a process the harness ends with `std::_Exit` never returns through its test main, so the harness removes the main's own directories first and names each on stderr, and the case checks they are gone. Three more planted targets do the same for a broken contract, an allocation past the cap and a hang (with a watchdog of 1 s), and one checks the replay. Debug builds only for the assert: release compiles `ENGINE_ASSERT` out.
- **The same seed gives the same inputs on every compiler.** The first thousand inputs of two fixed seeds, hashed, against `k_first_thousand_inputs`, and SplitMix64 against the reference implementation's first three outputs from 0. The hash was taken on MSVC; a GCC or Clang run that differs is a finding in the harness (a choice made through something the compilers disagree on), never a reason to take the value again.

## The libFuzzer build

`linux-clang-fuzz` is `linux-clang-asan` (Clang, Debug so asserts are live, ASan and UBSan) with `ENGINE_FUZZ` on and the x86-64-v2 baseline. `ENGINE_FUZZ` adds `-fsanitize=fuzzer-no-link` to every C++ target and builds `build/linux-clang-fuzz/fuzz/engine_fuzz_<target>`, linked with `-fsanitize=fuzzer`; the tests build as well and run sanitized. libFuzzer comes with Clang's runtime (`libclang-rt-18-dev` in the container image), so nothing is vendored or fetched. One run, in the container:

```sh
build/linux-clang-fuzz/fuzz/engine_fuzz_parse_json -max_len=65536 -timeout=10 -malloc_limit_mb=1024 \
  -max_total_time=60 <a scratch corpus directory> content/fuzz/parse_json/seeds
```

The first directory is where libFuzzer writes what it grows; it is scratch, outside the repository, and never committed. Where and how often this runs beyond a check that the build works is the owner's open question (the design's open question 2), and nothing in the tree schedules it.
