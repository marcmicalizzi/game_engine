#pragma once

// The rows that say how far a derived cage is from its reference (docs/subsystems/tissue.md, "How
// far the cage is from its reference"): the derivation record resolved (`cage.source`), volume and
// mass (`cage.volume`), the two boundaries' largest distance, chordal (`cage.boundary_distance`),
// each carried state's displacement against the reference's field (`cage.state_displacement`),
// and the share of the reference's strain energy the cage holds (`cage.strain_energy`). Private to
// the module. They run only for a region that carries a `CageDerivation`, so a definition without
// one reports exactly as it did; and the rows that need the reference's cells run where the file
// carries the source region itself or the validator was handed the source file (and its SHA-256 is
// the one the cage records), and are skipped, saying why, where it was not.

#include "model.h"

#include <domain/tissue/tissue_file.h>
#include <domain/tissue/validate.h>

namespace engine::tissue::detail {

void check_cage(const TissueFile& file, const Vector<RegionModel>& regions,
                const ValidateOptions& options, TissueReport& report);

}  // namespace engine::tissue::detail
