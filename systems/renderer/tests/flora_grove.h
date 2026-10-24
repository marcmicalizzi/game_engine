#pragma once

// A grove of hand-built trees for the poser's test and bench (flora_poser.h): two species, a
// palm-like one of nine parts and a broadleaf of a trunk, limbs and twigs, placed on a grid with
// turns, scales and world cells that differ tree to tree, and laid out as `GpuScene::create`'s
// flora path lays a scene's trees out — the same seed and the same inverse.

#include <core/containers/vector.h>
#include <core/math/math.h>
#include <core/math/world.h>
#include <domain/geometry/cluster_file.h>
#include <domain/gfx/cluster_cull.h>
#include <domain/gfx/flora_motion.h>
#include <systems/renderer/flora_motion.h>
#include <systems/renderer/flora_poser.h>

#include <cmath>
#include <span>

namespace engine::renderer::test_grove {

inline geometry::ClusterFileMotionPart grove_part(Vec3 pivot, Vec3 direction, f32 length,
                                                  u32 parent) {
  geometry::ClusterFileMotionPart p;
  p.pivot = pivot;
  p.direction = normalize(direction);
  p.length = length;
  p.parent = parent;
  return p;
}

// A trunk of `height` metres and `limbs` limbs up it, each with `twigs` twigs along it.
inline bool grove_tree(u32 limbs, u32 twigs, f32 height, FloraMesh& out) {
  Vector<geometry::ClusterFileMotionPart> parts;
  parts.push_back(grove_part(Vec3{}, Vec3{0.0f, 1.0f, 0.0f}, height, geometry::k_motion_no_parent));
  for (u32 l = 0; l < limbs; ++l) {
    const f32 a = 2.399963f * static_cast<f32>(l);  // the golden angle
    const Vec3 pivot{0.0f, height * (0.45f + 0.5f * static_cast<f32>(l) / static_cast<f32>(limbs)),
                     0.0f};
    const Vec3 dir{std::cos(a), 0.5f, std::sin(a)};
    const u32 limb = parts.size();
    parts.push_back(grove_part(pivot, dir, 0.4f * height, 0));
    const Vec3 along = normalize(dir);
    for (u32 t = 0; t < twigs; ++t) {
      const f32 at = 0.4f * height * static_cast<f32>(t + 1) / static_cast<f32>(twigs + 1);
      const f32 b = a + (t % 2 == 0 ? 0.9f : -0.9f);
      parts.push_back(grove_part(pivot + along * at, Vec3{std::cos(b), 0.2f, std::sin(b)},
                                 0.12f * height, limb));
    }
  }
  return flora_mesh(std::span<const geometry::ClusterFileMotionPart>(parts.data(), parts.size()),
                    {}, "broadleaf", out);  // synthetic geometry, catalogued motion traits
}

struct Grove {
  Vector<FloraMesh> meshes;
  Vector<Vec4> bounds;
  Vector<gfx::InstanceDesc> placed;
  Vector<FloraInstance> instances;
  Vector<FloraPartState> state;
  Vector<gfx::FloraPartPose> poses;
  u32 parts = 0;

  FloraPoseInputs inputs() {
    FloraPoseInputs in;
    in.instances = std::span<FloraInstance>(instances.data(), instances.size());
    in.meshes = std::span<const FloraMesh>(meshes.data(), meshes.size());
    in.bounds = std::span<const Vec4>(bounds.data(), bounds.size());
    in.placed = std::span<const gfx::InstanceDesc>(placed.data(), placed.size());
    in.state = std::span<FloraPartState>(state.data(), state.size());
    in.region = poses.data();
    return in;
  }
};

// `trees` trees on a grid `spacing` metres apart from x = z = 0 going +x and +z, species
// alternating, a turn and a scale each. False if a species failed to build.
inline bool make_grove(u32 trees, f32 spacing, Grove& g) {
  g = Grove{};
  g.meshes.resize(2);
  if (!grove_tree(8, 0, 6.0f, g.meshes[0]) || !grove_tree(6, 6, 10.0f, g.meshes[1])) return false;
  g.bounds.push_back(Vec4{0.0f, 4.5f, 0.0f, 6.0f});
  g.bounds.push_back(Vec4{0.0f, 6.0f, 0.0f, 8.0f});
  const u32 row = static_cast<u32>(std::ceil(std::sqrt(static_cast<f64>(trees))));
  for (u32 i = 0; i < trees; ++i) {
    const f32 turn = 0.7f * static_cast<f32>(i);
    const f32 s = 0.8f + 0.05f * static_cast<f32>(i % 9);
    const Mat4 linear(
        Vec4{std::cos(turn) * s, 0.0f, -std::sin(turn) * s, 0.0f}, Vec4{0.0f, s, 0.0f, 0.0f},
        Vec4{std::sin(turn) * s, 0.0f, std::cos(turn) * s, 0.0f}, Vec4{0.0f, 0.0f, 0.0f, 1.0f});
    gfx::InstanceDesc placed;
    placed.mesh = i % 2;
    gfx::set_instance_placement(
        placed, linear,
        WorldPos{static_cast<f64>(spacing) * static_cast<f64>(i % row) + 0.37,
                 0.25 * static_cast<f64>(i % 5),
                 static_cast<f64>(spacing) * static_cast<f64>(i / row) - 0.61});
    // As GpuScene lays a tree out: its seed, and its 3x3 inverted once.
    FloraInstance fi;
    fi.instance = i;
    fi.mesh = placed.mesh;
    fi.first_part = g.parts;
    fi.seed = i * 0x9e3779b9u + 0x632be5abu;
    const Vec3 r0 = placed.rows[0].xyz();
    const Vec3 r1 = placed.rows[1].xyz();
    const Vec3 r2 = placed.rows[2].xyz();
    const Vec3 c0{r0.x, r1.x, r2.x};
    const Vec3 c1{r0.y, r1.y, r2.y};
    const Vec3 c2{r0.z, r1.z, r2.z};
    const f32 det = dot(c0, cross(c1, c2));
    if (det != 0.0f) {
      const f32 inv = 1.0f / det;
      fi.inverse[0] = cross(c1, c2) * inv;
      fi.inverse[1] = cross(c2, c0) * inv;
      fi.inverse[2] = cross(c0, c1) * inv;
    }
    fi.inverse_scale = placed.scale_max > 0.0f ? 1.0f / placed.scale_max : 1.0f;
    g.placed.push_back(placed);
    g.instances.push_back(fi);
    g.parts += g.meshes[fi.mesh].part_count();
  }
  g.state.resize(g.parts, FloraPartState{});
  g.poses.resize(g.parts, gfx::FloraPartPose{});
  return true;
}

}  // namespace engine::renderer::test_grove
