#pragma once

// Output devices, and the null backend (docs/subsystems/audio.md, "Devices and the null
// backend").
//
// **Enumeration** asks the platform's audio API (WASAPI on Windows; PulseAudio, then ALSA, on
// Linux — both loaded at run time, so a machine without them runs the same binary) for its
// playback devices and **what each endpoint really is**: its channel map, read from the device as
// the platform reports it and matched against the declared layouts, and its native rate. Zero
// devices is an answer, not an error: the CI runners and the headless GPU server have none, and
// `enumerate_devices()` says so with an empty list and the name of the backend it asked, or "none"
// when no backend would start at all.
//
// **The mixer's layout is chosen, never assumed** (`resolve_layout`): the `audio.layout` setting
// when it names one, the device's own layout when the setting is `auto`, and stereo only when
// neither says. The device is then opened with the mixer's layout and its channel map, so if the
// two differ (a stereo setting on a 5.1 endpoint) the platform's converter maps one onto the
// other and the mix itself is unchanged.
//
// **An `Output`** pulls the mixer from somewhere. With the `Device` backend, miniaudio's device
// thread calls `Mixer::render()` once per period; with the `Null` backend nothing calls it until
// the caller does, into its own buffer, with `Output::render()`. The null backend is the same mix —
// the same `render()` — with no device behind it, which is what every test runs, and what a
// machine with no device falls back to when `OutputConfig::fall_back_to_null` is set (the
// default): the game keeps running and the mix is simply not heard.
//
// **Devices change under a running game** — a headset connects and becomes the default, a USB DAC
// is unplugged. The platform says so on a thread of its own (miniaudio's `stopped` and `rerouted`
// notifications; on Windows, the engine's endpoint watcher for a new default), and all that is
// allowed there is setting a flag (`Output::notify`). `Output::update()`, called once a tick on
// the controlling thread, acts on it: it closes the device and opens the current one with the same
// configuration and the mixer's unchanged layout, and the mixer, which never knew, carries on.
//
// Why the mix runs on the device's thread and not on the job system's Efficiency pool, which plan
// 11 §11.5 names for audio mixing: on a machine without efficiency cores that pool runs at
// below-normal priority, and the one property an audio callback must have is that nothing else on
// the machine can make it late. The device thread is the operating system's real-time audio thread
// (MMCSS "Pro Audio" on Windows). Decoding, which can be late, is what the Efficiency pool gets.

#include <core/base/macros.h>
#include <core/base/types.h>
#include <core/containers/vector.h>
#include <domain/audio/format.h>
#include <domain/audio/mixer.h>

#include <atomic>
#include <schemas/audio.h>
#include <string>

namespace engine::audio {

struct DeviceInfo {
  std::string name;
  // The endpoint's own layout, from its channel map; `Unknown` when the map is none of the
  // declared layouts or the device would not open to say.
  ChannelLayout layout = ChannelLayout::Unknown;
  u32 channels = 0;     // its native channel count, 0 when it would not say
  u32 sample_rate = 0;  // its native rate, 0 when it would not say
  bool is_default = false;
  // What the endpoint is — headphones, a headset, speakers, a digital link — from the platform's
  // endpoint properties (Windows' form factor). `Unknown` where the platform does not say, which
  // is Linux today (docs/subsystems/audio.md, "Headphones").
  EndpointFormFactor form_factor = EndpointFormFactor::Unknown;
};

struct DeviceList {
  // The platform API that answered: "wasapi", "pulseaudio", "alsa", or "none" when no backend
  // initialized — which is also a normal answer on a machine with no audio stack.
  const char* backend = "none";
  Vector<DeviceInfo> devices;
};

// Cold, and it may take tens of milliseconds per device (each endpoint is opened, unstarted, to
// read its channel map). Never on the audio thread, and not per tick.
DeviceList enumerate_devices();

// The layout a mixer should be declared with, given what a device reports: the `audio.layout`
// setting when it names a layout; else headphones when the endpoint is headphones or a headset
// with two channels or fewer (its channel map cannot say it is worn — a pair of headphones reports
// stereo — and one that reports 7.1 virtualizes its speakers itself); else `device_layout` when it
// is known; else stereo.
ChannelLayout resolve_layout(ChannelLayout device_layout,
                             EndpointFormFactor form_factor = EndpointFormFactor::Unknown) noexcept;
// The same for a device as enumeration or an output describes it.
ChannelLayout resolve_layout(const DeviceInfo& device) noexcept;

// "unknown", "speakers", "headphones", "headset", "line_level", "digital", "other".
const char* form_factor_name(EndpointFormFactor form_factor) noexcept;

enum class OutputBackend : u8 { Null = 0, Device };

const char* output_backend_name(OutputBackend backend) noexcept;

// What can happen to an open output's device behind the game's back (docs/subsystems/audio.md,
// "Device changes at run time"). Bits: several can arrive between two updates.
enum class DeviceEvent : u8 {
  // miniaudio's `stopped` notification, from a device nobody asked to stop. A hint: PulseAudio
  // also sends it when a sink is suspended, so it counts only if the device really is stopped.
  Stopped = 1u << 0,
  // miniaudio's `rerouted` notification: the platform moved the stream to another endpoint
  // (PulseAudio moves a default stream when the default sink changes).
  Rerouted = 1u << 1,
  // The system's default playback device changed (Windows' endpoint watcher).
  DefaultChanged = 1u << 2,
  // The endpoint this output plays to went away: unplugged, disabled, or its stream failed.
  Lost = 1u << 3,
};

// The most telling of the events in `events` (a mask of `DeviceEvent` bits), as a word for the log.
const char* device_event_name(u32 events) noexcept;

struct OutputConfig {
  OutputBackend backend = OutputBackend::Device;
  // A playback device by name, as `enumerate_devices()` spells it; empty is the system default.
  std::string device;
  // Frames per callback. 0 reads the `audio.period_frames` tunable (480: 10 ms).
  u32 period_frames = 0;
  // When the device cannot be opened, open the null backend instead of failing.
  bool fall_back_to_null = true;
};

class Output {
 public:
  // `mixer` must outlive the output.
  explicit Output(Mixer& mixer) noexcept;
  // Closes the device first: its thread calls into the mixer.
  ~Output();
  ENGINE_NON_COPYABLE(Output);

  // Opens the requested backend. With `Device`: the named (or default) playback device, fed in the
  // mixer's layout; when there is none, the null backend if `fall_back_to_null` allows it. False
  // only when the device failed and falling back was not allowed. Reopening closes whatever was
  // open.
  bool open(const OutputConfig& config);
  void close() noexcept;

  // **Device changes at run time.** Call once a tick, from the thread that opened the output. If
  // the device went away, or — for an output opened on the system default — the default changed,
  // since the last call, the output is closed and opened again with the same `OutputConfig`: on
  // the new default, in the mixer's declared layout, which does not change. The mixer is not
  // touched: its voices, their positions, the buses and the listener carry on from where the old
  // device's last block left them, and commands sent meanwhile wait in the ring. Returns true when
  // it reopened, and logs the change. The platform's notifications only set a flag (`notify`);
  // this is where anything is done about them. A named device does not follow the default.
  bool update();
  // Records a device event, as the platform's notification callbacks do: sets a bit and nothing
  // else, so any thread may call it, the device's real-time thread included. `update()` acts on
  // it. Also how a test reaches the reopen path without unplugging anything.
  void notify(DeviceEvent event) noexcept;
  // Times `update()` has reopened the output.
  u32 reopens() const noexcept { return reopens_; }

  bool is_open() const noexcept { return open_; }
  OutputBackend backend() const noexcept { return backend_; }
  // The opened endpoint as the platform reported it; empty and `Unknown` under the null backend.
  const DeviceInfo& device() const noexcept { return device_info_; }
  // Why the last `open()` did not get a device, when it did not; empty otherwise.
  const std::string& fallback_reason() const noexcept { return fallback_reason_; }

  // The null backend's pull: mixes `frames` frames of the mixer's layout into `out`. Does nothing
  // under the device backend, whose thread is already pulling.
  void render(f32* out, u32 frames) noexcept;

  struct Device;   // src/backend.cpp
  struct Watcher;  // src/endpoint.cpp

 private:
  Mixer* mixer_;
  Device* device_ = nullptr;
  // The platform's default-device watcher (Windows), alive while a `Device` backend is wanted —
  // also while it fell back to null, so a device that appears later is found.
  Watcher* watcher_ = nullptr;
  OutputBackend backend_ = OutputBackend::Null;
  bool open_ = false;
  u32 reopens_ = 0;
  // `DeviceEvent` bits set by the notification callbacks, taken by `update()`.
  std::atomic<u32> events_{0};
  OutputConfig config_;
  DeviceInfo device_info_;
  std::string fallback_reason_;
};

}  // namespace engine::audio
