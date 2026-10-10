# Console scripts

Luau programs for `engine-host --console` and `engine-cli --console` ([apps](../../docs/subsystems/apps.md#the-console-luau-scripts-over-the-protocol)): each one drives a live engine-host through the protocol, one method call at a time, and prints what it found. They are the fixtures of `apps/engine_cli/tests/console_tests.cpp` and, because each states its own command line, worked examples of what an agent at the keyboard writes.

| Script | Arguments | What it shows |
|---|---|---|
| `edit_session.luau` | a document directory | creates a document, applies two transactions with a rationale each, reads the journal back, undoes one, lists what is left, calls a method by name and catches an unknown one |
| `capture.luau` | an output directory | loads the procedural grid, captures it, prints the color file's URI; on a machine with no usable GPU prints `render unavailable` and ends cleanly |
| `forbidden_write.luau` | a document directory, optionally `uncaught` | under `--roles content/roles/roles.json --role qa`: the write is refused (1008, `reason` `method`) and the refusal reaches `pcall`; reads still work; `uncaught` makes the write unprotected and the run fails at that line |
| `runaway.luau` | none | loops forever, catching its own errors; the step budget stops it anyway |

Every check in them is an `assert`, so a wrong answer ends the run at the line that noticed it with exit code 1; the test reads the exit code and the printed lines.
