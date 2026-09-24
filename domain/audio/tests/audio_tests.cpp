// The capability's constants and the source model, each against the formula the header states
// (docs/subsystems/audio.md, "Sources are objects").
#include <domain/audio/audio.h>
#include <foundation/tunables/tunables.h>

#include <doctest/doctest.h>

#include <cmath>
#include <string>
#include <string_view>

using namespace engine;
using namespace engine::audio;

namespace {
constexpr f64 k_pi_d = 3.14159265358979323846;
}  // namespace

TEST_CASE("audio declares a determinism stance, a 48 kHz mix and its tunables") {
  CHECK(std::string_view{k_determinism} == "derived");
  CHECK(k_sample_rate == 48000u);
  CHECK(frames_for_ms(10) == 480u);
  CHECK(tunable_voices() == 64u);
  CHECK(tunable_command_queue() == 1024u);
  CHECK(tunable_period_frames() == 480u);
  CHECK(tunable_clip_budget_bytes() == 256ull * 1024u * 1024u);
  CHECK(tunable_layout() == ChannelLayout::Unknown);  // "auto"
}

TEST_CASE("the layout is chosen from the setting, then the device, and stereo only last") {
  CHECK(resolve_layout(ChannelLayout::Surround51) == ChannelLayout::Surround51);
  CHECK(resolve_layout(ChannelLayout::Unknown) == ChannelLayout::Stereo);
  // A setting names a layout: it wins over what the device says.
  tunables::Tunable* setting = tunables::find("audio.layout");
  REQUIRE(setting != nullptr);
  std::string error;
  REQUIRE(setting->set_from_text("quad", &error));
  CHECK(resolve_layout(ChannelLayout::Surround51) == ChannelLayout::Quad);
  CHECK(resolve_layout(ChannelLayout::Unknown) == ChannelLayout::Quad);
  setting->reset();
  CHECK(tunable_layout() == ChannelLayout::Unknown);
}

TEST_CASE("sin_quarter is within a few ulps of the sine, and pinned at both ends") {
  CHECK(sin_quarter(0.0f) == 0.0f);
  CHECK(sin_quarter(-1.0f) == 0.0f);
  CHECK(sin_quarter(1.0f) == 1.0f);
  CHECK(sin_quarter(2.0f) == 1.0f);
  f64 worst = 0.0;
  for (int i = 0; i <= 4096; ++i) {
    const f32 t = static_cast<f32>(i) / 4096.0f;
    const f64 expected = std::sin(k_pi_d / 2.0 * static_cast<f64>(t));
    const f64 error = std::fabs(static_cast<f64>(sin_quarter(t)) - expected);
    if (error > worst) worst = error;
  }
  CHECK(worst < 3.0e-7);
  // Monotonic: a pan law built on it never turns back.
  f32 previous = 0.0f;
  for (int i = 0; i <= 4096; ++i) {
    const f32 s = sin_quarter(static_cast<f32>(i) / 4096.0f);
    CHECK(s >= previous);
    previous = s;
  }
}

TEST_CASE("cos_degrees agrees with the cosine all the way round") {
  f64 worst = 0.0;
  for (int d = -720; d <= 720; ++d) {
    const f64 expected = std::cos(static_cast<f64>(d) * k_pi_d / 180.0);
    const f64 error = std::fabs(static_cast<f64>(cos_degrees(static_cast<f32>(d))) - expected);
    if (error > worst) worst = error;
  }
  CHECK(worst < 5.0e-7);
  CHECK(cos_degrees(0.0f) == 1.0f);
  CHECK(cos_degrees(90.0f) == 0.0f);
  CHECK(cos_degrees(180.0f) == -1.0f);
  const Cone omni = make_cone(360.0f, 360.0f);
  CHECK(omni.inner_cos == -1.0f);
  CHECK(omni.outer_cos == -1.0f);
  const Cone narrow = make_cone(120.0f, 60.0f);  // outer narrower than inner: widened to it
  CHECK(narrow.outer_cos == narrow.inner_cos);
}

TEST_CASE("constant-power pan keeps the power at one and the centre at -3 dB") {
  for (int i = -20; i <= 20; ++i) {
    const f32 pan = static_cast<f32>(i) / 20.0f;
    const PanGains g = pan_constant_power(pan);
    CHECK(static_cast<f64>(g.left * g.left + g.right * g.right) ==
          doctest::Approx(1.0).epsilon(1e-6));
  }
  const PanGains centre = pan_constant_power(0.0f);
  CHECK(centre.left == centre.right);
  CHECK(static_cast<f64>(centre.left) == doctest::Approx(std::sqrt(0.5)).epsilon(1e-6));
  CHECK(pan_constant_power(-1.0f).right == 0.0f);
  CHECK(pan_constant_power(-1.0f).left == 1.0f);
  CHECK(pan_constant_power(1.0f).left == 0.0f);
  CHECK(pan_constant_power(1.0f).right == 1.0f);
  CHECK(pan_constant_power(-7.0f).right == 0.0f);  // clamped
}

TEST_CASE("balance leaves a centred stereo source at unity") {
  CHECK(pan_balance(0.0f).left == 1.0f);
  CHECK(pan_balance(0.0f).right == 1.0f);
  CHECK(pan_balance(0.5f).left == 0.5f);
  CHECK(pan_balance(0.5f).right == 1.0f);
  CHECK(pan_balance(-0.25f).left == 1.0f);
  CHECK(pan_balance(-0.25f).right == 0.75f);
}

TEST_CASE("the distance models are the stated laws, continuous, monotonic, and zero at max") {
  constexpr DistanceModel tapered = DistanceModel::InverseTapered;
  CHECK(distance_gain(tapered, 0.0f, 2.0f, 20.0f) == 1.0f);
  CHECK(distance_gain(tapered, 2.0f, 2.0f, 20.0f) == 1.0f);
  CHECK(distance_gain(tapered, 20.0f, 2.0f, 20.0f) == 0.0f);
  CHECK(distance_gain(tapered, 100.0f, 2.0f, 20.0f) == 0.0f);
  // (min / d) * (max - d) / (max - min) at d = 4: 0.5 * 16 / 18.
  CHECK(static_cast<f64>(distance_gain(tapered, 4.0f, 2.0f, 20.0f)) ==
        doctest::Approx(0.5 * 16.0 / 18.0).epsilon(1e-6));
  // Linear: (max - d) / (max - min).
  CHECK(static_cast<f64>(distance_gain(DistanceModel::Linear, 4.0f, 2.0f, 20.0f)) ==
        doctest::Approx(16.0 / 18.0).epsilon(1e-6));
  CHECK(distance_gain(DistanceModel::None, 1000.0f, 2.0f, 20.0f) == 1.0f);
  for (DistanceModel model : {tapered, DistanceModel::Linear}) {
    f32 previous = 1.0f;
    for (int i = 0; i <= 1000; ++i) {
      const f32 d = static_cast<f32>(i) * 0.025f;
      const f32 a = distance_gain(model, d, 2.0f, 20.0f);
      CHECK(a <= previous);
      CHECK(a >= 0.0f);
      previous = a;
    }
  }
  // Degenerate distances do not divide by zero: max == min is a step.
  CHECK(distance_gain(tapered, 1.0f, 3.0f, 3.0f) == 1.0f);
  CHECK(distance_gain(tapered, 3.5f, 3.0f, 3.0f) == 0.0f);
  CHECK(distance_gain(tapered, 0.0f, 0.0f, 0.0f) == 1.0f);
}

TEST_CASE("the listener basis is orthonormal and right-handed, and repairs a degenerate one") {
  const ListenerBasis standard = make_listener_basis(Listener{});
  CHECK(standard.right == Vec3{1.0f, 0.0f, 0.0f});
  CHECK(standard.up == Vec3{0.0f, 1.0f, 0.0f});
  CHECK(standard.forward == Vec3{0.0f, 0.0f, -1.0f});

  Listener turned;
  turned.forward = Vec3{3.0f, 0.0f, 0.0f};  // facing +x, not normalized
  turned.up = Vec3{1.0f, 5.0f, 0.0f};       // leaning into forward: not orthogonal
  const ListenerBasis b = make_listener_basis(turned);
  CHECK(approx_equal(b.forward, Vec3{1.0f, 0.0f, 0.0f}));
  CHECK(approx_equal(b.right, Vec3{0.0f, 0.0f, 1.0f}));  // facing +x, right is +z
  CHECK(std::fabs(dot(b.up, b.forward)) < 1e-6f);
  CHECK(std::fabs(dot(b.up, b.right)) < 1e-6f);

  Listener broken;
  broken.forward = Vec3{};
  broken.up = Vec3{};
  const ListenerBasis repaired = make_listener_basis(broken);
  CHECK(repaired.forward == Vec3{0.0f, 0.0f, -1.0f});
  CHECK(repaired.right == Vec3{1.0f, 0.0f, 0.0f});

  Listener vertical;
  vertical.forward = Vec3{0.0f, -1.0f, 0.0f};  // looking straight down, up parallel
  const ListenerBasis down = make_listener_basis(vertical);
  CHECK(std::fabs(length(down.right) - 1.0f) < 1e-6f);
  CHECK(std::fabs(dot(down.right, down.forward)) < 1e-6f);
}

TEST_CASE(
    "spatialize puts a source on the right at +x and ahead at -z, and leaves a 2D one alone") {
  const ListenerBasis listener = make_listener_basis(Listener{});
  SourceSpatial right;
  right.position = Vec3{10.0f, 0.0f, 0.0f};
  right.min_distance = 1.0f;
  right.max_distance = 100.0f;
  const SpatialParams r = spatialize(right, listener);
  CHECK(r.direction.x == 1.0f);
  CHECK(static_cast<f64>(r.attenuation) == doctest::Approx(0.1 * 90.0 / 99.0).epsilon(1e-6));

  SourceSpatial ahead = right;
  ahead.position = Vec3{0.0f, 0.0f, -5.0f};
  const SpatialParams a = spatialize(ahead, listener);
  CHECK(a.direction.x == 0.0f);
  CHECK(a.direction.z == -1.0f);

  SourceSpatial here = right;
  here.position = Vec3{};
  const SpatialParams h = spatialize(here, listener);
  CHECK(h.direction == Vec3{});
  CHECK(h.attenuation == 1.0f);

  SourceSpatial flat = right;
  flat.flags = k_source_2d;
  flat.spread = 0.7f;
  const SpatialParams f = spatialize(flat, listener);
  CHECK(f.direction == Vec3{});
  CHECK(f.attenuation == 1.0f);
  CHECK(f.spread == 0.0f);
}
