// The only translation unit in the engine that includes miniaudio (cmake/EngineAudio.cmake,
// docs/subsystems/audio.md "What miniaudio is asked to do"). The implementation is compiled once,
// as C, into engine_miniaudio; this file sees its declarations and nothing sees this file's.
#include "backend.h"

#include <core/base/macros.h>
#include <domain/audio/format.h>
#include <domain/audio/mixer.h>

#include <cstring>
#include <limits>
#include <miniaudio.h>

namespace engine::audio {

// The device and the context it was opened from, at addresses that do not move: miniaudio's
// device thread holds a pointer to both, and its callbacks reach the rest through `pUserData`.
struct Output::Device {
  ma_context context;
  ma_device device;
  Mixer* mixer = nullptr;
  // The output's `DeviceEvent` flags: the notification callback sets bits here and does nothing
  // else.
  std::atomic<u32>* events = nullptr;
  // Set before the device is closed on purpose, so the `stopped` notification that closing
  // causes is not taken for a lost device.
  std::atomic<bool> closing{false};
};

namespace backend {

namespace {

// The platform APIs the engine asks, in order. One on Windows: WASAPI is on every Windows the
// engine supports, and DirectSound and WinMM are compiled out. On Linux PulseAudio first (it is
// what a desktop runs, and PipeWire answers it), then ALSA for a machine with no sound server.
// Both are loaded at run time, so their absence is a failed init here and not a failed build.
#if ENGINE_PLATFORM_WINDOWS
constexpr ma_backend k_backends[] = {ma_backend_wasapi};
#else
constexpr ma_backend k_backends[] = {ma_backend_pulseaudio, ma_backend_alsa};
#endif
constexpr ma_uint32 k_backend_count =
    static_cast<ma_uint32>(sizeof(k_backends) / sizeof(k_backends[0]));

const char* backend_name(ma_backend backend) noexcept {
  switch (backend) {
    case ma_backend_wasapi: return "wasapi";
    case ma_backend_pulseaudio: return "pulseaudio";
    case ma_backend_alsa: return "alsa";
    default: return "other";
  }
}

bool init_context(ma_context& context) noexcept {
  ma_context_config config = ma_context_config_init();
  // No sound server is started on the engine's behalf: a machine without one has no devices, and
  // that is an answer (device.h).
  config.pulse.tryAutoSpawn = MA_FALSE;
  return ma_context_init(k_backends, k_backend_count, &config, &context) == MA_SUCCESS;
}

// Reads of 4096 frames straight into the clip's own buffer: large enough that the per-call
// overhead vanishes, and the buffer grows geometrically when the decoder could not say its length
// up front (MP3 without a Xing header).
constexpr u32 k_read_frames = 4096;

DecodeStatus read_all(ma_decoder& decoder, u32 channels, DecodedClip& out) {
  // A u32 counts the samples, so the clip's frame count times its channels must fit one.
  constexpr u64 k_max_samples = std::numeric_limits<u32>::max() - u64{k_read_frames} * 2u;
  ma_uint64 expected = 0;
  if (ma_decoder_get_length_in_pcm_frames(&decoder, &expected) == MA_SUCCESS && expected > 0 &&
      expected * channels < k_max_samples) {
    out.samples.reserve(static_cast<u32>(expected * channels) + k_read_frames * channels);
  }
  u64 total = 0;
  for (;;) {
    const u32 at = out.samples.size();
    out.samples.resize(at + k_read_frames * channels);
    ma_uint64 read = 0;
    const ma_result result =
        ma_decoder_read_pcm_frames(&decoder, out.samples.data() + at, k_read_frames, &read);
    out.samples.resize(at + static_cast<u32>(read) * channels);
    total += read;
    if (total * channels >= k_max_samples) return DecodeStatus::TooLong;
    if (result == MA_AT_END || (result == MA_SUCCESS && read == 0)) break;
    if (result != MA_SUCCESS) return DecodeStatus::Corrupt;
  }
  if (total == 0) return DecodeStatus::Empty;
  out.frames = static_cast<u32>(total);
  return DecodeStatus::Ok;
}

}  // namespace

DecodeStatus decode(std::span<const u8> encoded, DecodedClip& out) {
  out = DecodedClip{};
  if (encoded.empty()) return DecodeStatus::UnknownFormat;

  // First with the source's own channel count, to learn it; a source with more than two channels
  // is opened a second time with miniaudio's channel converter folding it to stereo.
  ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 0, k_sample_rate);
  config.resampling.algorithm = ma_resample_algorithm_linear;
  // The highest filter order miniaudio offers: this runs once per clip at load, never per block,
  // so there is no reason to alias a 96 kHz source to save a few microseconds.
  config.resampling.linear.lpfOrder = MA_MAX_FILTER_ORDER;

  ma_decoder decoder;
  if (ma_decoder_init_memory(encoded.data(), encoded.size(), &config, &decoder) != MA_SUCCESS)
    return DecodeStatus::UnknownFormat;

  ma_format source_format = ma_format_unknown;
  ma_uint32 source_channels = 0;
  ma_uint32 source_rate = 0;
  if (ma_data_source_get_data_format(decoder.pBackend, &source_format, &source_channels,
                                     &source_rate, nullptr, 0) != MA_SUCCESS) {
    ma_decoder_uninit(&decoder);
    return DecodeStatus::Corrupt;
  }
  if (source_channels == 0 || source_rate == 0) {
    ma_decoder_uninit(&decoder);
    return DecodeStatus::Corrupt;
  }

  u32 channels = decoder.outputChannels;
  if (channels > 2) {
    ma_decoder_uninit(&decoder);
    config.channels = 2;
    if (ma_decoder_init_memory(encoded.data(), encoded.size(), &config, &decoder) != MA_SUCCESS)
      return DecodeStatus::Corrupt;
    channels = decoder.outputChannels;
  }
  if (channels != 1 && channels != 2) {
    ma_decoder_uninit(&decoder);
    return DecodeStatus::Corrupt;
  }

  const DecodeStatus status = read_all(decoder, channels, out);
  ma_decoder_uninit(&decoder);
  if (status != DecodeStatus::Ok) {
    out = DecodedClip{};
    return status;
  }
  out.channels = static_cast<u8>(channels);
  out.source_channels = static_cast<u8>(source_channels > 255u ? 255u : source_channels);
  out.source_rate = source_rate;
  return DecodeStatus::Ok;
}

namespace {

// ---- channel maps -------------------------------------------------------------------------------

Speaker speaker_of(ma_channel channel) noexcept {
  switch (channel) {
    case MA_CHANNEL_MONO: return Speaker::Mono;
    case MA_CHANNEL_FRONT_LEFT: return Speaker::FrontLeft;
    case MA_CHANNEL_FRONT_RIGHT: return Speaker::FrontRight;
    case MA_CHANNEL_FRONT_CENTER: return Speaker::FrontCentre;
    case MA_CHANNEL_LFE: return Speaker::LowFrequency;
    case MA_CHANNEL_BACK_LEFT: return Speaker::BackLeft;
    case MA_CHANNEL_BACK_RIGHT: return Speaker::BackRight;
    case MA_CHANNEL_SIDE_LEFT: return Speaker::SideLeft;
    case MA_CHANNEL_SIDE_RIGHT: return Speaker::SideRight;
    case MA_CHANNEL_TOP_FRONT_LEFT: return Speaker::TopFrontLeft;
    case MA_CHANNEL_TOP_FRONT_RIGHT: return Speaker::TopFrontRight;
    case MA_CHANNEL_TOP_BACK_LEFT: return Speaker::TopBackLeft;
    case MA_CHANNEL_TOP_BACK_RIGHT: return Speaker::TopBackRight;
    default: return Speaker::Count;  // a position no shipped profile has
  }
}

ma_channel channel_of(Speaker speaker) noexcept {
  switch (speaker) {
    case Speaker::Mono: return MA_CHANNEL_MONO;
    case Speaker::FrontLeft: return MA_CHANNEL_FRONT_LEFT;
    case Speaker::FrontRight: return MA_CHANNEL_FRONT_RIGHT;
    case Speaker::FrontCentre: return MA_CHANNEL_FRONT_CENTER;
    case Speaker::LowFrequency: return MA_CHANNEL_LFE;
    case Speaker::BackLeft: return MA_CHANNEL_BACK_LEFT;
    case Speaker::BackRight: return MA_CHANNEL_BACK_RIGHT;
    case Speaker::SideLeft: return MA_CHANNEL_SIDE_LEFT;
    case Speaker::SideRight: return MA_CHANNEL_SIDE_RIGHT;
    case Speaker::TopFrontLeft: return MA_CHANNEL_TOP_FRONT_LEFT;
    case Speaker::TopFrontRight: return MA_CHANNEL_TOP_FRONT_RIGHT;
    case Speaker::TopBackLeft: return MA_CHANNEL_TOP_BACK_LEFT;
    case Speaker::TopBackRight: return MA_CHANNEL_TOP_BACK_RIGHT;
    case Speaker::Count: break;
  }
  return MA_CHANNEL_NONE;
}

// Nothing plays through a probe: it is initialized to read the endpoint and never started.
void silent_callback(ma_device*, void*, const void*, ma_uint32) {}

// What the endpoint really is. The platform only says once a device is opened — WASAPI's mix format
// carries the speaker mask, PulseAudio's sink its channel map — so the device is opened in its own
// native format, read, and closed without ever starting.
struct Endpoint {
  u32 channels = 0;
  u32 sample_rate = 0;
  u32 map_channels = 0;
  ma_channel map[MA_MAX_CHANNELS] = {};
  ChannelLayout layout = ChannelLayout::Unknown;
};

bool probe(ma_context& context, const ma_device_id* id, Endpoint& out) {
  ma_device_config config = ma_device_config_init(ma_device_type_playback);
  config.playback.pDeviceID = id;
  config.playback.format = ma_format_f32;
  config.playback.channels = 0;  // native
  config.sampleRate = 0;         // native
  config.dataCallback = silent_callback;
  // ma_device is large; it lives on the heap for the few milliseconds the probe takes.
  auto* device = new ma_device;
  if (ma_device_init(&context, &config, device) != MA_SUCCESS) {
    delete device;
    return false;
  }
  out.channels = device->playback.internalChannels;
  out.sample_rate = device->playback.internalSampleRate;
  out.map_channels = out.channels < MA_MAX_CHANNELS ? out.channels : MA_MAX_CHANNELS;
  Speaker speakers[MA_MAX_CHANNELS];
  for (u32 c = 0; c < out.map_channels; ++c) {
    out.map[c] = device->playback.internalChannelMap[c];
    speakers[c] = speaker_of(out.map[c]);
  }
  out.layout = layout_from_speakers(std::span<const Speaker>(speakers, out.map_channels));
  ma_device_uninit(device);
  delete device;
  return true;
}

DeviceInfo describe(ma_context& context, const ma_device_info& device) {
  DeviceInfo info;
  info.name = device.name;
  info.is_default = device.isDefault != MA_FALSE;
  Endpoint endpoint;
  if (probe(context, &device.id, endpoint)) {
    info.layout = endpoint.layout;
    info.channels = endpoint.channels;
    info.sample_rate = endpoint.sample_rate;
    return info;
  }
  // A device that would not open to be read is still a device: whatever the platform lists for it
  // without opening, and an `Unknown` layout.
  ma_device_info detail;
  if (ma_context_get_device_info(&context, ma_device_type_playback, &device.id, &detail) ==
          MA_SUCCESS &&
      detail.nativeDataFormatCount > 0) {
    info.channels = detail.nativeDataFormats[0].channels;
    info.sample_rate = detail.nativeDataFormats[0].sampleRate;
  }
  return info;
}

// The audio thread's entry point. Everything below it is Mixer::render: no allocation, no lock,
// no log. `ma_device_config::noPreSilencedOutputBuffer` is set because render overwrites the whole
// buffer, and `noClip` because render clips itself.
void data_callback(ma_device* device, void* output, const void* input, ma_uint32 frames) {
  (void)input;
  static_cast<Output::Device*>(device->pUserData)->mixer->render(static_cast<f32*>(output), frames);
}

// miniaudio's device notifications, on whichever thread the backend raises them. They set a bit
// and return: the reopen is `Output::update()`'s, on the controlling thread (device.h).
void notification_callback(const ma_device_notification* notification) {
  auto* device = static_cast<Output::Device*>(notification->pDevice->pUserData);
  if (device == nullptr || device->closing.load(std::memory_order_acquire)) return;
  DeviceEvent event;
  switch (notification->type) {
    case ma_device_notification_type_stopped: event = DeviceEvent::Stopped; break;
    case ma_device_notification_type_rerouted: event = DeviceEvent::Rerouted; break;
    default: return;  // started, and the mobile platforms' interruptions and unlocks
  }
  device->events->fetch_or(static_cast<u32>(event), std::memory_order_release);
}

}  // namespace

DeviceList enumerate() {
  DeviceList list;
  ma_context context;
  if (!init_context(context)) return list;
  list.backend = backend_name(context.backend);

  ma_device_info* playback = nullptr;
  ma_uint32 playback_count = 0;
  if (ma_context_get_devices(&context, &playback, &playback_count, nullptr, nullptr) ==
      MA_SUCCESS) {
    // The list belongs to the context and a probe may refresh it, so it is copied first.
    Vector<ma_device_info> devices;
    devices.append(std::span<const ma_device_info>(playback, playback_count));
    list.devices.reserve(playback_count);
    for (const ma_device_info& device : devices)
      list.devices.push_back(describe(context, device));
  }
  ma_context_uninit(&context);
  return list;
}

Output::Device* open_device(const DeviceRequest& request, Mixer& mixer, std::atomic<u32>& events,
                            DeviceInfo& opened, std::string& why) {
  // Value-initialized: miniaudio's two structs start zeroed, as it expects.
  auto* out = new Output::Device();
  out->mixer = &mixer;
  out->events = &events;
  if (!init_context(out->context)) {
    why = "no platform audio API initialized";
    delete out;
    return nullptr;
  }

  ma_device_info* playback = nullptr;
  ma_uint32 playback_count = 0;
  if (ma_context_get_devices(&out->context, &playback, &playback_count, nullptr, nullptr) !=
          MA_SUCCESS ||
      playback_count == 0) {
    why = "no playback device";
    ma_context_uninit(&out->context);
    delete out;
    return nullptr;
  }

  // The named device, or the default one — which is also what a null id opens.
  ma_device_info chosen{};
  bool found = false;
  const bool by_name = request.name != nullptr && !request.name->empty();
  for (ma_uint32 i = 0; i < playback_count && !found; ++i) {
    if (by_name ? *request.name == playback[i].name : playback[i].isDefault != MA_FALSE) {
      chosen = playback[i];
      found = true;
    }
  }
  if (!found && by_name) {
    why = "no playback device named '" + *request.name + "'";
    ma_context_uninit(&out->context);
    delete out;
    return nullptr;
  }
  if (!found) chosen = playback[0];

  // Feed the device in the mixer's layout. When the endpoint *is* that layout, open it with the
  // endpoint's own channel map, so a 5.1 whose surrounds the platform labels "side" is fed as it
  // stands and the platform converts nothing; otherwise with the table's map, and the platform's
  // converter maps the declared layout onto the endpoint.
  const LayoutInfo& layout = layout_info(mixer.layout());
  Endpoint endpoint;
  const bool probed = probe(out->context, &chosen.id, endpoint);
  ma_channel map[k_max_layout_channels];
  if (probed && endpoint.layout == layout.layout) {
    for (u32 c = 0; c < layout.channels; ++c)
      map[c] = endpoint.map[c];
  } else {
    for (u32 c = 0; c < layout.channels; ++c)
      map[c] = channel_of(layout.speakers[c]);
  }

  ma_device_config config = ma_device_config_init(ma_device_type_playback);
  // The default is opened as "the default", not by the id it has today: PulseAudio then moves the
  // stream when the default sink changes and miniaudio says so (`rerouted`); a stream opened by id
  // is pinned to that sink. A named device is opened by its id.
  config.playback.pDeviceID = by_name ? &chosen.id : nullptr;
  config.playback.format = ma_format_f32;
  config.playback.channels = layout.channels;
  config.playback.pChannelMap = map;
  config.sampleRate = k_sample_rate;
  config.periodSizeInFrames = request.period_frames;
  config.performanceProfile = ma_performance_profile_low_latency;
  config.noPreSilencedOutputBuffer = MA_TRUE;
  config.noClip = MA_TRUE;
  // The device thread registers with MMCSS as "Pro Audio" on Windows: the mix has a deadline every
  // period and nothing else on the machine should be able to make it late (device.h). Elsewhere
  // miniaudio's device thread runs at the highest priority the platform grants it.
  config.wasapi.usage = ma_wasapi_usage_pro_audio;
  // miniaudio's own WASAPI rerouting is off. It reinitializes the device on the COM notification
  // thread, holding a mutex that `ma_device_uninit` destroys — so an output closed or reopened from
  // the controlling thread while a reroute was in flight would race it inside miniaudio. The engine
  // owns the policy instead: its own endpoint watcher (endpoint.cpp) and this device's
  // notifications only set a flag, and `Output::update()` reopens on the controlling thread.
  config.wasapi.noAutoStreamRouting = MA_TRUE;
  config.dataCallback = data_callback;
  config.notificationCallback = notification_callback;
  config.pUserData = out;

  if (ma_device_init(&out->context, &config, &out->device) != MA_SUCCESS) {
    why = "the device would not open";
    ma_context_uninit(&out->context);
    delete out;
    return nullptr;
  }
  if (ma_device_start(&out->device) != MA_SUCCESS) {
    why = "the device would not start";
    ma_device_uninit(&out->device);
    ma_context_uninit(&out->context);
    delete out;
    return nullptr;
  }

  opened = DeviceInfo{};
  opened.name = out->device.playback.name;
  opened.is_default = !by_name || chosen.isDefault != MA_FALSE;
  opened.channels = out->device.playback.internalChannels;
  opened.sample_rate = out->device.playback.internalSampleRate;
  Speaker speakers[MA_MAX_CHANNELS];
  const u32 count = opened.channels < MA_MAX_CHANNELS ? opened.channels : MA_MAX_CHANNELS;
  for (u32 c = 0; c < count; ++c)
    speakers[c] = speaker_of(out->device.playback.internalChannelMap[c]);
  opened.layout = layout_from_speakers(std::span<const Speaker>(speakers, count));
  return out;
}

void close_device(Output::Device* device) noexcept {
  if (device == nullptr) return;
  device->closing.store(true, std::memory_order_release);
  // Stops the device and joins its thread: after this no callback is running or will run.
  ma_device_uninit(&device->device);
  ma_context_uninit(&device->context);
  delete device;
}

bool device_stopped(const Output::Device* device) noexcept {
  return ma_device_get_state(&device->device) == ma_device_state_stopped;
}

const endpoint::IdChar* endpoint_id(const Output::Device* device) noexcept {
#if ENGINE_PLATFORM_WINDOWS
  return device->context.backend == ma_backend_wasapi ? device->device.playback.id.wasapi : nullptr;
#else
  (void)device;
  return nullptr;
#endif
}

}  // namespace backend
}  // namespace engine::audio
