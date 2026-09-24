#pragma once

// engine-content's commands beyond the cluster build (docs/subsystems/apps.md, "engine-content"):
//
//   tissue import|info|validate|report|example   the tissue definition (docs/subsystems/tissue.md),
//                                                 present when this build has the tissue capability
//   limit-dump                                    the limit-surface conformance exchange with the
//                                                 authoring side (docs/subsystems/geometry.md)
//   normal-cases                                  the footpoint-normal rule's edge cases, each
//                                                 against the authoring side's expectation
//
// Each prints one JSON line per result on stdout and returns the process exit code: 0 ok, 1 a file
// could not be read, written or validated, 2 usage.

namespace engine::content {

int tissue_command(int argc, char** argv);
int limit_dump_command(int argc, char** argv);
int normal_cases_command(int argc, char** argv);

}  // namespace engine::content
