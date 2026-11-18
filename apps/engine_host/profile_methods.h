#pragma once

// engine-host's `profile.*` methods (roadmap A10; docs/subsystems/protocol.md, "profile.*: the
// frame capture over the protocol"; plan 06 §6.6 and §6.9): core/profiling's frame capture turned
// on and off by a client, and foundation/profiler's frame report as the result, bounded in size,
// with the trace a person opens written beside it when the call names a directory. The frames are
// whatever the host draws meanwhile — `render.capture`, `render.benchmark`, a path — which mark
// themselves and record their GPU passes and CPU spans (systems/renderer, "The frame capture").
// The capture is the process's, so it is one for every client and scene of this host.

#include <domain/protocol/rpc.h>

namespace engine::host {

// Registers profile.start, profile.report and profile.stop.
void add_profile_methods(protocol::Dispatcher& dispatcher);

}  // namespace engine::host
