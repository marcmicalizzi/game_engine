#pragma once

// The platform's view of a playback endpoint beyond what miniaudio asks of it: the watcher that
// says the default device changed, and (Windows) the endpoint's form factor. Every platform
// include lives in endpoint.cpp and nowhere else; this header names only engine types and the
// platform's id character type, so backend.cpp can hand over miniaudio's device id without either
// file seeing the other's headers.

#include <core/base/macros.h>
#include <core/base/types.h>
#include <domain/audio/device.h>

#include <atomic>

namespace engine::audio::endpoint {

// A playback endpoint's id, as the platform spells it: IMMDevice::GetId's wide string on Windows,
// which is what miniaudio's WASAPI device id holds; a narrow name elsewhere.
#if ENGINE_PLATFORM_WINDOWS
using IdChar = wchar_t;
#else
using IdChar = char;
#endif

// Starts watching the platform's playback endpoints on behalf of an output: the system's default
// playback device changing sets `DeviceEvent::DefaultChanged` in `events`, and the endpoint `ours`
// (null for none: the output fell back to the null backend) going away sets `DeviceEvent::Lost`.
// The callbacks run on the platform's thread and do nothing else. Null where the platform has no
// such notification (everywhere but Windows: PulseAudio moves a default stream itself and
// miniaudio reports it as `rerouted`). The controlling thread; allocates.
Output::Watcher* watch(std::atomic<u32>* events, const IdChar* ours) noexcept;
// Stops the watcher and frees it. Null is fine.
void unwatch(Output::Watcher* watcher) noexcept;

}  // namespace engine::audio::endpoint
