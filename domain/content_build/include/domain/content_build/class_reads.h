#pragma once

// **What the class read at a load reads, by kind** (roadmap C72; renderer.md, "Content classes at
// load"): how many reads of each kind were asked for, how many found a file, how many bytes they
// returned and how long they took. Kept by whoever runs the read (the renderer's admission keeps
// one a load, in its own state) and handed to the content build's class reads through
// `HostReads::counts`, so the cost of a load's class read is a count a test can hold and a line the
// log prints, never a guess. Plain data: no io, no allocation, no global.

#include <core/base/types.h>

namespace engine::content_build {

enum class ClassRead : u8 {
  container_section,  // a listed container's section 54: its open and section-table read
  container_sidecar,  // the sidecar beside a container (listed, or a recipe's host)
  recipe_file,        // a recipe's own file, read for its generator's host probe
  recipe_sidecar,     // the sidecar beside a recipe
  pack_sets,          // a pack's sets' classes: asked per set, `bytes` the `pack.json` read
  host_probe,         // a recipe's generator hash run under the probe's hooks (CPU, no file)
  host_record,        // a container host's open and section 54
  gltf_sidecar,       // the sidecar beside a glTF (or any other source by path)
  ground_inputs,      // the sidecars beside the ground's heightfield source, layers and masks
  // How the walk got a recipe's hosts, apart from what reading them cost (the probe's own read and
  // hash are `recipe_file` and `host_probe`, wherever they ran), so a load that probed ahead can
  // say it used the slots:
  ahead_answer,  // taken from a probe `probe_recipes_ahead` made (CPU, no file)
  walk_probe,  // probed in the walk itself, with no slot to take: none, once the load probed ahead
};
inline constexpr u32 k_class_read_kinds = 11;

// The kind's name as the log line and the pages spell it.
inline const char* class_read_name(ClassRead kind) noexcept {
  constexpr const char* k_names[k_class_read_kinds] = {
      "container_section", "container_sidecar", "recipe_file", "recipe_sidecar",
      "pack_sets",         "host_probe",        "host_record", "gltf_sidecar",
      "ground_inputs",     "ahead_answer",      "walk_probe"};
  const u32 i = static_cast<u32>(kind);
  return i < k_class_read_kinds ? k_names[i] : "unknown";
}

struct ClassReadCount {
  u32 asked = 0;  // reads asked for
  u32 found = 0;  // of those, the ones that found a file (or a set that carries a class)
  u64 bytes = 0;  // bytes they returned
  i64 ns = 0;     // wall nanoseconds they took, summed over whichever threads ran them
};

struct ClassReads {
  ClassReadCount kinds[k_class_read_kinds];

  ClassReadCount& operator[](ClassRead kind) noexcept { return kinds[static_cast<u32>(kind)]; }
  const ClassReadCount& operator[](ClassRead kind) const noexcept {
    return kinds[static_cast<u32>(kind)];
  }
  void add(ClassRead kind, bool found, u64 bytes, i64 ns) noexcept {
    ClassReadCount& c = (*this)[kind];
    ++c.asked;
    c.found += found ? 1u : 0u;
    c.bytes += bytes;
    c.ns += ns;
  }
};

}  // namespace engine::content_build
