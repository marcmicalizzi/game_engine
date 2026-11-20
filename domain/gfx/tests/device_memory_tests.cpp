#include "raster_path.h"

#include <core/containers/vector.h>
#include <core/platform/address_space.h>
#include <domain/gfx/device.h>
#include <domain/gfx/device_memory.h>
#include <domain/gfx/resources.h>

#include <doctest/doctest.h>

#include <cstring>
#include <string>

using namespace engine;

// What the device holds (gfx.md, "What the device holds"), and the question the memory report
// exists for (docs/experiments/memory-commit-accounting.md): when the engine puts a known size on
// the GPU, how much does the *process* commit for it? A device-local buffer of 512 MiB is uploaded
// (through the staging buffer `upload_buffer` makes and frees) and a host-visible one of 256 MiB is
// made; the allocator's report must see both, and the process's commit charge and private memory
// before and after each are printed, because that is the driver's system-memory backing — the
// number the experiment page's table needs and no assertion can pin across drivers.

namespace {

constexpr u64 k_mib = 1024ull * 1024ull;

f64 mib_delta(u64 after, u64 before) {
  return (static_cast<f64>(after) - static_cast<f64>(before)) / static_cast<f64>(k_mib);
}

u64 private_committed(const platform::AddressSpaceReport& r) {
  return r.kinds[static_cast<u32>(platform::RegionKind::private_memory)].committed;
}
u64 private_resident(const platform::AddressSpaceReport& r) {
  return r.kinds[static_cast<u32>(platform::RegionKind::private_memory)].resident;
}

}  // namespace

TEST_CASE("gfx: the device memory report counts what is put on the device and in host memory") {
  gfx::Device device;
  if (!gfx_test::open_device(device)) return;
  std::string error;

  gfx::DeviceMemoryReport r0;
  REQUIRE(gfx::device_memory_report(device, r0));
  CHECK(r0.valid);
  platform::AddressSpaceReport a0;
  REQUIRE(platform::walk_address_space(a0));

  constexpr u64 k_device = 512 * k_mib;
  gfx::BufferResource local;
  {
    Vector<u8> data;
    data.resize(static_cast<u32>(k_device));
    std::memset(data.data(), 0x5a, k_device);
    REQUIRE_MESSAGE(
        gfx::upload_buffer(device, data.data(), k_device, gfx::BufferUsage::Storage, local, &error),
        error);
  }
  device.wait_idle();
  gfx::DeviceMemoryReport r1;
  REQUIRE(gfx::device_memory_report(device, r1));
  platform::AddressSpaceReport a1;
  REQUIRE(platform::walk_address_space(a1));
  CHECK(r1.device_local_block_bytes >= r0.device_local_block_bytes + k_device);
  CHECK(r1.device_local_allocation_bytes >= r0.device_local_allocation_bytes + k_device);
  MESSAGE("512 MiB uploaded to a device-local buffer: process commit "
          << mib_delta(a1.process_commit, a0.process_commit) << " MiB, private "
          << mib_delta(private_committed(a1), private_committed(a0)) << " MiB (resident "
          << mib_delta(private_resident(a1), private_resident(a0)) << " MiB), device-local blocks "
          << mib_delta(r1.device_local_block_bytes, r0.device_local_block_bytes)
          << " MiB, driver usage "
          << mib_delta(r1.device_local_usage_bytes, r0.device_local_usage_bytes) << " MiB");

  constexpr u64 k_host = 256 * k_mib;
  gfx::BufferResource host;
  REQUIRE_MESSAGE(
      gfx::create_buffer(device, k_host, gfx::BufferUsage::TransferSrc, true, host, &error), error);
  gfx::DeviceMemoryReport r2;
  REQUIRE(gfx::device_memory_report(device, r2));
  platform::AddressSpaceReport a2;
  REQUIRE(platform::walk_address_space(a2));
  CHECK(r2.host_block_bytes >= r1.host_block_bytes + k_host);
  CHECK(r2.host_allocation_bytes >= r1.host_allocation_bytes + k_host);
  MESSAGE("a 256 MiB host-visible buffer made and not written: process commit "
          << mib_delta(a2.process_commit, a1.process_commit) << " MiB, private "
          << mib_delta(private_committed(a2), private_committed(a1)) << " MiB, mapped "
          << mib_delta(a2.kinds[static_cast<u32>(platform::RegionKind::mapped_file)].committed,
                       a1.kinds[static_cast<u32>(platform::RegionKind::mapped_file)].committed)
          << " MiB (resident " << mib_delta(private_resident(a2), private_resident(a1))
          << " MiB private)");

  gfx::destroy_buffer(device, host);
  gfx::destroy_buffer(device, local);
}

// W51-6 (gfx.md, "What the device holds"): the log's events are every make and free in order, a
// free carrying its make's size and stack, and turning the log on empties them. No device: the
// log's own entry points, with keys no allocation has.
TEST_CASE("gfx: the large-buffer log keeps its makes and frees in order") {
  gfx::set_large_buffer_floor(gfx::k_large_buffer_bytes);
  gfx::set_large_buffer_log(true);
  REQUIRE(gfx::large_buffer_log_started_ns() != 0);
  constexpr u64 k_a = 0x1000, k_b = 0x2000;
  gfx::detail::note_large_buffer(k_a, 32 * k_mib, false);
  gfx::detail::note_large_buffer(k_b, 48 * k_mib, true);
  gfx::detail::forget_large_buffer(k_a);
  gfx::detail::forget_large_buffer(0x3000);  // never made: no event
  Vector<gfx::LargeBufferEvent> events;
  events.resize(8);
  const u32 kept = gfx::large_buffer_events(events.data(), 8);
  REQUIRE(kept == 3);
  CHECK(events[0].made);
  CHECK(events[0].bytes == 32 * k_mib);
  CHECK_FALSE(events[0].host_visible);
  CHECK(events[1].made);
  CHECK(events[1].host_visible);
  CHECK_FALSE(events[2].made);
  CHECK(events[2].bytes == 32 * k_mib);
  CHECK(events[2].depth == events[0].depth);
  CHECK(std::memcmp(events[2].stack, events[0].stack, sizeof(void*) * events[0].depth) == 0);
  CHECK(events[0].ns >= gfx::large_buffer_log_started_ns());
  CHECK(events[1].ns >= events[0].ns);
  CHECK(events[2].ns >= events[1].ns);
  CHECK(gfx::large_buffer_events_dropped() == 0);
  gfx::detail::forget_large_buffer(k_b);
  gfx::set_large_buffer_log(true);  // on again: a new log
  CHECK(gfx::large_buffer_events(events.data(), 8) == 0);
  gfx::set_large_buffer_log(false);
  CHECK(gfx::large_buffer_log_started_ns() == 0);
}
