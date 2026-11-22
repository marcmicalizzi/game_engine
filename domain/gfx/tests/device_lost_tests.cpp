// A lost device's names and the test seam's reading (device_lost.h, roadmap T212). No device: the
// path a real loss takes through a host is engine-view's case (apps/engine_view/tests/
// device_lost_view_tests.cpp), which drives it through the seam.

#include <domain/gfx/device.h>
#include <domain/gfx/device_lost.h>

#include <doctest/doctest.h>

#include <string_view>

using namespace engine;
using namespace engine::gfx;

TEST_CASE("gfx: a lost device's places and fault kinds have names, one each") {
  const DeviceLostAt places[] = {DeviceLostAt::frame_wait, DeviceLostAt::frame_submit,
                                 DeviceLostAt::immediate, DeviceLostAt::acquire,
                                 DeviceLostAt::present};
  for (u32 i = 0; i < 5; ++i) {
    for (u32 j = i + 1; j < 5; ++j)
      CHECK(std::string_view(device_lost_at_name(places[i])) != device_lost_at_name(places[j]));
  }
  CHECK(std::string_view(device_lost_at_name(DeviceLostAt::frame_wait)) == "frame_wait");
  const FaultAddressKind kinds[] = {FaultAddressKind::none,
                                    FaultAddressKind::read_invalid,
                                    FaultAddressKind::write_invalid,
                                    FaultAddressKind::execute_invalid,
                                    FaultAddressKind::instruction_pointer_unknown,
                                    FaultAddressKind::instruction_pointer_invalid,
                                    FaultAddressKind::instruction_pointer_fault,
                                    FaultAddressKind::other};
  u32 distinct = 0;
  for (u32 i = 0; i < 8; ++i) {
    bool unique = true;
    for (u32 j = 0; j < i; ++j)
      unique = unique && std::string_view(fault_address_kind_name(kinds[i])) !=
                             fault_address_kind_name(kinds[j]);
    distinct += unique ? 1u : 0u;
  }
  CHECK(distinct == 8);
}

TEST_CASE("gfx: the lost-device seam reads a positive decimal frame value and nothing else") {
  CHECK(parse_lose_device_at("") == 0);
  CHECK(parse_lose_device_at("0") == 0);
  CHECK(parse_lose_device_at("20") == 20);
  CHECK(parse_lose_device_at("18446744073709551") == 18446744073709551ull);
  CHECK(parse_lose_device_at("12a") == 0);
  CHECK(parse_lose_device_at("-3") == 0);
  CHECK(parse_lose_device_at(" 4") == 0);
  CHECK(parse_lose_device_at("12345678901234567890") == 0);  // past 19 digits
}

TEST_CASE("gfx: a device that was never created reports no loss and is not lost") {
  Device device;
  device.set_lost_handler(nullptr, nullptr);
  device.report_lost(DeviceLostAt::present, 1, 1, 0);  // no device: returns, calls nothing
  CHECK_FALSE(device.lost());
}
