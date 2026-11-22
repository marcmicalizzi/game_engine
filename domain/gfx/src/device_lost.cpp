#include <domain/gfx/device_lost.h>

// The names a lost device's log lines and a host's report use (device_lost.h). The reading of the
// fault is device.cpp's (`Device::report_lost`), and the test seam's variable is
// test_profile.cpp's, which owns the rule that honours a test's variables only under a test's main.

namespace engine::gfx {

const char* device_lost_at_name(DeviceLostAt at) noexcept {
  switch (at) {
    case DeviceLostAt::frame_wait: return "frame_wait";
    case DeviceLostAt::frame_submit: return "frame_submit";
    case DeviceLostAt::immediate: return "immediate";
    case DeviceLostAt::acquire: return "acquire";
    case DeviceLostAt::present: return "present";
  }
  return "unknown";
}

const char* fault_address_kind_name(FaultAddressKind kind) noexcept {
  switch (kind) {
    case FaultAddressKind::none: return "none";
    case FaultAddressKind::read_invalid: return "read_invalid";
    case FaultAddressKind::write_invalid: return "write_invalid";
    case FaultAddressKind::execute_invalid: return "execute_invalid";
    case FaultAddressKind::instruction_pointer_unknown: return "instruction_pointer_unknown";
    case FaultAddressKind::instruction_pointer_invalid: return "instruction_pointer_invalid";
    case FaultAddressKind::instruction_pointer_fault: return "instruction_pointer_fault";
    case FaultAddressKind::other: return "other";
  }
  return "other";
}

u64 parse_lose_device_at(std::string_view text) noexcept {
  if (text.empty() || text.size() > 19) return 0;
  u64 value = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') return 0;
    value = value * 10 + static_cast<u64>(c - '0');
  }
  return value;
}

}  // namespace engine::gfx
