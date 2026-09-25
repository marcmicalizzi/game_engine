#include "audio_log.h"
#include "backend.h"

#include <domain/audio/audio.h>
#include <domain/audio/device.h>

namespace engine::audio {

DeviceList enumerate_devices() { return backend::enumerate(); }

ChannelLayout resolve_layout(ChannelLayout device_layout, EndpointFormFactor form_factor) noexcept {
  const ChannelLayout setting = tunable_layout();
  if (setting != ChannelLayout::Unknown) return setting;
  // Headphones are chosen for an endpoint that is worn and has two channels (or will not say how
  // many). One that is worn and reports more — a gaming headset's 7.1 endpoint — renders those
  // speakers to the ears itself, in its driver: that is the decode happening at the endpoint, which
  // the direction note puts first (plan 05 §5.11), so it gets its own layout.
  const bool worn =
      form_factor == EndpointFormFactor::Headphones || form_factor == EndpointFormFactor::Headset;
  const bool two_or_fewer = device_layout == ChannelLayout::Unknown ||
                            device_layout == ChannelLayout::Stereo ||
                            device_layout == ChannelLayout::Mono;
  if (worn && two_or_fewer) return ChannelLayout::Headphones;
  if (device_layout != ChannelLayout::Unknown) return device_layout;
  return ChannelLayout::Stereo;
}

ChannelLayout resolve_layout(const DeviceInfo& device) noexcept {
  return resolve_layout(device.layout, device.form_factor);
}

const char* form_factor_name(EndpointFormFactor form_factor) noexcept {
  switch (form_factor) {
    case EndpointFormFactor::Unknown: return "unknown";
    case EndpointFormFactor::Speakers: return "speakers";
    case EndpointFormFactor::Headphones: return "headphones";
    case EndpointFormFactor::Headset: return "headset";
    case EndpointFormFactor::LineLevel: return "line_level";
    case EndpointFormFactor::Digital: return "digital";
    case EndpointFormFactor::Other: return "other";
  }
  return "unknown";
}

const char* output_backend_name(OutputBackend backend) noexcept {
  switch (backend) {
    case OutputBackend::Null: return "null";
    case OutputBackend::Device: return "device";
  }
  return "unknown";
}

const char* device_event_name(u32 events) noexcept {
  if ((events & static_cast<u32>(DeviceEvent::Lost)) != 0) return "lost";
  if ((events & static_cast<u32>(DeviceEvent::Stopped)) != 0) return "stopped";
  if ((events & static_cast<u32>(DeviceEvent::DefaultChanged)) != 0) return "default_changed";
  if ((events & static_cast<u32>(DeviceEvent::Rerouted)) != 0) return "rerouted";
  return "none";
}

Output::Output(Mixer& mixer) noexcept : mixer_(&mixer) {}

Output::~Output() { close(); }

bool Output::open(const OutputConfig& config) {
  // `config` may be `config_` itself (update() reopens with it): take the copy before closing.
  const OutputConfig wanted = config;
  close();
  config_ = wanted;
  fallback_reason_.clear();
  if (wanted.backend == OutputBackend::Device) {
    backend::DeviceRequest request;
    request.name = &wanted.device;
    request.period_frames =
        wanted.period_frames != 0 ? wanted.period_frames : tunable_period_frames();
    std::string why;
    device_ = backend::open_device(request, *mixer_, events_, device_info_, why);
    // Watch the endpoints whether or not a device opened: with none, a device that later becomes
    // the default is how the output finds one.
    watcher_ =
        endpoint::watch(&events_, device_ != nullptr ? backend::endpoint_id(device_) : nullptr);
    if (device_ != nullptr) {
      backend_ = OutputBackend::Device;
      open_ = true;
      ENGINE_LOG_INFO(log_audio, "output device opened", log::field("device", device_info_.name),
                      log::field("device_layout", layout_name(device_info_.layout)),
                      log::field("form_factor", form_factor_name(device_info_.form_factor)),
                      log::field("device_rate", device_info_.sample_rate),
                      log::field("mix_layout", layout_name(mixer_->layout())),
                      log::field("period_frames", request.period_frames));
      return true;
    }
    fallback_reason_ = why;
    if (!wanted.fall_back_to_null) {
      ENGINE_LOG_WARN(log_audio, "output device did not open", log::field("reason", why));
      endpoint::unwatch(watcher_);  // closed, and nothing will ask it to reopen
      watcher_ = nullptr;
      return false;
    }
    // A machine with no audio device is a normal case (device.h): the game runs, the mix is
    // computed and not heard, and the log says why once.
    ENGINE_LOG_INFO(log_audio, "no output device, using the null backend",
                    log::field("reason", why));
  }
  backend_ = OutputBackend::Null;
  device_info_ = DeviceInfo{};
  open_ = true;
  return true;
}

void Output::close() noexcept {
  // The watcher first, so nothing raises a flag about a device this output is letting go of; then
  // the device, whose close joins its thread.
  endpoint::unwatch(watcher_);
  watcher_ = nullptr;
  if (device_ != nullptr) {
    backend::close_device(device_);
    device_ = nullptr;
  }
  events_.store(0, std::memory_order_release);
  open_ = false;
  backend_ = OutputBackend::Null;
  device_info_ = DeviceInfo{};
}

void Output::notify(DeviceEvent event) noexcept {
  events_.fetch_or(static_cast<u32>(event), std::memory_order_release);
}

bool Output::update() {
  if (!open_) return false;
  u32 events = events_.exchange(0, std::memory_order_acq_rel);
  // A device that has stopped without being asked is gone, whether or not anything said so:
  // miniaudio posts no `stopped` when the stop itself fails, which is what an unplugged endpoint's
  // stream does, and ALSA has no notification at all. A `stopped` from a device still running is
  // PulseAudio suspending a sink, and is no loss.
  if (device_ != nullptr && backend::device_stopped(device_)) {
    events |= static_cast<u32>(DeviceEvent::Lost);
  } else {
    events &= ~static_cast<u32>(DeviceEvent::Stopped);
  }
  // An output opened on a named device plays to that device: a new default is none of its
  // business.
  if (!config_.device.empty()) events &= ~static_cast<u32>(DeviceEvent::DefaultChanged);
  if (events == 0) return false;

  const std::string previous = device_info_.name;
  const OutputBackend previous_backend = backend_;
  open(config_);
  ++reopens_;
  ENGINE_LOG_INFO(
      log_audio, "output device changed", log::field("reason", device_event_name(events)),
      log::field("previous", previous),
      log::field("previous_backend", output_backend_name(previous_backend)),
      log::field("device", device_info_.name), log::field("backend", output_backend_name(backend_)),
      log::field("device_layout", layout_name(device_info_.layout)),
      log::field("form_factor", form_factor_name(device_info_.form_factor)),
      log::field("mix_layout", layout_name(mixer_->layout())), log::field("reopens", reopens_));
  return true;
}

void Output::render(f32* out, u32 frames) noexcept {
  if (!open_ || backend_ != OutputBackend::Null) return;
  mixer_->render(out, frames);
}

}  // namespace engine::audio
