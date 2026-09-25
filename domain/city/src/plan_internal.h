#pragma once

// What plan.cpp shares with the plan file's reader (plan_io.cpp): the street graph is not stored in
// a plan file, it is rebuilt from the blocks and streets the file holds, by the same function the
// generator uses. Private to the module.

#include <domain/city/plan.h>

namespace engine::city {

// Nodes and segments from the blocks' sides and the interior streets, and each street's kept
// extent along its line. A street nothing keeps gets from > to.
void build_graph(Plan& plan);

}  // namespace engine::city
