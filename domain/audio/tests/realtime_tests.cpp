// The audio thread's contract (mixer.h): `render()` allocates nothing, takes no lock and logs
// nothing. Locks are absent by construction — the only shared state is two wait-free rings and a
// seqlock of atomics, and the queue test pins that the ring is lock-free — so what is measured here
// is the other two:
//
//   * every heap allocation the process makes goes through the replaced `operator new` below or
//     through `mem::allocate` (the engine containers), and both are counted on the thread that is
//     rendering, over a session that exercises every command kind, voices ending, stealing,
//     clipping, and a render longer than a block;
//   * a log sink counts every record while the same session renders, with every category open to
//     trace.
//
// Also here: the null backend, device enumeration on a machine that may have no devices at all,
// and the `audio.devices` protocol method.
#include "audio_test_support.h"

#include <core/json/json.h>
#include <core/log/log.h>
#include <core/memory/memory.h>
#include <domain/audio/audio.h>
#include <domain/audio/protocol.h>
#include <domain/protocol/rpc.h>

#include <doctest/doctest.h>
#include <test_paths.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <new>
#include <string>
#include <string_view>
#include <thread>

#if defined(_WIN32)
#include <malloc.h>
#endif

using namespace engine;
using namespace engine::audio;
using namespace engine::audio::test;

// ---- a counting operator new --------------------------------------------------------------------
//
// Replacing the global allocation functions is the only way to see an allocation the standard
// library makes on our behalf. They count on the calling thread only while it has asked to be
// counted, so doctest's own allocations on the same thread before and after do not interfere.

namespace {
thread_local bool t_counting = false;
thread_local u64 t_allocations = 0;

void* counted_malloc(std::size_t size) {
  if (t_counting) ++t_allocations;
  void* p = std::malloc(size != 0 ? size : 1);
  if (p == nullptr) std::abort();
  return p;
}

void* counted_aligned(std::size_t size, std::size_t align) {
  if (t_counting) ++t_allocations;
  if (size == 0) size = 1;
#if defined(_WIN32)
  void* p = _aligned_malloc(size, align);
#else
  void* p = nullptr;
  if (posix_memalign(&p, align < sizeof(void*) ? sizeof(void*) : align, size) != 0) p = nullptr;
#endif
  if (p == nullptr) std::abort();
  return p;
}

void aligned_release(void* p) {
#if defined(_WIN32)
  _aligned_free(p);
#else
  std::free(p);
#endif
}
}  // namespace

void* operator new(std::size_t size) { return counted_malloc(size); }
void* operator new[](std::size_t size) { return counted_malloc(size); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
  return counted_malloc(size);
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
  return counted_malloc(size);
}
void* operator new(std::size_t size, std::align_val_t align) {
  return counted_aligned(size, static_cast<std::size_t>(align));
}
void* operator new[](std::size_t size, std::align_val_t align) {
  return counted_aligned(size, static_cast<std::size_t>(align));
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t) noexcept { aligned_release(p); }
void operator delete[](void* p, std::align_val_t) noexcept { aligned_release(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { aligned_release(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { aligned_release(p); }

namespace {

class CountingSink final : public log::Sink {
 public:
  void write(const log::Record&) override { records.fetch_add(1, std::memory_order_relaxed); }
  std::atomic<u64> records{0};
};

// Renders one block with the calling thread counting, and returns what it counted.
struct Counted {
  u64 new_calls = 0;
  u64 engine_allocations = 0;
  u64 log_records = 0;
};

Counted render_counted(Mixer& mixer, f32* out, u32 frames, CountingSink& sink) {
  const u64 engine_before = mem::allocation_counter();
  const u64 records_before = sink.records.load();
  t_allocations = 0;
  t_counting = true;
  mixer.render(out, frames);
  t_counting = false;
  return Counted{t_allocations, mem::allocation_counter() - engine_before,
                 sink.records.load() - records_before};
}

}  // namespace

TEST_CASE("the audio thread allocates nothing and logs nothing, whatever the commands say") {
  CountingSink sink;
  log::add_sink(&sink);
  log::set_global_min_level(log::Level::Trace);
  std::string spec_error;
  REQUIRE(log::apply_level_spec("trace", &spec_error));

  // Twice: as configured by default, where the master's 4x drives the hard clip, and with the
  // limiter on, where it drives the limiter's per-frame path instead.
  for (const LimiterMode limiter : {LimiterMode::Off, LimiterMode::On}) {
    CAPTURE(limiter == LimiterMode::On);
    ClipStore clips;
    MixerConfig config;
    config.voices = 6;
    config.layout = ChannelLayout::Surround51;
    config.limiter = limiter;
    Mixer mixer(clips, config);
    const Vector<f32> tone = exact_sine(4800, 48, 0.6f);
    const Vector<f32> blip = exact_sine(200, 48, 0.9f, 2);
    const ClipHandle tone_clip = clips.add_pcm(Id128{9, 1}, tone, 1);
    const ClipHandle blip_clip = clips.add_pcm(Id128{9, 2}, blip, 2);

    Vector<f32> out;
    out.resize_exact(4000u * mixer.channels());  // longer than a block, to cross the chunking too
    Counted total;
    mixer.set_bus_gain(k_bus_master, 4.0f);  // loud enough to clip
    for (u32 block = 0; block < 40; ++block) {
      // The controlling thread's half, which may allocate (it does not, but it is not the
      // contract). Two plays a block, mostly loops, into a pool of six: it fills, and then steals
      // and refuses.
      mixer.update();
      VoiceHandle voice;
      for (u32 n = 0; n < 2; ++n) {
        PlayParams p;
        p.clip = (block + n) % 3u == 0 ? blip_clip : tone_clip;
        p.pitch = 0.75f + 0.05f * static_cast<f32>((block + n) % 7u);
        p.loop = (block + n) % 3u != 0;
        p.priority = static_cast<u8>((block * 2u + n) * 37u);
        p.source.position = Vec3{static_cast<f32>(block % 9u) - 4.0f, 0.0f, -2.0f};
        p.source.flags = (block + n) % 4u == 0 ? k_source_2d : u8{0};
        voice = mixer.play(p);
      }
      if (!voice.is_null() && block % 2u == 0) {
        VoiceParams v;
        v.gain = 1.5f;
        v.pitch = 1.3f;
        v.loop = true;
        mixer.set_params(voice, v);
        SourceSpatial s;
        s.position = Vec3{2.0f, 1.0f, 3.0f};
        s.directivity = Directivity::Cone;
        mixer.set_source(voice, s);
      }
      if (block % 6u == 5u) mixer.stop(voice);
      Listener l;
      l.forward = Vec3{static_cast<f32>(block) * 0.1f, 0.0f, -1.0f};
      mixer.set_listener(l);
      mixer.set_bus_gain(k_bus_sfx, 0.5f + 0.01f * static_cast<f32>(block));

      // The audio thread's half, counted.
      const Counted c = render_counted(mixer, out.data(), block % 2u == 0 ? 480u : 4000u, sink);
      total.new_calls += c.new_calls;
      total.engine_allocations += c.engine_allocations;
      total.log_records += c.log_records;
    }
    const MixerStats stats = mixer.stats();
    CHECK(stats.blocks > 40u);
    CHECK(mixer.control_stats().events > 0u);
    CHECK(mixer.control_stats().steals > 0u);
    if (limiter == LimiterMode::On) {
      CHECK(stats.limited_frames > 0u);
      CHECK(stats.clipped_samples == 0u);
    } else {
      CHECK(stats.clipped_samples > 0u);
    }
    CHECK(stats.events_dropped == 0u);

    CHECK(total.new_calls == 0u);
    CHECK(total.engine_allocations == 0u);
    CHECK(total.log_records == 0u);
  }

  // The counters count: an allocation on the counting thread is seen, and so is a record.
  t_allocations = 0;
  t_counting = true;
  void* probe = ::operator new(16);
  t_counting = false;
  ::operator delete(probe);
  CHECK(t_allocations == 1u);
  const u64 before = sink.records.load();
  {
    log::Category probe_category{"audio.test.probe"};
    ENGINE_LOG_INFO(probe_category, "a record the sink must see");
  }
  CHECK(sink.records.load() == before + 1u);

  log::reset_levels();
  log::remove_sink(&sink);
}

TEST_CASE("the null backend is the same mix, pulled by the caller") {
  ClipStore clips;
  Mixer direct(clips);
  Mixer pulled(clips);
  const Vector<f32> tone = exact_sine(4800, 48, 0.5f);
  const ClipHandle clip = clips.add_pcm(Id128{8, 1}, tone, 1);
  PlayParams p;
  p.clip = clip;
  p.source.position = Vec3{1.0f, 0.0f, -1.0f};
  REQUIRE_FALSE(direct.play(p).is_null());
  REQUIRE_FALSE(pulled.play(p).is_null());

  Output output(pulled);
  OutputConfig config;
  config.backend = OutputBackend::Null;
  REQUIRE(output.open(config));
  CHECK(output.is_open());
  CHECK(output.backend() == OutputBackend::Null);
  CHECK(output.device().name.empty());

  Vector<f32> a(960, 0.0f);
  Vector<f32> b(960, 0.0f);
  direct.render(a.data(), 480);
  output.render(b.data(), 480);
  CHECK(a == b);
  output.close();
  CHECK_FALSE(output.is_open());
}

namespace {

// The value of `key` in the last retained record whose message is `message`, or "" if none.
std::string last_field(const log::RingSink& ring, std::string_view message, std::string_view key) {
  std::string value;
  ring.for_each(0, [&](const log::RingSink::Entry& entry) {
    if (entry.message != message) return;
    for (const log::Field& f : entry.fields) {
      if (f.key == key && f.kind == log::Field::Kind::String) value = std::string(f.string());
    }
  });
  return value;
}

}  // namespace

// Device changes at run time (device.h, `Output::update`). A machine's default device cannot be
// changed from a test, and the null backend is no miniaudio device, so miniaudio's notification
// cannot be raised here: what is tested is the path from the flag on — `notify()` is exactly what
// the platform's callbacks call, and nothing more — through `update()`'s decision, the reopen and
// the log line, to a mixer that carries on as if nothing had happened. The opt-in device test
// below reopens a real endpoint the same way.
TEST_CASE("a device change reopens the output, and the mixer carries on where it was") {
  log::RingSink ring(64);
  log::add_sink(&ring);

  ClipStore clips;
  Mixer reference(clips);
  Mixer mixer(clips);
  const Vector<f32> tone = exact_sine(4800, 48, 0.5f);
  const ClipHandle clip = clips.add_pcm(Id128{8, 2}, tone, 1);
  PlayParams p;
  p.clip = clip;
  p.loop = true;
  p.pitch = 0.77f;  // a playhead that is not on a frame boundary when the device goes
  p.source.position = Vec3{2.0f, 0.0f, -1.0f};
  REQUIRE_FALSE(reference.play(p).is_null());
  const VoiceHandle voice = mixer.play(p);
  REQUIRE_FALSE(voice.is_null());

  Output output(mixer);
  OutputConfig config;
  config.backend = OutputBackend::Null;
  REQUIRE(output.open(config));
  CHECK_FALSE(output.update());  // nothing happened: nothing to do
  CHECK(output.reopens() == 0u);

  Vector<f32> expected(960, 0.0f);
  Vector<f32> got(960, 0.0f);
  for (int i = 0; i < 3; ++i) {
    reference.render(expected.data(), 480);
    output.render(got.data(), 480);
    CHECK(got == expected);
  }

  // The default device changed. The flag is all the callback sets; the next update reopens.
  output.notify(DeviceEvent::DefaultChanged);
  CHECK(output.is_open());  // nothing yet: notify() only records
  CHECK(output.update());
  CHECK(output.reopens() == 1u);
  CHECK(output.is_open());
  CHECK(output.backend() == OutputBackend::Null);
  CHECK(last_field(ring, "output device changed", "reason") == "default_changed");
  CHECK(last_field(ring, "output device changed", "backend") == "null");
  CHECK(last_field(ring, "output device changed", "mix_layout") == "stereo");
  CHECK_FALSE(output.update());  // once per change

  // The mixer did not notice: the voice is live and plays on from where it was, and a command sent
  // across the reopen is applied at the next block, as ever.
  CHECK(mixer.is_live(voice));
  Listener turned;
  turned.forward = Vec3{1.0f, 0.0f, -1.0f};
  REQUIRE(reference.set_listener(turned));
  REQUIRE(mixer.set_listener(turned));
  for (int i = 0; i < 3; ++i) {
    reference.render(expected.data(), 480);
    output.render(got.data(), 480);
    CHECK(got == expected);
  }

  // A `stopped` notice from a device that is not stopped is PulseAudio suspending a sink: no loss.
  output.notify(DeviceEvent::Stopped);
  CHECK_FALSE(output.update());
  // A lost device is.
  output.notify(DeviceEvent::Lost);
  CHECK(output.update());
  CHECK(output.reopens() == 2u);
  CHECK(last_field(ring, "output device changed", "reason") == "lost");
  // Several at once are one reopen, reported by the most telling.
  output.notify(DeviceEvent::Rerouted);
  output.notify(DeviceEvent::DefaultChanged);
  output.notify(DeviceEvent::Lost);
  CHECK(output.update());
  CHECK(output.reopens() == 3u);
  CHECK(last_field(ring, "output device changed", "reason") == "lost");
  reference.render(expected.data(), 480);
  output.render(got.data(), 480);
  CHECK(got == expected);

  // An output opened on a named device plays to that device: a new default is not its business.
  Output named(mixer);
  OutputConfig by_name = config;
  by_name.device = "Speakers (named)";
  REQUIRE(named.open(by_name));
  named.notify(DeviceEvent::DefaultChanged);
  CHECK_FALSE(named.update());
  named.notify(DeviceEvent::Lost);
  CHECK(named.update());
  CHECK(named.reopens() == 1u);

  // A closed output has nothing to reopen.
  output.close();
  output.notify(DeviceEvent::Lost);
  CHECK_FALSE(output.update());

  CHECK(std::string(device_event_name(0)) == "none");
  CHECK(std::string(device_event_name(static_cast<u32>(DeviceEvent::Rerouted))) == "rerouted");
  log::remove_sink(&ring);
}

TEST_CASE(
    "the device backend pulls the mix on its own thread (opt-in: ENGINE_AUDIO_DEVICE_TEST=1)") {
  // Opening a real endpoint is a side effect on somebody's machine, so the suite does it only when
  // asked. It plays nothing — no voice is started, so the device receives silence — and checks
  // that the device's thread called `render()` in the mixer's declared layout and that closing
  // joins it. Where there is no device the output falls back to the null backend, which is also a
  // pass: that is what a machine without one is supposed to do.
  if (engine::test::detail::environment("ENGINE_AUDIO_DEVICE_TEST") != "1") {
    MESSAGE("skipped: set ENGINE_AUDIO_DEVICE_TEST=1 to open the default playback device");
    return;
  }
  const DeviceList list = enumerate_devices();
  DeviceInfo default_device;
  for (const DeviceInfo& d : list.devices) {
    if (d.is_default) default_device = d;
  }
  ClipStore clips;
  MixerConfig config;
  config.layout = resolve_layout(default_device);
  Mixer mixer(clips, config);
  Output output(mixer);
  REQUIRE(output.open(OutputConfig{}));
  if (output.backend() == OutputBackend::Device) {
    for (int i = 0; i < 50 && mixer.stats().blocks < 5u; ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(mixer.stats().blocks >= 5u);
    MESSAGE("device " << output.device().name << ": "
                      << std::string(layout_name(output.device().layout)) << " at "
                      << output.device().sample_rate << " Hz, "
                      << std::string(form_factor_name(output.device().form_factor)) << ", mixing "
                      << std::string(layout_name(mixer.layout())));
    // What the enumeration said the default is, the opened output says too.
    CHECK(output.device().form_factor == default_device.form_factor);
    // The reopen a default-device change causes, on a real endpoint: the device is closed (its
    // thread joined) and opened again on the default, and its thread pulls the same mixer again.
    output.notify(DeviceEvent::DefaultChanged);
    CHECK(output.update());
    CHECK(output.reopens() == 1u);
    CHECK(output.backend() == OutputBackend::Device);
    const u64 before = mixer.stats().blocks;
    for (int i = 0; i < 50 && mixer.stats().blocks < before + 5u; ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(mixer.stats().blocks >= before + 5u);
    CHECK_FALSE(output.update());  // a running device raised nothing of its own
  } else {
    MESSAGE("no device (" << output.fallback_reason() << "): the null backend");
  }
  output.close();
  const u64 blocks = mixer.stats().blocks;
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  CHECK(mixer.stats().blocks == blocks);  // closed means no callback runs any more
}

TEST_CASE("device enumeration answers on a machine with no audio device at all") {
  // The CI runners and the headless server have no device; a desktop has several. Both are normal,
  // so the test asserts what holds for every answer rather than a count.
  const DeviceList list = enumerate_devices();
  const std::string backend = list.backend;
  CHECK((backend == "none" || backend == "wasapi" || backend == "pulseaudio" || backend == "alsa"));
  if (backend == "none") CHECK(list.devices.empty());
  u32 defaults = 0;
  for (const DeviceInfo& device : list.devices) {
    CHECK_FALSE(device.name.empty());
    if (device.is_default) ++defaults;
    if (device.layout != ChannelLayout::Unknown) {
      CHECK(device.channels == layout_channels(device.layout));
    }
  }
  CHECK(defaults <= 1u);
  // Windows says what every endpoint is (its form factor); a machine with devices has at least one
  // that says something other than "unknown". Elsewhere the platform does not say (audio.md).
  u32 known = 0;
  for (const DeviceInfo& device : list.devices)
    known += device.form_factor != EndpointFormFactor::Unknown ? 1u : 0u;
  if (backend == "wasapi" && !list.devices.empty()) CHECK(known > 0u);
  if (backend != "wasapi") CHECK(known == 0u);
  MESSAGE("audio backend " << backend << ", " << list.devices.size() << " playback devices");
  for (const DeviceInfo& device : list.devices) {
    const std::string line =
        "  " + device.name + ": " + layout_name(device.layout) + ", " +
        std::to_string(device.channels) + " channels at " + std::to_string(device.sample_rate) +
        " Hz, " + form_factor_name(device.form_factor) + (device.is_default ? " (default)" : "");
    MESSAGE(line);
  }
}

TEST_CASE("audio.devices answers through the protocol with the devices and the mix format") {
  protocol::Dispatcher dispatcher(protocol::Context{});
  register_methods(dispatcher);
  REQUIRE(dispatcher.find("audio.devices") != nullptr);
  const std::string response =
      dispatcher.dispatch_text(R"({"jsonrpc":"2.0","id":1,"method":"audio.devices"})");
  JsonValue value;
  REQUIRE(parse_json(response, value).ok);
  const JsonValue* result = value.find("result");
  REQUIRE(result != nullptr);
  REQUIRE(result->find("backend") != nullptr);
  REQUIRE(result->find("devices") != nullptr);
  CHECK(result->find("devices")->is_array());
  // Each device says what it is (schema version 2 of AudioDevice).
  for (const JsonValue& device : result->find("devices")->as_array())
    CHECK(device.find("form_factor") != nullptr);
  REQUIRE(result->find("mix_sample_rate") != nullptr);
  u64 rate = 0;
  CHECK(result->find("mix_sample_rate")->get_u64(rate));
  CHECK(rate == 48000u);
  REQUIRE(result->find("mix_layout") != nullptr);
  CHECK(result->find("mix_layout")->as_string() != "Unknown");
}
