#pragma once

// **What a place holds resident, by kind** (T138; docs/experiments/streaming-by-travel.md, row a;
// docs/subsystems/renderer.md, "What a place holds"): `GpuScene::place_bytes` sums the device bytes
// of every buffer the scene owns at the moment of asking into the kinds a budget's pools would
// hold, and the host bytes of the scene's merged streams still kept. Counters only: nothing here
// changes what is held. A scene read whole holds the same at every place but for `structures`,
// `frame` and `tiles`; a world's `tiles` follow the eye.

#include <core/base/types.h>

namespace engine::renderer {

struct PlaceBytes {
  u64 geometry = 0;  // the load's meshes: cluster and LOD tables, positions, attributes, triangles
  u64 companions = 0;  // beside them: 8-bit indices, skin, morphs, motion, coverage, shadow proxy
  u64 micromaps = 0;   // the opacity micromaps' resident bytes
  u64 materials = 0;   // the material table, the material of every cluster, the terrain's masks
  u64 textures = 0;    // the materials' images on the device
  u64 structures = 0;  // the traced chain's sets, bottom and top levels, scratch, cache, static
  u64 tiles = 0;       // the world's tiles: the mesh room, the terrain's rings and fields, staging
  u64 frame = 0;       // sized by the frame or the instances: visible lists, pair tables, pools
  u64 pages = 0;       // a paged scene's pool and its tables
  u64 host_geometry = 0;  // the merged streams the host still keeps (`SceneData::lod.mesh`)
  u64 device_total() const noexcept {
    return geometry + companions + micromaps + materials + textures + structures + tiles + frame +
           pages;
  }
};

}  // namespace engine::renderer
