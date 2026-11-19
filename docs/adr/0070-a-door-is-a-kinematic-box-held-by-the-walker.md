# ADR-0070: A door is a kinematic box the collision places at the tick, held by the walker it would enter

- **Status:** Proposed
- **Date:** 2026-11-18
- **Plan references:** docs/plan/14-structures.md §14.4 ("Doors: the first kinematic bodies"); roadmap S91, S89
- **Docs touched:** `docs/subsystems/scene_collision.md` ("Doors"), `docs/plan/14-structures.md`

## Context

Plan 14 §14.4 asks for a working door: the scene's first body that moves. Scene collision holds
static bodies only — a heightfield and a compound a tile, and since P45 a static body a game prop —
in a physics world that is **never stepped**: the walker is a virtual character
(`physics::CharacterBody`) stepped by its host, and nothing dynamic lives there. The plan named
`physics::World::move_kinematic` for the leaf, which sets the velocity that reaches a target in one
step, and so moves a body only in a world that is stepped. A virtual character is not pushed by a
body that moves into it. And a door's state decides where a walker ends up, so it has to be a
function of the ticks for a replay to hold.

## Decision

1. **A door is a box on the kinematic layer** (`physics::Layer::Kinematic`, `MotionType::Kinematic`),
   sized to its leaf, made by scene collision while the tile its hinge stands in is held and let go
   with it, as a prop's body is (P45). The walker collides as `Moving`, which meets the kinematic
   layer, so a closed door is a wall and an open one a wall at its angle.
2. **Its angle is integer millidegrees** from closed, moved each tick toward its state's target (0
   closed, `open_mdeg` open) by `speed_mdeg`, never past it. The pose is derived from the integer
   each tick, never accumulated in floats, so a replay reaches the same angle at the same tick; and
   the turn that integer names is core/math's deterministic sine and cosine of the half angle
   (`door_rotation`, `det::sin`/`det::cos`), never the C library's, whose last bit differs between
   MSVC's UCRT and glibc, so the leaf's pose and the walker's hold on it — and so where a walk
   through a door ends — are the same bits on every compiler, not only the same angle.
3. **It is placed, not driven**: `set_body_transform` at the tick, without activation, because the
   world is never stepped and `move_kinematic` would move nothing. A world that is stepped (a cart a
   door should push) switches that one call to `move_kinematic`.
4. **A door that would enter the walker holds its angle.** Before each tick's move the leaf at the
   next angle is tested against the walker's capsule — upright, like the leaf, so the test is exact:
   the distance in x and z from the capsule's axis to the leaf's rectangle in the leaf's frame,
   against the radius, with the heights overlapping. If it would come within 5 cm of it (past the character's contact padding, so a leaf never presses the walker aside and creeps after it) the door stays where
   it was this tick and counts a hold, as a real door stops at a body. A tick's travel is tested
   every degree along it and the leaf goes as far as the last angle clear of the walker, so a fast
   door a game sets (up to half a turn a tick) cannot pass through the walker in one tick; at the
   default speed (90 degrees in a second at 240 Hz) the travel is under a degree and the one test is
   its end, where a 0.9 m leaf's tip has moved 5.9 mm, a fiftieth of the capsule's radius. A leaf
   that already reaches into the walker (a restore, a drop or a teleport into its arc) is tested by
   how far it reaches: it goes while each next angle reaches less far, away from the walker, and
   holds toward it, so it can always clear the walker and never closes on it.
5. **A toggle is an input to the tick** (`toggle_door(key)`), from the walker's interact action or a
   game module, applied at the tick it was made, so a replayed walk opens the same doors.

## Consequences

- The world stays unstepped and every query is what it was; a door costs one box and one transform
  write a tick while it moves, and nothing while it stands.
- What a saved village keeps is a door's state and angle (`door(key)`); the document's write-back
  through a `@writeback` row on `engine.structure.Door` (roadmap D37) is the next stage, as are the
  leaf meshed on its own with its pivot on the hinge (G65), the walker's interact binding, the game
  module's service (an ABI step) and a closed door as an area navigation opens.

## When to revisit

When a dynamic body shares the walker's world (a pushed cart, debris), or when a game wants a door
that pushes rather than holds.
