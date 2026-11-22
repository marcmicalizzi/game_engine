#pragma once

// **A lost device is reported, and its host gets a turn before the process ends** (roadmap T212;
// docs/subsystems/gfx.md, "When the device is lost"). Until T212 the first `VK_ERROR_DEVICE_LOST`
// a frame met was an `ENGINE_VERIFY` and an abort: the owner's walk of 2026-11-21 died so at
// `FrameContext::recycle`'s wait, and left no input log, no frame records and no report, so the
// one walk that faulted the card could not be replayed or attributed.
//
// Now the first loss gfx meets — a frame's wait for its slot, a frame's or an immediate
// submission, a swapchain's acquire or present — goes through one function (`Device::report_lost`):
//
// - where the device has `VK_EXT_device_fault` (enabled wherever it is offered; nothing drawn
//   depends on it), the fault is read **once**: the driver's description, each address record
//   (what kind of access, the address, how precise it is), each vendor record, and the vendor's
//   binary crash dump when the device offers one (`deviceFaultVendorBinary`);
// - the loss and the fault go to the log as typed fields (`gfx.device_lost`, one line for the
//   loss, one per record);
// - the host's handler (`Device::set_lost_handler`) is called with a `DeviceLost` in the engine's
//   own vocabulary. It runs on the thread that met the loss, writes what the host has (engine-view:
//   its input log, its frame records, its report with a `device_lost` block, its startup trace,
//   apps.md "When the device is lost") and ends the process with a code of its own. **It must not
//   return**: if it does, or no handler is set, gfx aborts as it always did.
//
// A second thread meeting the loss while the first is in the handler waits for the process to end
// and never calls the handler twice. A handler that touches the device is allowed to (a lost
// device's calls return `VK_ERROR_DEVICE_LOST` in bounded time, by the specification), but
// whatever it does it bounds itself: engine-view's ends the process after a fixed time however far
// its writes got.
//
// **The test seam** (`k_test_lose_device_env`): under a test's main alone — the rule
// `test_profile.h` gives a device profile — `ENGINE_GFX_TEST_LOSE_DEVICE=<n>` makes the first
// frame wait for timeline value `n` or later answer "lost" as a real loss would, with no fault
// read (`DeviceLost::simulated`). So a case drives a host's whole path without faulting a card.

#include <core/base/types.h>
#include <core/containers/vector.h>

#include <string>
#include <string_view>

namespace engine::gfx {

// Where the loss was met.
enum class DeviceLostAt : u8 {
  frame_wait,    // FrameContext's wait for a slot's last submission (`begin_frame`)
  frame_submit,  // FrameContext::end_frame's queue submission
  immediate,     // an immediate submission or its fence (uploads, `submit_immediate`)
  acquire,       // Swapchain::acquire
  present,       // Swapchain::present
};
const char* device_lost_at_name(DeviceLostAt at) noexcept;

// What kind of address a fault record names (VK_EXT_device_fault's address types, renamed).
enum class FaultAddressKind : u8 {
  none,
  read_invalid,                 // a read of an address that was not valid
  write_invalid,                // a write of one
  execute_invalid,              // an instruction fetch from one
  instruction_pointer_unknown,  // an instruction pointer, not known to be the faulting one
  instruction_pointer_invalid,  // the instruction pointer of an invalid instruction
  instruction_pointer_fault,    // the instruction pointer of the fault
  other,                        // a kind this build does not know
};
const char* fault_address_kind_name(FaultAddressKind kind) noexcept;

struct FaultAddress {
  FaultAddressKind kind = FaultAddressKind::none;
  u64 address = 0;
  // The address is known to within this many bytes, aligned down to it (a power of two; 0 or 1 is
  // exact). VK_EXT_device_fault's `addressPrecision`.
  u64 precision = 0;
};

struct FaultVendorRecord {
  std::string description;
  u64 code = 0;
  u64 data = 0;
};

// The device's account of a fault (VK_EXT_device_fault), as read once on the first loss.
struct DeviceFault {
  // The device had the extension and answered the query. False: the rest is empty, and the reason
  // is in `unavailable` ("no VK_EXT_device_fault on this device", "a simulated loss", the call's
  // result).
  bool read = false;
  std::string unavailable;
  std::string description;
  Vector<FaultAddress> addresses;
  Vector<FaultVendorRecord> vendors;
  // The vendor's binary crash dump, as the driver gave it (its own header first); empty when the
  // device does not offer one or had none. A host writes it to a file beside its report.
  Vector<u8> vendor_binary;
};

// What a host's handler is told.
struct DeviceLost {
  DeviceLostAt at = DeviceLostAt::frame_wait;
  // The frame timeline's values (FrameContext): the one the loss was met at (the value waited for,
  // or the submission's), the last submitted, and the last the device had signalled, as read when
  // the loss was met (0 where it could not be read). 0 for a loss met outside a frame context.
  u64 value = 0;
  u64 last_submitted = 0;
  u64 last_completed = 0;
  bool simulated = false;  // the test seam, not the device
  DeviceFault fault;
};

// Must not return (see above). `context` is the host's.
using DeviceLostHandler = void (*)(void* context, const DeviceLost& lost);

// The test seam's variable, and the frame value it names in this process: 0 when it is unset, or
// set and not honoured (no live test run named: `test_profile.h`'s rule, which the log says).
inline constexpr const char* k_test_lose_device_env = "ENGINE_GFX_TEST_LOSE_DEVICE";
u64 test_lose_device_at();
// The value of `text` as the seam reads it: a positive decimal, or 0.
u64 parse_lose_device_at(std::string_view text) noexcept;

}  // namespace engine::gfx
