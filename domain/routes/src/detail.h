#pragma once

// What the routes' two sources share: the planner split at the lattice, so a plan's key is read
// from the sampled ground before anything is planned (routes.md, "One plan").
#include "domain/routes/routes.h"

#include <string>

namespace engine::routes::detail {

// The lattice of the layer's region over `ground`: `out`'s width, depth, origin, spacing and
// `ground_mm`, and `wet` from `water` when it is not null; its lines empty. False with a sentence.
bool sample_lattice(const RouteLayer& layer, const scene_gen::Ground& ground, jobs::JobSystem* jobs,
                    RouteNetwork& out, std::string& error, const scene_gen::Exclusion* water);
// The planner over a sampled lattice (`net` as `sample_lattice` left it): the legs, the network,
// the lines. The one place it runs, counted for `plans_made`.
bool plan_from_lattice(const RouteLayer& layer, const scene_gen::Ground& ground, RouteNetwork& net,
                       std::string& error, const scene_gen::Exclusion* water);
// A plan read from the cache against the layer it claims (T198; routes.md, "One plan"): its
// lattice is the one `sample_lattice` makes of the layer's region, and every station is finite,
// inside that lattice, and as near the one before it as the stationing spaces them. A plan's
// closing hash is not a MAC, so a writer's stations are refused here before a band is made of
// them. False with a sentence.
bool plan_fits_layer(const RouteLayer& layer, const RouteNetwork& net, std::string& error);

}  // namespace engine::routes::detail
