// Size table for systems/kinematics (ADR-0019). The two components are the capability's whole
// state and sit in every moving entity's archetype row, so a byte on either is a byte per mover.
#include <core/base/size_table.h>
#include <systems/kinematics/kinematics.h>

using namespace engine;

// Two vec3: linear and angular velocity.
ENGINE_EXPECT_SIZE(24, 4, kinematics::Velocity);
// Position and orientation: a worldpos (three f64, ADR-0053) and a quat. 40 bytes, 8-aligned, where
// a float32 vec3 position made it 28: twelve bytes a mover for a position that moves 420 km out.
ENGINE_EXPECT_SIZE(40, 8, world::Transform);
ENGINE_EXPECT_SIZE(4, 4, kinematics::KinematicsStats);
