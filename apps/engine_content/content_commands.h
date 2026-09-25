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
//
// Each prints one JSON line per result on stdout and returns the process exit code: 0 ok, 1 a file
// could not be read, written or validated, 2 usage.
//
// And the derived steps capabilities hand to the content build (clip_commands.cpp): the audio
// capability's `.clip`, which `build`, `build-all` and `info` reach through these, present when
// this build has the audio capability.

#include <domain/content_build/content_build.h>

#include <span>
#include <string>

namespace engine::content {

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

}  // namespace engine::content
