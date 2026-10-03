// Size table for foundation/window (ADR-0019).
#include <core/base/size_table.h>
#include <foundation/window/window.h>

using namespace engine;

// 32 bytes until the gamepad events: the slot id, the button and the axis pack into one word
// after `dy`, and the axis value adds the next. The raw-joystick slot, index, and hat fill that
// word's last byte and start another, so `value` lands at 40 and the record is 44. One Event
// exists at a time per poll(), so the twelve bytes buy clarity at no cost. Since 2026-10-03 the
// platform's timestamp (`timestamp_ns`, a u64) follows at 48 and the record is 56, aligned to 8:
// it is what tells motion reported before relative mode came on from motion after it.
ENGINE_EXPECT_SIZE(56, 8, window::Event);
// `relative_since_ns_`, the moment relative mode came on, took the window from 24 bytes to 32.
ENGINE_EXPECT_SIZE(32, 8, window::Window);
