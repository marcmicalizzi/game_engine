#include <domain/audio/device.h>
#include <domain/audio/format.h>
#include <domain/audio/protocol.h>
#include <domain/protocol/rpc.h>

#include <schemas/audio.h>

namespace engine::audio {

namespace {

bool devices(protocol::Context&, AudioDevicesResult& result, protocol::RpcError&) {
  const DeviceList list = enumerate_devices();
  result.backend = list.backend;
  result.mix_sample_rate = k_sample_rate;
  ChannelLayout default_layout = ChannelLayout::Unknown;
  result.devices.reserve(list.devices.size());
  for (const DeviceInfo& device : list.devices) {
    AudioDevice out;
    out.name = device.name;
    out.layout = device.layout;
    out.channels = device.channels;
    out.sample_rate = device.sample_rate;
    out.is_default = device.is_default;
    if (device.is_default) default_layout = device.layout;
    result.devices.push_back(std::move(out));
  }
  result.mix_layout = resolve_layout(default_layout);
  return true;
}

}  // namespace

void register_methods(protocol::Dispatcher& dispatcher) {
  dispatcher.add(protocol::method_no_params<AudioDevicesResult, &devices>(
      "audio.devices",
      "The playback devices this machine has, each with the speaker layout and rate its endpoint "
      "really has, the platform API that listed them, and the layout a mixer would be declared "
      "with now. An empty list is a normal answer: the engine then mixes into the null backend."));
}

}  // namespace engine::audio
