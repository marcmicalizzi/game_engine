// The character (character.h; docs/subsystems/physics.md, "The character"): Jolt's virtual
// character behind the engine's types, stepped at a fixed tick from a plain input.

#include "world_impl.h"

#include <core/hash/hash.h>
#include <domain/physics/character.h>

#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>

#include <bit>
#include <cmath>

namespace engine::physics {

namespace {

constexpr f32 k_degrees_to_radians = 3.14159265358979323846f / 180.0f;

u32 bits(f32 v) noexcept {
  if (v == 0.0f) v = 0.0f;  // -0 and +0 are the same state
  return std::bit_cast<u32>(v);
}

u64 bits(f64 v) noexcept {
  if (v == 0.0) v = 0.0;  // -0 and +0 are the same state
  return std::bit_cast<u64>(v);
}

u64 hash_vec3(u64 seed, Vec3 v) noexcept {
  u64 h = hash_combine(seed, bits(v.x));
  h = hash_combine(h, bits(v.y));
  return hash_combine(h, bits(v.z));
}

u64 hash_world(u64 seed, WorldPos p) noexcept {
  u64 h = hash_combine(seed, bits(p.x));
  h = hash_combine(h, bits(p.y));
  return hash_combine(h, bits(p.z));
}

// The two decisions the backend leaves to its user, made the way its own character sample makes
// them, because each is what a walker needs:
//
// - **Standing still on a slope holds.** A capsule on a slope under the limit, asked to go
//   nowhere, would otherwise slide: gravity's velocity projected onto the slope's plane is a slide
//   along it, a few centimetres a second on a gentle dune. So while the input asks for no
//   movement, a contact with a static, walkable surface zeroes the velocity the solver hands back.
//   A surface past the limit is exempt, which is what makes the character slide there.
// - **A back face is only a wall.** A two-sided triangle seen from behind can hold the character
//   up only if it faces sideways; from below it is a ceiling, and standing on it would stick the
//   capsule to the underside of a ledge.
class Listener final : public JPH::CharacterContactListener {
 public:
  bool allow_sliding = true;

  bool OnContactValidate(const JPH::CharacterVirtual* character,
                         const JPH::CharacterContact& contact) override {
    return !contact.mIsBackFacingContact ||
           contact.mSurfaceNormal.Dot(character->GetUp()) < 0.866025f;
  }

  void OnContactSolve(const JPH::CharacterVirtual* character, const JPH::BodyID&,
                      const JPH::SubShapeID&, JPH::RVec3Arg, JPH::Vec3Arg contact_normal,
                      JPH::Vec3Arg contact_velocity, const JPH::PhysicsMaterial*, JPH::Vec3Arg,
                      JPH::Vec3& new_velocity) override {
    if (!allow_sliding && contact_velocity.IsNearZero() &&
        !character->IsSlopeTooSteep(contact_normal)) {
      new_velocity = JPH::Vec3::sZero();
    }
  }
};

Ground ground_of(JPH::CharacterBase::EGroundState state) noexcept {
  switch (state) {
    case JPH::CharacterBase::EGroundState::OnGround: return Ground::OnGround;
    case JPH::CharacterBase::EGroundState::OnSteepGround: return Ground::OnSteepGround;
    case JPH::CharacterBase::EGroundState::NotSupported: return Ground::NotSupported;
    case JPH::CharacterBase::EGroundState::InAir: return Ground::InAir;
  }
  return Ground::InAir;
}

bool valid_config(const CharacterConfig& c) noexcept {
  const auto positive = [](f32 v) { return v > 0.0f; };  // false for NaN too
  return positive(c.radius) && positive(c.height) && c.height > 2.0f * c.radius &&
         positive(c.eye_height) && c.eye_height <= c.height && c.step_height >= 0.0f &&
         c.step_height < c.height && positive(c.walk_speed) && positive(c.sprint_speed) &&
         c.max_slope_deg > 0.0f && c.max_slope_deg < 90.0f && c.gravity >= 0.0f &&
         c.jump_speed >= 0.0f && positive(c.mass) && c.step_hz > 0;
}

}  // namespace

const char* ground_name(Ground ground) noexcept {
  switch (ground) {
    case Ground::OnGround: return "on_ground";
    case Ground::OnSteepGround: return "on_steep_ground";
    case Ground::NotSupported: return "not_supported";
    case Ground::InAir: return "in_air";
  }
  return "unknown";
}

u64 hash_character_state(u64 seed, const CharacterState& state) noexcept {
  u64 h = hash_world(seed, state.position);
  h = hash_vec3(h, state.velocity);
  h = hash_vec3(h, state.ground_normal);
  h = hash_combine(h, static_cast<u64>(state.ground));
  return hash_combine(h, state.tick);
}

struct CharacterBody::Impl {
  JPH::PhysicsSystem* system = nullptr;
  JPH::TempAllocator* temp = nullptr;
  CharacterConfig config;
  JPH::Ref<JPH::CharacterVirtual> character;
  Listener listener;
  JPH::CharacterVirtual::ExtendedUpdateSettings update;
  f32 dt = 1.0f / 60.0f;
  u64 tick = 0;
  u64 hash = 0;

  // The character collides with everything a moving body does: static geometry, other moving and
  // kinematic bodies, and debris. Never with `Layer::Query`, which is geometry to cast against.
  JPH::DefaultBroadPhaseLayerFilter broad_phase_filter() const {
    return system->GetDefaultBroadPhaseLayerFilter(to_object_layer(Layer::Moving));
  }
  JPH::DefaultObjectLayerFilter object_filter() const {
    return system->GetDefaultLayerFilter(to_object_layer(Layer::Moving));
  }

  CharacterState state() const noexcept {
    CharacterState out;
    out.position = from_jph_world(character->GetPosition());
    out.velocity = from_jph(character->GetLinearVelocity());
    out.ground = ground_of(character->GetGroundState());
    out.ground_normal =
        out.ground == Ground::InAir ? Vec3{} : from_jph(character->GetGroundNormal());
    out.tick = tick;
    return out;
  }
};

CharacterBody::CharacterBody() noexcept = default;

CharacterBody::~CharacterBody() { destroy(); }

void CharacterBody::destroy() noexcept {
  delete impl_;
  impl_ = nullptr;
}

const CharacterConfig& CharacterBody::config() const noexcept {
  static const CharacterConfig k_none;
  return impl_ != nullptr ? impl_->config : k_none;
}

Status CharacterBody::create(World& world, const CharacterConfig& config, WorldPos feet) {
  if (impl_ != nullptr || world.impl_ == nullptr) return Status::InvalidArgument;
  if (!valid_config(config)) return Status::InvalidArgument;

  // The capsule stands on its feet: the shape is lifted by half its height, so the character's
  // position is the bottom of the capsule and not its middle, which is what a caller means by
  // "where it stands" and what a ground query returns.
  const f32 half_cylinder = 0.5f * (config.height - 2.0f * config.radius);
  JPH::RotatedTranslatedShapeSettings shape_settings(
      JPH::Vec3(0.0f, 0.5f * config.height, 0.0f), JPH::Quat::sIdentity(),
      new JPH::CapsuleShape(half_cylinder, config.radius));
  const JPH::ShapeSettings::ShapeResult shape = shape_settings.Create();
  if (shape.HasError()) {
    ENGINE_LOG_ERROR(log_physics, "character shape build failed",
                     log::field("error", shape.GetError().c_str()));
    return Status::BackendError;
  }

  JPH::Ref<JPH::CharacterVirtualSettings> settings = new JPH::CharacterVirtualSettings();
  settings->mShape = shape.Get();
  settings->mMaxSlopeAngle = config.max_slope_deg * k_degrees_to_radians;
  settings->mMass = config.mass;
  settings->mID = JPH::CharacterID(config.id);
  // Only the round bottom holds the character up: a contact on its side is a wall, however the
  // wall's normal leans.
  settings->mSupportingVolume = JPH::Plane(JPH::Vec3::sAxisY(), -config.radius);

  Impl* impl = new Impl();
  impl->system = &world.impl_->system;
  impl->temp = world.impl_->temp_allocator;
  impl->config = config;
  impl->dt = 1.0f / static_cast<f32>(config.step_hz);
  impl->character =
      new JPH::CharacterVirtual(settings, to_jph(feet), JPH::Quat::sIdentity(), 0, impl->system);
  impl->character->SetListener(&impl->listener);
  // Walking down a slope keeps the feet on it (the stick-to-floor sweep reaches at least a step
  // down, and half a metre, the backend's own default, on anything shallower), and walking into a
  // ledge under the step height climbs it.
  const f32 stick = config.step_height > 0.5f ? config.step_height : 0.5f;
  impl->update.mStickToFloorStepDown = JPH::Vec3(0.0f, -stick, 0.0f);
  impl->update.mWalkStairsStepUp = JPH::Vec3(0.0f, config.step_height, 0.0f);
  impl->character->RefreshContacts(impl->broad_phase_filter(), impl->object_filter(), {}, {},
                                   *impl->temp);
  impl->hash = hash_character_state(hash_combine(k_hash_seed, config.id), impl->state());
  impl_ = impl;
  return Status::Ok;
}

Status CharacterBody::step(const CharacterInput& input) {
  if (impl_ == nullptr) return Status::InvalidArgument;
  Impl& impl = *impl_;
  JPH::CharacterVirtual& character = *impl.character;
  const CharacterConfig& config = impl.config;
  const f32 dt = impl.dt;

  // What the input asks for, horizontally: a direction no longer than one, times the speed.
  f32 mx = input.move.x;
  f32 mz = input.move.z;
  if (!(mx == mx) || !(mz == mz)) mx = mz = 0.0f;  // a NaN is no direction
  const f32 length2 = mx * mx + mz * mz;
  if (length2 > 1.0f) {
    const f32 inverse = 1.0f / std::sqrt(length2);
    mx = mx * inverse;
    mz = mz * inverse;
  }
  const f32 speed = input.sprint ? config.sprint_speed : config.walk_speed;
  const JPH::Vec3 desired(mx * speed, 0.0f, mz * speed);
  impl.listener.allow_sliding = length2 > 0.0f;

  // The velocity the sweep starts from. On ground it can stand on, the character moves with the
  // ground (zero for everything static) and a jump leaves it; anywhere else it keeps its vertical
  // speed, which is what makes a fall a fall and a slide down a steep face a slide. Gravity
  // either way, and the input's horizontal velocity on top — in the air too, since a walker that
  // cannot steer a jump is a worse walker, not a more physical one.
  character.UpdateGroundVelocity();
  const JPH::Vec3 up = character.GetUp();
  const JPH::Vec3 vertical = character.GetLinearVelocity().Dot(up) * up;
  const JPH::Vec3 ground_velocity = character.GetGroundVelocity();
  const bool towards_ground = (vertical.GetY() - ground_velocity.GetY()) < 0.1f;
  JPH::Vec3 velocity;
  if (character.GetGroundState() == JPH::CharacterBase::EGroundState::OnGround &&
      !character.IsSlopeTooSteep(character.GetGroundNormal())) {
    velocity = ground_velocity;
    if (input.jump && towards_ground) velocity += config.jump_speed * up;
  } else {
    velocity = vertical;
  }
  const JPH::Vec3 gravity(0.0f, -config.gravity, 0.0f);
  velocity += gravity * dt;
  velocity += desired;
  character.SetLinearVelocity(velocity);

  character.ExtendedUpdate(dt, gravity, impl.update, impl.broad_phase_filter(),
                           impl.object_filter(), {}, {}, *impl.temp);
  ++impl.tick;
  impl.hash = hash_character_state(impl.hash, impl.state());
  return Status::Ok;
}

Status CharacterBody::teleport(WorldPos feet) {
  if (impl_ == nullptr) return Status::InvalidArgument;
  Impl& impl = *impl_;
  impl.character->SetPosition(to_jph(feet));
  impl.character->SetLinearVelocity(JPH::Vec3::sZero());
  impl.character->RefreshContacts(impl.broad_phase_filter(), impl.object_filter(), {}, {},
                                  *impl.temp);
  impl.hash = hash_character_state(hash_combine(impl.hash, 0x7e1e9047ull), impl.state());
  return Status::Ok;
}

CharacterState CharacterBody::state() const noexcept {
  return impl_ != nullptr ? impl_->state() : CharacterState{};
}

WorldPos CharacterBody::feet() const noexcept {
  return impl_ != nullptr ? from_jph_world(impl_->character->GetPosition()) : WorldPos{};
}

WorldPos CharacterBody::eye() const noexcept {
  const f32 eye_height = impl_ != nullptr ? impl_->config.eye_height : 0.0f;
  return feet() + DVec3{0.0, static_cast<f64>(eye_height), 0.0};
}

u64 CharacterBody::hash() const noexcept { return impl_ != nullptr ? impl_->hash : 0; }

}  // namespace engine::physics
