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

}  // namespace engine::routes::detail
