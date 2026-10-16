#include <core/log/log.h>
#include <core/time/time.h>
#include <domain/anim/skeleton.h>
#include <domain/ecs/components.h>
#include <domain/ecs/identity.h>
#include <domain/ecs/systems.h>
#include <domain/sim/materialize.h>
#include <systems/animation/animation.h>
#include <systems/muscle/supervisor.h>
#include <systems/muscle_sim/muscle_sim.h>

#include <cmath>
#include <cstring>
#include <limits>
#include <schemas/animation.h>
#include <schemas/muscle_ecs.h>
#include <string_view>

namespace engine::muscle_sim {

ENGINE_LOG_CATEGORY_DEFINE(log_muscle_sim, "muscle_sim");

namespace {

constexpr std::string_view k_record_type = "engine.muscle.LineActuator";

bool animation_find_joint(void* context, flecs::entity character, std::string_view bone,
                          u32& joint) {
  const auto* system = static_cast<const animation::AnimationSystem*>(context);
  if (!character.is_valid() || !character.has<animation::SkeletonInstance>()) return false;
  const animation::SkeletonInstance& instance = character.get<animation::SkeletonInstance>();
  if (instance.skeleton_index >= system->library().skeleton_count()) return false;
  const i32 found = system->library().skeleton_data(instance.skeleton_index).find(bone);
  if (found < 0) return false;
  joint = static_cast<u32>(found);
  return true;
}

bool animation_model_transforms(void* context, flecs::entity character, std::span<Mat4> out,
                                u32& joints) {
  const auto* system = static_cast<const animation::AnimationSystem*>(context);
  if (!character.is_valid() || !character.has<animation::SkeletonInstance>()) return false;
  const animation::SkeletonInstance& instance = character.get<animation::SkeletonInstance>();
  if (instance.pose_slot == animation::k_no_slot || instance.joints > out.size() ||
      instance.skeleton_index >= system->library().skeleton_count())
    return false;
  const anim::Skeleton& skeleton = system->library().skeleton_data(instance.skeleton_index);
  anim::local_to_model(skeleton, system->poses().pose(instance.pose_slot),
                       out.first(instance.joints));
  joints = instance.joints;
  return true;
}

}  // namespace

PoseSource animation_pose_source(animation::AnimationSystem& animation) noexcept {
  PoseSource source;
  source.context = &animation;
  source.find_joint = animation_find_joint;
  source.model_transforms = animation_model_transforms;
  source.reads_resources = sim::resource_mask(animation::k_resource_pose_pool);
  return source;
}

MuscleSimSystem::MuscleSimSystem(const PoseSource& poses, const MuscleSimConfig& config)
    : poses_(poses), config_(config) {
  // Every per-actuator array is sized here, once: the tick and materialization never grow them.
  const u32 n = config_.capacity;
  store_.reserve(n);
  slots_.reserve(n);
  live_.resize(n, 0);
  resolved_.resize(n, 0);
  record_.resize(n, Id128{});
  character_.resize(n, 0);
  route_.resize(static_cast<usize>(n) * k_max_route_points, RoutePoint{});
  route_points_.resize(n, 0);
  history_.resize(n, PathHistory{});
  excitation_.resize(n, 0.0);
  length_.resize(n, 0.0);
  path_.resize(n, muscle::HermitePath{0.0, 0.0, 0.0, 0.0});
  output_.resize(n, nullptr);
  free_.reserve(n);
  order_.reserve(n);
  events_.resize(static_cast<usize>(n) * muscle::k_event_slots, muscle::ValidationEvent{});
  model_.resize(config_.max_joints, Mat4::identity());
}

void MuscleSimSystem::install(ecs::SimWorld& sim) {
  flecs::world& world = sim.world();
  world_ = &world;
  // Seam 1: the components systems/muscle's schema declares, through the header schemac generated.
  muscle::register_muscle_components(world);

  desc_ = sim::SystemDesc{};
  desc_.name = "muscle_sim.tick";
  desc_.phase = k_phase;
  desc_.reads = ecs::mask_of<muscle::LineActuatorSpec, muscle::LineExcitation>(world);
  desc_.writes = ecs::mask_of<muscle::LineOutput>(world);
  desc_.reads_resources = poses_.reads_resources;
  desc_.tiers = k_lod_tiers;
  desc_.determinism = sim::Determinism::Hashed;
  desc_.context = this;
  MuscleSimSystem* self = this;
  ecs::register_system(sim, desc_, [self](flecs::world& w, flecs::entity phase) {
    // Not multi-threaded: one pass over the whole store in slot order is the tick, which is what
    // makes its bytes independent of the worker count and of the entity store's table order.
    return w
        .system<const muscle::LineActuatorSpec, const muscle::LineExcitation, muscle::LineOutput>(
            "muscle_sim.tick")
        .kind(phase)
        .run([self](flecs::iter& it) { self->run(it); });
  });
  ENGINE_LOG_INFO(log_muscle_sim, "capability installed", log::field("capacity", config_.capacity));
}

sim::MaterializationHooks MuscleSimSystem::hooks() noexcept {
  sim::MaterializationHooks row;
  row.name = "muscle_sim";
  row.context = this;
  row.tiers = k_lod_tiers;
  row.materialize = [](void* context, const sim::EntityRecord& record, u8) {
    return static_cast<MuscleSimSystem*>(context)->materialize(record);
  };
  row.dematerialize = [](void* context, sim::EntityHandle entity) {
    static_cast<MuscleSimSystem*>(context)->dematerialize(entity);
  };
  return row;
}

u32 MuscleSimSystem::slot_of(const Id128& record) const noexcept {
  const auto found = slots_.find(record);
  return found == slots_.end() ? k_no_slot : found->second;
}

void MuscleSimSystem::refuse(const Id128& record, const muscle::ValidationResult& result,
                             const char* reason) {
  RecordRefusal r;
  r.record = record;
  r.result = result;
  r.reason = reason;
  refusals_.push_back(r);
  ++stats_.refused;
  ENGINE_LOG_WARN(log_muscle_sim, "line actuator refused",
                  log::field("code", muscle::refusal_code(result.refusal)),
                  log::field("field", result.field), log::field("value", result.value),
                  log::field("rule", result.rule), log::field("reason", reason));
}

void MuscleSimSystem::release(u32 slot) {
  live_[slot] = 0;
  resolved_[slot] = 0;
  store_.initialized[slot] = 0;
  slots_.erase(record_[slot]);
  record_[slot] = Id128{};
  output_[slot] = nullptr;
  free_.push_back(slot);
  --stats_.actuators;
}

sim::EntityHandle MuscleSimSystem::materialize(const sim::EntityRecord& record) {
  if (world_ == nullptr || record.source == nullptr || record.source->mapping == nullptr ||
      std::string_view(record.source->mapping->record->qualified_name) != k_record_type)
    return sim::EntityHandle{};
  // The entity store's hook ran first and wrote the authored components; this row decides whether
  // the entity is an actuator.
  const flecs::entity e = ecs::entity_for(*world_, record.entity);
  if (!e.is_valid() || !e.has<muscle::LineActuatorSpec>()) return sim::EntityHandle{};
  const muscle::LineActuatorSpec& spec = e.get<muscle::LineActuatorSpec>();
  const u32 held = slot_of(record.entity);

  muscle::ActuatorSpec actuator{};
  actuator.actuator_id = record.entity;
  actuator.path_id = record.entity;  // the route is the record's own
  actuator.fiber = spec.fiber;
  actuator.series = spec.series;
  muscle::ValidationResult result{};
  const char* reason = "";
  if (!muscle::parse_sha256(spec.parameter_sha256, actuator.parameter_sha256)) {
    result.refusal = muscle::Refusal::DomainUnqualified;
    result.field = "parameter_sha256";
    result.rule = "64 hex digits naming a qualified parameter set";
  } else {
    result = muscle::validate(muscle::k_model_sha256, actuator.parameter_sha256, actuator.fiber,
                              actuator.series);
  }
  if (result.ok() && 2u + spec.via.size() > k_max_route_points)
    reason = "the route has more than 8 points (the origin, the insertion, six via points)";
  if (result.ok() && *reason == '\0' && held == k_no_slot && free_.empty() &&
      store_.size() >= store_.capacity())
    reason = "the world's line actuator capacity (MuscleSimConfig::capacity) is full";
  if (!result.ok() || *reason != '\0') {
    refuse(record.entity, result, reason);
    if (held != k_no_slot) {
      release(held);
      e.remove<muscle::LineOutput>();
    }
    return sim::EntityHandle{};
  }

  if (held != k_no_slot) {
    // An update of an actuator this world holds: the same law keeps its integration state, and
    // only the route is resolved again; another parameter set starts the actuator over.
    const muscle::LawRow row = muscle::make_law_row(actuator.fiber, actuator.series);
    const bool same = actuator.parameter_sha256 == store_.parameter_sha256[held] &&
                      std::memcmp(&row, &store_.law[held], sizeof(row)) == 0;
    resolved_[held] = 0;
    if (same) return ecs::handle_of(e);
    store_.assign(held, actuator);
    history_[held] = PathHistory{};
    return ecs::handle_of(e);
  }

  u32 slot = 0;
  if (!free_.empty()) {
    slot = free_.back();
    free_.pop_back();
    store_.assign(slot, actuator);
  } else {
    store_.add(actuator, slot);
  }
  live_[slot] = 1;
  resolved_[slot] = 0;
  record_[slot] = record.entity;
  character_[slot] = 0;
  history_[slot] = PathHistory{};
  output_[slot] = nullptr;
  slots_[record.entity] = slot;
  ++stats_.actuators;
  muscle::LineOutput out{};
  out.slot = slot;
  e.set<muscle::LineOutput>(out);
  return ecs::handle_of(e);
}

void MuscleSimSystem::dematerialize(sim::EntityHandle handle) {
  if (world_ == nullptr) return;
  const flecs::entity e = ecs::entity_of(*world_, handle);
  if (!e.is_valid() || !e.has<muscle::LineOutput>()) return;
  const u32 slot = e.get<muscle::LineOutput>().slot;
  if (slot < store_.size() && live_[slot] != 0) release(slot);
  e.remove<muscle::LineOutput>();
}

bool MuscleSimSystem::resolve_route(u32 slot, flecs::entity character,
                                    const muscle::LineActuatorSpec& spec) {
  if (!character.is_valid() || poses_.find_joint == nullptr) return false;
  RoutePoint* route = route_.data() + static_cast<usize>(slot) * k_max_route_points;
  u32 n = 0;
  const auto add = [&](const muscle::LineAttachment& a) {
    u32 joint = 0;
    if (!poses_.find_joint(poses_.context, character, a.bone, joint)) return false;
    route[n++] = RoutePoint{joint, a.offset};
    return true;
  };
  if (!add(spec.origin)) return false;
  for (const muscle::LineAttachment& via : spec.via)
    if (!add(via)) return false;
  if (!add(spec.insertion)) return false;
  route_points_[slot] = static_cast<u8>(n);
  resolved_[slot] = 1;
  return true;
}

void MuscleSimSystem::run(flecs::iter& it) {
  const flecs::world world = it.world();
  const u64 tick = world.get<SimTick>().value;

  // Gather: every actuator's excitation, character and output row into its slot, so that what
  // follows walks slots in order whatever order the entity store's tables came in.
  while (it.next()) {
    auto specs = it.field<const muscle::LineActuatorSpec>(0);
    auto excitations = it.field<const muscle::LineExcitation>(1);
    auto outputs = it.field<muscle::LineOutput>(2);
    for (const auto row : it) {
      const u32 slot = outputs[row].slot;
      if (slot >= store_.size() || live_[slot] == 0) continue;
      const flecs::entity character = it.entity(row).parent();
      character_[slot] = character.id();
      excitation_[slot] = excitations[row].value;
      output_[slot] = &outputs[row];
      if (resolved_[slot] == 0) resolve_route(slot, character, specs[row]);
    }
  }

  // The routed length at this tick's end and the tick's path, slot by slot; a character's skeleton
  // is composed once for consecutive actuators that ride it (a character's muscles materialize
  // together, so they hold consecutive slots).
  stats_.ticked = 0;
  stats_.frozen = 0;
  stats_.rigid = 0;
  stats_.inert = 0;
  stats_.waiting = 0;
  order_.clear();
  flecs::entity_t posed = 0;
  bool posed_ok = false;
  u32 joints = 0;
  const double nan = std::numeric_limits<double>::quiet_NaN();
  for (u32 slot = 0; slot < store_.size(); ++slot) {
    if (live_[slot] == 0 || output_[slot] == nullptr) continue;
    double length = nan;
    if (resolved_[slot] != 0) {
      if (character_[slot] != posed) {
        posed = character_[slot];
        posed_ok = poses_.model_transforms != nullptr &&
                   poses_.model_transforms(poses_.context, flecs::entity(world.c_ptr(), posed),
                                           std::span<Mat4>(model_.data(), model_.size()), joints);
      }
      const RoutePoint* route = route_.data() + static_cast<usize>(slot) * k_max_route_points;
      bool in_range = posed_ok;
      for (u32 p = 0; p < route_points_[slot] && in_range; ++p)
        in_range = route[p].joint < joints;
      if (in_range)
        length = route_length(std::span<const Mat4>(model_.data(), joints),
                              std::span<const RoutePoint>(route, route_points_[slot]));
    }
    length_[slot] = length;
    if (store_.initialized[slot] == 0) {
      // The static start: as of the tick before, at the length routed now and the activation the
      // held excitation settles at. Until a route and a pose give a length, the actuator waits.
      const double u = excitation_[slot];
      const double a = std::isfinite(u) && u >= 0.0 && u <= 1.0 ? u : 0.0;
      muscle::Failure f{};
      if (!std::isfinite(length) ||
          !muscle::initialize_static(store_, slot, tick - 1, a, length, f)) {
        ++stats_.waiting;
        muscle::LineOutput& o = *output_[slot];
        o.tendon_force = 0.0;
        o.force_output = false;
        o.forces_stale = false;
        o.tier = muscle::LineTier::Compliant;
        o.path_length = length;
        o.failed_test = f.test;
        output_[slot] = nullptr;
        continue;
      }
      history_[slot] = PathHistory{};
    }
    path_[slot] = next_path(history_[slot], length);
    order_.push_back(slot);
  }

  // The batch: every ready actuator through the supervisor, in slot order.
  for (const u32 slot : order_) {
    if (!muscle::supervise_actuator<muscle::HermitePath>(store_, slot, tick, excitation_[slot],
                                                         path_[slot]) &&
        store_.supervisory[slot].tier != muscle::LineTier::Animation)
      ++stats_.frozen;
  }

  // The outputs and the tick's events.
  event_count_ = 0;
  for (const u32 slot : order_) {
    const muscle::Supervisory& s = store_.supervisory[slot];
    muscle::LineOutput& o = *output_[slot];
    o.tendon_force = muscle::output_force(store_, slot);
    o.force_output = muscle::has_force_output(store_, slot);
    o.forces_stale = s.forces_stale;
    o.tier = s.tier;
    o.path_length = length_[slot];
    o.failed_test = store_.outcome[slot].failed_test;
    output_[slot] = nullptr;
    for (const muscle::ValidationEvent& e : muscle::tick_events(store_, slot))
      events_[event_count_++] = e;
    ++stats_.ticked;
    if (s.tier == muscle::LineTier::Rigid) ++stats_.rigid;
    if (s.tier == muscle::LineTier::Animation) ++stats_.inert;
  }
  stats_.events = event_count_;
}

bool MuscleSimSystem::save(const Id128& record, muscle::LineState& out,
                           PathHistory* history) const {
  const u32 slot = slot_of(record);
  if (slot == k_no_slot) return false;
  muscle::save(store_, slot, out);
  if (history != nullptr) *history = history_[slot];
  return true;
}

bool MuscleSimSystem::restore(const Id128& record, const muscle::LineState& in,
                              const PathHistory* history) {
  const u32 slot = slot_of(record);
  if (slot == k_no_slot) return false;
  if (muscle::restore(store_, slot, in) != muscle::RestoreStatus::Restored) return false;
  if (history != nullptr) history_[slot] = *history;
  return true;
}

}  // namespace engine::muscle_sim
