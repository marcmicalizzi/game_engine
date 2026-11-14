#pragma once

// **A solved channel's vocabulary** (roadmap W120; docs/subsystems/channels.md, "The water's
// description"): the bits of `engine.scene.WaterChannel.flags` and the one check every reader of a
// description makes before it draws, excludes or measures by it. Here, in the module the writer
// (`domain/channels`), the draw (`systems/water`) and the scene's reader all stand on, so none of
// them keeps a copy of the bits; header-only, so nothing links for it.
//
// A description is **solved**: its level, depth, speed, Froude number and wet reaches at every
// station are the steady one-dimensional profile of its discharge through the ground's own
// sections. A block that carries none of the solved arrays was cooked before W120 (a level laid as
// a share of a cut's depth) and is refused by name; there is no reading of it in the old meaning.

#include <core/base/types.h>

#include <cmath>
#include <schemas/scene.h>
#include <string>

namespace engine::scene_gen {

// ---- `WaterChannel.flags`, one u16 a station ----------------------------------------------------
//
// **A control** is a station whose level the flow upstream or downstream of it is solved from, and
// its kind says what set it: a crest (critical depth over a step's or a fall's lip), a mouth (the
// level of the body the channel ends in), a confluence (the main stem's level where a tributary
// joins it), a spill (a pool's outlet: the level at which it runs over).
inline constexpr u16 k_channel_control = 1u << 0;
inline constexpr u16 k_channel_control_kind_shift = 1;
inline constexpr u16 k_channel_control_kind_mask = 3u << k_channel_control_kind_shift;
enum class ChannelControl : u8 { Crest = 0, Mouth = 1, Confluence = 2, Spill = 3 };
// **A discontinuity is a pair of neighbouring stations.** A hydraulic jump: its head carries the
// supercritical level, its foot the sequent level, a jump's length downstream. A fall: its lip
// (critical depth) and its foot (where the channel's surface resumes), with no channel surface
// between them (the falls' cook owns the water in the air). A head is always followed by its
// foot, a lip by its foot; between any two neighbours that are not such a pair the surface is the
// straight line between them (channels.md pins how near).
inline constexpr u16 k_channel_jump_head = 1u << 3;
inline constexpr u16 k_channel_jump_foot = 1u << 4;
inline constexpr u16 k_channel_fall_lip = 1u << 5;
inline constexpr u16 k_channel_fall_foot = 1u << 6;
// The solved level stands over a bank's top here: the bed does not hold its discharge. The cook
// counts and reports these; the wet reach stops at the bank's top (water is never written outside
// its bed).
inline constexpr u16 k_channel_over_bank = 1u << 7;
inline constexpr u16 k_channel_flags_known = 0x00ffu;

inline constexpr u16 channel_control_flags(ChannelControl kind) noexcept {
  return static_cast<u16>(k_channel_control |
                          (static_cast<u16>(kind) << k_channel_control_kind_shift));
}
inline constexpr bool channel_is_control(u16 flags) noexcept {
  return (flags & k_channel_control) != 0;
}
inline constexpr ChannelControl channel_control_kind(u16 flags) noexcept {
  return static_cast<ChannelControl>((flags & k_channel_control_kind_mask) >>
                                     k_channel_control_kind_shift);
}

// **The check** (the contract's refusals, each a sentence naming the channel): two points or more;
// every per-station array the points' long; a block with no solved array at all is "cooked before
// W120: cook again"; finite numbers, a positive width, depth and discharge, a speed, a Froude
// number and wet reaches that are not negative; no unknown flag bit; every jump head followed by
// its foot and every fall lip by its foot, and no foot without one.
inline bool water_channel_solved(const scene::WaterChannel& c, std::string* error) {
  const auto fail = [&](std::string sentence) {
    if (error != nullptr) *error = "water channel \"" + c.name + "\": " + std::move(sentence);
    return false;
  };
  const u32 n = c.points.size();
  if (n < 2) return fail("it has fewer than two points; a channel runs from one point to another");
  if (c.speed_mps.empty() && c.froude.empty() && c.wet_left_m.empty() && c.wet_right_m.empty() &&
      c.flags.empty()) {
    if (!c.discharge_m3s.empty())
      return fail(
          "it names a discharge and is not solved: `engine-content flow solve` writes "
          "its stations");
    return fail(
        "cooked before W120: cook again (its level was laid, not solved: it carries no "
        "discharge_m3s, speed_mps, froude, wet_left_m, wet_right_m or flags; "
        "`engine-content flow solve` writes them)");
  }
  const auto sized = [&](u32 size, const char* name) {
    if (size == n) return true;
    return fail(std::string("its ") + name + " has " + std::to_string(size) + " entries for " +
                std::to_string(n) + " points; every per-station array has one entry a point");
  };
  if (!sized(c.width_m.size(), "width_m") || !sized(c.depth_m.size(), "depth_m") ||
      !sized(c.slope.size(), "slope") || !sized(c.discharge_m3s.size(), "discharge_m3s") ||
      !sized(c.speed_mps.size(), "speed_mps") || !sized(c.froude.size(), "froude") ||
      !sized(c.wet_left_m.size(), "wet_left_m") || !sized(c.wet_right_m.size(), "wet_right_m") ||
      !sized(c.flags.size(), "flags"))
    return false;
  const auto positive = [](f32 v) { return std::isfinite(v) && v > 0.0f; };
  const auto not_negative = [](f32 v) { return std::isfinite(v) && v >= 0.0f; };
  for (u32 i = 0; i < n; ++i) {
    const std::string at = " at point " + std::to_string(i);
    const WorldPos& p = c.points[i];
    if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
      return fail("its point " + std::to_string(i) + " is not a number");
    if (!positive(c.width_m[i])) return fail("its width_m" + at + " must be positive");
    if (!positive(c.depth_m[i])) return fail("its depth_m" + at + " must be positive");
    if (!not_negative(c.slope[i])) return fail("its slope" + at + " must not be negative");
    if (!positive(c.discharge_m3s[i])) return fail("its discharge_m3s" + at + " must be positive");
    if (!not_negative(c.speed_mps[i])) return fail("its speed_mps" + at + " must not be negative");
    if (!not_negative(c.froude[i])) return fail("its froude" + at + " must not be negative");
    if (!not_negative(c.wet_left_m[i]) || !not_negative(c.wet_right_m[i]))
      return fail("its wet_left_m and wet_right_m" + at + " must not be negative");
    const u16 f = c.flags[i];
    if ((f & ~k_channel_flags_known) != 0)
      return fail("its flags" + at + " hold a bit this build does not know");
    const u16 before = i > 0 ? c.flags[i - 1] : u16{0};
    const u16 after = i + 1 < n ? c.flags[i + 1] : u16{0};
    if ((f & k_channel_jump_head) != 0 && (after & k_channel_jump_foot) == 0)
      return fail("its jump head" + at + " is not followed by the jump's foot");
    if ((f & k_channel_jump_foot) != 0 && (before & k_channel_jump_head) == 0)
      return fail("its jump foot" + at + " does not follow a jump's head");
    if ((f & k_channel_fall_lip) != 0 && (after & k_channel_fall_foot) == 0)
      return fail("its fall lip" + at + " is not followed by the fall's foot");
    if ((f & k_channel_fall_foot) != 0 && (before & k_channel_fall_lip) == 0)
      return fail("its fall foot" + at + " does not follow a fall's lip");
  }
  return true;
}

}  // namespace engine::scene_gen
