#pragma once

// engine-host's `--listen` (docs/subsystems/apps.md, "engine-host --listen"; protocol.md,
// "Transport"): the protocol's own listener (`protocol::Listener`, domain/protocol/listener.h) over
// the host's dispatcher and its dispatch lock, beside the stdio loop. What is the host's alone is
// the captures it keeps: `render.capture {keep}` holds the last eight, and `GET /capture/<id>`
// serves one — the colour PNG, or with `/ids` the id buffer raw — so the browser shell never needs
// a file path. That is this source; everything else (the token, the Host and Origin checks, the
// static files, the dispatch) is the listener's and identical in every host that runs one.

#include "host_state.h"

#include <domain/protocol/listener.h>

namespace engine::host {

class HostCaptures final : public protocol::CaptureSource {
 public:
  explicit HostCaptures(HostState& state) noexcept : state_(&state) {}
  bool capture(std::string_view id, std::string_view part, net::HttpResponse& response) override;

 private:
  HostState* state_;
};

}  // namespace engine::host
