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
// setting when it names a layout, else `device_layout` when it is known, else stereo.
ChannelLayout resolve_layout(ChannelLayout device_layout) noexcept;

enum class OutputBackend : u8 { Null = 0, Device };

const char* output_backend_name(OutputBackend backend) noexcept;

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

  bool is_open() const noexcept { return open_; }
  OutputBackend backend() const noexcept { return backend_; }
  // The opened endpoint as the platform reported it; empty and `Unknown` under the null backend.
  const DeviceInfo& device() const noexcept { return device_info_; }
  // Why the last `open()` did not get a device, when it did not; empty otherwise.
  const std::string& fallback_reason() const noexcept { return fallback_reason_; }

  // The null backend's pull: mixes `frames` frames of the mixer's layout into `out`. Does nothing
  // under the device backend, whose thread is already pulling.
  void render(f32* out, u32 frames) noexcept;

  struct Device;  // src/backend.cpp

 private:
  Mixer* mixer_;
  Device* device_ = nullptr;
  OutputBackend backend_ = OutputBackend::Null;
  bool open_ = false;
  DeviceInfo device_info_;
  std::string fallback_reason_;
};

}  // namespace engine::audio
