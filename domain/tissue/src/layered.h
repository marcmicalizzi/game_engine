#pragma once

// The layered volume model's rows (docs/subsystems/tissue.md, "The layered model" and
// "Validators"): parameters and the active rest shape, frame states and attachment enforcement,
// the thickness field and its depots, the material boundary and its skin, contact pairs and their
// policy, and certificate provenance. Private to the module.
//
// They run after every row of the definition as it was through version 1, over the regions and
// frames `validate.cpp` resolved, and a definition that uses none of the records gets none of the
// rows: its report is the one it always had. The geometric rows are **chordal** — a six-node face
// is tested by its four chords, the subdivision's boundary — and say so; what the engine cannot
// evaluate (curved and continuous separation, the contact potential's mechanics, the membrane's
// relaxation) the rows report and name the authoring certificate as the authority for.

#include "model.h"

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <domain/tissue/tissue_file.h>
#include <domain/tissue/validate.h>

#include <string>

namespace engine::tissue::detail {

// What the rows read, resolved by `validate.cpp`.
struct LayeredContext {
  const TissueFile& file;
  const Vector<RegionModel>& regions;
  const Vector<FrameModel>& frames;
  // The observation's positions, or null when the definition declares none (or it did not resolve).
  const Vector<Vec3>* observation;
  const ValidateOptions& options;
};

// A point of a material surface: a face of it and barycentrics over the face's corners
// (MaterialSurfaceCoordinates, 16 bytes).
struct SurfacePoint {
  u32 face = 0;
  f32 b[3] = {1.0f, 0.0f, 0.0f};
};
static_assert(sizeof(SurfacePoint) == 16, "a MaterialSurfaceCoordinates element is 16 bytes");

struct SurfaceModel {
  const MaterialBoundarySurface* def = nullptr;
  i32 region = -1;
  Vector<u32> refs;   // two per face: the cell, the local face
  Vector<u32> nodes;  // six per face: region nodes, Gmsh's six-node order
  bool ok = false;    // the region is ten-node and every reference is in range
  u32 face_count() const noexcept { return static_cast<u32>(refs.size() / 2); }
  // The face whose reference is (cell, local), or ~0u.
  u32 find(u32 cell, u32 local) const noexcept {
    for (u32 f = 0; f + 1 < refs.size(); f += 2)
      if (refs[f] == cell && refs[f + 1] == local) return f / 2;
    return ~0u;
  }
};

struct SkinModel {
  const MaterialSkin* def = nullptr;
  i32 surface = -1;
  Vector<u32> ids;
  Vector<SurfacePoint> points;
  Vector<Vec3> rest;  // the natural metric's configuration: the rest block, or the construction
  bool ok = false;
};

struct SampleModel {
  const ParameterSample* def = nullptr;
  i32 region = -1;
  const TissueBlock* block = nullptr;
  Vector<Vec3> snapshot;
  bool ok = false;
};

struct DepotModel {
  Vector<f32> shares;
  bool ok = false;
};

struct PartitionModel {
  const DepotPartition* def = nullptr;
  Vector<DepotModel> depots;
};

struct ExclusionModel {
  Vector<u32> a;  // two per face
  Vector<u32> b;
  bool ok = false;
};

struct PairModel {
  const ContactPair* def = nullptr;
  Vector<ExclusionModel> exclusions;
};

struct PatchModel {
  Vector<u32> refs;  // two per face; empty when the attachment names no patch
  bool ok = false;
};

struct LayeredModel {
  Vector<SurfaceModel> surfaces;
  Vector<SkinModel> skins;
  Vector<SampleModel> samples;
  Vector<PartitionModel> partitions;
  Vector<PairModel> pairs;
  Vector<PatchModel> patches;  // one per attachment, in the definition's order
};

// Resolves the layered records' blocks: a reference that does not resolve, or a count or an index
// out of range, is a problem for `blocks.resolve`, which is written after this.
void resolve_layered(const LayeredContext& context, Vector<std::string>& problems,
                     LayeredModel& out);

// The layered model's rows, appended to `report`.
void check_layered(const LayeredContext& context, const LayeredModel& model, TissueReport& report);

}  // namespace engine::tissue::detail
