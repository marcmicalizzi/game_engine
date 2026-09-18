#include <core/base/assert.h>
#include <core/log/log.h>
#include <domain/sim/scheduler.h>

#include <cstring>

namespace engine::sim {

ENGINE_LOG_CATEGORY_DEFINE(log_sim, "sim");

namespace {

constexpr const char* k_phase_names[k_phase_count] = {
    "input", "events_in", "lod", "systems", "physics", "post_physics", "events_out", "persist",
};

// FNV-1a. The schedule hash is an identity for "this registration produces this tick order", not
// a cryptographic digest, and keeping it here means the module needs no dependency on core/hash.
constexpr u64 k_fnv_offset = 1469598103934665603ull;
constexpr u64 k_fnv_prime = 1099511628211ull;

u64 fnv1a(u64 seed, const void* data, usize size) noexcept {
  const auto* bytes = static_cast<const u8*>(data);
  u64 hash = seed;
  for (usize i = 0; i < size; ++i) {
    hash ^= static_cast<u64>(bytes[i]);
    hash *= k_fnv_prime;
  }
  return hash;
}

u64 fnv1a_text(u64 seed, const char* text) noexcept {
  u64 hash = seed;
  if (text == nullptr) return hash;
  for (const char* p = text; *p != '\0'; ++p) {
    hash ^= static_cast<u64>(static_cast<u8>(*p));
    hash *= k_fnv_prime;
  }
  return hash;
}

bool covers(u8 mask, u8 tier) noexcept {
  if (static_cast<u32>(tier) >= 8u) return false;
  return ((static_cast<u32>(mask) >> tier) & 1u) != 0u;
}

// Two systems conflict when one writes something the other reads or writes. Read-read is free,
// which is the whole point of declaring the sets. A named resource is tested by exactly the same
// rule as a component, because to the schedule it is exactly the same kind of claim: the only
// difference is that the scheduler can never look at what is behind it.
bool conflicts(const SystemDesc& a, const SystemDesc& b) noexcept {
  return a.writes.intersects(b.writes) || a.writes.intersects(b.reads) ||
         a.reads.intersects(b.writes) || a.writes_resources.intersects(b.writes_resources) ||
         a.writes_resources.intersects(b.reads_resources) ||
         a.reads_resources.intersects(b.writes_resources);
}

}  // namespace

const char* phase_name(TickPhase phase) noexcept {
  const u32 index = static_cast<u32>(phase);
  return index < k_phase_count ? k_phase_names[index] : "unknown";
}

void ComponentMask::set(u32 component) noexcept {
  ENGINE_ASSERT(component < k_max_components, "ComponentMask: component id out of range");
  if (component >= k_max_components) return;
  words[component / 64u] |= (u64{1} << (component % 64u));
}

bool ComponentMask::test(u32 component) const noexcept {
  if (component >= k_max_components) return false;
  return (words[component / 64u] & (u64{1} << (component % 64u))) != 0;
}

bool ComponentMask::any() const noexcept {
  for (u32 i = 0; i < k_component_mask_words; ++i) {
    if (words[i] != 0) return true;
  }
  return false;
}

bool ComponentMask::intersects(const ComponentMask& other) const noexcept {
  for (u32 i = 0; i < k_component_mask_words; ++i) {
    if ((words[i] & other.words[i]) != 0) return true;
  }
  return false;
}

void ResourceMask::set(u32 resource) noexcept {
  ENGINE_ASSERT(resource < k_max_resources, "ResourceMask: resource id out of range");
  if (resource >= k_max_resources) return;
  words[resource / 64u] |= (u64{1} << (resource % 64u));
}

bool ResourceMask::test(u32 resource) const noexcept {
  if (resource >= k_max_resources) return false;
  return (words[resource / 64u] & (u64{1} << (resource % 64u))) != 0;
}

bool ResourceMask::any() const noexcept {
  for (u32 i = 0; i < k_resource_mask_words; ++i) {
    if (words[i] != 0) return true;
  }
  return false;
}

bool ResourceMask::intersects(const ResourceMask& other) const noexcept {
  for (u32 i = 0; i < k_resource_mask_words; ++i) {
    if ((words[i] & other.words[i]) != 0) return true;
  }
  return false;
}

// --- the resource registry --------------------------------------------------------------------

u32 ResourceRegistry::find(const char* name) const noexcept {
  if (name == nullptr) return k_invalid_resource;
  for (u32 i = 0; i < names_.size(); ++i) {
    // Compared by text and not by pointer: two translation units may hold two copies of the same
    // literal, and a capability that registered "animation.pose_pool" twice means one resource.
    if (std::strcmp(names_[i], name) == 0) return i;
  }
  return k_invalid_resource;
}

const char* ResourceRegistry::name_of(u32 resource) const noexcept {
  return resource < names_.size() ? names_[resource] : nullptr;
}

u32 ResourceRegistry::id(const char* name) {
  ENGINE_ASSERT(name != nullptr, "ResourceRegistry: a resource is registered under a name");
  const u32 existing = find(name);
  if (existing != k_invalid_resource) return existing;
  if (names_.size() >= k_max_resources) {
    // Counted and logged rather than aliased onto somebody else's bit: an aliased resource would
    // make the schedule quietly wrong, which is the one failure the declaration exists to prevent.
    ++overflowed_;
    ENGINE_LOG_WARN(log_sim, "resource registry is full; this resource has no bit",
                    log::field("resource", name != nullptr ? name : "(unnamed)"),
                    log::field("max", k_max_resources));
    return k_invalid_resource;
  }
  names_.push_back(name);
  return names_.size() - 1u;
}

ResourceRegistry& ResourceRegistry::global() noexcept {
  static ResourceRegistry registry;
  return registry;
}

u32 resource_id(const char* name) { return ResourceRegistry::global().id(name); }

SimScheduler::SimScheduler(const SimSchedulerConfig& config)
    : clock_(config.hz, config.max_steps_per_advance),
      game_clock_(clock_, config.game_seconds_per_real_second),
      wheel_(TimingWheelConfig{GameTime{game_clock_.us_per_tick()}, config.epoch, 0}),
      tiers_(config.job_system),
      jobs_(config.job_system) {
  game_clock_.jump_to(config.epoch);
}

u16 SimScheduler::add_system(const SystemDesc& desc) {
  ENGINE_VERIFY(systems_.size() < 0xFFFEu, "SimScheduler: too many systems");
  systems_.push_back(desc);
  schedule_dirty_ = true;
  return static_cast<u16>(systems_.size() - 1u);
}

u16 SimScheduler::add_hooks(const MaterializationHooks& hooks) {
  hooks_.push_back(hooks);
  return static_cast<u16>(hooks_.size() - 1u);
}

void SimScheduler::set_persist_hook(void (*hook)(void* hook_context, SimTick at_tick,
                                                 GameTime at_time),
                                    void* hook_context) noexcept {
  persist_hook_ = hook;
  persist_context_ = hook_context;
}

void SimScheduler::rebuild_schedule() const {
  schedule_.clear();
  wave_of_.assign(systems_.size(), u16{0});
  for (u32 phase = 0; phase < k_phase_count; ++phase) {
    phase_first_[phase] = schedule_.size();
    phase_count_[phase] = 0;
    phase_waves_[phase] = 0;

    // Longest-path layering in declaration order: a system runs one wave after the latest system
    // it conflicts with. That is the minimum number of barriers that still preserves declaration
    // order between every conflicting pair, and it depends on nothing but the registration.
    u16 max_wave = 0;
    u32 members = 0;
    for (u32 i = 0; i < systems_.size(); ++i) {
      if (static_cast<u32>(systems_[i].phase) != phase) continue;
      u16 wave = 0;
      for (u32 j = 0; j < i; ++j) {
        if (static_cast<u32>(systems_[j].phase) != phase) continue;
        if (!conflicts(systems_[i], systems_[j])) continue;
        const u16 candidate = static_cast<u16>(wave_of_[j] + 1u);
        if (candidate > wave) wave = candidate;
      }
      wave_of_[i] = wave;
      if (wave > max_wave) max_wave = wave;
      ++members;
    }
    if (members == 0) continue;

    for (u16 wave = 0; wave <= max_wave; ++wave) {
      for (u32 i = 0; i < systems_.size(); ++i) {
        if (static_cast<u32>(systems_[i].phase) != phase) continue;
        if (wave_of_[i] != wave) continue;
        schedule_.push_back(ScheduleEntry{static_cast<u16>(i), wave});
      }
    }
    phase_count_[phase] = members;
    phase_waves_[phase] = static_cast<u16>(max_wave + 1u);
  }

  // One invocation per (system, batch) of the widest wave, reserved once so a tick allocates
  // nothing (docs/plan/11-performance-principles.md §11.2).
  u32 widest = 0;
  for (u32 phase = 0; phase < k_phase_count; ++phase) {
    for (u16 wave = 0; wave < phase_waves_[phase]; ++wave) {
      u32 total = 0;
      for (u32 k = phase_first_[phase]; k < phase_first_[phase] + phase_count_[phase]; ++k) {
        if (schedule_[k].wave != wave) continue;
        const SystemDesc& desc = systems_[schedule_[k].system];
        total += desc.batches != 0 ? desc.batches : 1u;
      }
      if (total > widest) widest = total;
    }
  }
  invocations_.reserve(widest);
  jobs_buffer_.reserve(widest);
  schedule_dirty_ = false;
}

std::span<const ScheduleEntry> SimScheduler::schedule(TickPhase phase) const {
  if (schedule_dirty_) rebuild_schedule();
  const u32 index = static_cast<u32>(phase);
  if (index >= k_phase_count) return {};
  return {schedule_.data() + phase_first_[index], phase_count_[index]};
}

u16 SimScheduler::wave_count(TickPhase phase) const {
  if (schedule_dirty_) rebuild_schedule();
  const u32 index = static_cast<u32>(phase);
  return index < k_phase_count ? phase_waves_[index] : u16{0};
}

u16 SimScheduler::wave_of(u16 index) const {
  if (schedule_dirty_) rebuild_schedule();
  return index < wave_of_.size() ? wave_of_[index] : u16{0};
}

u64 SimScheduler::schedule_hash() const {
  if (schedule_dirty_) rebuild_schedule();
  u64 hash = k_fnv_offset;
  for (u32 phase = 0; phase < k_phase_count; ++phase) {
    hash = fnv1a_text(hash, k_phase_names[phase]);
    for (u32 k = phase_first_[phase]; k < phase_first_[phase] + phase_count_[phase]; ++k) {
      const ScheduleEntry entry = schedule_[k];
      const SystemDesc& desc = systems_[entry.system];
      hash = fnv1a_text(hash, desc.name);
      hash = fnv1a(hash, &entry.wave, sizeof(entry.wave));
      hash = fnv1a(hash, &desc.batches, sizeof(desc.batches));
      hash = fnv1a(hash, &desc.reads, sizeof(desc.reads));
      hash = fnv1a(hash, &desc.writes, sizeof(desc.writes));
      hash = fnv1a(hash, &desc.reads_resources, sizeof(desc.reads_resources));
      hash = fnv1a(hash, &desc.writes_resources, sizeof(desc.writes_resources));
    }
  }
  return hash;
}

void SimScheduler::run_invocation(void* data) {
  auto* invocation = static_cast<Invocation*>(data);
  invocation->desc->tick(invocation->context, invocation->batch);
}

void SimScheduler::run_wave(TickPhase phase, u16 wave, u32 first, u32 count) {
  invocations_.clear();
  for (u32 k = first; k < first + count; ++k) {
    const ScheduleEntry entry = schedule_[k];
    if (entry.wave != wave) continue;
    const SystemDesc& desc = systems_[entry.system];
    if (desc.tick == nullptr) continue;
    const u32 batches = desc.batches != 0 ? desc.batches : 1u;
    for (u32 b = 0; b < batches; ++b) {
      Invocation invocation;
      invocation.desc = &desc;
      invocation.batch = Batch{b, b + 1u};
      invocation.context.tick = tick_;
      invocation.context.time = game_clock_.now();
      invocation.context.step = step_size();
      invocation.context.context = desc.context;
      invocation.context.phase = phase;
      invocation.context.system = entry.system;
      invocation.context.wave = wave;
      invocations_.push_back(invocation);
    }
  }
  if (invocations_.empty()) return;

  // One invocation is not worth a job submission and a counter wait; and with no job system the
  // wave is simply serial, which is the same answer computed on one thread.
  if (jobs_ == nullptr || invocations_.size() == 1u) {
    for (u32 i = 0; i < invocations_.size(); ++i) {
      invocations_[i].desc->tick(invocations_[i].context, invocations_[i].batch);
    }
    return;
  }

  jobs_buffer_.clear();
  jobs::Counter counter;
  for (u32 i = 0; i < invocations_.size(); ++i) {
    jobs_buffer_.push_back(jobs::Job{&SimScheduler::run_invocation, &invocations_[i], &counter});
  }
  jobs_->schedule(jobs::Pool::Performance,
                  std::span<const jobs::Job>(jobs_buffer_.data(), jobs_buffer_.size()), counter);
  jobs_->wait(counter);
}

void SimScheduler::run_phase(TickPhase phase) {
  const u32 index = static_cast<u32>(phase);

  // The wheel is pumped at the top of `events in`, so the phase's systems see this tick's
  // events and nothing arrives mid-phase.
  if (phase == TickPhase::EventsIn) wheel_.advance(game_clock_.now(), event_sink_);

  SystemContext context;
  context.tick = tick_;
  context.time = game_clock_.now();
  context.step = step_size();
  context.phase = phase;
  context.wave = 0;

  for (u32 i = 0; i < systems_.size(); ++i) {
    if (systems_[i].phase != phase || systems_[i].begin_tick == nullptr) continue;
    context.system = static_cast<u16>(i);
    context.context = systems_[i].context;
    systems_[i].begin_tick(context);
  }

  for (u16 wave = 0; wave < phase_waves_[index]; ++wave) {
    run_wave(phase, wave, phase_first_[index], phase_count_[index]);
  }

  for (u32 i = 0; i < systems_.size(); ++i) {
    if (systems_[i].phase != phase || systems_[i].end_tick == nullptr) continue;
    context.system = static_cast<u16>(i);
    context.context = systems_[i].context;
    systems_[i].end_tick(context);
  }

  if (phase == TickPhase::Persist && persist_hook_ != nullptr) {
    persist_hook_(persist_context_, tick_, game_clock_.now());
  }
}

void SimScheduler::step() {
  if (schedule_dirty_) rebuild_schedule();
  ++tick_;
  game_clock_.advance_tick();
  for (u32 phase = 0; phase < k_phase_count; ++phase)
    run_phase(static_cast<TickPhase>(phase));
}

u32 SimScheduler::advance(i64 real_ns) {
  clock_.advance(real_ns);
  u32 steps = 0;
  while (clock_.step()) {
    step();
    ++steps;
  }
  return steps;
}

void SimScheduler::apply_tier_changes(std::span<const TierChange> changes,
                                      std::span<const EntityHandle> entities) {
  for (usize c = 0; c < changes.size(); ++c) {
    const TierChange change = changes[c];
    if (change.index >= entities.size()) continue;
    const EntityHandle entity = entities[change.index];
    // A null handle is a row no hook materialized: there is nothing live to promote or demote, and
    // handing a hook a handle that names nothing is how a capability learns to check for it.
    if (entity.is_null()) continue;
    if (change.to == change.from) continue;
    for (u32 h = 0; h < hooks_.size(); ++h) {
      const MaterializationHooks& hook = hooks_[h];
      if (!covers(hook.tiers, change.from) && !covers(hook.tiers, change.to)) continue;
      if (change.to < change.from) {
        if (hook.promote != nullptr) hook.promote(hook.context, entity, change.from, change.to);
      } else if (hook.demote != nullptr) {
        hook.demote(hook.context, entity, change.from, change.to);
      }
    }
  }
}

EntityHandle SimScheduler::materialize(const EntityRecord& record, u8 tier) {
  // Registration order, and the first hook that gives the record a runtime existence names it.
  // Every hook still runs: a second one attaching its own state to the entity the first created is
  // the ordinary case, and it is why the handle is *returned* rather than passed in.
  EntityHandle handle;
  for (u32 h = 0; h < hooks_.size(); ++h) {
    const MaterializationHooks& hook = hooks_[h];
    if (!covers(hook.tiers, tier)) continue;
    if (hook.materialize == nullptr) continue;
    const EntityHandle produced = hook.materialize(hook.context, record, tier);
    if (handle.is_null()) handle = produced;
  }
  return handle;
}

void SimScheduler::dematerialize(std::span<const EntityHandle> entities) {
  for (usize e = 0; e < entities.size(); ++e) {
    if (entities[e].is_null()) continue;
    for (u32 h = 0; h < hooks_.size(); ++h) {
      const MaterializationHooks& hook = hooks_[h];
      if (hook.dematerialize != nullptr) hook.dematerialize(hook.context, entities[e]);
    }
  }
}

ReconcileResult SimScheduler::reconcile_tile(const ReconcileParams& params, const TileStore& store,
                                             const ObserverSet& observers,
                                             const TierParams& tier_params) {
  ReconcileResult result;

  // 1. the tile's stored state (plan 05 §5.5 step 1).
  TileState state;
  if (store.tile_state != nullptr) {
    result.known = store.tile_state(store.context, params.tile, state);
  }

  // 2. elapsed game time since the tile was last active (step 2).
  if (result.known && params.now > state.last_active) result.gap = params.now - state.last_active;

  // 3. summarize the LOD3 systems over the gap, from the tile's seed and nothing else (step 3).
  // Registration order, so two runs summarize in the same order whatever the tile contains.
  if (result.gap.us > 0) {
    SummarizeInterval interval;
    interval.from = state.last_active;
    interval.to = params.now;
    interval.tile = params.tile;
    interval.seed = state.seed;
    interval.tier = params.summarize_tier;
    const std::span<const Summarizer> registered = wheel_.summarizers();
    for (usize i = 0; i < registered.size(); ++i) {
      const Summarizer& summarizer = registered[i];
      if (!covers(summarizer.tiers, params.summarize_tier)) continue;
      interval.system = summarizer.system;
      if (summarizer.fn != nullptr) summarizer.fn(summarizer.context, interval);
      ++result.summarized;
    }
  }

  // 4. materialize at LOD2 (step 4).
  records_.clear();
  if (store.load_records != nullptr) store.load_records(store.context, params.tile, records_);
  result.records = records_.size();

  positions_.clear();
  importance_.clear();
  tiers_buffer_.clear();
  handles_.clear();
  positions_.reserve(records_.size());
  importance_.reserve(records_.size());
  tiers_buffer_.reserve(records_.size());
  handles_.reserve(records_.size());
  for (u32 i = 0; i < records_.size(); ++i) {
    const EntityRecord& record = records_[i];
    // Step 5 promotes what step 4 brought in, and a promotion needs a *runtime* handle: the
    // record's `Id128` is the name that survived the disk and not the one the hooks act on. So the
    // handle comes back out of the materialization rather than being looked up again.
    handles_.push_back(materialize(record, params.materialize_tier));
    ++result.materialized;
    positions_.push_back(record.position);
    importance_.push_back(record.importance);
    tiers_buffer_.push_back(params.materialize_tier);
  }

  // 5. promote by observer distance (step 5). The plan's own step 5 — derived visual state —
  // is the renderer's and is deliberately not done here.
  changes_.clear();
  if (!records_.empty()) {
    TierInput input;
    input.positions = std::span<const Vec3>(positions_.data(), positions_.size());
    input.importance = std::span<const f32>(importance_.data(), importance_.size());
    input.tiers = std::span<u8>(tiers_buffer_.data(), tiers_buffer_.size());
    tiers_.assign_tiers(input, observers, tier_params, changes_);
    apply_tier_changes(std::span<const TierChange>(changes_.data(), changes_.size()),
                       std::span<const EntityHandle>(handles_.data(), handles_.size()));
    for (u32 i = 0; i < changes_.size(); ++i) {
      if (changes_[i].to < changes_[i].from) ++result.promoted;
    }
  }

  ENGINE_LOG_DEBUG(log_sim, "tile reconciled", log::field("tile", params.tile),
                   log::field("gap_us", result.gap.us), log::field("records", result.records),
                   log::field("summarized", result.summarized),
                   log::field("promoted", result.promoted));
  return result;
}

}  // namespace engine::sim
