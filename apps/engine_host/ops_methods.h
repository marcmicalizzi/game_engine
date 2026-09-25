#pragma once

// engine-host's operation methods (docs/plan/06-agent-tooling.md §6.9, docs/subsystems/protocol.md,
// "The day-one operations"): the rest of the plan's day-one list beside `render.*`.
//
//   content.build          ContentBuildParams  -> ContentBuildResult    build_content
//   session.events         SessionEventsParams -> SessionEventsResult   events
//   engine.budgets         BudgetsParams       -> BudgetsResult         get_budgets
//   session.run_headless   RunHeadlessParams   -> RunHeadlessResult     run_headless
//   engine.run_tests       RunTestsParams      -> RunTestsResult        run_tests
//   session.materialize    MaterializeParams   -> MaterializeResult     (the materialization
//   report)
//   session.state_hash     StateHashParams     -> StateHashResult       (05 §5.10's hash)
//   session.save_game      SaveGameParams      -> SaveGameResult        (03 §3.5's save game)
//   session.load_game      LoadGameParams      -> LoadGameResult        (and its load)
//
// The last three are the world capability's (docs/subsystems/world.md, "Save and load") and answer
// 1006 in a build without it.
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

// One session's runtime world: a materialization of the session's document, made by the first
// `session.run_headless` or `session.materialize` on that session and kept while the host runs, so
// a second call continues from the tick the first stopped at. Defined in ops_methods.cpp, because
// what it holds depends on the capabilities this build has: always the engine's `sim::SimScheduler`
// (the tick, the hooks, the write-back) and the materialization driver; with the ECS capability, a
// flecs world whose systems the scheduler runs, the entity store's hook, and the kinematics,
// animation and audio emitter capabilities where they are built.
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
  // A world for the session made at a clock — a loaded save's tick and game time — replacing any
  // world the session had (docs/subsystems/world.md, "Save and load").
  RuntimeWorld* world_at(std::string_view session, const protocol::SessionManager& sessions,
                         u64 tick, i64 game_time_us);
  u32 count() const noexcept { return worlds_.size(); }
  RuntimeWorld* at(u32 index) noexcept { return worlds_[index].get(); }

 private:
  Vector<std::unique_ptr<RuntimeWorld>> worlds_;
};

// Registers content.build, session.events, engine.budgets, session.run_headless,
// engine.run_tests, session.materialize, session.state_hash, session.save_game and
// session.load_game. `Context::app` must point at the host's `HostState`.
void add_ops_methods(protocol::Dispatcher& dispatcher);

}  // namespace engine::host
