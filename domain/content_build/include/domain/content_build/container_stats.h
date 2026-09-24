#pragma once

// The content-build metrics of a built container (docs/plan/07-content-pipeline.md §7.3): what
// `engine-content stats` prints and what the protocol's `content.build` returns per output, from
// one function so the two can never report different numbers for the same file.
//
// How the clusters spread over the DAG's levels and how full they are; how much the cluster layout
// duplicates the source vertices; where the bytes went, section by section, with the header, the
// table and the alignment padding named; the position grid; the page table with the fly-in
// streaming sweep; the images slot by slot; the morph stream channel by channel; the canonical
// vertex ids; how fragmented the UV atlas is; and how much of the texture a coarse LOD cut moves.
// docs/subsystems/apps.md ("engine-content", "stats") says what each number means and why it is
// there.

#include <core/containers/vector.h>
#include <core/json/json_value.h>
#include <domain/geometry/cluster_file.h>
#include <domain/geometry/cluster_pages.h>

#include <string>

namespace engine::content_build {

// Reads a container and hands back its bytes as well, so a caller can walk the section table
// straight from the file rather than from what this build understands of it: `engine-content
// info` lists a section a newer build wrote by reading the table this way.
bool load_container(const std::string& path, std::string& file, geometry::ClusterFileHeader& header,
                    Vector<geometry::ClusterFileSection>& sections, geometry::ClusterFileData& data,
                    std::string& error);

// The page table at a glance: how many pages, how full they are, how much of the mesh is pinned
// (the pages holding a group with no coarser version, which the residency manager never evicts),
// and how connected they are. The head of `info`'s and of `container_stats`' page object.
JsonValue page_summary(const geometry::ClusterPages& pages);

// Reads the container at `path` and computes its metrics. `summary` is the one JSON object
// `engine-content stats` prints on stdout; `human` is the tables and lines it prints on stderr for
// a person — the streaming sweep, the morph, identity and atlas lines, and the LOD attribute error
// — in the order it prints them, so a caller that has no person to show them to can drop them.
// False, with `error`, when the file cannot be read or is not a container.
bool container_stats(const std::string& path, JsonValue& summary, std::string& human,
                     std::string& error);

}  // namespace engine::content_build
