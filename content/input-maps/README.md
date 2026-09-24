# Input maps

Action maps (`input::ActionMap`, [input](../../docs/subsystems/input.md)) as data: the bindings
an application ships, in the canonical JSON `ActionMap::to_json` writes, so a player's rebind is a
diff of one of these files.

## `engine-view.json` — the interactive camera's bindings

What `engine-view --interactive` flies with unless `--input-map <file>` names another
([apps](../../docs/subsystems/apps.md#--interactive-a-camera-somebody-flies)): eight actions —
`move`, `lift`, `look`, `turn`, `fast`, `slow`, `capture` and `marker` — on the keyboard and mouse
and on a gamepad. Key codes are SDL scancodes, gamepad codes `window::GamepadButton` and
`window::GamepadAxis`, and a pointer axis is `input::MouseAxisCode`.

**This file is `view::default_fly_map()`, byte for byte**, and
`apps/engine_view/tests/fly_tests.cpp` fails when the two differ, writing the map the file should
be into its kept scratch directory. The map is compiled into engine-view as well, because a binary
on a machine with no checkout — a test bundle, a packaged build — still has to be flyable; the test
is what keeps the compiled copy and the readable one the same map.

**Changing it stops every recorded session replaying.** A session log carries the hash of the map
it was recorded against, and a replay under another is refused (`input::InputLog::check_map`),
because the same key would mean another action. That includes reordering: the hash covers the
actions and their bindings in order. So a new binding is appended to its action, a new action is
appended to the list, and the change says in its commit message that older sessions need the old
file passed back with `--input-map`.
