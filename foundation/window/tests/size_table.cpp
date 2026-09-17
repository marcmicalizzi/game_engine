// Size table for foundation/window (ADR-0019).
#include <core/base/size_table.h>
#include <foundation/window/window.h>

using namespace engine;

// 32 bytes until the gamepad events: the slot id, the button and the axis pack into one word
// after `dy`, and the axis value adds the next. The raw-joystick slot, index, and hat fill that
// word's last byte and start another, so `value` lands at 40 and the record is 44. One Event
// exists at a time per poll(), so the twelve bytes buy clarity at no cost.
ENGINE_EXPECT_SIZE(44, 4, window::Event);
ENGINE_EXPECT_SIZE(24, 8, window::Window);
