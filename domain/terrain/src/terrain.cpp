#include <domain/terrain/terrain.h>
#include <foundation/tunables/tunables.h>

namespace engine::terrain {

namespace {

// The rates and thresholds of the overlay and the feedback (docs/subsystems/terrain.md,
// "Tunables"). Why each default is what it is lives beside `OverlayRules` and `FeedbackRules`; the
// short form: a 3 cm footprint fills in about half a day of the mean wind and the deepest pit in
// about 33 days; a vertex within 5 mm of the steady state is buried; a lag unit is 8 cm and the
// ceiling 96 of them; a pit or a drift of a cubic metre is a large obstacle.
tunables::Int fill_um_per_cm2{"terrain.overlay.fill_um_per_cm2", 11, 0, 1'000'000,
                              "Micrometres of sand the wind puts back into a deformed vertex per "
                              "square centimetre of sand flux that blows over it"};
tunables::Int bury_mm{"terrain.overlay.bury_mm", 5, 0, 1'000,
                      "A deformation within this many millimetres of the steady state is buried: "
                      "zero, and stored as nothing"};
tunables::Int max_depth_mm{"terrain.overlay.max_depth_mm", 2'000, 1, 30'000,
                           "The deepest (and highest) a stamp may take the surface, millimetres: "
                           "what bounds how long a tile keeps a record"};
tunables::Int lag_unit_mm{"terrain.feedback.lag_unit_mm", 80, 1, 10'000,
                          "The feedback lag's quantum, millimetres"};
tunables::Int lag_max_units{"terrain.feedback.lag_max_units", 96, 0, 255,
                            "The most units a tile may hold a band of dunes back by"};
tunables::Int pit_volume_l{"terrain.feedback.pit_volume_l", 1'000, 1, 1'000'000,
                           "A pit this many litres deep nudges its tile's lag a unit, once a day"};
tunables::Int drift_volume_l{"terrain.feedback.drift_volume_l", 1'000, 1, 1'000'000,
                             "A declared drift this many litres large holds its tile back a unit "
                             "a day from the day it was declared"};

}  // namespace

OverlayRules overlay_rules_from_tunables() noexcept {
  OverlayRules rules;
  rules.fill_um_per_cm2 = static_cast<i32>(fill_um_per_cm2.get());
  rules.bury_mm = static_cast<i32>(bury_mm.get());
  rules.max_depth_mm = static_cast<i32>(max_depth_mm.get());
  rules.feedback.lag_unit_mm = static_cast<i32>(lag_unit_mm.get());
  rules.feedback.lag_max_units = static_cast<i32>(lag_max_units.get());
  rules.feedback.pit_volume_mm3 = pit_volume_l.get() * 1'000'000;
  rules.feedback.drift_volume_mm3 = drift_volume_l.get() * 1'000'000;
  return rules;
}

}  // namespace engine::terrain
