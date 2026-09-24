#pragma once

// What `protocol::Context::app` points at in engine-host: everything the host's own methods keep
// between calls. It is one object because `Context::app` is one pointer, and deliberately so
// (docs/subsystems/protocol.md, "Methods an app registers"): the protocol has a single untyped
// registration point, and an app that registers several families of methods gathers their state
// behind it rather than asking the protocol for a second one.

#include "ops_methods.h"
#include "render_methods.h"

namespace engine::host {

struct HostState {
  // The renderer's device and scenes. Nothing is created until the first render.* call, so a host
  // that only edits documents never opens a device.
  RenderHost render;
  // The sessions' runtime worlds, made by session.run_headless.
  OpsHost ops;
};

}  // namespace engine::host
