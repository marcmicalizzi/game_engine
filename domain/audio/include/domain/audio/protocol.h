#pragma once

// The audio capability's protocol methods (docs/plan/06-agent-tooling.md §6.2, ADR-0027's
// "protocol methods" registration point).
//
//   audio.devices   no params -> engine.audio.AudioDevicesResult
//
// The app that owns the dispatcher calls `register_methods()`; engine-host does when the
// capability is in its build, so `engine-cli audio.devices` is the probe for "what can this
// machine play to" — the same question `gpu.adapters` answers for the GPU, and with the same
// property that an empty answer is a normal one.

namespace engine::protocol {
class Dispatcher;
}

namespace engine::audio {

void register_methods(protocol::Dispatcher& dispatcher);

}  // namespace engine::audio
