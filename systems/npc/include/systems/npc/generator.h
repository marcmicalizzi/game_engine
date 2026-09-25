#pragma once

// The resident generator (docs/subsystems/npc.md, "The generator"): a seeded, partitioned document
// layer of `engine.npc.Place` and `engine.npc.Resident` records. Derived data — a function of its
// parameters alone, written under a test's scratch directory or by `engine-content npcs`, and never
// committed.
//
// ECS-free, like the routine: it reads the routine tables to write each resident's state at the
// layer's starting time, which is the closed form the world will compute when it materializes the
// record, so a world that starts there writes nothing back until the first transition.
//
// **The output does not depend on the thread count.** Every record is a pure function of (params,
// its index), the job system only splits the index range, and the records go into the layer by id,
// never by completion order; the test compares the layer's bytes at none, one and four workers.

#include <core/base/types.h>
#include <core/ids/id128.h>
#include <core/jobs/job_system.h>
#include <core/json/json_value.h>
#include <domain/doc/document.h>
#include <foundation/io/vfs.h>

#include <string>
#include <string_view>

namespace engine::npc {

struct GeneratorParams {
  // The world seed: every resident's variation is drawn from it (ADR-0045), and every placement.
  u64 seed = 1;
  u32 residents = 1000;
  // Places by role. Zero means a default proportional to the residents: a home per four, a
  // workplace per 40, a service place per 50, a leisure place per 100 — each at least one.
  u32 homes = 0;
  u32 workplaces = 0;
  u32 services = 0;
  u32 leisure = 0;
  // Where places go, metres on the ground plane: [min_x, max_x) × [min_z, max_z), on a
  // quarter-metre grid so a position is a short, exact JSON number.
  f64 min_x = 0.0;
  f64 min_z = 0.0;
  f64 max_x = 512.0;
  f64 max_z = 512.0;
  // The layer's partition: its tile size must be the ring's (docs/subsystems/world.md).
  f64 tile_size = 32.0;
  std::string layer = "residents";
  // The game time the residents' state is written at.
  i64 time_us = 0;
  // Every resident's `clock_offset`: the routine's time of day at game time 0 (0: midnight).
  i64 clock_offset_us = 0;
  // Relative weights of the six routines, in `Routine` order.
  u32 mix[6] = {40, 10, 5, 20, 15, 10};
};

// Reads a params object ({"seed":…, "residents":…, "places":{"homes":…,…}, "extent":[x0,z0,x1,z1],
// "tile_size":…, "layer":…, "time_us":…, "clock_offset_us":…, "mix":{"day_worker":…,…}});
// unknown keys are refused.
bool parse_params(const JsonValue& json, GeneratorParams& out, std::string& error);

struct GeneratorStats {
  u32 places = 0;
  u32 residents = 0;
  u32 tiles = 0;  // occupied tiles of the layer
  f64 ms = 0;
};

// The ids the generator gives record `index`: a pure function of the seed and the index.
Id128 place_id(u64 seed, u32 index) noexcept;
Id128 resident_id(u64 seed, u32 index) noexcept;
// The defaults of `params` resolved: the place counts it will make.
GeneratorParams resolved(const GeneratorParams& params) noexcept;

// A partitioned layer holding the places, then the residents. `jobs` null runs on this thread.
doc::Layer generate_layer(const GeneratorParams& params, jobs::JobSystem* jobs = nullptr,
                          GeneratorStats* stats = nullptr);

// The layer, added as the edit layer to the document at `dir` — created when there is none — and
// saved. A document that already has a layer of that name is refused.
bool generate_document(const io::Vfs& vfs, std::string_view dir, const GeneratorParams& params,
                       jobs::JobSystem* jobs, std::string& error, GeneratorStats* stats = nullptr);

}  // namespace engine::npc
