#pragma once

// **The constants of the one-dimensional profile** (roadmap W120; the grounding in
// docs/experiments/stream-profile.md): what the solver and the fixture's closed forms share, so the
// two are held to one model and differ only in how they integrate it.

#include <core/base/types.h>
#include <domain/channels/falls.h>

namespace engine::channels {

// g, the falls' constant: a Froude number here and a jet there are the same water.
inline constexpr f64 k_gravity = k_fall_gravity;

// **Below a free fall** (Rand 1955, "Flow geometry at straight drop spillways", as Chow 1959
// section 15-12 gives it for a drop of height h and a unit discharge whose critical depth is y_c,
// the drop number D = (y_c / h)^3): the depth where the channel's surface resumes at the nappe's
// foot, y_1 / y_c = 0.54 (y_c / h)^0.275, and the distance from the lip to it,
// L_d / h = 4.30 (y_c / h)^0.81. For a section that is not a rectangle the critical depth is read
// as the hydraulic depth at critical (A_c / T_c) and the foot's depth as the area
// A_1 = 0.54 (D_c / h)^0.275 A_c: the same ratio of speeds, which is what the formula measures.
inline constexpr f64 k_drop_foot_share = 0.54;
inline constexpr f64 k_drop_foot_exponent = 0.275;
inline constexpr f64 k_drop_length_heights = 4.30;
inline constexpr f64 k_drop_length_exponent = 0.81;

// **A hydraulic jump's length**, from its head to where the sequent depth stands: six times the
// rise, L = 6 (y_2 - y_1) (Smetana's rule, within the scatter of the USBR's curve for Froude
// numbers of 2 to 9; stream-profile.md says how far each was read).
inline constexpr f64 k_jump_length_rises = 6.0;

}  // namespace engine::channels
