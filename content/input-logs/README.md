# Device log corpus

Recordings of real input devices, as `input::InputLog` files. They are the only description this
repository has of hardware nobody working on it can plug in, so they are committed, replayed on
every build, and treated as fixtures rather than as samples: `foundation/input/tests/corpus_tests.cpp`
loads each one, checks what it says about its device, and replays it through an `InputState`.

Every file here was produced by `engine-input` (docs/subsystems/apps.md) on **2026-09-17**, on
Windows 11 with SDL 3.4.16, one command per device:

```bash
engine-input devices
engine-input probe --seconds 10 --log <device>.jsonl --events <device>.events.jsonl
```

During each ten seconds the owner swept every axis end to end, pressed and released every button
once, and pushed every hat to each of its four directions and back to centre, slowly and one
thing at a time so the indices read cleanly out of the file. (`--events` did not exist yet when
these were taken; the stream went through a shell redirect, and on PowerShell that wrote UTF-16
and broke every text tool that met it afterwards. That is why the option exists now.)

**The `.events.jsonl` streams are not committed.** They are the same events in a human-readable
per-event form, with device names and enum names attached, and they were how the devices were
read the first time; but they are three times the size, they are not a format anything loads,
and everything a test needs is in the `.jsonl` logs. What they said about names and slots is
written down here instead.

A log holds `[tick, source, code, value, device]` rows and nothing else: **no device names, and
no vendor or product ids**. The slot numbers below are the only handle a log has on a device, so
this file is where a slot is tied to a name — and the corpus test checks that every name in its
expectation table still appears here.

## The logs

### `f710.jsonl` — Logitech F710 Gamepad

A wireless pad on its own dongle, and the only device here that SDL has a gamepad mapping for,
so its events are the *named* sources: `gamepad_axis` and `gamepad_button`, on gamepad slot 0.

- 796 events, ticks 9 to 583 (at the probe's 60 Hz stamping).
- Four axes: 0 and 1 are the left stick, 2 and 3 the right. The two analog triggers were not
  moved during this recording and do not appear.
- Eight of its twelve buttons: South, East, West, North (codes 1–4) and the four d-pad
  directions (12–15). **The d-pad arrives as buttons**, not as a hat: SDL's mapping turns the
  pad's hat into `DpadUp`/`DpadDown`/`DpadLeft`/`DpadRight`, which is the whole point of having
  a mapping. No `joystick_hat` event is in this file.
- SDL called the device `XInput Controller #1` at the time, because the pad's switch was on
  XInput and SDL has no name for that mode. Its ids (`0x046D 0xC21F`) are in the name table in
  `foundation/window/src/window.cpp`, so it reports as *Logitech F710 Gamepad* now.

### `t16000m.jsonl` — Thrustmaster T.16000M

A flight stick. SDL has no mapping for it, so it is a **raw joystick** on joystick slot 0 with
bare indices: `joystick_axis`, `joystick_button`, `joystick_hat`.

- 907 events, ticks 15 to 596.
- Four axes (0–3): stick x and y, the twist rudder, and the throttle wheel on the base.
- Eleven of its sixteen buttons: 0, 1, 5, 6, 7, 8, 11, 12, 13, 14, 15.
- **One hat**, index 0, and the only hat in the corpus. Each hat event is recorded as four
  digital signals, one per cardinal direction, coded `input::hat_code(0, direction)` — so the
  file holds codes 1, 2, 4, and 8 and the event count is four times the hat movements.
- SDL named this one itself, from the device's own product string.

### `t300.jsonl` — Thrustmaster T300 RS, with its gear shift

**Two devices in one recording**, which is the point of keeping it: a rig is not one joystick.
Both are raw joysticks, on two slots of the same space.

- 2,073 events, ticks 5 to 591.
- **Joystick slot 0 — Thrustmaster T300 RS** (the wheel base), 2,062 events. Four axes:
  steering on axis 0, and the three pedals of the T3PA pedal set on axes 1, 2, and 3. A pedal
  rests at one end of its travel and so reads −1 at rest; that is the device's truth and the
  binding's `scale` to deal with, not the window layer's. Ten of its sixteen rim buttons
  (2–11), no hat.
  The Windows driver gave SDL no name for it at all: it enumerated as `44F B66E`, which is its
  USB vendor and product in hex. That is the case the name table in
  `foundation/window/src/window.cpp` exists for, and it reports as *Thrustmaster T300 RS* now.
- **Joystick slot 1 — Thrustmaster T500 RS Gear Shift**, 11 events. **Buttons only**: six of
  them (0, 1, 2, 3, 6, 7), one per gate, no axis and no hat. A gear is a button that stays
  down, which is why a shifter needs no analog anything.
  The device reports its own name with a typo in it — `Thustmaster T500 RS Gear Shift`, missing
  the *r* — and the name table corrects that by the misspelled string, because this probe
  recorded no ids for it to be keyed on. A `devices` line from this rig would let that row be
  replaced with a vendor/product one.
- A `XInput Controller #1` gamepad was attached during this probe and during the T.16000M one;
  it was never touched and contributed no events, so neither log holds a gamepad source.

Note that the wheel **went limp** during this recording. Opening a wheel hands it to the
application and stops the driver's own centring spring, and nothing sent a force back. That is
what `engine-input ffb` and `window::set_spring` were built for; see docs/subsystems/window.md.

## Adding a device

1. Have its owner run the two commands above and send back `<device>.jsonl` and the `devices`
   line. Ten seconds is enough; more is not better, because every event is committed.
2. Drop the `.jsonl` here and write a section for it above: what the device is, which slot and
   which sources it used, what each axis physically is, and anything the driver got wrong.
3. Add a row to `k_logs` in `foundation/input/tests/corpus_tests.cpp`. Every number in it is
   measured from the file, not chosen; run the test and read them off the failures.
4. If the driver named it badly or not at all, add its ids to the name table in
   `foundation/window/src/window.cpp` and a line to docs/subsystems/window.md.

## Recorded sessions: `sessions/`

A device recording says what a device sends. A **session** says what somebody did with the
engine: an `engine-view --interactive --record-input` log whose header carries an
`engine.view.session` block — the scene it drew, where the camera started, the tick rate, the
camera's parameters, how many ticks it ran, and the build that recorded it
([apps](../../docs/subsystems/apps.md#--interactive-a-camera-somebody-flies)). `engine-view
--replay-input <log>` flies it again, bit for bit. The corpus test above does not read this
directory; the sessions have tests of their own in `apps/engine_view/tests/`.

### `sessions/fly-synthetic.jsonl` — synthetic, and the replay test's fixture

**Nobody typed this.** CI cannot move a mouse, so the session is scripted by
`make_synthetic_session` in `apps/engine_view/tests/fly_tests.cpp`, and the committed file is what
that function writes, checked byte for byte on every build: regenerate it from code, never edit it
by hand. Two seconds at 240 Hz over the procedural heightfield at `--grid 65` (no sample assets,
and a fraction of a second to load in a debug build), from a camera 22 m south of the field looking
north and 0.3 rad down, at 4 m/s:

- Escape at tick 1 (when the fixture was written the window started without the pointer and this
  took it; since 2026-09-27 a window starts holding it and Escape gives it back, so fed live
  through `--inject-input` the session gives the pointer back here and looks with none of the
  motion that follows — its own recording, which the end-to-end test replays, then holds none of
  it. A replay feeds every event in the log whatever the pointer did, so the committed trajectory
  below is the same either way);
- W from tick 10 to 130 while the pointer moves 4 px right per tick for 60 ticks;
- D from 140 to 200 with Shift held for 40 of it; E from 210 to 260 while looking up 3 px a tick;
- S with Alt from 270 to 330; Q and A together from 340 to 400 while looking left and down;
- one 2,000-pixel pull upwards at tick 430, far past the pitch clamp;
- W and D together from 440 to 470, the diagonal that must not be faster than W alone;
- M pressed at ticks 60, 240, 420, 450 and 475: five markers.

Pointer motion is one event per axis per tick, which is what engine-view's edge writes. The log
names the hash of **the first revision** of engine-view's action map, the eight camera actions;
the second (2026-09-27) appends the time-lapse's four keys, and engine-view replays a log with the
revision its hash names, so this file — like every session recorded before then — replays
unchanged and is the test that says so.

### `sessions/fly-synthetic.trajectory.json` — where it goes

The camera the synthetic session flies, as `engine-view` prints it in its summary's
`interactive.trajectory`: a hash over every tick's position, yaw and pitch **bit for bit**, where
it ended, and each marker's tick and camera. It was committed from MSVC on Windows, and
`fly_tests.cpp` replays the fixture and compares with no GPU, so every machine CI has checks that
its compiler flies the recording to the same place. The end-to-end test replays the fixture twice
offscreen with `--marker-captures`, and checks that both runs print this same trajectory and draw
the same five PNGs byte for byte.

It moves only when the camera's integration does. If it does, bump `view::k_fly_version` in
`apps/engine_view/fly_camera.h` — which makes every older session log refuse to replay rather than
replay somewhere else — and commit the file the failing test writes into its kept scratch
directory.
