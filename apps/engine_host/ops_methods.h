#pragma once

// engine-host's operation methods (docs/plan/06-agent-tooling.md §6.9, docs/subsystems/protocol.md,
// "The day-one operations"): the rest of the plan's day-one list beside `render.*`.
//
//   content.build          ContentBuildParams  -> ContentBuildResult    build_content
//   session.events         SessionEventsParams -> SessionEventsResult   events
//   engine.budgets         BudgetsParams       -> BudgetsResult         get_budgets
//   session.run_headless   RunHeadlessParams   -> RunHeadlessResult     run_headless
//   engine.run_tests       RunTestsParams      -> RunTestsResult        run_tests
//
// They live in the app rather than in `domain/protocol` for the reason `render.*` does: each is
// over something above the protocol's layer — the content build (`domain/content_build`), the
// renderer's scenes, a runtime world of `domain/ecs` and the capabilities that tick in it, the
// store, the tissue validators — and a method over a module cannot be registered from below it.
// Their state reaches them through `protocol::Context::app`, which points at a `HostState`
// (host_state.h).
//
// Every capability they touch is optional (ADR-0027) and each is linked only when this
// configuration has it (CMakeLists.txt): a method that needs one that is missing answers 1006
// `Unavailable` naming it, and a method that merely reports less without one says so in its result.

#include <core/base/macros.h>
#include <core/containers/vector.h>
#include <domain/protocol/rpc.h>

#include <memory>
#include <string>
#include <string_view>

namespace engine::protocol {
class SessionManager;
}

namespace engine::host {

// One session's runtime world: made by the first `session.run_headless` on that session and kept
// while the host runs, so a second call continues from the tick the first stopped at. Defined in
// ops_methods.cpp, because what it holds depends on the capabilities this build has: a flecs world
// with the animation and audio emitter systems installed when the ECS capability is built, and the
// engine's own `sim::SimScheduler` with no systems otherwise.
class RuntimeWorld;

class OpsHost {
 public:
  OpsHost() noexcept;
  ~OpsHost();
  ENGINE_NON_COPYABLE(OpsHost);

  // The session's world, made when `create` is set and it has none. Worlds of sessions that have
  // been closed are dropped first: session ids are never reused within a process, so such a world
  // is unreachable, and holding a flecs world for it would be a leak with a name.
  RuntimeWorld* world(std::string_view session, const protocol::SessionManager& sessions,
                      bool create);
  u32 count() const noexcept { return worlds_.size(); }
  RuntimeWorld* at(u32 index) noexcept { return worlds_[index].get(); }

 private:
  Vector<std::unique_ptr<RuntimeWorld>> worlds_;
};

// Registers content.build, session.events, engine.budgets, session.run_headless and
// engine.run_tests. `Context::app` must point at the host's `HostState`.
void add_ops_methods(protocol::Dispatcher& dispatcher);

}  // namespace engine::host
