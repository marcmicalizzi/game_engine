#pragma once

// engine-content's commands beyond the cluster build (docs/subsystems/apps.md, "engine-content"):
//
//   tissue import|info|validate|report|example   the tissue definition (docs/subsystems/tissue.md),
//                                                 present when this build has the tissue capability
//   limit-dump                                    the limit-surface conformance exchange with the
//                                                 authoring side (docs/subsystems/geometry.md)
//   normal-cases                                  the footpoint-normal rule's edge cases, each
//                                                 against the authoring side's expectation
//   ruins, ruins-kit, ruins-block-kit             the ruin assembler: a tile's building as a scene
//                                                 fragment, in sections or laid block by block
//                                                 (--blocks), and the synthetic kit of boxes and
//                                                 block kit (docs/subsystems/ruins.md), present
//                                                 when this build has the ruins capability
//   terrain                                       one tile of a scene's dune field at a game
//                                                 time (docs/subsystems/terrain.md), present when
//                                                 this build has the terrain capability
//   city params|plan|building|fragment|yield      Island City's plan, one lot's building, the proxy
//                                                 scene fragment and E18's yield
//                                                 (docs/subsystems/city.md), present when this
//                                                 build has the city capability
//   flora                                         a flora file (a species and its parameters)
//                                                 solved and built into a container through the
//                                                 cache (docs/subsystems/flora.md), present when
//                                                 this build has the flora capability
//   rocks                                         a rock file (a boulder, an outcrop or scree)
//                                                 solved and built into a container through the
//                                                 cache, or written as a GLB for E10 (docs/
//                                                 subsystems/rocks.md), present when this build
//                                                 has the rocks capability
//
// Each prints one JSON line per result on stdout and returns the process exit code: 0 ok, 1 a file
// could not be read, written or validated, 2 usage.
//
// And the derived steps capabilities hand to the content build (clip_commands.cpp): the audio
// capability's `.clip`, which `build`, `build-all` and `info` reach through these, present when
// this build has the audio capability.

#include <domain/content_build/content_build.h>

#include <cstdio>
#include <span>
#include <string>
#include <string_view>

namespace engine::content {

// `--help` or `-h`: a command that takes it prints its usage on stdout and exits 0.
inline bool is_help_flag(std::string_view arg) noexcept { return arg == "--help" || arg == "-h"; }

// **An output named with a leading dash is an option where a path was expected** (T210): one
// command once took `--help` for its output directory and wrote a folder of that name into the
// directory it ran from. Refused, with one line naming what and why; true when refused, and the
// caller prints its usage and exits 2. A path that really begins with a dash is named `./-x`.
inline bool refuse_dash_output(const char* command, const char* what, std::string_view path) {
  if (path.empty() || path.front() != '-') return false;
  std::fprintf(stderr,
               "engine-content %s: the %s '%.*s' begins with a dash: an option where a path was "
               "expected (name such a path ./%.*s)\n",
               command, what, static_cast<int>(path.size()), path.data(),
               static_cast<int>(path.size()), path.data());
  return true;
}

// The steps this configuration has, in the order `build` asks them whether they take a source.
std::span<const content_build::DerivedStep* const> derived_steps();
// Prints `info` for a `.clip` and sets the exit code; false when `path` is not one (or this build
// has no audio capability), for `info` to try the next kind.
bool clip_info(const std::string& path, int& code);

int tissue_command(int argc, char** argv);
int limit_dump_command(int argc, char** argv);
int normal_cases_command(int argc, char** argv);
int ruins_command(int argc, char** argv);
int ruins_kit_command(int argc, char** argv);
int ruins_block_kit_command(int argc, char** argv);
// The static world's kit: plain glTF meshes and PNGs carrying nothing a generator adds
// (static_kit_commands.cpp; content/test-scenes/static-world/). Present in every configuration.
int static_kit_command(int argc, char** argv);

// engine-content alpha-merge: a base colour and a separate opacity map made one RGBA PNG, for
// bought foliage whose glTF ships its mask outside the material (alpha_merge_commands.cpp;
// tools/fetch-foliage.ps1; roadmap T139). Present in every configuration.
int alpha_merge_command(int argc, char** argv);
int terrain_command(int argc, char** argv);
int tileset_command(int argc, char** argv);      // terrain-synth, terrain-tiles, terrain-masks
int masks_check_command(int argc, char** argv);  // masks_check_commands.cpp: masks check (T72)
// What the drawn ground is at a point and over a disc round it, through the scene reader's ground
// plan (ground_commands.cpp; G201 stage 2): `ground sample`. Present in every configuration.
int ground_command(int argc, char** argv);
int city_command(int argc, char** argv);
int npcs_command(int argc, char** argv);
int flora_command(int argc, char** argv);
int rocks_command(int argc, char** argv);
// The water's channel description from a scene's "channels" placements (channels_commands.cpp,
// docs/subsystems/channels.md); present when this build has the channels capability.
int channels_command(int argc, char** argv);
// Where a scene's channels' water leaves its bed: the fall test and the falls cooked beside each
// entry's plan (falls_commands.cpp, docs/subsystems/channels.md, "Where the water leaves its bed
// (S104)"); present when this build has the channels capability.
int falls_command(int argc, char** argv);
// A stream's water from its discharge (flow_commands.cpp, docs/subsystems/channels.md, "The
// water's description"; W120): `flow fixture`, the closed-form scene the solver and the draw are
// held to. Present when this build has the channels capability and the tiles ground provider.
int flow_command(int argc, char** argv);
// `flow solve` (flow_solve.cpp; channels.md, "The cook"): the scene's streams solved on the
// ground it draws — its channels placements' wet links and its authors' lines.
int flow_solve_command(int argc, char** argv);
// A region sketch solved into an authored heightfield and its fields (landform_commands.cpp,
// docs/subsystems/landform.md); present when this build has the landform capability.
int landform_command(int argc, char** argv);
// Generated material sets built into a pack directory (matgen_commands.cpp,
// docs/subsystems/matgen.md); present when this build has the matgen capability.
int matgen_command(int argc, char** argv);
// A scene's footpaths written as benched treads, their mask regions and plan keys
// (routes_commands.cpp, docs/subsystems/routes.md); present when this build has the routes
// capability.
int routes_command(int argc, char** argv);
// The footbridges where a scene's routes ford its streams: a `bridges` placement entry, the
// approaches as ground edits and the footprints (bridges_commands.cpp, docs/subsystems/bridges.md);
// present when this build has the bridges and routes capabilities.
int bridges_command(int argc, char** argv);

// The rural filler as a tool: a plot, a household and a seed to a house's `.structure.json`, its
// plan as text and its walk (dwellings_commands.cpp, docs/subsystems/dwellings.md); present when
// this build has the dwellings capability.
int dwelling_command(int argc, char** argv);
// A scene's village as its plan, its treads' mask regions and a camera path into one of its houses
// (village_commands.cpp, docs/subsystems/village.md); present when this build has the village
// capability.
int village_command(int argc, char** argv);
// A cave network's plan against a hillside, as a plan view, a long section and the holes its
// mouths ask of the terrain (caves_commands.cpp, docs/subsystems/caves.md); present when this
// build has the caves capability.
int caves_command(int argc, char** argv);
int soil_command(int argc, char** argv);
int vine_command(int argc, char** argv);   // cover_commands.cpp
int cover_command(int argc, char** argv);  // cover_commands.cpp
// MakeHuman shape targets appended to a canonical-topology body as morph channels (morphs.cpp,
// docs/subsystems/assets.md); present in every configuration.
int morphs_command(int argc, char** argv);

// The texture step `build --cache` runs, for a generated mesh's container (flora, rocks): every
// image its records name built into `<ddc>/textures/<key>.tex`, or found there, with the paths
// resolved beside `source` (the recipe). The step's object for the command's summary; `ddc` empty
// is the object that says it did not run. `failures` counts the images that failed to build.
JsonValue generated_textures(const geometry::ClusterFileData& data, const std::string& source,
                             const std::string& ddc, u32& failures);

}  // namespace engine::content
