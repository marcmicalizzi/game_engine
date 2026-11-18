# The hostile-input harness's corpora

One directory a target of the harness (roadmap T13), named for the target: `parse_json/` is the
doctest case "fuzz: parse_json" in `engine_json_tests` and, under the `linux-clang-fuzz` preset,
the libFuzzer executable `engine_fuzz_parse_json`. How a target is added and what each file is for:
[docs/ci/fuzzing.md](../../docs/ci/fuzzing.md).

| Path | What |
|---|---|
| `<target>/seeds/` | Text inputs the mutators start from, any name, read in name order. At most 16 KiB a seed and 256 KiB a target. |
| `<target>/regress/` | Inputs that found something, replayed before any mutation on every build, with `NOTES.txt` saying what each found. |
| `<target>/known-failures.txt` | Inputs known to fail, each with the roadmap row that will mend it. |

The whole directory stays under 2 MiB; the cases refuse a corpus past any of the three caps. Seeds
and regressions are text, and `.gitattributes` keeps their bytes exactly as committed, since an
input is named by its seed and an iteration and reproduces only from the same bytes. No binary
input is committed: a binary format's seeds are made at test time by the engine's own writers
(docs/experiments/fuzzing-design.md, "Corpora").
