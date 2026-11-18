#include <systems/renderer/temporal_change.h>

#include <cmath>
#include <cstdio>
#include <cstring>

namespace engine::renderer {

namespace {

// The light a code stands for, as the encode made it (a 1/2.2 power; renderer.md, "The output
// encode"): a table, since every pixel of every frame looks it up three times.
struct LightTable {
  f64 value[256];
  LightTable() {
    for (u32 i = 0; i < 256; ++i) value[i] = std::pow(static_cast<f64>(i) / 255.0, 2.2);
  }
};
const LightTable& light_table() {
  static const LightTable table;
  return table;
}

f64 luma_of(const u8* p) {
  return 0.2126 * static_cast<f64>(p[0]) + 0.7152 * static_cast<f64>(p[1]) +
         0.0722 * static_cast<f64>(p[2]);
}
f64 light_of(const u8* p) {
  const LightTable& t = light_table();
  return 0.2126 * t.value[p[0]] + 0.7152 * t.value[p[1]] + 0.0722 * t.value[p[2]];
}
u32 band_of(f64 luma) {
  u32 band = 0;
  for (u32 b = 1; b < TemporalChange::k_bands; ++b)
    if (luma >= static_cast<f64>(TemporalChange::k_band_low[b])) band = b;
  return band;
}

// The value at quantile `q` of a histogram of `bins` bins `width` wide: the lower edge of the bin
// the quantile falls in (so a change of nothing reads 0).
f64 quantile(const u64* hist, u32 bins, f64 width, u64 total, f64 q) {
  if (total == 0) return 0.0;
  const f64 target = q * static_cast<f64>(total);
  u64 seen = 0;
  for (u32 i = 0; i < bins; ++i) {
    seen += hist[i];
    if (static_cast<f64>(seen) >= target) return static_cast<f64>(i) * width;
  }
  return static_cast<f64>(bins) * width;
}

void append_series(std::string& out, const char* name, const Vector<f64>& v) {
  out += "\"";
  out += name;
  out += "\":[";
  char buf[32];
  for (u32 i = 0; i < static_cast<u32>(v.size()); ++i) {
    std::snprintf(buf, sizeof(buf), i == 0 ? "%.5f" : ",%.5f", v[i]);
    out += buf;
  }
  out += "]";
}

}  // namespace

void TemporalChange::count(u32 band, f64 codes, f64 light, bool lit, u32 channel_codes) {
  // A whole step computed as a difference of two weighted sums can come out a hair under 1.0: the
  // bin is taken a millionth of a bin up, so a step is counted as one.
  const f64 at = codes / k_code_bin + 1e-6;
  const u32 cb = at >= static_cast<f64>(k_code_bins - 1) ? k_code_bins - 1 : static_cast<u32>(at);
  const u32 lb = light / k_light_bin >= static_cast<f64>(k_light_bins - 1)
                     ? k_light_bins - 1
                     : static_cast<u32>(light / k_light_bin);
  for (const u32 row : {band, k_bands}) {
    ++code_hist_[row * k_code_bins + cb];
    code_sum_[row] += codes;
    if (channel_codes >= 1) ++channel_one_[row];
    if (channel_codes >= 3) ++channel_three_[row];
    if (lit) {
      ++light_hist_[row * k_light_bins + lb];
      light_sum_[row] += light;
      ++lit_pairs_[row];
    }
  }
}

bool TemporalChange::add(const u8* rgba, u32 width, u32 height) {
  const usize bytes = static_cast<usize>(width) * height * 4u;
  const bool same = width == width_ && height == height_ && !previous_.empty();
  if (code_hist_.empty()) {
    code_hist_.resize((k_bands + 1) * k_code_bins, 0);
    light_hist_.resize((k_bands + 1) * k_light_bins, 0);
    code_sum_.resize(k_bands + 1, 0.0);
    light_sum_.resize(k_bands + 1, 0.0);
    lit_pairs_.resize(k_bands + 1, 0);
    channel_one_.resize(k_bands + 1, 0);
    channel_three_.resize(k_bands + 1, 0);
  }
  f64 luma_sum = 0.0;
  f64 light_sum = 0.0;
  f64 centre_luma = 0.0;
  f64 centre_light = 0.0;
  u64 centre_pixels = 0;
  // A pixel black in both frames has no light to change relative to; one code of light, half of
  // it, is the floor a mean is held above.
  const f64 floor = 0.5 * light_table().value[1];
  for (u32 y = 0; y < height; ++y) {
    const bool centre_row = y >= height / 4 && y < height - height / 4;
    for (u32 x = 0; x < width; ++x) {
      const usize at = (static_cast<usize>(y) * width + x) * 4u;
      const u8* p = rgba + at;
      const f64 luma = luma_of(p);
      const f64 light = light_of(p);
      luma_sum += luma;
      light_sum += light;
      if (centre_row && x >= width / 4 && x < width - width / 4) {
        centre_luma += luma;
        centre_light += light;
        ++centre_pixels;
      }
      if (!same) continue;
      const u8* q = previous_.data() + at;
      const f64 luma_before = luma_of(q);
      const f64 light_before = light_of(q);
      const f64 level = 0.5 * (light + light_before);
      const bool lit = level > floor;
      // The largest step any one channel took, in whole codes (the forest session's measure).
      u32 channel_codes = 0;
      for (u32 c = 0; c < 3; ++c) {
        const u32 d = p[c] > q[c] ? u32{p[c]} - q[c] : u32{q[c]} - p[c];
        channel_codes = d > channel_codes ? d : channel_codes;
      }
      count(band_of(0.5 * (luma + luma_before)), std::fabs(luma - luma_before),
            lit ? std::fabs(light - light_before) / level : 0.0, lit, channel_codes);
    }
  }
  const f64 pixels = static_cast<f64>(width) * static_cast<f64>(height);
  mean_luma_.push_back(pixels > 0.0 ? luma_sum / pixels : 0.0);
  mean_light_.push_back(pixels > 0.0 ? light_sum / pixels : 0.0);
  centre_luma_.push_back(centre_pixels > 0 ? centre_luma / static_cast<f64>(centre_pixels) : 0.0);
  centre_light_.push_back(centre_pixels > 0 ? centre_light / static_cast<f64>(centre_pixels) : 0.0);
  if (previous_.size() != bytes) previous_.resize(static_cast<u32>(bytes));
  std::memcpy(previous_.data(), rgba, bytes);
  width_ = width;
  height_ = height;
  ++frames_;
  return same || frames_ == 1;
}

TemporalChange::Stats TemporalChange::stats(u32 band) const {
  Stats s;
  if (code_hist_.empty() || band > k_bands) return s;
  const u64* codes = code_hist_.data() + band * k_code_bins;
  const u64* light = light_hist_.data() + band * k_light_bins;
  for (u32 i = 0; i < k_code_bins; ++i) s.pairs += codes[i];
  if (s.pairs == 0) return s;
  const f64 n = static_cast<f64>(s.pairs);
  s.mean_codes = code_sum_[band] / n;
  s.median_codes = quantile(codes, k_code_bins, k_code_bin, s.pairs, 0.5);
  s.p95_codes = quantile(codes, k_code_bins, k_code_bin, s.pairs, 0.95);
  s.p99_codes = quantile(codes, k_code_bins, k_code_bin, s.pairs, 0.99);
  s.p999_codes = quantile(codes, k_code_bins, k_code_bin, s.pairs, 0.999);
  u64 one = 0;
  u64 two = 0;
  for (u32 i = 16; i < k_code_bins; ++i) one += codes[i];
  for (u32 i = 32; i < k_code_bins; ++i) two += codes[i];
  s.at_least_one = static_cast<f64>(one) / n;
  s.at_least_two = static_cast<f64>(two) / n;
  s.any_channel_one = static_cast<f64>(channel_one_[band]) / n;
  s.any_channel_three = static_cast<f64>(channel_three_[band]) / n;
  s.light_pairs = lit_pairs_[band];
  if (s.light_pairs > 0) {
    s.mean_light = light_sum_[band] / static_cast<f64>(s.light_pairs);
    s.median_light = quantile(light, k_light_bins, k_light_bin, s.light_pairs, 0.5);
    s.p95_light = quantile(light, k_light_bins, k_light_bin, s.light_pairs, 0.95);
    s.p99_light = quantile(light, k_light_bins, k_light_bin, s.light_pairs, 0.99);
    s.p999_light = quantile(light, k_light_bins, k_light_bin, s.light_pairs, 0.999);
  }
  return s;
}

std::string TemporalChange::json() const {
  std::string out;
  char buf[768];
  std::snprintf(buf, sizeof(buf), "{\"format\":\"engine.temporal-change.v1\",\"frames\":%u,"
                                  "\"width\":%u,\"height\":%u,",
                frames_, width_, height_);
  out += buf;
  const auto stats_json = [&](u32 band) {
    const Stats s = stats(band);
    std::snprintf(buf, sizeof(buf),
                  "\"pairs\":%llu,\"codes\":{\"mean\":%.4f,\"median\":%.4f,\"p95\":%.4f,"
                  "\"p99\":%.4f,\"p999\":%.4f,\"at_least_one\":%.6f,\"at_least_two\":%.6f},"
                  "\"channels\":{\"at_least_one\":%.6f,\"at_least_three\":%.6f},"
                  "\"light\":{\"pairs\":%llu,\"mean\":%.5f,\"median\":%.4f,\"p95\":%.4f,"
                  "\"p99\":%.4f,\"p999\":%.4f}",
                  static_cast<unsigned long long>(s.pairs), s.mean_codes, s.median_codes,
                  s.p95_codes, s.p99_codes, s.p999_codes, s.at_least_one, s.at_least_two,
                  s.any_channel_one, s.any_channel_three,
                  static_cast<unsigned long long>(s.light_pairs), s.mean_light, s.median_light,
                  s.p95_light, s.p99_light, s.p999_light);
    out += buf;
  };
  out += "\"all\":{";
  stats_json(k_bands);
  out += "},\"bands\":[";
  for (u32 b = 0; b < k_bands; ++b) {
    std::snprintf(buf, sizeof(buf), "%s{\"luma\":[%u,%u],", b == 0 ? "" : ",", k_band_low[b],
                  b + 1 < k_bands ? k_band_low[b + 1] : 256u);
    out += buf;
    stats_json(b);
    out += "}";
  }
  out += "],";
  append_series(out, "mean_luma", mean_luma_);
  out += ",";
  append_series(out, "mean_light", mean_light_);
  out += ",";
  append_series(out, "centre_luma", centre_luma_);
  out += ",";
  append_series(out, "centre_light", centre_light_);
  out += "}\n";
  return out;
}

}  // namespace engine::renderer
