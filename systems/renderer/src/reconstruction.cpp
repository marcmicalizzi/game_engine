#include <systems/renderer/reconstruction.h>

namespace engine::renderer {

namespace {

// **The null provider**: it hands its input on, so the encode reads the colour image the resolve
// and the frame passes left, and a frame drawn through it is the frame drawn without one through
// the linear image. It keeps nothing; its state is a marker, since a null state refuses a load.
u32 g_null_state = 0;

void* null_create(const ReconstructionCreate&, std::string*) { return &g_null_state; }
void null_destroy(void*) noexcept {}
bool null_resize(void*, u32, u32, std::string*) { return true; }
bool null_record(void*, const ReconstructionFrame&, ReconstructionOutput& out, std::string*) {
  out = ReconstructionOutput{};
  return true;
}
u64 null_bytes(const void*) noexcept { return 0; }

constexpr ReconstructionProviderDesc k_null_provider{k_reconstruction_null, &null_create,
                                                     &null_destroy,        &null_resize,
                                                     &null_record,         &null_bytes};

ReconstructionProviderDesc g_providers[k_max_reconstruction_providers] = {k_null_provider};
u32 g_provider_count = 1;

bool same(const char* name, std::string_view other) noexcept {
  return name != nullptr && std::string_view(name) == other;
}

}  // namespace

bool register_reconstruction_provider(const ReconstructionProviderDesc& desc) noexcept {
  if (desc.name == nullptr || desc.name[0] == '\0' || same(desc.name, k_reconstruction_engine) ||
      desc.create == nullptr || desc.destroy == nullptr || desc.record == nullptr) {
    return false;
  }
  if (find_reconstruction_provider(desc.name) != nullptr) return false;
  if (g_provider_count == k_max_reconstruction_providers) return false;
  g_providers[g_provider_count++] = desc;
  return true;
}

const ReconstructionProviderDesc* find_reconstruction_provider(std::string_view name) noexcept {
  for (u32 i = 0; i < g_provider_count; ++i) {
    if (same(g_providers[i].name, name)) return &g_providers[i];
  }
  return nullptr;
}

}  // namespace engine::renderer
