#pragma once

// The door's record in the store's event log (docs/subsystems/world.md, "The door in the store";
// docs/plan/03-data-model.md §3.10: "player input and runtime-LLM outputs, both logged as events
// and replayed from the log"; roadmap S18).
//
// Every entry of a `sim::Door` — a player's input event, an external source's raise, a client's
// flush or transaction between ticks — is one row of the store's `events` table with the entry's
// origin (`Player`, `Agent`, ...; never `Deterministic`), its tick and game time, and its kind as
// the row's type. **What the log then guarantees**: every input the world took that it did not
// compute is a row of the log, in the order the world took it, beside the write-backs the world
// computed from them; the persistent-state hash (`state_hash`) covers those rows, so two runs whose
// inputs differ do not hash the same however their state came out; and `read_door_events` gives a
// replay the door back from the log alone, with no file beside it.
//
// The rows are filed under one tile no world reaches, `k_door_tile` — (-2^31, -2^31) under the
// write-back's packing — so their sequence is the door's own order and nothing the world writes in
// its tiles interleaves with it: a run and its replay append the same rows with the same keys
// whenever each appends them.

#include <domain/sim/door.h>
#include <foundation/store/event_log.h>

namespace engine::world {

inline constexpr store::TileId k_door_tile = 0x8000'0000'8000'0000ull;

// Appends the door's entries from `cursor` on, in one transaction, and moves `cursor` past them. A
// host calls it where it commits (the persist hook, the end of a call) and before a save or a hash;
// a replay calls it the same way, so its log holds the same rows.
store::Status append_door_events(store::EventLog& log, const sim::Door& door, u32& cursor);

// Every door row of the log into `door`, in the log's order, as recorded entries (`door` must hold
// none). `read` is how many. A row whose payload or origin is not a door entry's is refused.
store::Status read_door_events(store::EventLog& log, sim::Door& door, u32* read = nullptr);

}  // namespace engine::world
