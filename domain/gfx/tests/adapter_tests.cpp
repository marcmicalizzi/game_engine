#include <domain/gfx/adapter.h>

#include <doctest/doctest.h>

#include <string>

using namespace engine;

TEST_CASE("gfx: adapter enumeration reports devices or a reason") {
  std::string error;
  Vector<gfx::AdapterInfo> adapters;
  const bool ok = gfx::enumerate_adapters(adapters, &error);
  if (!ok) {
    // Build and test machines without a GPU driver land here; that is a supported outcome. A
    // loader may be present without any driver behind it (CI runners), so vulkan_available()
    // can be true here.
    MESSAGE("Vulkan unavailable: " << error);
    CHECK_FALSE(error.empty());
    CHECK(adapters.empty());
    return;
  }
  CHECK(gfx::vulkan_available());
  CHECK(error.empty());
  REQUIRE_FALSE(adapters.empty());
  MESSAGE(gfx::describe_adapters(adapters));
  for (const gfx::AdapterInfo& a : adapters) {
    CHECK_FALSE(a.name.empty());
    CHECK(a.api_version.find('.') != std::string::npos);
    CHECK(a.api_version[0] == '1');
    CHECK(a.extensions.size() == gfx::extensions_of_interest().size());
    CHECK((a.tier == "raster" || a.tier == "rt" || a.tier == "rt-cluster"));
    CHECK_FALSE(a.queue_families.empty());
    bool graphics = false;
    for (const gfx::QueueFamilyInfo& q : a.queue_families)
      graphics = graphics || q.graphics;
    if (a.type != gfx::AdapterType::Cpu) CHECK(graphics);
    for (const char* name : gfx::extensions_of_interest()) {
      const bool present = *a.extensions.find_value(std::string_view(name));
      const std::string line = a.name + ": " + name + " = " + (present ? "yes" : "no");
      MESSAGE(line);
    }
  }
  // Sorted: discrete first, then by memory.
  for (u32 i = 1; i < adapters.size(); ++i) {
    const bool prev_discrete = adapters[i - 1].type == gfx::AdapterType::DiscreteGpu;
    const bool cur_discrete = adapters[i].type == gfx::AdapterType::DiscreteGpu;
    CHECK((prev_discrete || !cur_discrete));
  }
}
