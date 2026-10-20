#include "time_controls.h"

#include <cmath>
#include <cstdio>

namespace engine::view {

f64 ladder_step(std::span<const f64> ladder, f64 rate, bool up) noexcept {
  if (ladder.empty()) return rate;
  if (up) {
    for (const f64 rung : ladder) {
      if (rung > rate) return rung;
    }
    return rate > ladder[ladder.size() - 1] ? ladder[ladder.size() - 1] : rate;
  }
  for (usize i = ladder.size(); i > 0; --i) {
    if (ladder[i - 1] < rate) return ladder[i - 1];
  }
  return rate < ladder[0] ? ladder[0] : rate;
}

const char* format_rate(f64 rate, char* out, usize size) noexcept {
  if (size == 0) return out;
  out[0] = '\0';
  if (!std::isfinite(rate) || rate < 0.0 || rate >= 1.0e15) {
    std::snprintf(out, size, "%g", rate);
    return out;
  }
  // Below one, the slow-motion rungs as they are: "0.125", "0.25", "0.5".
  if (rate > 0.0 && rate < 1.0) {
    std::snprintf(out, size, "%.3g", rate);
    return out;
  }
  // Tenths, rounded once, so 5,000.46 is "5,000.5" and 59.97 is "60.0" rather than "59.10".
  const u64 tenths = static_cast<u64>(std::llround(rate * 10.0));
  const u64 whole = tenths / 10;
  const u64 tenth = tenths % 10;
  char digits[24];
  const int n =
      std::snprintf(digits, sizeof(digits), "%llu", static_cast<unsigned long long>(whole));
  char grouped[40];
  usize g = 0;
  for (int i = 0; i < n; ++i) {
    if (i > 0 && (n - i) % 3 == 0) grouped[g++] = ',';
    grouped[g++] = digits[i];
  }
  grouped[g] = '\0';
  const bool fraction = rate != std::floor(rate);
  if (fraction) {
    std::snprintf(out, size, "%s.%u", grouped, static_cast<unsigned>(tenth));
  } else {
    std::snprintf(out, size, "%s", grouped);
  }
  return out;
}

const char* format_day_length(f64 sun_rate, char* out, usize size) noexcept {
  if (size == 0) return out;
  if (!(sun_rate > 0.0) || !std::isfinite(sun_rate)) {
    std::snprintf(out, size, "still");
    return out;
  }
  const f64 length = 86'400.0 / sun_rate;
  f64 value = length;
  const char* unit = "s";
  if (length >= 3'600.0) {
    value = length / 3'600.0;
    unit = "h";
  } else if (length >= 60.0) {
    value = length / 60.0;
    unit = "min";
  }
  const f64 tenths = std::round(value * 10.0);
  if (std::fmod(tenths, 10.0) == 0.0) {
    std::snprintf(out, size, "%.0f %s", tenths / 10.0, unit);
  } else if (value >= 0.1) {
    std::snprintf(out, size, "%.1f %s", tenths / 10.0, unit);
  } else {
    std::snprintf(out, size, "%.3g %s", value, unit);
  }
  return out;
}

const char* format_hour(f64 hour, char* out, usize size) noexcept {
  if (size == 0) return out;
  if (!std::isfinite(hour)) hour = 0.0;
  f64 minutes = std::floor(hour * 60.0);
  minutes -= std::floor(minutes / 1'440.0) * 1'440.0;
  const int m = static_cast<int>(minutes);
  std::snprintf(out, size, "%02d:%02d", (m / 60) % 24, m % 60);
  return out;
}

usize format_status(const TitleStatus& status, char* out, usize size) noexcept {
  if (size == 0) return 0;
  // UTF-8 spelled as bytes, so the source says the same thing under any compiler's idea of the
  // execution character set: U+00D7 MULTIPLICATION SIGN and U+00B7 MIDDLE DOT.
  constexpr const char* k_times = "\xC3\x97";
  constexpr const char* k_dot = " \xC2\xB7 ";
  char world[32];
  char day[32];
  char hour[16];
  format_rate(status.world_rate, world, sizeof(world));
  format_day_length(status.sun_rate, day, sizeof(day));
  hour[0] = '\0';
  if (status.has_hour) format_hour(status.hour, hour, sizeof(hour));
  const char* mode = "";
  switch (status.mode) {
    case TitleMode::none: break;
    case TitleMode::flying: mode = "flying"; break;
    case TitleMode::walking: mode = "walking"; break;
    case TitleMode::walking_ground: mode = "walking (ground only)"; break;
  }
  const char* after_mode = mode[0] != '\0' ? k_dot : "";
  const char* after_hour = status.has_hour ? k_dot : "";
  const int n = std::snprintf(out, size, "%s%s%s%sday %s%sworld %s%s", mode, after_mode, hour,
                              after_hour, day, k_dot, k_times, world);
  if (n < 0) {
    out[0] = '\0';
    return 0;
  }
  usize used = static_cast<usize>(n) < size ? static_cast<usize>(n) : size - 1;
  if (status.live && used + 1 < size) {
    const int m = std::snprintf(out + used, size - used, "%s%s", k_dot,
                                status.captured ? "mouse captured (Esc frees it)"
                                                : "mouse free (click to capture, Esc again exits)");
    if (m > 0)
      used += static_cast<usize>(m) < size - used ? static_cast<usize>(m) : size - used - 1;
  }
  return used;
}

}  // namespace engine::view
