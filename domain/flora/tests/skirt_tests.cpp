// The trunk's skirt (G201; docs/subsystems/flora.md, "The trunk's skirt";
// docs/experiments/tree-base-on-slopes.md, "Seating"): every tree mesh the solver makes carries its
// stem on below its collar, deep enough that no slope of up to 45 degrees at any azimuth shows a
// gap under it, in the trunk's own primitive, part and UV frame, and in every form the tree is
// drawn in.
#include "skirt_support.h"

#include <domain/flora/flora.h>
#include <domain/flora/trees.h>
#include <domain/geometry/cluster_lod.h>
#include <domain/geometry/cluster_pages.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <string>

using namespace engine;

namespace {

// What the requirement is, from the geometry alone: a tree stands on the ground at its origin, so
// on a plane of slope tan(a) through the origin falling towards azimuth phi the ground under a
// point (x, z) is at -tan(a) (x cos phi + z sin phi). The skirt's lowest ring has to be under that
// for every ramp the design names (0, 15, 30 and 45 degrees) and every azimuth, with the margin the
// design gives for ground that curves away under the rim. Not the solver's formula: its
// consequence.
constexpr f64 k_margin_m = 0.15;
constexpr f64 k_ramps_tan[] = {0.0, 0.2679491924311227, 0.5773502691896257, 1.0};
constexpr u32 k_azimuths = 64;

struct Skirt {
  u32 first = 0;  // its first vertex
  u32 sides = 0;
  f64 rim = 0;     // the collar ring's radius round its part's pivot, across the level plane
  f64 rho = 0;     // the collar ring's farthest point from the origin, across the level plane
  f64 bottom = 0;  // the lowest ring's height
};

// The skirts of a mesh, each a run of skirt vertices: one ring of `sides + 1` under a ring of the
// stem's own vertices, which the skirt's triangles name.
Vector<Skirt> skirts_of(const assets::MeshData& mesh, const flora::Motion& motion, u32& triangles) {
  const flora::test_support::SkirtFound found = flora::test_support::find_skirt(mesh, motion);
  triangles = found.triangles;
  Vector<Skirt> out;
  for (u32 v = 0; v < mesh.positions.size();) {
    if (found.vertex[v] == 0) {
      ++v;
      continue;
    }
    u32 end = v;
    while (end < mesh.positions.size() && found.vertex[end] != 0)
      ++end;
    Skirt s;
    s.first = v;
    s.sides = end - v - 1;
    // The ring above: the stem's vertices this skirt's triangles name, in order.
    Vector<u32> top;
    for (u32 t = 0; t < found.triangle.size(); ++t) {
      if (found.triangle[t] == 0) continue;
      bool ours = false;
      for (u32 k = 0; k < 3; ++k) {
        const u32 i = mesh.indices[t * 3 + k];
        ours = ours || (i >= v && i < end);
      }
      if (!ours) continue;
      for (u32 k = 0; k < 3; ++k) {
        const u32 i = mesh.indices[t * 3 + k];
        if (found.vertex[i] == 0 && std::find(top.begin(), top.end(), i) == top.end())
          top.push_back(i);
      }
    }
    std::sort(top.begin(), top.end());
    REQUIRE(top.size() == s.sides + 1);
    const u32 part = motion.vertex_part[top[0]];
    const Vec3 pivot = motion.parts[part].pivot;
    s.bottom = 1e30;
    for (u32 i = 0; i <= s.sides; ++i) {
      CHECK(top[i] == top[0] + i);  // the stem's bottom ring, one run of vertices
      const Vec3 c = mesh.positions[top[i]];
      const f64 dx = static_cast<f64>(c.x) - static_cast<f64>(pivot.x);
      const f64 dz = static_cast<f64>(c.z) - static_cast<f64>(pivot.z);
      s.rim = std::max(s.rim, std::sqrt(dx * dx + dz * dz));
      s.rho = std::max(s.rho, std::sqrt(static_cast<f64>(c.x) * static_cast<f64>(c.x) +
                                        static_cast<f64>(c.z) * static_cast<f64>(c.z)));
      // The stem's own surface carried straight down: its part, no bend, its normal, its u, its v
      // below the collar's by the metres it drops.
      const u32 k = v + i;
      CHECK(motion.vertex_part[k] == part);
      CHECK(motion.vertex_weight[k] == 0.0f);
      CHECK(mesh.positions[k].x == c.x);
      CHECK(mesh.positions[k].z == c.z);
      CHECK(std::abs(mesh.normals[k].x - mesh.normals[top[i]].x) <= 1e-6f);  // renormalised
      CHECK(std::abs(mesh.normals[k].z - mesh.normals[top[i]].z) <= 1e-6f);
      CHECK(mesh.uvs[k].x == mesh.uvs[top[i]].x);
      CHECK(mesh.uvs[k].y < mesh.uvs[top[i]].y);
      s.bottom = std::min(s.bottom, static_cast<f64>(mesh.positions[k].y));
    }
    out.push_back(s);
    v = end;
  }
  return out;
}

// Every skirt's lowest ring under every ramp at every azimuth, and at or below the depth the 45
// degree ramp asks of its rim.
void check_skirts(const std::string& name, const assets::MeshData& mesh,
                  const flora::Motion& motion, u32 stems) {
  CAPTURE(name);
  u32 triangles = 0;
  const Vector<Skirt> skirts = skirts_of(mesh, motion, triangles);
  REQUIRE(skirts.size() == stems);
  u32 expected = 0;
  for (const Skirt& s : skirts) {
    expected += 2 * s.sides;
    // A 45 degree plane through the origin is rho tan 45 under the origin at the collar's farthest
    // point; the skirt reaches that and the margin (as the mesh stores it, in f32).
    CHECK(s.bottom <= static_cast<f64>(static_cast<f32>(-(s.rho * k_ramps_tan[3] + k_margin_m))));
    MESSAGE(name << ": a skirt of " << s.sides << " sides, rim " << s.rim << " m (" << s.rho
                 << " m from the origin), down to " << s.bottom << " m");
    u32 above = 0;  // a lowest-ring vertex not under some ramp at some azimuth
    for (u32 i = 0; i <= s.sides; ++i) {
      const Vec3 p = mesh.positions[s.first + i];  // the lowest ring
      for (const f64 slope : k_ramps_tan)
        for (u32 a = 0; a < k_azimuths; ++a) {
          const f64 phi = 2.0 * 3.14159265358979323846 * a / k_azimuths;
          const f64 ground = -slope * (static_cast<f64>(p.x) * std::cos(phi) +
                                       static_cast<f64>(p.z) * std::sin(phi));
          if (static_cast<f64>(p.y) > ground - k_margin_m + 1e-6) ++above;
        }
    }
    CHECK(above == 0);
  }
  CHECK(triangles == expected);
  MESSAGE(name << ": " << triangles << " skirt triangles of " << mesh.indices.size() / 3);
}

}  // namespace

TEST_CASE("flora: every tree's skirt is under ground of up to 45 degrees at every azimuth") {
  const struct {
    const char* name;
    const char* recipe;
    u32 stems;
  } trees[] = {
      {"date palm", R"({"species":"date_palm"})", 1},
      {"broadleaf", R"({"species":"broadleaf"})", 1},
      {"conifer", R"({"species":"conifer"})", 1},
      {"birch", R"({"species":"birch"})", 1},
      {"birch clump", R"({"species":"birch","birch":{"stems":3,"seed":5}})", 3},
      {"willow", R"({"species":"willow"})", 1},
      {"orchard", R"({"species":"orchard"})", 1},
      {"ancient", R"({"species":"ancient","ancient":{"triangle_budget":20000}})", 1},
  };
  for (const auto& tree : trees) {
    flora::FloraFile file;
    std::string error;
    REQUIRE_MESSAGE(flora::read_flora_file(tree.recipe, file, error), error);
    assets::MeshData mesh;
    flora::Motion motion;
    REQUIRE_MESSAGE(flora::solve_flora(file, mesh, motion, nullptr, nullptr, error), error);
    check_skirts(tree.name, mesh, motion, tree.stems);
  }
  for (const char* name : {"maple", "oak", "elm", "walnut", "beech", "ash", "pine", "spruce",
                           "cedar", "juniper", "aspen", "fir", "hemlock", "alder"}) {
    flora::FloraFile file;
    std::string error;
    REQUIRE_MESSAGE(
        flora::read_flora_file(std::string(R"({"species":")") + name +
                                   R"(","tree":{"height_m":10,"card_bake_px":0,"impostor_px":0}})",
                               file, error),
        error);
    assets::MeshData mesh;
    flora::Motion motion;
    REQUIRE_MESSAGE(flora::solve_flora(file, mesh, motion, nullptr, nullptr, error), error);
    check_skirts(name, mesh, motion, 1);
  }
}

TEST_CASE("flora: the skirt stays in every cut of the LOD DAG and every distance form") {
  // A spruce built as the forest builds it, with its chain at small tiles so a debug run is quick:
  // the cards and the star are the far forms, and the star's planes stand on the box the skirt
  // reaches down into.
  flora::FloraFile file;
  std::string error;
  REQUIRE(flora::read_flora_file(
      R"({"species":"spruce","tree":{"height_m":10,"card_bake_px":32,"impostor_px":64}})", file,
      error));
  geometry::ClusterFileData data;
  REQUIRE_MESSAGE(flora::build_flora_container(file, {}, geometry::ClusterPagesOptions{}.page_bytes,
                                               data, nullptr, error),
                  error);
  const geometry::ClusterMesh& mesh = data.mesh.mesh;
  const auto lowest = [&](const Vector<u32>& clusters) {
    f64 y = 1e30;
    for (const u32 c : clusters) {
      const geometry::ClusterDesc& d = mesh.clusters[c];
      for (u32 v = 0; v < d.vertex_count; ++v)
        y = std::min(y, static_cast<f64>(mesh.vertices[d.vertex_offset + v].y));
    }
    return y;
  };
  Vector<u32> all(mesh.clusters.size());
  for (u32 c = 0; c < all.size(); ++c)
    all[c] = c;
  const f64 bottom = lowest(all);
  CHECK(bottom < -0.3);  // a 10 m spruce's rim is 0.17 m: its skirt reaches 0.34 m
  // Every cut, from the finest to the coarsest: the skirt's bottom stays where it is (to a
  // centimetre, the grid's and the simplifier's slack), so no rung of the DAG lifts it off the
  // ground before the tree is gone.
  for (f32 t = 1e-4f; t < 1e4f; t *= 2.0f) {
    Vector<u32> cut;
    geometry::select_lod_raw(data.mesh, t, cut);
    if (cut.empty()) continue;
    CAPTURE(t);
    CHECK(lowest(cut) <= bottom + 0.01);
  }
  for (const f32 distance : {5.0f, 20.0f, 60.0f, 200.0f, 1000.0f}) {
    CAPTURE(distance);
    geometry::LodFormCut cut;
    cut.distance = distance;
    cut.proj_scale = 1080;
    geometry::ClusterLodMesh form;
    geometry::LodFormMap map;
    Vector<u32> coverage;
    REQUIRE(geometry::extract_lod_form(data.mesh, {}, cut, form, coverage, map, &error));
    f64 y = 1e30;
    for (const Vec3& v : form.mesh.vertices)
      y = std::min(y, static_cast<f64>(v.y));
    CHECK(y <= bottom + 0.01);
  }
}
