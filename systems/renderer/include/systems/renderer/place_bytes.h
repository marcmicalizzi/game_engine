#pragma once

// **What a place holds resident, by kind** (T138; docs/experiments/streaming-by-travel.md, row a;
// docs/subsystems/renderer.md, "What a place holds"): `GpuScene::place_bytes` sums the device bytes
// of every buffer the scene owns at the moment of asking into the kinds a budget's pools would
// hold, and the host bytes of the scene's merged streams still kept. Counters only: nothing here
// changes what is held. A scene read whole holds the same at every place but for `structures`,
// `frame` and `tiles`; a world's `tiles` follow the eye.

#include <core/base/types.h>

namespace engine::renderer {

// **The traced chain's bytes by kind** (R81; docs/experiments/rt-structures-memory-2026-11-17.md):
// `PlaceBytes::structures` split by what sizes each part, because the parts answer to different
// rules: `frame_sets` and `frame_tables` follow the capacity (`RtCapacity`, the budget their
// ceiling), `bottom_level` the capacity and the references, `cache` the cached meshes' clusters up
// to its own ceiling, `static_forms` the forms, and the rest the instances and the pairs.
struct StructureBytes {
  u64 frame_sets = 0;    // the frame's own cluster structures: the chunks, the driver's most a slot
  u64 frame_tables = 0;  // the capacity's records, record-to-entry table, addresses, sizes, counts
  u64 bottom_level = 0;  // every instance's bottom-level structure, one set, and its addresses
  u64 top_level = 0;     // the top-level structure and its instance records
  u64 scratch = 0;       // what the builds share
  u64 per_instance = 0;  // by the instances or the pairs: bucketing slots, counts, records, places
  u64 cache = 0;         // the resident cache: its slots (the driver's most each), keys, tables
  u64 cache_slots = 0;   // of `cache`, the slots alone
  u64 static_forms = 0;  // rigid forms' structures built once, packed to their size
  u64 total() const noexcept {
    return frame_sets + frame_tables + bottom_level + top_level + scratch + per_instance + cache +
           static_forms;
  }
};

struct PlaceBytes {
  u64 geometry = 0;  // the load's meshes: cluster and LOD tables, positions, attributes, triangles
  u64 companions = 0;  // beside them: 8-bit indices, skin, morphs, motion, coverage, shadow proxy
  u64 micromaps = 0;   // the opacity micromaps' resident bytes
  u64 materials = 0;   // the material table, the material of every cluster, the terrain's masks
  u64 textures = 0;    // the materials' images on the device
  u64 structures = 0;  // the traced chain's sets, bottom and top levels, scratch, cache, static
  StructureBytes structure;  // `structures` by kind: the same bytes, split
  u64 tiles = 0;  // the world's tiles: the mesh room, the terrain's rings and fields, staging
  u64 frame = 0;  // sized by the frame or the instances: visible lists, pair tables, pools
  u64 pages = 0;  // a paged scene's pool and its tables
  u64 host_geometry = 0;  // the merged streams the host still keeps (`SceneData::lod.mesh`)
  u64 device_total() const noexcept {
    return geometry + companions + micromaps + materials + textures + structures + tiles + frame +
           pages;
  }
};

}  // namespace engine::renderer
