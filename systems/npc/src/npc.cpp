#include <core/log/log.h>
#include <core/schema/materialize.h>
#include <domain/ecs/components.h>
#include <domain/ecs/identity.h>
#include <domain/sim/materialize.h>
#include <foundation/tunables/tunables.h>
#include <systems/npc/npc.h>

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

bool read_vec3(const JsonValue* value, Vec3& out) {
  if (value == nullptr || !value->is_array() || value->size() < 3) return false;
  f64 c[3] = {};
  for (usize i = 0; i < 3; ++i) {
    if (!(*value)[i].get_f64(c[i])) return false;
  }
  out = Vec3{static_cast<f32>(c[0]), static_cast<f32>(c[1]), static_cast<f32>(c[2])};
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

void PlaceIndex::add(const Id128& id, Vec3 position, PlaceRole role) {
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
  Vector<Vec3> positions;
  Vector<PlaceRole> roles;
  for (const Id128& id : document.objects()) {
    if (document.type_of(id) != k_place_type) continue;
    Vec3 position;
    if (!read_vec3(document.property(id, "position"), position)) position = Vec3{};
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
    fallback_.push_back(Vec3{});
    points_.push_back(RoutinePoint{});
    timers_.push_back(sim::TimerHandle{});
    drawn_.push_back(Vec3{});
    importance_.push_back(1.0f);
    const bool observed = observers_ != nullptr && !observers_->empty();
    tier_.push_back(observed ? u8{2} : (tier < 2 ? tier : u8{2}));
    by_handle_.insert(handle.value, i);
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
  remove(*held);
  ++stats_.dematerialized;
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

Vec3 NpcSystem::place_position(u32 i, PlaceRole role) const noexcept {
  const u32 place = places_of_[i * 4 + role_index(role)];
  return place != k_no_place ? places_.position(place) : fallback_[i];
}

Vec3 NpcSystem::position_at(u32 i, i64 t_us) const noexcept {
  const RoutinePoint& p = points_[i];
  const Vec3 anchor = place_position(i, p.place);
  if (p.state != ResidentState::Travelling || p.end_us <= p.start_us) return anchor;
  const Vec3 from = place_position(i, p.from);
  i64 elapsed = t_us - p.start_us;
  if (elapsed < 0) elapsed = 0;
  const f32 f =
      static_cast<f32>(static_cast<f64>(elapsed) / static_cast<f64>(p.end_us - p.start_us));
  const f32 k = f < 1.0f ? f : 1.0f;
  return Vec3{from.x + (anchor.x - from.x) * k, from.y + (anchor.y - from.y) * k,
              from.z + (anchor.z - from.z) * k};
}

Vec3 NpcSystem::drawn_at(u32 i, i64 t_us) const noexcept {
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
  input.positions = std::span<const Vec3>(self->scored_.data(), self->scored_.size());
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
  bytes += u64{fallback_.capacity()} * sizeof(Vec3);
  bytes += u64{points_.capacity()} * sizeof(RoutinePoint);
  bytes += u64{timers_.capacity()} * sizeof(sim::TimerHandle);
  bytes += u64{drawn_.capacity()} * sizeof(Vec3);
  bytes += u64{scored_.capacity()} * sizeof(Vec3);
  bytes += u64{importance_.capacity()} * sizeof(f32);
  bytes += u64{tier_.capacity()};
  // The handle index: a key, a value and a bucket per entry, at its capacity.
  bytes += u64{by_handle_.capacity()} * (sizeof(u64) + sizeof(u32)) +
           u64{by_handle_.bucket_count()} * sizeof(u32);
  return bytes;
}

}  // namespace engine::npc
