// The mesh-generator registry (mesh_generator.h).
#include <core/base/assert.h>
#include <domain/assets/mesh_generator.h>

#include <algorithm>

namespace engine::assets {

std::string_view mesh_generator_of(std::string_view path) noexcept {
  constexpr std::string_view k_json = ".json";
  if (path.size() <= k_json.size() || path.substr(path.size() - k_json.size()) != k_json) return {};
  const std::string_view stem = path.substr(0, path.size() - k_json.size());
  // The second extension: after the last '.', within the file's own name.
  const usize dot = stem.find_last_of('.');
  const usize slash = stem.find_last_of("/\\");
  if (dot == std::string_view::npos || (slash != std::string_view::npos && dot < slash)) return {};
  // A dot that begins the file's name is a hidden file's, not an extension.
  if (dot == 0 || (slash != std::string_view::npos && dot == slash + 1)) return {};
  const std::string_view name = stem.substr(dot + 1);
  return name.empty() ? std::string_view{} : name;
}

MeshGeneratorRegistry& MeshGeneratorRegistry::global() {
  // A function-local static, so a registrar in another translation unit's static initialization
  // finds it constructed whatever the order the linker put them in.
  static MeshGeneratorRegistry registry;
  return registry;
}

bool MeshGeneratorRegistry::add(const MeshGeneratorDesc& desc) noexcept {
  if (desc.name == nullptr || desc.name[0] == '\0' || desc.source_hash == nullptr ||
      desc.build == nullptr) {
    return false;
  }
  for (const MeshGeneratorDesc* held : generators_) {
    if (held == &desc) return true;
    if (std::string_view(held->name) == desc.name) return false;
  }
  generators_.push_back(&desc);
  return true;
}

const MeshGeneratorDesc* MeshGeneratorRegistry::find(std::string_view name) const noexcept {
  for (const MeshGeneratorDesc* held : generators_)
    if (std::string_view(held->name) == name) return held;
  return nullptr;
}

Vector<std::string_view> MeshGeneratorRegistry::names() const {
  Vector<std::string_view> out;
  for (const MeshGeneratorDesc* held : generators_)
    out.push_back(held->name);
  std::sort(out.begin(), out.end());
  return out;
}

std::string MeshGeneratorRegistry::unknown(std::string_view path, std::string_view name) const {
  std::string upper(name);
  for (char& c : upper)
    c = (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
  std::string text = std::string(path) + " is a mesh made by the generator \"" + std::string(name) +
                     "\", which this build does not carry: it comes with the " + std::string(name) +
                     " capability (ENGINE_WITH_" + upper +
                     "), switched off here or not linked into this program; the generators "
                     "it has: ";
  const Vector<std::string_view> have = names();
  if (have.empty()) text += "none";
  for (u32 i = 0; i < have.size(); ++i) {
    if (i != 0) text += ", ";
    text += have[i];
  }
  return text;
}

MeshGeneratorRegistrar::MeshGeneratorRegistrar(const MeshGeneratorDesc& desc) noexcept {
  ENGINE_VERIFY(MeshGeneratorRegistry::global().add(desc),
                "assets: a mesh generator with no name, source_hash or build, or a second one "
                "under a name another holds");
}

}  // namespace engine::assets
