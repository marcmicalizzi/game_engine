#include <core/log/log.h>
#include <foundation/scripting/scripting.h>

namespace engine::scripting {

ENGINE_LOG_CATEGORY_DECLARE(log_scripting);

u32 ScriptingSystem::add(FunctionId on_tick, BindingId self) {
  const u32 index = instances_.size();
  instances_.push_back(ScriptInstance{on_tick, self, 0});
  return index;
}

void ScriptingSystem::set_tier(u32 instance, u32 tier) noexcept {
  if (instance < instances_.size()) instances_[instance].tier = tier;
}

void ScriptingSystem::begin_tick(u64 tick, f64 fixed_step) noexcept {
  tick_ = tick;
  calls_ = 0;
  failures_ = 0;
  results_.clear();
  context_->set_time(tick, fixed_step);
}

void ScriptingSystem::tick(f32 dt) {
  // One argument array for the whole tick: the dt slot never changes, the self slot is rewritten
  // per instance, and nothing here allocates unless a script returns data.
  Arg args[2] = {Arg::nil(), Arg::number(static_cast<f64>(dt))};
  for (u32 i = 0; i < instances_.size(); ++i) {
    const ScriptInstance& instance = instances_[i];
    if (instance.tier != 0) continue;
    args[0] = Arg::binding(instance.self);
    ++calls_;
    const Status status = context_->call(instance.on_tick, args, &scratch_);
    if (status != Status::Ok) {
      if (failures_++ == 0) {
        const ScriptError& error = context_->last_error();
        ENGINE_LOG_WARN(log_scripting, "on_tick failed", log::field("instance", i),
                        log::field("status", status_name(status)),
                        log::field("script", error.chunk), log::field("line", error.line),
                        log::field("message", error.message));
      }
      continue;
    }
    if (!scratch_.is_null()) {
      results_.push_back(TickResult{i, std::move(scratch_)});
      scratch_ = JsonValue();
    }
  }
}

u32 ScriptingSystem::lod_tier(f32 observer_score) noexcept {
  // Scores are distance-like: lower is nearer. The boundary is a placeholder until a consumer
  // game sets one; what is not a placeholder is that a far routine is not run at all.
  return observer_score < 1.0f ? 0u : 1u;
}

}  // namespace engine::scripting
