#pragma once

// The one door to miniaudio (cmake/EngineAudio.cmake). backend.cpp is the only translation unit
// in the engine that includes <miniaudio.h>; everything else in this module speaks the engine
// types below, so the library's types, its defines and its warnings stay in one file.

#include "endpoint.h"

#include <core/base/types.h>
#include <domain/audio/clip_store.h>
#include <domain/audio/device.h>

#include <atomic>
#include <span>
#include <string>

namespace engine::audio::backend {

// Bytes in, 48 kHz f32 mono or stereo out (ClipStore's jobs, `decode_clip`).
DecodeStatus decode(std::span<const u8> encoded, DecodedClip& out);

// Playback devices from the first platform backend that initializes, each with the layout and
// rate its endpoint really has.
DeviceList enumerate();

struct DeviceRequest {
  const std::string* name = nullptr;  // empty or null: the default device
  u32 period_frames = 0;
};

// Opens and starts a playback device fed in the mixer's layout, whose callback renders `mixer`.
// Null on failure, with the reason in `why`; `opened` describes the endpoint as the platform
// reported it. miniaudio's device notifications set `DeviceEvent` bits in `events`, and nothing
// else. The default device is opened as "the default" rather than by its id, so a PulseAudio
// stream follows the default sink; WASAPI's automatic rerouting is off (backend.cpp says why).
Output::Device* open_device(const DeviceRequest& request, Mixer& mixer, std::atomic<u32>& events,
                            DeviceInfo& opened, std::string& why);
// Stops the device (joining its thread, so no callback is running when this returns) and frees it.
void close_device(Output::Device* device) noexcept;
// Whether the device has stopped. Nothing stops an open device but a failed stream — the
// platform's own rerouting is off — so a device that says it has is gone. Any thread.
bool device_stopped(const Output::Device* device) noexcept;
// The endpoint's id as the platform spells it (endpoint.h), for the watcher; null if none.
const endpoint::IdChar* endpoint_id(const Output::Device* device) noexcept;

}  // namespace engine::audio::backend
