# Input maps

Action maps (`input::ActionMap`, [input](../../docs/subsystems/input.md)) as data: the bindings
an application ships, in the canonical JSON `ActionMap::to_json` writes, so a player's rebind is a
diff of one of these files.

## `engine-view.json` — the interactive camera's bindings

What `engine-view --interactive` flies and walks with unless `--input-map <file>` names another
([apps](../../docs/subsystems/apps.md#--interactive-a-camera-somebody-flies)): the camera's eight
actions — `move`, `lift`, `look`, `turn`, `fast`, `slow`, `capture` and `marker` — then the
time-lapse's four keys (`sun_slower`, `sun_faster`, `dunes_slower`, `dunes_faster`, revision 2),
then the walk mode's two (`walk` on F and `jump` on Space and a pad's South, revision 3,
[apps](../../docs/subsystems/apps.md#walking)), on the keyboard and mouse and on a gamepad. Key
codes are SDL scancodes, gamepad codes `window::GamepadButton` and `window::GamepadAxis`, and a
pointer axis is `input::MouseAxisCode`.

**This file is `view::default_fly_map()`, byte for byte** — the latest revision — and
`apps/engine_view/tests/fly_tests.cpp` fails when the two differ, writing the map the file should
be into its kept scratch directory. The map is compiled into engine-view as well, because a binary
on a machine with no checkout — a test bundle, a packaged build — still has to be flyable; the test
is what keeps the compiled copy and the readable one the same map.

**Changing it moves its hash, and a log names the hash it was recorded against.** A replay under
another map is refused (`input::InputLog::check_map`), because the same key would mean another
action. That includes reordering: the hash covers the actions and their bindings in order. So a new
binding is appended to its action, a new action is appended to the list, and the new list is a new
**revision** of the map (`view::k_fly_map_revision`): engine-view keeps every earlier revision and
replays a log with the one whose hash it names (`view::default_fly_map_for`), so a session recorded
before a revision replays exactly as it was flown, and the keys the later revision added press
nothing in it. A player's own map is not a revision and still needs `--input-map`.
