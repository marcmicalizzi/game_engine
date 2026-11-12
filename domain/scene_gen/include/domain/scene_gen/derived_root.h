#pragma once

// **Where a generator keeps what it derives** (T119; scene_gen.md, "The derived-data root"): the
// one rule for the root every host hands a generator in `Context::ddc`, and the one way a
// generator reads it.
//
// **Why one rule, and why at the host.** Until T119 every generator that keeps a file — a plan, a
// recipe, a built mesh — found its own root when the context handed none, by walking up from the
// scene's directory and, with no directory, from the working directory. Two hosts handed none (the
// world's streamed consumer and the collision), so a streamed scene given `--ddc` read its plans
// from wherever the process was started: the forest's world refused "the plan … is not in
// <working directory>/ddc/routes" under an `--ddc` that held it, and the content commands left
// 29 GB under the working directory's `ddc/`. A rule copied into a dozen generators drifts, and a
// fall-back that reaches the working directory is a root nobody named. So the host decides, here,
// once a read; a generator is handed the answer and refuses by name when it is handed none, rather
// than guess.

#include <string>
#include <string_view>

namespace engine::scene_gen {

struct Context;

// **The scene's own root**: the repository's `ddc/` above the scene's directory (the directory
// holding `AGENTS.md`, `geometry::find_ddc_root`), else `ddc/` in the scene's directory itself.
// A relative `scene_dir` is taken against the working directory, as the scene's path was given;
// an empty one is the working directory because the scene's file is there. Absolute, with `/`.
std::string scene_derived_root(std::string_view scene_dir);

// **The one rule** (T119): the root a host hands every generator it opens. The root the host was
// named (`--ddc`, `render.load`'s `ddc`), as given; else, for a scene read from a file
// (`scene_file`), the scene's own (above, from `scene_dir`); else — a document, a description built
// in memory, which have no scene file — the repository's found from the executable. **Never the
// working directory** unless the scene's file is in it. Empty only with no name, no scene file and
// no repository above the executable; a generator handed that refuses (`generator_derived_dir`).
std::string host_derived_root(std::string_view named, std::string_view scene_dir, bool scene_file);

// What a generator calls for the directory it keeps its files in: `<Context::ddc>/<sub>`, absolute
// and made. False with a sentence that names the generator and the entry when the context has no
// root (a host that did not follow the rule), or when the directory cannot be made.
bool generator_derived_dir(const Context& context, std::string_view generator, std::string_view sub,
                           std::string& out, std::string& error);
// The same for a caller that holds a root and no context (an `engine-content` command).
bool derived_dir_under(std::string_view root, std::string_view generator, std::string_view sub,
                       std::string& out, std::string& error);

// **Where a file the root does not hold is instead**, for a refusal's sentence (T112's rule, which
// the channels' plan follows): `<other>/<sub>/<file>` under the scene's own root and under the
// working directory's (its repository's `ddc/`, else `ddc/` in it), where a host before T119 would
// have looked; the first that holds the file and is not `dir`. Returns "; it is in <dir>, under the
// scene's own root: …" or "…, under the working directory: …", or empty. Only a sentence: the file
// is never read from there, because reading either root silently is what the rule forbids.
std::string derived_elsewhere(const Context& context, std::string_view sub, std::string_view file,
                              std::string_view dir);

}  // namespace engine::scene_gen
