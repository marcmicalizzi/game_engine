#pragma once

// The one door to miniaudio (cmake/EngineAudio.cmake). backend.cpp is the only translation unit
// in the engine that includes <miniaudio.h>; everything else in this module speaks the engine
// types below, so the library's types, its defines and its warnings stay in one file.

#include <core/base/types.h>
#include <domain/audio/clip_store.h>
#include <domain/audio/device.h>

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
// reported it.
Output::Device* open_device(const DeviceRequest& request, Mixer& mixer, DeviceInfo& opened,
                            std::string& why);
// Stops the device (joining its thread, so no callback is running when this returns) and frees it.
void close_device(Output::Device* device) noexcept;

}  // namespace engine::audio::backend
