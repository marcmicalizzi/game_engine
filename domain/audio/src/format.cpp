#include <domain/audio/format.h>
#include <domain/audio/spatial.h>

namespace engine::audio {

namespace {

// The shipped profiles. Angles are the canonical ones — ITU-R BS.775 for 5.1 (fronts at 30
// degrees, surrounds at 110), BS.2051 System I for 7.1 (sides at 90, backs at 135) and System J for
// 7.1.4 (the same bed plus a height ring at azimuth 45 and 135, elevation 45) — written as plain
// numbers so the table is data a calibration can later override row for row.

using S = Speaker;

constexpr LayoutInfo k_unknown{};

constexpr LayoutInfo k_mono{ChannelLayout::Mono, "mono", 1, 0, 0, {S::Mono}, {0.0f}, {0.0f}};

constexpr LayoutInfo k_stereo{ChannelLayout::Stereo,         "stereo",        2,           0, 1,
                              {S::FrontLeft, S::FrontRight}, {-30.0f, 30.0f}, {0.0f, 0.0f}};

// The transducers are at the ears. The stereo panner reads only the front pair, so it treats this
// row exactly as stereo; the positions are what a binaural or crosstalk-aware decoder will read.
constexpr LayoutInfo k_headphones{ChannelLayout::Headphones,     "headphones",    2,           0, 1,
                                  {S::FrontLeft, S::FrontRight}, {-90.0f, 90.0f}, {0.0f, 0.0f}};

constexpr LayoutInfo k_quad{ChannelLayout::Quad,
                            "quad",
                            4,
                            0,
                            1,
                            {S::FrontLeft, S::FrontRight, S::BackLeft, S::BackRight},
                            {-45.0f, 45.0f, -135.0f, 135.0f},
                            {0.0f, 0.0f, 0.0f, 0.0f}};

constexpr LayoutInfo k_surround51{
    ChannelLayout::Surround51,
    "surround51",
    6,
    0,
    1,
    {S::FrontLeft, S::FrontRight, S::FrontCentre, S::LowFrequency, S::BackLeft, S::BackRight},
    {-30.0f, 30.0f, 0.0f, 0.0f, -110.0f, 110.0f},
    {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}};

constexpr LayoutInfo k_surround71{ChannelLayout::Surround71,
                                  "surround71",
                                  8,
                                  0,
                                  1,
                                  {S::FrontLeft, S::FrontRight, S::FrontCentre, S::LowFrequency,
                                   S::BackLeft, S::BackRight, S::SideLeft, S::SideRight},
                                  {-30.0f, 30.0f, 0.0f, 0.0f, -135.0f, 135.0f, -90.0f, 90.0f},
                                  {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}};

constexpr LayoutInfo k_surround714{
    ChannelLayout::Surround714,
    "surround714",
    12,
    0,
    1,
    {S::FrontLeft, S::FrontRight, S::FrontCentre, S::LowFrequency, S::BackLeft, S::BackRight,
     S::SideLeft, S::SideRight, S::TopFrontLeft, S::TopFrontRight, S::TopBackLeft, S::TopBackRight},
    {-30.0f, 30.0f, 0.0f, 0.0f, -135.0f, 135.0f, -90.0f, 90.0f, -45.0f, 45.0f, -135.0f, 135.0f},
    {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 45.0f, 45.0f, 45.0f, 45.0f}};

constexpr const LayoutInfo* k_profiles[] = {&k_mono,       &k_stereo,     &k_headphones, &k_quad,
                                            &k_surround51, &k_surround71, &k_surround714};

bool same(std::span<const Speaker> map, const LayoutInfo& layout) noexcept {
  if (map.size() != layout.channels) return false;
  for (usize i = 0; i < map.size(); ++i) {
    if (map[i] != layout.speakers[i]) return false;
  }
  return true;
}

}  // namespace

const char* speaker_name(Speaker speaker) noexcept {
  switch (speaker) {
    case Speaker::FrontLeft: return "front_left";
    case Speaker::FrontRight: return "front_right";
    case Speaker::FrontCentre: return "front_centre";
    case Speaker::LowFrequency: return "low_frequency";
    case Speaker::BackLeft: return "back_left";
    case Speaker::BackRight: return "back_right";
    case Speaker::SideLeft: return "side_left";
    case Speaker::SideRight: return "side_right";
    case Speaker::TopFrontLeft: return "top_front_left";
    case Speaker::TopFrontRight: return "top_front_right";
    case Speaker::TopBackLeft: return "top_back_left";
    case Speaker::TopBackRight: return "top_back_right";
    case Speaker::Mono: return "mono";
    case Speaker::Count: break;
  }
  return "unknown";
}

const LayoutInfo& layout_info(ChannelLayout layout) noexcept {
  switch (layout) {
    case ChannelLayout::Mono: return k_mono;
    case ChannelLayout::Stereo: return k_stereo;
    case ChannelLayout::Headphones: return k_headphones;
    case ChannelLayout::Quad: return k_quad;
    case ChannelLayout::Surround51: return k_surround51;
    case ChannelLayout::Surround71: return k_surround71;
    case ChannelLayout::Surround714: return k_surround714;
    case ChannelLayout::Unknown: break;
  }
  return k_unknown;
}

const char* layout_name(ChannelLayout layout) noexcept { return layout_info(layout).name; }

Vec3 speaker_direction(const LayoutInfo& layout, u32 channel) noexcept {
  if (channel >= layout.channels || layout.speakers[channel] == Speaker::LowFrequency)
    return Vec3{};
  const f32 azimuth = layout.azimuth[channel];
  const f32 elevation = layout.elevation[channel];
  const f32 horizontal = cos_degrees(elevation);
  // sin(a) = cos(90 - a).
  return Vec3{cos_degrees(90.0f - azimuth) * horizontal, cos_degrees(90.0f - elevation),
              -cos_degrees(azimuth) * horizontal};
}

ChannelLayout layout_from_speakers(std::span<const Speaker> speakers) noexcept {
  // Headphones share stereo's map and are never what a map says; stereo comes first and wins.
  for (const LayoutInfo* layout : k_profiles) {
    if (layout->layout == ChannelLayout::Headphones) continue;
    if (same(speakers, *layout)) return layout->layout;
  }
  if (speakers.size() == 1 && speakers[0] == Speaker::FrontCentre) return ChannelLayout::Mono;
  if (speakers.size() == 6 && speakers[0] == Speaker::FrontLeft &&
      speakers[1] == Speaker::FrontRight && speakers[2] == Speaker::FrontCentre &&
      speakers[3] == Speaker::LowFrequency && speakers[4] == Speaker::SideLeft &&
      speakers[5] == Speaker::SideRight) {
    return ChannelLayout::Surround51;
  }
  return ChannelLayout::Unknown;
}

}  // namespace engine::audio
