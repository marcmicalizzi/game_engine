// The path's terms (path.h; roadmap S114; docs/experiments/world-sound.md, "Read again by S114").

#include <systems/world_sound/path.h>

#include <cmath>

namespace engine::world_sound {

namespace {

// ISO 9613-1:1993's reference temperature and the triple point of water, as sengpielaudio gives
// them; the air is at the reference pressure (101.325 kPa), so pa / pr is one.
constexpr f64 k_reference_k = 293.15;
constexpr f64 k_triple_point_k = 273.16;
constexpr f64 k_celsius_k = 273.15;

}  // namespace

f64 air_absorption_db_per_m(f64 f_hz, f64 temperature_c, f64 humidity_pct) noexcept {
  // The standard's range: -20 to 50 degC, 10 to 100 % (held there), at one atmosphere.
  const f64 tc = temperature_c < -20.0 ? -20.0 : temperature_c > 50.0 ? 50.0 : temperature_c;
  const f64 hr = humidity_pct < 10.0 ? 10.0 : humidity_pct > 100.0 ? 100.0 : humidity_pct;
  const f64 t = tc + k_celsius_k;
  const f64 pa_pr = 1.0;  // pa / pr: the field's air is at the reference pressure
  // psat / pr = 10^(-6.8346 (T01 / T)^1.261 + 4.6151); h = hr (psat / pr) / (pa / pr), in percent.
  const f64 psat_pr = std::pow(10.0, -6.8346 * std::pow(k_triple_point_k / t, 1.261) + 4.6151);
  const f64 h = hr * psat_pr / pa_pr;
  const f64 tr = t / k_reference_k;
  const f64 fro = pa_pr * (24.0 + 4.04e4 * h * (0.02 + h) / (0.391 + h));
  const f64 frn = pa_pr * std::pow(tr, -0.5) *
                  (9.0 + 280.0 * h * std::exp(-4.170 * (std::pow(tr, -1.0 / 3.0) - 1.0)));
  const f64 f2 = f_hz * f_hz;
  const f64 z = 0.1068 * std::exp(-3352.0 / t) / (frn + f2 / frn);
  const f64 y = std::pow(tr, -2.5) * (0.01275 * std::exp(-2239.1 / t) / (fro + f2 / fro) + z);
  return 8.686 * f2 * (1.84e-11 / pa_pr * std::sqrt(tr) + y);
}

void clip_air_db_per_m(f64 temperature_c, f64 humidity_pct, f32& db_per_m_8k,
                       f32& db_per_m_1k) noexcept {
  if (temperature_c == k_default_air_c && humidity_pct == k_default_air_pct) {
    db_per_m_8k = k_default_air_db_per_m_8k;
    db_per_m_1k = k_default_air_db_per_m_1k;
    return;
  }
  db_per_m_8k = static_cast<f32>(air_absorption_db_per_m(8000.0, temperature_c, humidity_pct));
  db_per_m_1k = static_cast<f32>(air_absorption_db_per_m(1000.0, temperature_c, humidity_pct));
}

f64 foliage_db(f64 f_hz, f64 r_m) noexcept {
  if (!(r_m > 0.0) || !(f_hz > 0.0)) return 0.0;
  return 0.01 * r_m * std::cbrt(f_hz);
}

void ground_bands_db(f64 source_h_m, f64 receiver_h_m, f64 ground_m, f64 g,
                     f64 (&out)[k_path_bands]) noexcept {
  const f64 gg = g < 0.0 ? 0.0 : g > 1.0 ? 1.0 : g;
  const f64 dp = ground_m > 0.0 ? ground_m : 0.0;
  const f64 far = 1.0 - std::exp(-dp / 50.0);
  const f64 far2 = 1.0 - std::exp(-2.8e-6 * dp * dp);
  for (f64& o : out)
    o = 0.0;
  // A region's term at height h (ISO 9613-2 table 3 through the help): -1.5 at 63 Hz,
  // -1.5 + G a'(h), b'(h), c'(h), d'(h) at 125 Hz to 1 kHz, -1.5 (1 - G) above.
  const auto region = [&](f64 h) noexcept {
    const f64 hh = h > 0.0 ? h : 0.0;
    const f64 e09 = std::exp(-0.09 * hh * hh);
    out[0] += -1.5;
    out[1] += -1.5 +
              gg * (1.5 + 3.0 * std::exp(-0.12 * (hh - 5.0) * (hh - 5.0)) * far + 5.7 * e09 * far2);
    out[2] += -1.5 + gg * (1.5 + 8.6 * e09 * far);
    out[3] += -1.5 + gg * (1.5 + 14.0 * std::exp(-0.46 * hh * hh) * far);
    out[4] += -1.5 + gg * (1.5 + 5.0 * std::exp(-0.9 * hh * hh) * far);
    for (u32 b = 5; b < k_path_bands; ++b)
      out[b] += -1.5 * (1.0 - gg);
  };
  region(source_h_m);
  region(receiver_h_m);
  // The middle region: q = 0 within 30 (hs + hr), else 1 - 30 (hs + hr) / dp; -3q at 63 Hz and
  // -3q (1 - G) above (the help prints the 125 Hz row; the rows above are taken the same).
  const f64 near =
      30.0 * ((source_h_m > 0.0 ? source_h_m : 0.0) + (receiver_h_m > 0.0 ? receiver_h_m : 0.0));
  const f64 q = dp > near ? 1.0 - near / dp : 0.0;
  out[0] += -3.0 * q;
  for (u32 b = 1; b < k_path_bands; ++b)
    out[b] += -3.0 * q * (1.0 - gg);
}

f64 ground_db(u32 band, f64 source_h_m, f64 receiver_h_m, f64 ground_m, f64 g) noexcept {
  f64 out[k_path_bands];
  ground_bands_db(source_h_m, receiver_h_m, ground_m, g, out);
  return band < k_path_bands ? out[band] : 0.0;
}

f64 barrier_db(f64 f_hz, f64 delta_m) noexcept {
  if (!(delta_m > 0.0) || !(f_hz > 0.0)) return 0.0;
  constexpr f64 k_sound_m_s = 343.2;  // ISO 9613-1's speed of sound at 20 degC
  const f64 lambda = k_sound_m_s / f_hz;
  const f64 dz = 10.0 * std::log10(3.0 + 20.0 / lambda * delta_m);
  return dz < 20.0 ? dz : 20.0;
}

}  // namespace engine::world_sound
