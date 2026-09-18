// A minimal wrap of DetourCrowd: local avoidance and path following for the agents close enough
// to need them.
//
// dtCrowd allocates everything up front from its cap, so `CrowdOptions::max_agents` is the whole
// memory story and a game with no crowd has no proximity grid, no path queue, and no per-agent
// corridor (plan 11 §11.10). It also owns a dtNavMeshQuery of its own, which is why a crowd is
// safe to update while nothing else queries the same mesh: the two do not share A* scratch.

#include "nav_log.h"
#include "recast_backend.h"

#include <core/base/assert.h>
#include <core/containers/slot_map.h>
#include <core/containers/vector.h>
#include <domain/nav/nav_mesh.h>

namespace engine::nav {
namespace {

// dtCrowd's steering flags. Anticipating turns and optimizing the corridor's visibility are what
// make an agent walk a corner instead of scraping it; separation is off unless the caller asks,
// because its weight has to be tuned against the agent's radius and a wrong default reads as
// agents pushing each other around.
u8 update_flags(const AgentDesc& desc) noexcept {
  unsigned int flags = DT_CROWD_ANTICIPATE_TURNS;
  if (desc.optimize_visibility) flags |= DT_CROWD_OPTIMIZE_VIS | DT_CROWD_OPTIMIZE_TOPO;
  if (desc.avoid_obstacles) flags |= DT_CROWD_OBSTACLE_AVOIDANCE;
  if (desc.avoid_agents && desc.separation_weight > 0.0f) flags |= DT_CROWD_SEPARATION;
  return static_cast<u8>(flags);
}

struct AgentSlot {
  i32 index = -1;
  u64 user_data = 0;
  Vec3 target{};
  bool has_target = false;
};

}  // namespace

struct Crowd::Impl {
  dtCrowd* crowd = nullptr;
  CrowdOptions options;
  SlotMap<AgentSlot> agents;
  Vector<SlotHandle> by_index;  // dtCrowd index -> our handle, so a read is one indirection
};

Crowd::Crowd() noexcept = default;

Crowd::~Crowd() { shutdown(); }

Status Crowd::init(NavMesh& mesh, const CrowdOptions& options) {
  if (impl_ != nullptr) return Status::InvalidArgument;
  if (!mesh.initialized()) return Status::NotFound;
  if (options.max_agents == 0 || !(options.max_agent_radius > 0.0f)) return Status::InvalidArgument;

  auto* nav = static_cast<dtNavMesh*>(mesh.backend());
  if (nav == nullptr) return Status::NotFound;

  auto* impl = new Impl();
  impl->options = options;
  impl->crowd = dtAllocCrowd();
  if (impl->crowd == nullptr) {
    delete impl;
    return Status::BackendError;
  }
  if (!impl->crowd->init(static_cast<int>(options.max_agents), options.max_agent_radius, nav)) {
    dtFreeCrowd(impl->crowd);
    delete impl;
    return Status::BackendError;
  }
  // The crowd's own filter has to agree with the engine's flag convention, or an agent walks
  // through a door the path query refused.
  if (dtQueryFilter* filter = impl->crowd->getEditableFilter(0); filter != nullptr) {
    filter->setIncludeFlags(static_cast<u16>(~k_flag_disabled));
    filter->setExcludeFlags(k_flag_disabled);
  }
  impl->by_index.resize(options.max_agents);
  impl->agents.reserve(options.max_agents);
  impl_ = impl;
  return Status::Ok;
}

void Crowd::shutdown() noexcept {
  if (impl_ == nullptr) return;
  if (impl_->crowd != nullptr) dtFreeCrowd(impl_->crowd);
  delete impl_;
  impl_ = nullptr;
}

Status Crowd::add_agent(const AgentDesc& desc, AgentId& out) {
  out = AgentId{};
  if (impl_ == nullptr) return Status::NotFound;
  if (!(desc.radius > 0.0f) || !(desc.height > 0.0f)) return Status::InvalidArgument;
  if (desc.radius > impl_->options.max_agent_radius) return Status::InvalidArgument;
  if (impl_->agents.size() >= impl_->options.max_agents) return Status::LimitReached;

  dtCrowdAgentParams params{};
  params.radius = desc.radius;
  params.height = desc.height;
  params.maxAcceleration = desc.max_acceleration;
  params.maxSpeed = desc.max_speed;
  // Detour's own sample ratios: look twelve radii ahead for neighbours and thirty for corridor
  // shortcuts. They are derived from the radius rather than exposed, because an absolute value
  // here means something different for a rat and for a truck.
  params.collisionQueryRange = desc.radius * 12.0f;
  params.pathOptimizationRange = desc.radius * 30.0f;
  params.separationWeight = desc.separation_weight;
  params.updateFlags = update_flags(desc);
  params.obstacleAvoidanceType = 3;  // dtCrowd's highest-quality preset
  params.queryFilterType = 0;
  params.userData = nullptr;

  float position[3] = {desc.position.x, desc.position.y, desc.position.z};
  const int index = impl_->crowd->addAgent(position, &params);
  if (index < 0) return Status::LimitReached;

  AgentSlot slot;
  slot.index = index;
  slot.user_data = desc.user_data;
  const SlotHandle handle = impl_->agents.insert(slot);
  impl_->by_index[static_cast<u32>(index)] = handle;
  out = AgentId{handle};
  return Status::Ok;
}

bool Crowd::remove_agent(AgentId id) {
  if (impl_ == nullptr) return false;
  const AgentSlot* slot = impl_->agents.get(id.handle);
  if (slot == nullptr) return false;
  impl_->crowd->removeAgent(slot->index);
  impl_->by_index[static_cast<u32>(slot->index)] = SlotHandle{};
  impl_->agents.erase(id.handle);
  return true;
}

bool Crowd::contains(AgentId id) const noexcept {
  return impl_ != nullptr && impl_->agents.contains(id.handle);
}

u32 Crowd::agent_count() const noexcept { return impl_ != nullptr ? impl_->agents.size() : 0; }

u32 Crowd::max_agents() const noexcept { return impl_ != nullptr ? impl_->options.max_agents : 0; }

bool Crowd::request_move(AgentId id, Vec3 target) {
  if (impl_ == nullptr) return false;
  AgentSlot* slot = impl_->agents.get(id.handle);
  if (slot == nullptr) return false;

  const dtNavMeshQuery* query = impl_->crowd->getNavMeshQuery();
  const dtQueryFilter* filter = impl_->crowd->getFilter(0);
  const float* extents = impl_->crowd->getQueryHalfExtents();
  if (query == nullptr || filter == nullptr || extents == nullptr) return false;

  float centre[3] = {target.x, target.y, target.z};
  float nearest[3] = {0.0f, 0.0f, 0.0f};
  dtPolyRef ref = 0;
  if (dtStatusFailed(query->findNearestPoly(centre, extents, filter, &ref, nearest)) || ref == 0)
    return false;
  if (!impl_->crowd->requestMoveTarget(slot->index, ref, nearest)) return false;
  slot->target = Vec3(nearest[0], nearest[1], nearest[2]);
  slot->has_target = true;
  return true;
}

bool Crowd::stop(AgentId id) {
  if (impl_ == nullptr) return false;
  AgentSlot* slot = impl_->agents.get(id.handle);
  if (slot == nullptr) return false;
  impl_->crowd->resetMoveTarget(slot->index);
  slot->has_target = false;
  return true;
}

void Crowd::update(f32 dt) {
  if (impl_ == nullptr || dt <= 0.0f) return;
  impl_->crowd->update(dt, nullptr);
}

bool Crowd::agent_state(AgentId id, AgentState& out) const {
  out = AgentState{};
  if (impl_ == nullptr) return false;
  const AgentSlot* slot = impl_->agents.get(id.handle);
  if (slot == nullptr) return false;
  const dtCrowdAgent* agent = impl_->crowd->getAgent(slot->index);
  if (agent == nullptr || !agent->active) return false;
  out.position = Vec3(agent->npos[0], agent->npos[1], agent->npos[2]);
  out.velocity = Vec3(agent->vel[0], agent->vel[1], agent->vel[2]);
  out.target = slot->target;
  out.user_data = slot->user_data;
  out.has_target = slot->has_target;
  return true;
}

void Crowd::read_positions(std::span<const AgentId> ids, std::span<Vec3> out) const {
  if (impl_ == nullptr) return;
  const usize count = ids.size() < out.size() ? ids.size() : out.size();
  for (usize i = 0; i < count; ++i) {
    const AgentSlot* slot = impl_->agents.get(ids[i].handle);
    if (slot == nullptr) continue;
    const dtCrowdAgent* agent = impl_->crowd->getAgent(slot->index);
    if (agent == nullptr || !agent->active) continue;
    out[i] = Vec3(agent->npos[0], agent->npos[1], agent->npos[2]);
  }
}

}  // namespace engine::nav
