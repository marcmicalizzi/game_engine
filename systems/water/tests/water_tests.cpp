// The water capability on the CPU (docs/subsystems/water.md, "Testing"): the block checked, a
// body's inside and its depth, the mesh and the depths it carries over a synthetic ground, the
// vertex budget, the GLB as the importer reads it, the generator through the registry, and the
// renderer's packing of a body's shading into its material. No device.
#include <core/hash/hash.h>
#include <core/math/math.h>
#include <core/math/world.h>
#include <domain/assets/gltf.h>
#include <domain/gfx/visibility_resolve.h>
#include <domain/scene_gen/scene_gen.h>
#include <foundation/io/vfs.h>
#include <systems/water/water.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <cmath>
#include <string>

using namespace engine;

namespace {

// A bowl round (100, 50): 2 m deep at its centre, rising 2 cm per square metre of radius, so the
// ground meets a surface at 0 ten metres out and stands above it beyond.
f64 bowl(const void*, i64 x_mm, i64 z_mm) noexcept {
  const f64 x = static_cast<f64>(x_mm) / 1000.0 - 100.0;
  const f64 z = static_cast<f64>(z_mm) / 1000.0 - 50.0;
  return -2.0 + 0.02 * (x * x + z * z);
}

scene_gen::Ground bowl_ground() {
  scene_gen::Ground g;
  g.surface_fn = &bowl;
  return g;
}

scene::WaterBody circle_block(f64 cx, f64 cz, f32 radius, f64 level) {
  scene::WaterBody b;
  b.name = "pool";
  b.level_m = level;
  scene::WaterCircle c;
  c.center = WorldPos{cx, 0.0, cz};
  c.radius_m = radius;
  b.shape.circle = c;
  return b;
}

water::Body read(const scene::WaterBody& block) {
  water::Body body;
  std::string error;
  REQUIRE_MESSAGE(water::read_body(block, body, &error), error);
  return body;
}

}  // namespace

TEST_CASE("water: the polynomial sine is the sine to 1e-14 everywhere a circle asks") {
  for (u32 i = 0; i <= 4096; ++i) {
    const f64 a = 6.283185307179586 * static_cast<f64>(i) / 4096.0;
    f64 s = 0.0;
    f64 c = 0.0;
    water::sin_cos(a, s, c);
    CHECK(std::fabs(s - std::sin(a)) < 1e-14);
    CHECK(std::fabs(c - std::cos(a)) < 1e-14);
  }
}

TEST_CASE("water: a block is read, and refused with a sentence when it cannot be drawn") {
  const water::Body body = read(circle_block(100.0, 50.0, 12.0f, 0.0));
  CHECK(body.circle());
  CHECK(body.center == WorldPos{100.0, 0.0, 50.0});
  std::string error;
  water::Body out;
  scene::WaterBody none = circle_block(0.0, 0.0, 5.0f, 0.0);
  none.shape.circle.reset();
  CHECK_FALSE(water::read_body(none, out, &error));
  CHECK(error.find("exactly one") != std::string::npos);
  scene::WaterBody both = circle_block(0.0, 0.0, 5.0f, 0.0);
  both.shape.polygon = {WorldPos{0, 0, 0}, WorldPos{1, 0, 0}, WorldPos{0, 0, 1}};
  CHECK_FALSE(water::read_body(both, out, &error));
  scene::WaterBody flat = circle_block(0.0, 0.0, 0.0f, 0.0);
  CHECK_FALSE(water::read_body(flat, out, &error));
  scene::WaterBody rough = circle_block(0.0, 0.0, 5.0f, 0.0);
  rough.roughness = 0.0f;
  CHECK_FALSE(water::read_body(rough, out, &error));
  CHECK(error.find("roughness") != std::string::npos);
  scene::WaterBody chop = circle_block(0.0, 0.0, 5.0f, 0.0);
  chop.chop = 1.5f;
  CHECK_FALSE(water::read_body(chop, out, &error));
  // A polygon in either winding, its centroid the centre; one not star-shaped from it is refused.
  scene::WaterBody square;
  square.level_m = 1.0;
  square.shape.polygon = {WorldPos{10, 0, 10}, WorldPos{10, 0, 14}, WorldPos{14, 0, 14},
                          WorldPos{14, 0, 10}};
  const water::Body sq = read(square);
  CHECK(sq.center.x == doctest::Approx(12.0));
  CHECK(sq.center.z == doctest::Approx(12.0));
  CHECK(sq.center.y == 1.0);
  REQUIRE(sq.outline.size() == 4);
  scene::WaterBody arrow;
  // A star from its centroid it is not: a C whose centroid, (4.08, 5), is in its own slot.
  arrow.shape.polygon = {WorldPos{0, 0, 0},   WorldPos{10, 0, 0}, WorldPos{10, 0, 2},
                         WorldPos{2, 0, 2},   WorldPos{2, 0, 8},  WorldPos{10, 0, 8},
                         WorldPos{10, 0, 10}, WorldPos{0, 0, 10}};
  CHECK_FALSE(water::read_body(arrow, out, &error));
  CHECK(error.find("star-shaped") != std::string::npos);
}

TEST_CASE("water: inside a body, at its edge, and outside it") {
  const water::Body circle = read(circle_block(0.0, 0.0, 10.0f, 0.0));
  CHECK(water::inside_distance(circle, 0.0f, 0.0f) == 10.0f);
  CHECK(water::inside_distance(circle, 6.0f, 8.0f) == 0.0f);
  CHECK(water::inside_distance(circle, 9.0f, 0.0f) == doctest::Approx(1.0f));
  CHECK(water::inside_distance(circle, 11.0f, 0.0f) < 0.0f);
  scene::WaterBody square;
  square.shape.polygon = {WorldPos{-4, 0, -4}, WorldPos{4, 0, -4}, WorldPos{4, 0, 4},
                          WorldPos{-4, 0, 4}};
  const water::Body sq = read(square);
  CHECK(water::inside_distance(sq, 0.0f, 0.0f) == doctest::Approx(4.0f));
  CHECK(water::inside_distance(sq, 3.0f, 0.0f) == doctest::Approx(1.0f));
  CHECK(water::inside_distance(sq, 4.0f, 4.0f) == doctest::Approx(0.0f).epsilon(1e-5));
  CHECK(water::inside_distance(sq, 4.5f, 0.0f) < 0.0f);
  CHECK(water::inside_distance(sq, -3.0f, 3.9f) > 0.0f);
}

TEST_CASE(
    "water: the depth at a point is the level less the ground, inside a body, never below 0") {
  const scene_gen::Ground ground = bowl_ground();
  water::Body bodies[2] = {read(circle_block(100.0, 50.0, 12.0f, 0.0)),
                           read(circle_block(100.0, 50.0, 3.0f, 0.5))};
  // At the centre the deeper of the two: the small one's level is half a metre higher.
  CHECK(water::depth_at(bodies, ground, WorldPos{100.0, 7.0, 50.0}) == doctest::Approx(2.5f));
  CHECK(water::depth_at(std::span<const water::Body>(bodies, 1), ground,
                        WorldPos{100.0, 0.0, 50.0}) == doctest::Approx(2.0f));
  // Five metres out: 2 - 0.5 = 1.5 m.
  CHECK(water::depth_at(bodies, ground, WorldPos{105.0, 0.0, 50.0}) == doctest::Approx(1.5f));
  // The shore: inside the outline, the ground above the water.
  CHECK(water::depth_at(bodies, ground, WorldPos{111.0, 0.0, 50.0}) == 0.0f);
  // Outside every body.
  CHECK(water::depth_at(bodies, ground, WorldPos{113.0, 0.0, 50.0}) == 0.0f);
  CHECK(water::depth_at(std::span<const water::Body>(), ground, WorldPos{100.0, 0.0, 50.0}) ==
        0.0f);
}

TEST_CASE("water: the mesh is rings to a fan, facing up, carrying the depth under every vertex") {
  const scene_gen::Ground ground = bowl_ground();
  const water::Body body = read(circle_block(100.0, 50.0, 12.0f, 0.0));
  water::Mesh mesh;
  water::build_mesh(body, ground, mesh);
  // The shore's default 2 m asks for two thirds of a metre: 114 edge samples and 18 rings.
  CHECK(mesh.spacing == doctest::Approx(2.0f / 3.0f));
  CHECK(mesh.edge == 114u);
  CHECK((mesh.rings == 18u || mesh.rings == 19u));  // 12 m over a float's two thirds
  REQUIRE(mesh.positions.size() == mesh.edge * mesh.rings + 1);
  REQUIRE(mesh.uvs.size() == mesh.positions.size());
  REQUIRE(mesh.normals.size() == mesh.positions.size());
  CHECK(mesh.indices.size() == (mesh.rings - 1) * mesh.edge * 6 + mesh.edge * 3);
  MESSAGE("a 24 m pool: " << mesh.positions.size() << " vertices, " << mesh.indices.size() / 3
                          << " triangles");
  for (u32 i = 0; i < mesh.positions.size(); ++i) {
    const Vec3 p = mesh.positions[i];
    CHECK(p.y == 0.0f);
    CHECK(mesh.normals[i] == Vec3{0.0f, 1.0f, 0.0f});
    // The depth: the level less the ground under the vertex, clamped at zero, to the bit.
    const WorldPos at{100.0 + static_cast<f64>(p.x), 0.0, 50.0 + static_cast<f64>(p.z)};
    const f32 depth = static_cast<f32>(std::max(0.0, 0.0 - ground.surface(at)));
    CHECK(mesh.uvs[i].y == depth);
    // The distance from the edge, which for a circle is the radius less the vertex's.
    const f32 r = std::sqrt(p.x * p.x + p.z * p.z);
    CHECK(mesh.uvs[i].x == doctest::Approx(12.0f - r).epsilon(1e-4));
  }
  // The edge ring is at the edge, beyond the shore, where the ground stands above the water.
  for (u32 j = 0; j < mesh.edge; ++j) {
    CHECK(mesh.uvs[j].x == 0.0f);
    CHECK(mesh.uvs[j].y == 0.0f);
  }
  CHECK(mesh.max_depth == doctest::Approx(2.0f));
  // Every triangle faces +y: a pool faces up, which its normal cones assume.
  u32 down = 0;
  for (u32 t = 0; t < mesh.indices.size(); t += 3) {
    const Vec3 a = mesh.positions[mesh.indices[t]];
    const Vec3 b = mesh.positions[mesh.indices[t + 1]];
    const Vec3 c = mesh.positions[mesh.indices[t + 2]];
    const Vec3 n = cross(b - a, c - a);
    if (!(n.y > 0.0f)) ++down;
  }
  CHECK(down == 0u);
}

TEST_CASE("water: a 20 m pool's vertex budget, and a lake's spacing grown to fit the budget") {
  const scene_gen::Ground none;
  water::Mesh pool;
  water::build_mesh(read(circle_block(0.0, 0.0, 10.0f, 0.0)), none, pool);
  MESSAGE("a 20 m pool: " << pool.positions.size() << " vertices, " << pool.indices.size() / 3
                          << " triangles at " << pool.spacing << " m");
  CHECK(pool.positions.size() < 2000u);
  // With no ground the ground is at 0, and a surface at 0 is everywhere 0 deep.
  CHECK(pool.max_depth == 0.0f);
  water::Mesh lake;
  water::build_mesh(read(circle_block(0.0, 0.0, 2000.0f, 0.0)), none, lake);
  CHECK(lake.positions.size() <= water::k_max_vertices);
  CHECK(lake.spacing > 1.0f);
  MESSAGE("a 4 km lake: " << lake.positions.size() << " vertices at " << lake.spacing << " m");
}

TEST_CASE("water: a polygon's mesh is rings of its outline, and its edge is at its outline") {
  scene::WaterBody block;
  block.shore_m = 1.5f;
  block.shape.polygon = {WorldPos{-6, 0, -3}, WorldPos{6, 0, -3}, WorldPos{6, 0, 3},
                         WorldPos{0, 0, 5}, WorldPos{-6, 0, 3}};
  const water::Body body = read(block);
  water::Mesh mesh;
  water::build_mesh(body, scene_gen::Ground{}, mesh);
  REQUIRE(mesh.positions.size() == mesh.edge * mesh.rings + 1);
  for (u32 j = 0; j < mesh.edge; ++j) {
    const Vec3 p = mesh.positions[j];
    CHECK(water::inside_distance(body, p.x, p.z) == doctest::Approx(0.0f).epsilon(1e-4));
  }
  u32 down = 0;
  for (u32 t = 0; t < mesh.indices.size(); t += 3) {
    const Vec3 a = mesh.positions[mesh.indices[t]];
    const Vec3 b = mesh.positions[mesh.indices[t + 1]];
    const Vec3 c = mesh.positions[mesh.indices[t + 2]];
    if (!(cross(b - a, c - a).y > 0.0f)) ++down;
  }
  CHECK(down == 0u);
}

TEST_CASE("water: the GLB is what the importer reads, and the same bytes every time") {
  const scene_gen::Ground ground = bowl_ground();
  const water::Body body = read(circle_block(100.0, 50.0, 12.0f, 0.0));
  water::Mesh mesh;
  water::build_mesh(body, ground, mesh);
  const Vec3 shallow{0.3f, 0.55f, 0.5f};
  const Vector<u8> glb = water::mesh_glb(mesh, shallow, 0.08f);
  water::Mesh again;
  water::build_mesh(body, ground, again);
  const Vector<u8> glb2 = water::mesh_glb(again, shallow, 0.08f);
  REQUIRE(glb.size() == glb2.size());
  CHECK(std::equal(glb.begin(), glb.end(), glb2.begin()));
  const std::string name = water::mesh_file_name(std::span<const u8>(glb.data(), glb.size()));
  CHECK(name.rfind("water-", 0) == 0);
  // The same bytes from every toolchain: f32 + - * / and sqrt, the sine our own (water.h), the
  // numbers printed with %.9g. Pinned on MSVC; the Linux containers reproduce it.
  const u64 h = hash_bytes(glb.data(), glb.size());
  MESSAGE("GLB " << glb.size() << " bytes, hash " << h);
  CHECK(glb.size() == 114560u);
  CHECK(h == 10307564092827010698ull);
  assets::MeshData data;
  std::string error;
  REQUIRE_MESSAGE(
      assets::load_gltf_memory(std::span<const u8>(glb.data(), glb.size()), "", data, &error),
      error);
  REQUIRE(data.positions.size() == mesh.positions.size());
  REQUIRE(data.uvs.size() == mesh.uvs.size());
  for (u32 i = 0; i < data.uvs.size(); ++i)
    CHECK(data.uvs[i] == mesh.uvs[i]);
  REQUIRE(data.materials.size() == 1);
  CHECK(data.materials[0].base_color.x == shallow.x);
  CHECK(data.materials[0].roughness == 0.08f);
  CHECK(data.materials[0].metallic == 0.0f);
}

TEST_CASE("water: the generator makes one mesh and one placement per body, in order") {
  const test::TempDir tmp("water_generator");
  const scene_gen::PlacementGeneratorDesc* generator =
      scene_gen::GeneratorRegistry::global().find_placement(water::k_generator);
  REQUIRE(generator != nullptr);
  scene::WaterBody blocks[2] = {circle_block(100.0, 50.0, 12.0f, 0.0),
                                circle_block(-40.0, 10.0, 4.0f, -1.5)};
  blocks[1].name = "puddle";
  scene_gen::Context context;
  context.ground = bowl_ground();
  const std::string dir = tmp.path();
  context.dir = dir;
  context.where = "test";
  void* state = nullptr;
  std::string error;
  REQUIRE_MESSAGE(generator->open(water::generator_params(blocks), context, &state, &error), error);
  scene_gen::Placements placed;
  REQUIRE_MESSAGE(generator->expand(state, context, placed, &error), error);
  generator->close(state);
  REQUIRE(placed.meshes.size() == 2);
  REQUIRE(placed.instances.size() == 2);
  CHECK(placed.meshes[0].name == "water/pool");
  CHECK(placed.meshes[1].name == "water/puddle");
  CHECK(io::exists(placed.meshes[0].path));
  CHECK(io::exists(placed.meshes[1].path));
  CHECK(placed.instances[0].position == WorldPos{100.0, 0.0, 50.0});
  CHECK(placed.instances[1].position == WorldPos{-40.0, -1.5, 10.0});
  CHECK(placed.instances[1].mesh == 1u);
  CHECK(placed.instances[0].rotation == Quat::identity());
  // A body the capability refuses is refused through the generator with its sentence.
  scene::WaterBody bad = circle_block(0.0, 0.0, -1.0f, 0.0);
  CHECK_FALSE(generator->open(water::generator_params(std::span<const scene::WaterBody>(&bad, 1)),
                              context, &state, &error));
  CHECK(error.find("radius_m") != std::string::npos);
}

TEST_CASE("water: a body's shading is packed into its material where only the branch reads it") {
  gfx::WaterShading s;
  s.shallow = Vec3{0.1f, 0.2f, 0.3f};
  s.deep = Vec3{0.01f, 0.02f, 0.03f};
  s.roughness = 0.07f;
  s.depth_fade_m = 2.5f;
  s.wave_amplitude_m = 0.03f;
  s.wave_length_m = 1.75f;
  s.chop = 0.4f;
  s.shore_m = 1.25f;
  s.direction = Vec2{0.6f, 0.8f};
  s.seed = 77;
  gfx::ResolveMaterial m;
  m.albedo_texture = 5;  // anything the material had is replaced
  gfx::set_water_material(m, s);
  CHECK(m.flags == gfx::k_material_water);
  CHECK(m.albedo == Vec4{0.1f, 0.2f, 0.3f, 0.07f});
  CHECK(m.emissive == Vec4{});
  CHECK(m.uv_transform == Vec4{0.01f, 0.02f, 0.03f, 2.5f});
  CHECK(m.uv_offset == Vec4{0.03f, 1.75f, 0.4f, 1.25f});
  CHECK(m.normal_scale == 0.6f);
  CHECK(m.occlusion_strength == 0.8f);
  CHECK(m.sampler == 77u);
  // What every other reader sees: an untextured dielectric of the shallow colour, the UVs as given.
  CHECK(m.albedo_texture == gfx::k_no_texture);
  CHECK(m.metallic_roughness_texture == gfx::k_no_texture);
  CHECK(m.normal_texture == gfx::k_no_texture);
  CHECK(m.occlusion_texture == gfx::k_no_texture);
  CHECK(m.emissive_texture == gfx::k_no_texture);
  CHECK(m.uv_scale == 1.0f);
  CHECK((m.flags & gfx::k_material_uv_transform) == 0u);
  // The water's clock: the frame's time modulo the period, in double.
  CHECK(gfx::water_clock(0.0) == 0.0f);
  CHECK(gfx::water_clock(1024.0 * 1000.0 + 12.5) == 12.5f);
  CHECK(gfx::water_clock(-1.0) == 1023.0f);
  // The volume tier's optics (water.md, "Translucent by volume"): the flag on the water's material
  // and the entry after it — the extinction and the transmission, the scattering, the ground's UV
  // frame and its material — with the turbidity's sediment added to both rates.
  gfx::WaterOptics o;
  o.absorption = Vec3{0.5f, 0.1f, 0.05f};
  o.scattering = Vec3{0.02f, 0.03f, 0.04f};
  o.transmission = 0.75f;
  o.turbidity = 0.5f;
  o.ground = 12;
  o.ground_uv = Vec2{0.5f, 0.25f};
  o.ground_uv_per_m = 0.004f;
  gfx::ResolveMaterial optics;
  gfx::set_water_optics(m, optics, o);
  CHECK(m.flags == (gfx::k_material_water | gfx::k_material_water_volume));
  const Vec3 scatter = o.scattering + gfx::k_water_silt_scatter * 0.5f;
  const Vec3 extinction = o.absorption + gfx::k_water_silt_absorb * 0.5f + scatter;
  CHECK(optics.albedo == Vec4{extinction, 0.75f});
  CHECK(optics.emissive == Vec4{scatter, 0.0f});
  CHECK(optics.uv_transform == Vec4{0.5f, 0.25f, 0.004f, 0.0f});
  CHECK(optics.sampler == 12u);
  CHECK(optics.flags == 0u);
  CHECK(optics.albedo_texture == gfx::k_no_texture);
}

TEST_CASE("water: the volume tier's optics are read and refused with a sentence") {
  scene::WaterBody ok = circle_block(0.0, 0.0, 5.0f, 0.0);
  ok.absorption = Vec3{0.4f, 0.1f, 0.0f};
  ok.scattering = Vec3{0.0f, 0.0f, 0.0f};
  ok.transmission = 0.0f;
  ok.turbidity = 3.0f;
  water::Body out;
  std::string error;
  CHECK(water::read_body(ok, out, &error));
  scene::WaterBody absorbs = ok;
  absorbs.absorption = Vec3{0.4f, -0.1f, 0.0f};
  CHECK_FALSE(water::read_body(absorbs, out, &error));
  CHECK(error.find("absorption") != std::string::npos);
  scene::WaterBody scatters = ok;
  scatters.scattering = Vec3{0.0f, 0.0f, std::nanf("")};
  CHECK_FALSE(water::read_body(scatters, out, &error));
  CHECK(error.find("scattering") != std::string::npos);
  scene::WaterBody passes = ok;
  passes.transmission = 1.5f;
  CHECK_FALSE(water::read_body(passes, out, &error));
  CHECK(error.find("transmission") != std::string::npos);
  scene::WaterBody silty = ok;
  silty.turbidity = -1.0f;
  CHECK_FALSE(water::read_body(silty, out, &error));
  CHECK(error.find("turbidity") != std::string::npos);
}
