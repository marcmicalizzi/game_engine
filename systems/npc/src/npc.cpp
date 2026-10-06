#include <core/log/log.h>
#include <core/schema/json_reflect.h>
#include <core/schema/materialize.h>
#include <core/time/time.h>
#include <domain/doc/partition.h>
#include <domain/ecs/components.h>
#include <domain/ecs/identity.h>
#include <domain/sim/materialize.h>
#include <foundation/tunables/tunables.h>
#include <systems/npc/npc.h>

#include <algorithm>
#include <schemas/npc_ecs.h>
#include <schemas/world.h>
#include <schemas/world_ecs.h>
#include <string_view>

namespace engine::npc {

ENGINE_LOG_CATEGORY_DEFINE(log_npc, "npc");

namespace {

// ---- tunables (read once, when a host installs the capability) --------------------------------
//
// Why these defaults: 05 §5.6 puts 50–200 residents at LOD0 and 500–2,000 at LOD1. At the ring's
// 32 m tiles and the densities the generator makes (tens of residents a tile), a 16 m radius holds
// tens and a 64 m radius the low thousands round one observer — the table's orders of magnitude.
// Reassigning tiers every sixth tick (10 Hz) costs a sixth of what every tick would; a resident
// walks a metre or so between two looks, well inside the 15% band. The fast-forward budget is the
// number of transitions a caller will execute before it would rather summarize: 10^5 is one game
// day of 10^4 residents, about 40 ms of delivery at the measured cost per event.
tunables::Float t_near{"npc.lod.near_m", 16.0, 0.0, 1.0e6,
                       "Residents within this many metres of an observer are LOD0 (05 §5.4)."};
tunables::Float t_mid{"npc.lod.mid_m", 64.0, 0.0, 1.0e6,
                      "Residents within this many metres of an observer are LOD1; beyond, LOD2."};
tunables::Float t_hysteresis{"npc.lod.hysteresis", 0.15, 0.0, 4.0,
                             "A resident leaves a tier only this fraction past its boundary."};
tunables::Int t_max_promotions{"npc.lod.max_promotions", 64, 0, 1 << 24,
                               "Residents promoted per tier pass, nearest first; 0 is unlimited."};
tunables::Int t_max_demotions{"npc.lod.max_demotions", 256, 0, 1 << 24,
                              "Residents demoted per tier pass, farthest first; 0 is unlimited."};
tunables::Int t_lod_every{"npc.lod.every_ticks", 6, 1, 3600,
                          "Ticks between two tier passes over the residents."};
tunables::Int t_motion_every{"npc.motion.every_ticks", 1, 1, 3600,
                             "Ticks between two moves of the LOD0-1 residents on a trip."};
tunables::Int t_budget{"npc.fast_forward.budget", 100000, 0, i64{1} << 40,
                       "Events a fast-forward executes before it summarizes instead."};

// A place's position as the document holds it, f64 and unrounded (ADR-0053).
bool read_world_position(const JsonValue* value, WorldPos& out) {
  if (value == nullptr || !value->is_array() || value->size() < 3) return false;
  f64 c[3] = {};
  for (usize i = 0; i < 3; ++i) {
    if (!(*value)[i].get_f64(c[i])) return false;
  }
  const WorldPos p{c[0], c[1], c[2]};
  if (!world_cell_valid(p)) return false;
  out = p;
  return true;
}

PlaceRole role_of(const JsonValue* value) {
  std::string_view text;
  if (value == nullptr || !value->get_string(text)) return PlaceRole::Home;
  if (text == "Work") return PlaceRole::Work;
  if (text == "Service") return PlaceRole::Service;
  if (text == "Leisure") return PlaceRole::Leisure;
  return PlaceRole::Home;
}

u32 role_index(PlaceRole role) noexcept { return static_cast<u32>(role) & 3u; }

// A tile as one word, x in the high half: the driver's and the document's own packing.
u64 tile_key(doc::TileCoord tile) noexcept {
  return (static_cast<u64>(static_cast<u32>(tile.x)) << 32) | static_cast<u32>(tile.y);
}

doc::TileCoord tile_of_key(u64 key) noexcept {
  return doc::TileCoord{static_cast<i32>(static_cast<u32>(key >> 32)),
                        static_cast<i32>(static_cast<u32>(key))};
}

bool read_id(const JsonValue* value, Id128& out) {
  std::string_view text;
  return value != nullptr && value->get_string(text) && Id128::from_hex(text, out);
}

}  // namespace

NpcConfig config_from_tunables() {
  NpcConfig config;
  config.near_m = static_cast<f32>(t_near.get());
  config.mid_m = static_cast<f32>(t_mid.get());
  config.hysteresis = static_cast<f32>(t_hysteresis.get());
  config.max_promotions = static_cast<u32>(t_max_promotions.get());
  config.max_demotions = static_cast<u32>(t_max_demotions.get());
  config.lod_every = static_cast<u32>(t_lod_every.get());
  config.motion_every = static_cast<u32>(t_motion_every.get());
  config.fast_forward_budget = static_cast<u64>(t_budget.get());
  return config;
}

// ---- PlaceIndex --------------------------------------------------------------------------------

void PlaceIndex::clear() noexcept {
  by_id_.clear();
  positions_.clear();
  roles_.clear();
  ids_.clear();
  revision_ = ~u64{0};
  ++generation_;
}

void PlaceIndex::add(const Id128& id, WorldPos position, PlaceRole role) {
  const u32* existing = by_id_.find_value(id);
  if (existing != nullptr) {
    positions_[*existing] = position;
    roles_[*existing] = role;
    return;
  }
  by_id_.insert(id, positions_.size());
  positions_.push_back(position);
  roles_.push_back(role);
  ids_.push_back(id);
  ++generation_;
}

u32 PlaceIndex::refresh(const doc::Document& document) {
  if (document.revision() == revision_) return size();
  // `objects()` is the composed index's ids in id order, so the index is built in the same order
  // whatever the layers hold and wherever the places came from.
  Vector<Id128> ids;
  Vector<WorldPos> positions;
  Vector<PlaceRole> roles;
  for (const Id128& id : document.objects()) {
    if (document.type_of(id) != k_place_type) continue;
    WorldPos position;
    if (!read_world_position(document.property(id, "position"), position)) position = WorldPos{};
    ids.push_back(id);
    positions.push_back(position);
    roles.push_back(role_of(document.property(id, "role")));
  }
  bool same = ids.size() == ids_.size();
  for (u32 i = 0; same && i < ids.size(); ++i)
    same = ids[i] == ids_[i] && positions[i] == positions_[i] && roles[i] == roles_[i];
  if (!same) {
    by_id_.clear();
    for (u32 i = 0; i < ids.size(); ++i)
      by_id_.insert(ids[i], i);
    ids_ = std::move(ids);
    positions_ = std::move(positions);
    roles_ = std::move(roles);
    ++generation_;
  }
  revision_ = document.revision();
  return size();
}

u32 PlaceIndex::find(const Id128& id) const noexcept {
  const u32* index = by_id_.find_value(id);
  return index != nullptr ? *index : k_no_place;
}

// ---- NpcSystem ---------------------------------------------------------------------------------

NpcSystem::NpcSystem(const NpcConfig& config) : config_(config) {
  tier_params_.tier_count = 3;
  tier_params_.boundaries[0] = config_.near_m;
  tier_params_.boundaries[1] = config_.mid_m;
  tier_params_.boundaries[2] = 0.0f;
  tier_params_.hysteresis = config_.hysteresis;
  tier_params_.max_promotions = config_.max_promotions != 0 ? config_.max_promotions : ~0u;
  tier_params_.max_demotions = config_.max_demotions != 0 ? config_.max_demotions : ~0u;
  if (config_.lod_every == 0) config_.lod_every = 1;
  if (config_.motion_every == 0) config_.motion_every = 1;
}

NpcSystem::~NpcSystem() = default;

void NpcSystem::install(ecs::SimWorld& sim, sim::SimScheduler& scheduler, sim::EventSink next) {
  sim_ = &sim;
  scheduler_ = &scheduler;
  next_ = next;
  flecs::world& world = sim.world();
  // Seam 1: the world's own components (idempotent: a host registers `Transform` first) and ours.
  world::register_world_components(world);
  register_npc_components(world);
  resident_mapping_ = schema::MaterializeRegistry::global().find(k_resident_type);

  // The tier pass, at Lod: scores every held resident against the observers and drives the
  // promotions and demotions through every capability's hooks, ours among them.
  sim::SystemDesc lod;
  lod.name = "npc.lod";
  lod.phase = sim::TickPhase::Lod;
  lod.reads = ecs::mask_of<NpcState>(world);
  lod.writes = ecs::mask_of<world::Transform>(world);
  lod.tiers = k_entity_tiers;
  lod.determinism = sim::Determinism::Derived;
  lod.context = this;
  lod.tick = &NpcSystem::lod_tick;
  scheduler.add_system(lod);

  // The trips of LOD0–1 residents, at Systems. Derived: the transform it writes is never written
  // back, so nothing persistent depends on it.
  sim::SystemDesc motion;
  motion.name = "npc.motion";
  motion.phase = sim::TickPhase::Systems;
  motion.reads = ecs::mask_of<NpcState>(world);
  motion.writes = ecs::mask_of<world::Transform>(world);
  motion.tiers = 0b0011u;
  motion.determinism = sim::Determinism::Derived;
  motion.context = this;
  motion.tick = &NpcSystem::motion_tick;
  scheduler.add_system(motion);

  sim::Summarizer summarizer;
  summarizer.name = "npc.routine";
  summarizer.fn = &NpcSystem::summarize_fn;
  summarizer.context = this;
  summarizer.tiers = 0b1100u;  // LOD2 and LOD3: every resident the transitions would have moved
  scheduler.wheel().add_summarizer(summarizer);
  scheduler.set_event_sink(sink());
  ENGINE_LOG_INFO(log_npc, "capability installed", log::field("world_seed", config_.world_seed));
}

sim::MaterializationHooks NpcSystem::hooks() noexcept {
  sim::MaterializationHooks hooks;
  hooks.name = "npc";
  hooks.context = this;
  hooks.tiers = 0x0Fu;
  hooks.materialize = &NpcSystem::materialize_hook;
  hooks.promote = &NpcSystem::promote_hook;
  hooks.demote = &NpcSystem::promote_hook;
  hooks.dematerialize = &NpcSystem::dematerialize_hook;
  return hooks;
}

sim::EventSink NpcSystem::sink() noexcept {
  sim::EventSink sink;
  sink.fn = &NpcSystem::deliver;
  sink.context = this;
  return sink;
}

// ---- materialization ---------------------------------------------------------------------------

sim::EntityHandle NpcSystem::materialize_hook(void* context, const sim::EntityRecord& record,
                                              u8 tier) {
  return static_cast<NpcSystem*>(context)->materialize(record, tier);
}

void NpcSystem::promote_hook(void* context, sim::EntityHandle entity, u8 /*from*/, u8 to) {
  static_cast<NpcSystem*>(context)->set_tier(entity, to);
}

void NpcSystem::dematerialize_hook(void* context, sim::EntityHandle entity) {
  static_cast<NpcSystem*>(context)->dematerialize(entity);
}

sim::EntityHandle NpcSystem::materialize(const sim::EntityRecord& record, u8 tier) {
  if (sim_ == nullptr || scheduler_ == nullptr) return {};
  // A record from the document names its mapping; one from the store (reconciliation) carries none
  // and is a resident only if we already hold it.
  const bool is_resident =
      record.source != nullptr
          ? static_cast<const void*>(record.source->mapping) == resident_mapping_
          : true;
  if (!is_resident) return {};
  flecs::world& world = sim_->world();
  const flecs::entity e = ecs::entity_for(world, record.entity);
  if (!e.is_valid()) return {};
  const NpcRoutine* routine = e.try_get<NpcRoutine>();
  if (routine == nullptr) return {};
  const sim::EntityHandle handle = ecs::handle_of(e);

  u32 i = 0;
  const u32* held = by_handle_.find_value(handle.value);
  if (held != nullptr) {
    i = *held;
  } else {
    if (record.source == nullptr) return {};  // not ours to make from a projection
    i = ids_.size();
    ids_.push_back(record.entity);
    handles_.push_back(handle);
    variations_.push_back(Variation{});
    offsets_.push_back(0);
    for (u32 r = 0; r < 4; ++r)
      places_of_.push_back(k_no_place);
    fallback_.push_back(WorldPos{});
    points_.push_back(RoutinePoint{});
    timers_.push_back(sim::TimerHandle{});
    drawn_.push_back(WorldPos{});
    importance_.push_back(1.0f);
    const bool observed = observers_ != nullptr && !observers_->empty();
    tier_.push_back(observed ? u8{2} : (tier < 2 ? tier : u8{2}));
    by_handle_.insert(handle.value, i);
    // Held from now: its own timer runs it, and a watch is not needed any more.
    if (const u32* slot = schedule_by_id_.find_value(record.entity)) {
      schedule_[*slot].held = true;
      unwatch(*slot);
    }
  }
  // What the document says, before we move it: where the record is, as the entity store wrote it.
  if (const world::Transform* t = e.try_get<world::Transform>()) fallback_[i] = t->position;
  variations_[i] = draw_variation(routine->routine, config_.world_seed, record.entity);
  offsets_[i] = routine->clock_offset;
  resolve_places(i, *routine);
  // Where the routine has it now: the closed form at the scheduler's time, whenever and however
  // often this record is materialized (13 §13.2: the same shift schedule every time).
  const i64 now = scheduler_->game_time().us;
  move_to(i, point_at(i, now), now);
  ++stats_.materialized;
  return handle;
}

void NpcSystem::dematerialize(sim::EntityHandle entity) {
  const u32* held = by_handle_.find_value(entity.value);
  if (held == nullptr) return;
  const Id128 id = ids_[*held];
  remove(*held);
  ++stats_.dematerialized;
  // Let go — its tile went, or its routine took it into one that is not live — but its routine goes
  // on: an arrival if it has it in a live tile already, watched while it visits one.
  if (const u32* slot = schedule_by_id_.find_value(id)) {
    schedule_[*slot].held = false;
    if (!live_tiles_.empty() && scheduler_ != nullptr)
      look_again(*slot, scheduler_->game_time().us);
  }
}

void NpcSystem::remove(u32 i) {
  if (scheduler_ != nullptr && timers_[i].valid()) scheduler_->wheel().cancel(timers_[i]);
  by_handle_.erase(handles_[i].value);
  const u32 last = ids_.size() - 1u;
  if (i != last) {
    ids_[i] = ids_[last];
    handles_[i] = handles_[last];
    variations_[i] = variations_[last];
    offsets_[i] = offsets_[last];
    for (u32 r = 0; r < 4; ++r)
      places_of_[i * 4 + r] = places_of_[last * 4 + r];
    fallback_[i] = fallback_[last];
    points_[i] = points_[last];
    timers_[i] = timers_[last];
    drawn_[i] = drawn_[last];
    importance_[i] = importance_[last];
    tier_[i] = tier_[last];
    *by_handle_.find_value(handles_[i].value) = i;
  }
  ids_.pop_back();
  handles_.pop_back();
  variations_.pop_back();
  offsets_.pop_back();
  for (u32 r = 0; r < 4; ++r)
    places_of_.pop_back();
  fallback_.pop_back();
  points_.pop_back();
  timers_.pop_back();
  drawn_.pop_back();
  importance_.pop_back();
  tier_.pop_back();
}

void NpcSystem::set_tier(sim::EntityHandle entity, u8 to) {
  const u32* held = by_handle_.find_value(entity.value);
  if (held == nullptr) return;
  const u32 i = *held;
  // The tier pass has already written `tier_` (it is the array it assigned); a caller that is not
  // the tier pass — reconciliation's promotion — has not, and this is where it lands.
  tier_[i] = to < 2 ? to : u8{2};
  // A tier decides only where a trip is drawn: at LOD2 at its destination, nearer along it.
  drawn_[i] = drawn_at(i, scheduler_ != nullptr ? scheduler_->game_time().us : 0);
  write_components(i);
}

void NpcSystem::resolve_places(u32 i, const NpcRoutine& routine) noexcept {
  places_of_[i * 4 + role_index(PlaceRole::Home)] = places_.find(routine.home);
  places_of_[i * 4 + role_index(PlaceRole::Work)] = places_.find(routine.job);
  places_of_[i * 4 + role_index(PlaceRole::Service)] = places_.find(routine.service);
  places_of_[i * 4 + role_index(PlaceRole::Leisure)] = places_.find(routine.leisure);
}

u32 NpcSystem::refresh_places(const doc::Document& document) {
  const u32 count = places_.refresh(document);
  refresh_schedule(document);
  if (places_.generation() == places_generation_) return count;
  places_generation_ = places_.generation();
  if (sim_ == nullptr) return count;
  for (u32 i = 0; i < ids_.size(); ++i) {
    const flecs::entity e = ecs::entity_of(sim_->world(), handles_[i]);
    const NpcRoutine* routine = e.is_valid() ? e.try_get<NpcRoutine>() : nullptr;
    if (routine == nullptr) continue;
    // A place that moved moves the residents at it: the anchor is the place's position.
    resolve_places(i, *routine);
    drawn_[i] = drawn_at(i, scheduler_ != nullptr ? scheduler_->game_time().us : 0);
    write_components(i);
  }
  return count;
}

// ---- the routine on the wheel ------------------------------------------------------------------

RoutinePoint NpcSystem::point_at(u32 i, i64 t_us) const noexcept {
  return routine_at_offset(variations_[i], t_us, offsets_[i]);
}

WorldPos NpcSystem::place_position(u32 i, PlaceRole role) const noexcept {
  const u32 place = places_of_[i * 4 + role_index(role)];
  return place != k_no_place ? places_.position(place) : fallback_[i];
}

WorldPos NpcSystem::position_at(u32 i, i64 t_us) const noexcept {
  const RoutinePoint& p = points_[i];
  const WorldPos anchor = place_position(i, p.place);
  if (p.state != ResidentState::Travelling || p.end_us <= p.start_us) return anchor;
  const WorldPos from = place_position(i, p.from);
  i64 elapsed = t_us - p.start_us;
  if (elapsed < 0) elapsed = 0;
  const f64 f = static_cast<f64>(elapsed) / static_cast<f64>(p.end_us - p.start_us);
  // In f64 along the segment between two world positions (ADR-0053): a trip 420 km out is drawn
  // where it is, not on float32's 3.1 cm grid.
  return lerp(from, anchor, f < 1.0 ? f : 1.0);
}

WorldPos NpcSystem::drawn_at(u32 i, i64 t_us) const noexcept {
  // Near, a trip is drawn where it is; at LOD2 at its destination, which is where the record is.
  return tier_[i] <= 1 ? position_at(i, t_us) : place_position(i, points_[i].place);
}

void NpcSystem::write_components(u32 i) {
  if (sim_ == nullptr) return;
  const flecs::entity e = ecs::entity_of(sim_->world(), handles_[i]);
  if (!e.is_valid()) return;
  const RoutinePoint& p = points_[i];
  if (NpcState* state = e.try_get_mut<NpcState>()) {
    state->state = p.state;
    state->next_event = p.end_us;
    state->anchor = place_position(i, p.place);
  }
  if (world::Transform* transform = e.try_get_mut<world::Transform>())
    transform->position = drawn_[i];
}

void NpcSystem::move_to(u32 i, const RoutinePoint& point, i64 t_us) {
  points_[i] = point;
  drawn_[i] = drawn_at(i, t_us);
  write_components(i);
  if (scheduler_ == nullptr) return;
  sim::TimingWheel& wheel = scheduler_->wheel();
  if (timers_[i].valid()) wheel.cancel(timers_[i]);
  sim::TimerPayload payload;
  payload.subject = handles_[i].value;
  payload.kind = k_timer_kind;
  timers_[i] = wheel.schedule(GameTime{point.end_us}, payload);
}

void NpcSystem::deliver(void* context, const sim::TimerEvent& event) {
  NpcSystem* self = static_cast<NpcSystem*>(context);
  if (event.payload.kind == k_watch_kind) {
    self->deliver_watch(event);
    return;
  }
  if (event.payload.kind != k_timer_kind) {
    self->next_(event);
    return;
  }
  const u32* held = self->by_handle_.find_value(event.payload.subject);
  if (held == nullptr) return;
  const u32 i = *held;
  if (!(self->timers_[i] == event.handle)) return;  // re-armed since: a stale firing
  // The timer is spent; the next one is armed by move_to.
  self->timers_[i] = sim::TimerHandle{};
  // The row that starts when the last one ends: the closed form at the event's own time, not at
  // the wheel's position, so a fast-forward that delivers several transitions in one advance
  // stamps each with its own instant.
  self->move_to(i, self->point_at(i, event.at.us), event.at.us);
  ++self->stats_.transitions;
}

void NpcSystem::summarize_fn(void* context, const sim::SummarizeInterval& interval) {
  static_cast<NpcSystem*>(context)->summarize(interval.from, interval.to);
}

u64 NpcSystem::summarize(GameTime /*from*/, GameTime to) {
  // sim.md's five conditions (docs/subsystems/npc.md, "The summarizer"): the closed form at `to`
  // is what executing every transition in the gap would have reached (1, exactly); it reads `to`,
  // the resident's routine and variation and nothing else (2); where a resident is at c does not
  // depend on where it was summarized to before (3); it writes the resident's own state and arms
  // one timer past `to`, and delivers nothing (4); it visits each resident once, whatever the gap
  // (5). A resident whose row already runs past `to` is left alone: its timer is the next event.
  ++stats_.summaries;
  u64 visited = 0;
  for (u32 i = 0; i < ids_.size(); ++i) {
    ++visited;
    if (points_[i].end_us > to.us) continue;
    move_to(i, point_at(i, to.us), to.us);
    ++stats_.summarized;
  }
  stats_.summary_visits += visited;
  // A watched resident's timer is a one-shot, which the wheel delivers through a summary rather
  // than coarsen; left alone it would fire at every transition of the gap. Its watch is looked at
  // once, at `to`, as its own transitions are: the same answer the firings would have reached at
  // the end.
  for (u32 slot = 0; slot < schedule_.size(); ++slot) {
    Scheduled& s = schedule_[slot];
    if (!s.watch.valid() || s.held) continue;
    GameTime due;
    if (scheduler_ != nullptr && scheduler_->wheel().due_time(s.watch, due) && due.us > to.us)
      continue;
    unwatch(slot);
    look_again(slot, to.us);
  }
  return visited;
}

sim::FastForwardResult NpcSystem::fast_forward(GameTime to, u64 budget) {
  sim::FastForwardResult none;
  if (scheduler_ == nullptr) return none;
  sim::TimingWheel& wheel = scheduler_->wheel();
  return wheel.advance(wheel.now(), to, budget != 0 ? budget : config_.fast_forward_budget, sink());
}

// ---- the two table systems ---------------------------------------------------------------------

void NpcSystem::lod_tick(sim::SystemContext& context, sim::Batch /*batch*/) {
  NpcSystem* self = static_cast<NpcSystem*>(context.context);
  if (self->observers_ == nullptr || self->observers_->empty() || self->ids_.empty()) return;
  if (context.tick.value % self->config_.lod_every != 0) return;
  // Scored on where each resident is, which no tier changes: scored on where it is drawn, a trip
  // near a boundary is promoted (drawn along its segment, farther), demoted (drawn at its
  // destination, nearer) and promoted again on every pass.
  const i64 now = context.time.us;
  self->scored_.clear();
  for (u32 i = 0; i < self->ids_.size(); ++i)
    self->scored_.push_back(self->position_at(i, now));
  sim::TierInput input;
  input.positions = std::span<const WorldPos>(self->scored_.data(), self->scored_.size());
  input.importance = std::span<const f32>(self->importance_.data(), self->importance_.size());
  input.tiers = std::span<u8>(self->tier_.data(), self->tier_.size());
  // `assign_tiers` appends: a list not cleared is every change ever made, applied again each pass.
  self->changes_.clear();
  const sim::TierStats stats =
      self->tiers_.assign_tiers(input, *self->observers_, self->tier_params_, self->changes_);
  ++self->stats_.lod_runs;
  self->stats_.promotions += stats.promoted;
  self->stats_.demotions += stats.demoted;
  if (self->changes_.empty()) return;
  // Through the scheduler, so every capability's hooks see the change — ours redraws the trip.
  self->scheduler_->apply_tier_changes(
      std::span<const sim::TierChange>(self->changes_.data(), self->changes_.size()),
      std::span<const sim::EntityHandle>(self->handles_.data(), self->handles_.size()));
}

void NpcSystem::motion_tick(sim::SystemContext& context, sim::Batch /*batch*/) {
  NpcSystem* self = static_cast<NpcSystem*>(context.context);
  if (self->ids_.empty() || context.tick.value % self->config_.motion_every != 0) return;
  const i64 now = context.time.us;
  for (u32 i = 0; i < self->ids_.size(); ++i) {
    if (self->tier_[i] > 1 || self->points_[i].state != ResidentState::Travelling) continue;
    self->drawn_[i] = self->drawn_at(i, now);
    const flecs::entity e = ecs::entity_of(self->sim_->world(), self->handles_[i]);
    if (!e.is_valid()) continue;
    if (world::Transform* transform = e.try_get_mut<world::Transform>())
      transform->position = self->drawn_[i];
    ++self->stats_.moved;
  }
}

// ---- queries -----------------------------------------------------------------------------------

bool NpcSystem::find(const Id128& id, ResidentView& out) const {
  for (u32 i = 0; i < ids_.size(); ++i) {
    if (!(ids_[i] == id)) continue;
    out.id = id;
    out.entity = handles_[i];
    out.point = points_[i];
    out.anchor = place_position(i, points_[i].place);
    out.drawn = drawn_[i];
    out.tier = tier_[i];
    out.timer_live = scheduler_ != nullptr && scheduler_->wheel().is_live(timers_[i]);
    return true;
  }
  return false;
}

void NpcSystem::tier_counts(u32 (&out)[4]) const noexcept {
  out[0] = out[1] = out[2] = out[3] = 0;
  for (u32 i = 0; i < tier_.size(); ++i)
    ++out[tier_[i] < 4 ? tier_[i] : 3];
}

u64 NpcSystem::bytes_held() const noexcept {
  u64 bytes = 0;
  bytes += u64{ids_.capacity()} * sizeof(Id128);
  bytes += u64{handles_.capacity()} * sizeof(sim::EntityHandle);
  bytes += u64{variations_.capacity()} * sizeof(Variation);
  bytes += u64{offsets_.capacity()} * sizeof(i64);
  bytes += u64{places_of_.capacity()} * sizeof(u32);
  bytes += u64{fallback_.capacity()} * sizeof(WorldPos);
  bytes += u64{points_.capacity()} * sizeof(RoutinePoint);
  bytes += u64{timers_.capacity()} * sizeof(sim::TimerHandle);
  bytes += u64{drawn_.capacity()} * sizeof(WorldPos);
  bytes += u64{scored_.capacity()} * sizeof(WorldPos);
  bytes += u64{importance_.capacity()} * sizeof(f32);
  bytes += u64{tier_.capacity()};
  // The handle index: a key, a value and a bucket per entry, at its capacity.
  bytes += u64{by_handle_.capacity()} * (sizeof(u64) + sizeof(u32)) +
           u64{by_handle_.bucket_count()} * sizeof(u32);
  return bytes;
}

// ---- the schedule index ------------------------------------------------------------------------

sim::TileSource NpcSystem::tile_source() noexcept {
  sim::TileSource source;
  source.context = this;
  source.name = "npc";
  source.where = &NpcSystem::where_fn;
  source.tile_in = &NpcSystem::tile_in_fn;
  source.tile_out = &NpcSystem::tile_out_fn;
  source.arrivals = &NpcSystem::arrivals_fn;
  return source;
}

u64 NpcSystem::bytes_scheduled() const noexcept {
  u64 bytes = u64{schedule_.capacity()} * sizeof(Scheduled);
  bytes += u64{schedule_by_id_.capacity()} * (sizeof(Id128) + sizeof(u32)) +
           u64{schedule_by_id_.bucket_count()} * sizeof(u32);
  bytes += u64{schedule_by_tile_.capacity()} * (sizeof(u64) + sizeof(Vector<u32>)) +
           u64{schedule_by_tile_.bucket_count()} * sizeof(u32);
  for (const Vector<u32>& list : schedule_by_tile_.values())
    bytes += u64{list.capacity()} * sizeof(u32);
  bytes += u64{place_tiles_.capacity()} * sizeof(u64) + u64{place_tiled_.capacity()};
  return bytes;
}

u32 NpcSystem::watching() const noexcept {
  u32 n = 0;
  for (const Scheduled& s : schedule_)
    n += s.live && !s.held && s.watch.valid() ? 1u : 0u;
  return n;
}

void NpcSystem::visiting(doc::TileCoord tile, Vector<Id128>& out) const {
  out.clear();
  const Vector<u32>* slots = schedule_by_tile_.find_value(tile_key(tile));
  if (slots == nullptr) return;
  for (const u32 slot : *slots)
    out.push_back(schedule_[slot].id);
  std::sort(out.begin(), out.end());
}

bool NpcSystem::place_tile(u32 place, f64 tile_size, u64& out) {
  if (place_tiles_size_ != tile_size || place_tiles_generation_ != places_.generation()) {
    // Once per grid and per change of the place index: the tile of every place, by the function the
    // partition files a record by, fed the position exactly as the write-back will write it.
    place_tiles_.clear();
    place_tiled_.clear();
    place_tiles_size_ = tile_size;
    place_tiles_generation_ = places_.generation();
    for (u32 p = 0; p < places_.size(); ++p) {
      const WorldPos at = places_.position(p);
      JsonValue position = JsonValue::array();
      position.push_back(JsonValue(at.x));
      position.push_back(JsonValue(at.y));
      position.push_back(JsonValue(at.z));
      doc::TileCoord tile;
      const bool tiled = doc::tile_of_position(position, tile_size, tile);
      place_tiles_.push_back(tiled ? tile_key(tile) : 0u);
      place_tiled_.push_back(tiled ? u8{1} : u8{0});
    }
  }
  if (place >= place_tiles_.size() || place_tiled_[place] == 0) return false;
  out = place_tiles_[place];
  return true;
}

bool NpcSystem::read_scheduled(const doc::Document& document, const Id128& id, Scheduled& out) {
  if (!document.exists(id) || document.type_of(id) != k_resident_type) return false;
  // The composed values of the six properties the closed form and the places need, in one visit
  // of the layers that hold the record, and the defining record's layer, whose grid is the one the
  // resident's record is filed on.
  static constexpr std::string_view k_keys[6] = {"home",    "job",     "service",
                                                 "leisure", "routine", "clock_offset"};
  const JsonValue* values[6] = {};
  u32 defining = ~0u;
  document.visit_records(id, [&](u32 layer, const doc::ObjectRecord& record) {
    if (!record.type.empty()) defining = layer;
    for (u32 k = 0; k < 6; ++k) {
      if (const JsonValue* v = record.properties.find_value(k_keys[k])) values[k] = v;
    }
  });
  if (defining == ~0u) return false;
  const doc::Layer& layer = document.layer(defining);
  // A resident on no grid is never in a tile, and one on a grid that reads another property than
  // the one the anchor is written to is not moved between tiles by its routine: neither is ours to
  // place.
  if (!layer.partitioned()) return false;
  if (doc::position_property(k_resident_type, layer.partition()) != "position") return false;
  out = Scheduled{};
  out.id = id;
  out.live = true;
  if (values[4] != nullptr) {
    schema::ReadContext ctx;
    Routine routine = Routine::Idle;
    if (schema::from_json(routine, *values[4], ctx)) out.routine = routine;
  }
  if (values[5] != nullptr) (void)values[5]->get_i64(out.clock_offset);
  for (u32 r = 0; r < 4; ++r) {
    Id128 place;
    if (!read_id(values[r], place) || place.is_null()) continue;
    const u32 index = places_.find(place);
    u64 key = 0;
    // A place the index does not know is where a held resident falls back to its record's own
    // position: the document's tile, which is no placement of ours.
    if (index == k_no_place || !place_tile(index, layer.partition().tile_size, key)) continue;
    out.tiles[r] = key;
    out.tiled = static_cast<u8>(out.tiled | (1u << r));
  }
  return true;
}

void NpcSystem::link_tiles(u32 slot) {
  const Scheduled& s = schedule_[slot];
  for (u32 r = 0; r < 4; ++r) {
    if (((s.tiled >> r) & 1u) == 0) continue;
    bool seen = false;
    for (u32 q = 0; q < r; ++q)
      seen = seen || (((s.tiled >> q) & 1u) != 0 && s.tiles[q] == s.tiles[r]);
    if (seen) continue;  // two places in one tile: the resident is listed there once
    Vector<u32>& list = schedule_by_tile_[s.tiles[r]];
    const auto at = std::lower_bound(list.begin(), list.end(), slot);
    list.emplace(static_cast<u32>(at - list.begin()), slot);
  }
}

void NpcSystem::unlink_tiles(u32 slot) {
  const Scheduled& s = schedule_[slot];
  for (u32 r = 0; r < 4; ++r) {
    if (((s.tiled >> r) & 1u) == 0) continue;
    Vector<u32>* list = schedule_by_tile_.find_value(s.tiles[r]);
    if (list == nullptr) continue;
    const auto at = std::lower_bound(list->begin(), list->end(), slot);
    const u32 pos = static_cast<u32>(at - list->begin());
    if (pos < list->size() && (*list)[pos] == slot) list->erase_at(pos);
    if (list->empty()) schedule_by_tile_.erase(s.tiles[r]);
  }
}

void NpcSystem::build_schedule(const doc::Document& document) {
  const i64 start = time::monotonic_ns();
  // The slots change, so every watch's payload would name the wrong resident: all go, and the ones
  // still wanted are armed again below.
  for (u32 slot = 0; slot < schedule_.size(); ++slot)
    unwatch(slot);
  schedule_.clear();
  schedule_by_id_.clear();
  schedule_by_tile_.clear();
  // `objects()` is in id order, so slots are, and every tile's list comes out sorted as it is
  // built.
  for (const Id128& id : document.objects()) {
    Scheduled s;
    if (!read_scheduled(document, id, s)) continue;
    const u32 slot = schedule_.size();
    schedule_.push_back(s);
    schedule_by_id_.insert(id, slot);
    link_tiles(slot);
  }
  for (const Id128& id : ids_) {
    if (const u32* slot = schedule_by_id_.find_value(id)) schedule_[*slot].held = true;
  }
  if (!live_tiles_.empty() && scheduler_ != nullptr) {
    const i64 now = scheduler_->game_time().us;
    for (u32 slot = 0; slot < schedule_.size(); ++slot) {
      if (!schedule_[slot].held) look_again(slot, now);
    }
  }
  ++schedule_stats_.builds;
  schedule_stats_.build_ms = static_cast<f64>(time::monotonic_ns() - start) / 1.0e6;
}

void NpcSystem::update_scheduled(const doc::Document& document, const Id128& id) {
  Scheduled fresh;
  const bool keep = read_scheduled(document, id, fresh);
  const u32* at = schedule_by_id_.find_value(id);
  ++schedule_stats_.updates;
  if (at == nullptr) {
    if (!keep) return;
    const u32 slot = schedule_.size();
    schedule_.push_back(fresh);
    schedule_by_id_.insert(id, slot);
    link_tiles(slot);
    return;
  }
  const u32 slot = *at;
  Scheduled& s = schedule_[slot];
  if (!keep) {
    // Gone from the document, or no longer a resident on a grid: its slot stays, dead, until the
    // next whole build, because a watch may still name it.
    unwatch(slot);
    unlink_tiles(slot);
    s.live = false;
    s.tiled = 0;
    schedule_by_id_.erase(id);
    return;
  }
  const bool same = s.routine == fresh.routine && s.clock_offset == fresh.clock_offset &&
                    s.tiled == fresh.tiled &&
                    std::equal(std::begin(s.tiles), std::end(s.tiles), std::begin(fresh.tiles));
  if (same) return;  // a write-back of its state, which moves nothing the index keeps
  unlink_tiles(slot);
  s.routine = fresh.routine;
  s.clock_offset = fresh.clock_offset;
  s.tiled = fresh.tiled;
  std::copy(std::begin(fresh.tiles), std::end(fresh.tiles), std::begin(s.tiles));
  link_tiles(slot);
}

u32 NpcSystem::refresh_schedule(const doc::Document& document) {
  places_.refresh(document);
  const bool whole = !schedule_built_ || schedule_document_ != &document ||
                     schedule_places_generation_ != places_.generation();
  if (!whole && document.revision() == schedule_revision_) return scheduled();
  Vector<Id128> changed;
  if (whole || !document.changed_since(schedule_revision_, changed)) {
    build_schedule(document);
  } else {
    for (const Id128& id : changed)
      update_scheduled(document, id);
  }
  schedule_built_ = true;
  schedule_document_ = &document;
  schedule_revision_ = document.revision();
  schedule_places_generation_ = places_.generation();
  return scheduled();
}

RoutinePoint NpcSystem::scheduled_point(const Scheduled& s, i64 t_us) const noexcept {
  return routine_at_offset(draw_variation(s.routine, config_.world_seed, s.id), t_us,
                           s.clock_offset);
}

bool NpcSystem::anchor_tile(const Scheduled& s, const RoutinePoint& point, u64& out) noexcept {
  const u32 r = role_index(point.place);
  if (((s.tiled >> r) & 1u) == 0) return false;
  out = s.tiles[r];
  return true;
}

bool NpcSystem::visits_live(const Scheduled& s) const noexcept {
  for (u32 r = 0; r < 4; ++r) {
    if (((s.tiled >> r) & 1u) != 0 && live_tiles_.contains(s.tiles[r])) return true;
  }
  return false;
}

void NpcSystem::watch(u32 slot, i64 t_us) {
  Scheduled& s = schedule_[slot];
  if (s.watch.valid() || scheduler_ == nullptr) return;
  sim::TimerPayload payload;
  payload.subject = slot;
  payload.kind = k_watch_kind;
  s.watch = scheduler_->wheel().schedule(GameTime{scheduled_point(s, t_us).end_us}, payload);
  ++schedule_stats_.watches;
}

void NpcSystem::unwatch(u32 slot) {
  Scheduled& s = schedule_[slot];
  if (s.watch.valid() && scheduler_ != nullptr) scheduler_->wheel().cancel(s.watch);
  s.watch = sim::TimerHandle{};
}

void NpcSystem::look_again(u32 slot, i64 t_us) {
  const Scheduled& s = schedule_[slot];
  if (!s.live || s.held) return;
  const RoutinePoint point = scheduled_point(s, t_us);
  u64 at = 0;
  if (anchor_tile(s, point, at) && live_tiles_.contains(at)) {
    arrivals_.push_back(s.id);
    ++schedule_stats_.arrivals;
  }
  // Watched either way while its routine visits a live tile: an arrival the driver could not take
  // (its tile went first) is looked at again at the next transition, and one it took is held, and
  // the materialization hook cancels the watch.
  if (visits_live(s)) watch(slot, t_us);
}

void NpcSystem::deliver_watch(const sim::TimerEvent& event) {
  const u64 slot = event.payload.subject;
  if (slot >= schedule_.size()) return;
  Scheduled& s = schedule_[static_cast<u32>(slot)];
  if (!(s.watch == event.handle)) return;  // re-armed or cancelled since: a stale firing
  s.watch = sim::TimerHandle{};
  ++schedule_stats_.wakes;
  // At the timer's own time, as a transition is: the row that starts when the watched one ends.
  look_again(static_cast<u32>(slot), event.at.us);
}

bool NpcSystem::where_fn(void* context, const doc::Document&, const Id128& id,
                         doc::TileCoord& out) {
  auto* self = static_cast<NpcSystem*>(context);
  const u32* slot = self->schedule_by_id_.find_value(id);
  if (slot == nullptr || self->scheduler_ == nullptr) return false;
  const Scheduled& s = self->schedule_[*slot];
  u64 at = 0;
  if (!anchor_tile(s, self->scheduled_point(s, self->scheduler_->game_time().us), at)) return false;
  out = tile_of_key(at);
  return true;
}

void NpcSystem::tile_in_fn(void* context, const doc::Document& document, doc::TileCoord tile,
                           Vector<Id128>& out) {
  auto* self = static_cast<NpcSystem*>(context);
  // A host that never refreshed the places before its first tile: built here, once. After that the
  // index follows the document at every `refresh_places`, which a host calls at the start of every
  // call; within one, the document changes only by write-backs, which move nothing it keeps.
  if (!self->schedule_built_ || self->schedule_document_ != &document)
    self->refresh_schedule(document);
  const u64 key = tile_key(tile);
  self->live_tiles_.insert(key);
  ++self->schedule_stats_.tile_passes;
  const Vector<u32>* slots = self->schedule_by_tile_.find_value(key);
  if (slots == nullptr || self->scheduler_ == nullptr) return;
  const i64 now = self->scheduler_->game_time().us;
  for (const u32 slot : *slots) {
    const Scheduled& s = self->schedule_[slot];
    if (!s.live) continue;
    ++self->schedule_stats_.visits;
    const RoutinePoint point = self->scheduled_point(s, now);
    u64 at = 0;
    const bool placed = anchor_tile(s, point, at);
    if (placed && at == key) {
      // In this tile now, wherever its record is: the pass brings it in, or files it here.
      out.push_back(s.id);
      doc::TileCoord filed;
      if (!document.object_tile(s.id, filed) || tile_key(filed) != key)
        ++self->schedule_stats_.placed;
      continue;
    }
    if (s.held) continue;
    // Elsewhere now: an arrival if that is live too (it came in with a tile it was not in then),
    // and watched, since its routine visits this one.
    self->look_again(slot, now);
  }
}

void NpcSystem::tile_out_fn(void* context, doc::TileCoord tile) {
  // A watch on a resident whose routine visited only this tile is let lapse at its next firing,
  // which finds nothing live, rather than looked for here.
  static_cast<NpcSystem*>(context)->live_tiles_.erase(tile_key(tile));
}

void NpcSystem::arrivals_fn(void* context, Vector<Id128>& out) {
  auto* self = static_cast<NpcSystem*>(context);
  for (const Id128& id : self->arrivals_)
    out.push_back(id);
  self->arrivals_.clear();
}

}  // namespace engine::npc
