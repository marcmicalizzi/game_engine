#include "audio_log.h"
#include "backend.h"

#include <domain/audio/audio.h>
#include <domain/audio/device.h>

namespace engine::audio {

DeviceList enumerate_devices() { return backend::enumerate(); }

ChannelLayout resolve_layout(ChannelLayout device_layout) noexcept {
  const ChannelLayout setting = tunable_layout();
  if (setting != ChannelLayout::Unknown) return setting;
  if (device_layout != ChannelLayout::Unknown) return device_layout;
  return ChannelLayout::Stereo;
}

const char* output_backend_name(OutputBackend backend) noexcept {
  switch (backend) {
    case OutputBackend::Null: return "null";
    case OutputBackend::Device: return "device";
  }
  return "unknown";
}

Output::Output(Mixer& mixer) noexcept : mixer_(&mixer) {}

Output::~Output() { close(); }

bool Output::open(const OutputConfig& config) {
  close();
  fallback_reason_.clear();
  if (config.backend == OutputBackend::Device) {
    backend::DeviceRequest request;
    request.name = &config.device;
    request.period_frames =
        config.period_frames != 0 ? config.period_frames : tunable_period_frames();
    std::string why;
    device_ = backend::open_device(request, *mixer_, device_info_, why);
    if (device_ != nullptr) {
      backend_ = OutputBackend::Device;
      open_ = true;
      ENGINE_LOG_INFO(log_audio, "output device opened", log::field("device", device_info_.name),
                      log::field("device_layout", layout_name(device_info_.layout)),
                      log::field("device_rate", device_info_.sample_rate),
                      log::field("mix_layout", layout_name(mixer_->layout())),
                      log::field("period_frames", request.period_frames));
      return true;
    }
    fallback_reason_ = why;
    if (!config.fall_back_to_null) {
      ENGINE_LOG_WARN(log_audio, "output device did not open", log::field("reason", why));
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
  if (device_ != nullptr) {
    backend::close_device(device_);
    device_ = nullptr;
  }
  open_ = false;
  backend_ = OutputBackend::Null;
  device_info_ = DeviceInfo{};
}

void Output::render(f32* out, u32 frames) noexcept {
  if (!open_ || backend_ != OutputBackend::Null) return;
  mixer_->render(out, frames);
}

}  // namespace engine::audio
