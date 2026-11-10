// Every generator the build tags is registered in this host (roadmap T79; docs/subsystems/
// scene_gen.md, "Which generators a host carries"). Compiled into the end-to-end test executable of
// each host on ENGINE_GENERATOR_HOSTS by cmake/EngineGenerators.cmake, which defines
// ENGINE_HOST_NAME and ENGINE_HOST_GENERATORS: every `<kind>:<name>` registration the
// configuration's tagged modules declare, less what this host excludes in writing. That executable
// is linked by engine_app() from the host's own DEPS, so its registries are the host's.
//
// Why at run time as well as at configure. The configure sees the link line; it cannot see a
// registrar. A static library whose registrar object the linker dropped — no WHOLE_ARCHIVE, a
// linker that honours it differently, a registrar moved into an object something else stopped
// referencing — builds, links and runs, and the first sign is a scene refused with "which this
// build does not carry". The three toolchains (MSVC's link, GNU ld, lld) do not drop the same
// things, so this runs on each (`<host>.generators`, no device).
//
// The second half — nothing is registered that no tag names — keeps the tags true: a registrar
// added to a tagged module without its name on the module's GENERATOR line fails here. Two kinds of
// registration are not a capability's and are let through: the renderer's own ground (`waves`,
// systems/renderer/src/terrain.cpp, in every executable that draws), and a generator a test
// registers from its own source, which by convention has `test` in its name.

#include <domain/assets/mesh_generator.h>
#include <domain/scene_gen/scene_gen.h>
#include <domain/scene_gen/sky.h>

#include <doctest/doctest.h>

#include <string>
#include <string_view>
#include <vector>

#if !defined(ENGINE_HOST_NAME) || !defined(ENGINE_HOST_GENERATORS)
#error "defined by _engine_check_generator_hosts() in cmake/EngineGenerators.cmake"
#endif

using namespace engine;

namespace {

struct Tag {
  std::string kind;
  std::string name;
};

std::vector<Tag> build_tags() {
  std::vector<Tag> out;
  const std::string_view list = ENGINE_HOST_GENERATORS;
  usize start = 0;
  while (start < list.size()) {
    usize comma = list.find(',', start);
    if (comma == std::string_view::npos) comma = list.size();
    const std::string_view tag = list.substr(start, comma - start);
    const usize colon = tag.find(':');
    REQUIRE_MESSAGE(colon != std::string_view::npos, "a tag without its kind: " << tag);
    out.push_back(Tag{std::string(tag.substr(0, colon)), std::string(tag.substr(colon + 1))});
    start = comma + 1;
  }
  return out;
}

bool registered(const Tag& tag) {
  const scene_gen::GeneratorRegistry& scenes = scene_gen::GeneratorRegistry::global();
  if (tag.kind == "ground") return scenes.find_ground(tag.name) != nullptr;
  if (tag.kind == "placement") return scenes.find_placement(tag.name) != nullptr;
  if (tag.kind == "sky") return scenes.find_sky(tag.name) != nullptr;
  if (tag.kind == "mesh") return assets::MeshGeneratorRegistry::global().find(tag.name) != nullptr;
  return false;
}

bool tagged(const std::vector<Tag>& tags, std::string_view kind, std::string_view name) {
  for (const Tag& t : tags)
    if (t.kind == kind && t.name == name) return true;
  return false;
}

bool not_a_capability(std::string_view kind, std::string_view name) {
  if (kind == "ground" && name == "waves") return true;
  return name.find("test") != std::string_view::npos;
}

void untagged(const std::vector<Tag>& tags, std::string_view kind,
              const Vector<std::string_view>& names, std::string& out) {
  for (std::string_view name : names)
    if (!tagged(tags, kind, name) && !not_a_capability(kind, name))
      out += " " + std::string(kind) + ":" + std::string(name);
}

}  // namespace

TEST_CASE("generators: every registration the build tags is in " ENGINE_HOST_NAME "'s registries") {
  const std::vector<Tag> tags = build_tags();
  std::string missing;
  for (const Tag& tag : tags)
    if (!registered(tag)) missing += " " + tag.kind + ":" + tag.name;
  MESSAGE(ENGINE_HOST_NAME " carries " << tags.size() << " tagged registrations");
  CHECK_MESSAGE(missing.empty(), ENGINE_HOST_NAME
                                     " links the generators tagged with these, and "
                                     "their registrars are not in its registries (dropped by the "
                                     "linker, or the tag names what no source registers):"
                                     << missing);
}

TEST_CASE("generators: nothing is registered in " ENGINE_HOST_NAME " that no tag names") {
  const std::vector<Tag> tags = build_tags();
  const scene_gen::GeneratorRegistry& scenes = scene_gen::GeneratorRegistry::global();
  std::string extra;
  untagged(tags, "ground", scenes.ground_names(), extra);
  untagged(tags, "placement", scenes.placement_names(), extra);
  untagged(tags, "sky", scenes.sky_names(), extra);
  untagged(tags, "mesh", assets::MeshGeneratorRegistry::global().names(), extra);
  CHECK_MESSAGE(extra.empty(), "registered in " ENGINE_HOST_NAME
                               " and named by no GENERATOR tag (add each to its module's "
                               "engine_module() line):"
                                   << extra);
}
