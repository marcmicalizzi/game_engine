// Size table for systems/kinematics (ADR-0019). The two components are the capability's whole
// state and sit in every moving entity's archetype row, so a byte on either is a byte per mover.
#include <core/base/size_table.h>
#include <systems/kinematics/kinematics.h>

using namespace engine;

// Two vec3: linear and angular velocity.
ENGINE_EXPECT_SIZE(24, 4, kinematics::Velocity);
// Position and orientation: a vec3 and a quat.
ENGINE_EXPECT_SIZE(28, 4, world::Transform);
ENGINE_EXPECT_SIZE(4, 4, kinematics::KinematicsStats);
