// The GPU-driven half of the cluster acceleration structure path: clas_records.slang turns a
// scene's visible list and its count into the 64-byte CLAS build records, grouped by instance so
// every instance can have its own cluster bottom-level structure, plus the record count and one
// bottom-level record per instance. The shader's bytes must equal what write_cluster_build_records
// writes on the CPU for the same clusters, so the CPU and GPU paths can never drift apart. One of
// the two instances is **deformed**, so its records must name the frame's deformed-vertex pool
// instead of the mesh's rest positions, which is the rule every position reader follows. A run of
// **shadow casters** sits where the renderer puts it (`k_caster_run`): those are built into their
// instance's records like the drawn entries, with their visible index as their geometry index, but
// not opaque, which is what keeps a primary ray from seeing them.
//
// The second case holds the **capacity** rule (docs/subsystems/renderer.md, "The ray tracing
// chain's memory"): the set is sized by the frame, and a frame that wants more than it holds keeps
// whole instances, every instance's drawn clusters before any caster. The shader needs no ray
// tracing feature, so both run on any device with a driver.
#include "raster_path.h"

#include <domain/geometry/cluster.h>
#include <domain/gfx/cluster_acceleration.h>
#include <domain/gfx/cluster_cull.h>
#include <domain/gfx/device.h>
#include <domain/gfx/frame.h>
#include <domain/gfx/render_graph.h>
#include <domain/gfx/vulkan.h>

#include <doctest/doctest.h>

#include <cstring>
#include <shaders/clas_records.spv.h>
#include <string>

using namespace engine;

namespace {

void make_grid(u32 n, Vector<Vec3>& positions, Vector<u32>& indices) {
  for (u32 y = 0; y < n; ++y) {
    for (u32 x = 0; x < n; ++x)
      positions.push_back(
          Vec3{static_cast<f32>(x), static_cast<f32>(y) * 0.5f, static_cast<f32>(x * y) * 0.01f});
  }
  for (u32 y = 0; y + 1 < n; ++y) {
    for (u32 x = 0; x + 1 < n; ++x) {
      const u32 a = y * n + x;
      indices.push_back(a);
      indices.push_back(a + n);
      indices.push_back(a + 1);
      indices.push_back(a + 1);
      indices.push_back(a + n);
      indices.push_back(a + n + 1);
    }
  }
}

constexpr u32 k_instances = 2;
constexpr u32 k_deformed = 1;
// The pool is addressed per **visible entry**, so a record's vertex address comes out of
// `MeshDesc::deform_slots[visible_index]` and not out of the instance. The bias keeps a block a few
// slots off the mesh's own vertex index, so a record that used the wrong one shows up rather than
// cancelling out.
constexpr u32 k_pool_bias = 7;
constexpr u64 k_fake_addresses = 0x1000;  // the shader offsets this into the BLAS records

// Two instances of one mesh, a visible list with both interleaved in a scrambled order, and a run
// of shadow casters for the rigid one; everything the three passes read, uploaded, and the passes
// themselves. `run(capacity)` records them once with the capacity the set was sized for.
struct RecordsFixture {
  gfx::Device device;
  std::string error;
  geometry::ClusterMesh mesh;
  u32 cluster_count = 0;
  u32 pair_count = 0;
  Vector<u32> visible;  // uint2 per drawn entry, in list order
  u32 count = 0;        // drawn entries the cull pass counted (the last listed one is beyond it)
  Vector<u32> caster_entries;
  u32 caster_count = 0;
  u32 caster_base = 0;
  Vector<u32> list;  // the whole list as the renderer lays it out: three runs of pair_count
  Vector<u32> deform_slot_table;
  gfx::BufferResource clusters, vertices, index8, instances, visible_buffer, count_buffer,
      caster_count_buffer, slots, instance_counts, records, record_count, blas_records, meshes,
      deform_table, deform_slots, pool;
  VkShaderModule module = VK_NULL_HANDLE;
  gfx::ComputePipeline bucket, ranges, emit;
  bool ready = false;

  u32 entry_instance(u32 entry) const { return list[u64{entry} * 2]; }
  u32 entry_cluster(u32 entry) const { return list[u64{entry} * 2 + 1]; }

  bool create() {
    if (!gfx_test::open_device(device)) return false;
    Vector<Vec3> positions;
    Vector<u32> indices;
    make_grid(33, positions, indices);  // 2,048 triangles
    REQUIRE(geometry::build_clusters(positions, indices, geometry::ClusterBuildOptions{}, mesh,
                                     &error));
    cluster_count = mesh.clusters.size();
    REQUIRE(cluster_count >= 8);
    // The pair space is twice the cluster count and the second instance's pairs start at its
    // first_pair. The second one is deformed, so its records must point into the pool.
    pair_count = cluster_count * k_instances;
    gfx::InstanceDesc instance_table[k_instances];
    for (u32 i = 0; i < k_instances; ++i) {
      gfx::set_instance_transform(instance_table[i],
                                  translation(Vec3{40.0f * static_cast<f32>(i), 0.0f, 0.0f}));
      instance_table[i].mesh = 0;
      instance_table[i].first_pair = i * cluster_count;
    }
    instance_table[k_deformed].deform = 0;
    gfx::DeformDesc deform{};
    deform.stages = gfx::k_deform_stage_procedural | gfx::k_deform_wave;
    // Even clusters of both instances, interleaved, walking down: the last entry is beyond the
    // count and must be ignored.
    for (u32 i = cluster_count; i-- > 0;) {
      if (i % 2 != 0) continue;
      for (u32 k = 0; k < k_instances; ++k) {
        visible.push_back(k);
        visible.push_back(i);
      }
    }
    const u32 entry_count = visible.size() / 2;
    count = entry_count - 1;
    // The shadow casters: the rigid instance's odd clusters, which the list above does not draw,
    // in run `k_caster_run` of the one view, and again one entry beyond their count. A deformed
    // instance is never cone-tested, so it never has one.
    for (u32 i = 1; i < cluster_count; i += 2) {
      caster_entries.push_back(0);
      caster_entries.push_back(i);
    }
    caster_count = caster_entries.size() / 2 - 1;
    caster_base = gfx::k_caster_run * pair_count;  // run-major, one view
    REQUIRE(count + caster_count + 1 < pair_count);
    list.assign(u64{3} * pair_count * 2, 0xffffffffu);
    for (u32 i = 0; i < visible.size(); ++i)
      list[i] = visible[i];
    for (u32 i = 0; i < caster_entries.size(); ++i)
      list[u64{caster_base} * 2 + i] = caster_entries[i];
    Vector<u8> indices8;
    gfx::pack_cluster_indices(std::span<const u32>(mesh.triangles.data(), mesh.triangles.size()),
                              indices8);
    deform_slot_table.assign(u64{3} * pair_count, gfx::k_no_pool_slot);
    for (u32 e = 0; e < entry_count; ++e) {
      if (visible[e * 2] != k_deformed) continue;
      deform_slot_table[e] = k_pool_bias + mesh.clusters[visible[e * 2 + 1]].vertex_offset;
    }
    constexpr VkBufferUsageFlags k_storage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    constexpr VkBufferUsageFlags k_address = k_storage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    REQUIRE(gfx::upload_buffer(device, mesh.clusters.data(),
                               cluster_count * sizeof(geometry::ClusterDesc), k_storage, clusters,
                               &error));
    REQUIRE(gfx::upload_buffer(device, mesh.vertices.data(), mesh.vertices.size() * sizeof(Vec3),
                               k_storage, vertices, &error));
    REQUIRE(
        gfx::upload_buffer(device, indices8.data(), indices8.size(), k_storage, index8, &error));
    REQUIRE(gfx::upload_buffer(device, instance_table, sizeof(instance_table), k_storage, instances,
                               &error));
    REQUIRE(gfx::upload_buffer(device, list.data(), list.size() * sizeof(u32), k_storage,
                               visible_buffer, &error));
    REQUIRE(gfx::upload_buffer(device, &count, sizeof(u32), k_storage, count_buffer, &error));
    REQUIRE(gfx::upload_buffer(device, &caster_count, sizeof(u32), k_storage, caster_count_buffer,
                               &error));
    REQUIRE(gfx::upload_buffer(device, &deform, sizeof(deform), k_storage, deform_table, &error));
    REQUIRE(gfx::upload_buffer(device, deform_slot_table.data(),
                               deform_slot_table.size() * sizeof(u32), k_address, deform_slots,
                               &error));
    REQUIRE(gfx::create_buffer(device, (u64{k_pool_bias} + mesh.vertices.size()) * 12, k_address,
                               false, pool, &error));
    gfx::MeshDesc mesh_desc{};
    mesh_desc.cluster_count = cluster_count;
    mesh_desc.deform_pool = pool.address;
    mesh_desc.deform_slots = deform_slots.address;
    REQUIRE(gfx::upload_buffer(device, &mesh_desc, sizeof(mesh_desc), k_storage, meshes, &error));
    REQUIRE(
        gfx::create_buffer(device, u64{pair_count} * sizeof(u32), k_address, false, slots, &error));
    REQUIRE(gfx::create_buffer(device, sizeof(u32) * k_instances * 3,
                               k_address | VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, instance_counts,
                               &error));
    REQUIRE(gfx::create_buffer(device, gfx::k_cluster_build_record_bytes * pair_count, k_address,
                               true, records, &error));
    REQUIRE(gfx::create_buffer(device, sizeof(u32) * gfx::k_cluster_record_count_words, k_address,
                               true, record_count, &error));
    REQUIRE(gfx::create_buffer(device, gfx::k_cluster_blas_record_bytes * k_instances, k_address,
                               true, blas_records, &error));
    module = gfx::create_shader_module(device, shaders::k_clas_records_spirv,
                                       shaders::k_clas_records_spirv_size, &error);
    REQUIRE(module != VK_NULL_HANDLE);
    const u32 push = sizeof(gfx::ClusterRecordParams);
    REQUIRE_MESSAGE(
        gfx::create_compute_pipeline(device, module, "records_main", {}, push, bucket, &error),
        error);
    REQUIRE(gfx::create_compute_pipeline(device, module, "ranges_main", {}, push, ranges, &error));
    REQUIRE(gfx::create_compute_pipeline(device, module, "emit_main", {}, push, emit, &error));
    ready = true;
    return true;
  }

  // Records the three passes with a set sized for `capacity` structures.
  void run(u32 capacity) {
    std::memset(records.mapped, 0xcd, gfx::k_cluster_build_record_bytes * pair_count);
    std::memset(record_count.mapped, 0xcd, sizeof(u32) * gfx::k_cluster_record_count_words);
    std::memset(blas_records.mapped, 0xcd, gfx::k_cluster_blas_record_bytes * k_instances);
    gfx::ClusterRecordParams params{};
    params.clusters = clusters.address;
    params.vertices = vertices.address;
    params.indices8 = index8.address;
    params.instances = instances.address;
    params.meshes = meshes.address;
    params.visible = visible_buffer.address;
    params.visible_count = count_buffer.address;
    params.slots = slots.address;
    params.instance_counts = instance_counts.address;
    params.caster_count = caster_count_buffer.address;
    params.records = records.address;
    params.record_count = record_count.address;
    params.blas_records = blas_records.address;
    params.clas_addresses = k_fake_addresses;
    params.instance_count = k_instances;
    params.pair_count = pair_count;
    // One view: the run is the whole list and its count word is the first.
    params.mode = gfx::cluster_records_mode(1, false);
    params.capacity = capacity;
    REQUIRE(gfx::submit_immediate(
        device,
        [&](VkCommandBuffer cb) {
          vkCmdFillBuffer(cb, instance_counts.buffer, 0, VK_WHOLE_SIZE, 0);
          VkMemoryBarrier2 barrier{};
          barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
          barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
          barrier.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
          barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
          barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
          VkDependencyInfo dependency{};
          dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
          dependency.memoryBarrierCount = 1;
          dependency.pMemoryBarriers = &barrier;
          const gfx::ComputePipeline* passes[3] = {&bucket, &ranges, &emit};
          // The bucketing pass covers the drawn run and the caster run; the emit pass the slots.
          const u32 groups[3] = {(2 * pair_count + 63) / 64, 1, (pair_count + 63) / 64};
          for (u32 p = 0; p < 3; ++p) {
            vkCmdPipelineBarrier2(cb, &dependency);
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, passes[p]->pipeline);
            vkCmdPushConstants(cb, passes[p]->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                               sizeof(params), &params);
            vkCmdDispatch(cb, groups[p], 1, 1);
          }
        },
        &error));
  }

  const u32* drawn_out() const { return static_cast<const u32*>(instance_counts.mapped); }
  const u32* casters_out() const { return drawn_out() + k_instances; }
  const u32* first_out() const { return drawn_out() + 2 * k_instances; }
  u32 record_word(u32 i) const {
    u32 value = 0;
    std::memcpy(&value, static_cast<const u8*>(record_count.mapped) + i * sizeof(u32),
                sizeof(value));
    return value;
  }
  // The drawn entries and casters the list holds for instance `k`.
  u32 drawn_of(u32 k) const {
    u32 n = 0;
    for (u32 i = 0; i < count; ++i)
      n += visible[i * 2] == k ? 1u : 0u;
    return n;
  }
  u32 casters_of(u32 k) const {
    u32 n = 0;
    for (u32 i = 0; i < caster_count; ++i)
      n += caster_entries[i * 2] == k ? 1u : 0u;
    return n;
  }

  // Every record the set holds, checked against the CPU writer: byte-identical for the entry its
  // cluster id names, inside its instance's run, drawn clusters before casters, each listed entry
  // at most once. Returns how many of each were seen through `drawn_seen` and `casters_seen`.
  void check_records(u32 expected_records, Vector<u32>& seen) {
    auto listed = [&](u32 entry) {
      return entry < count || (entry >= caster_base && entry < caster_base + caster_count);
    };
    const auto* got = static_cast<const u8*>(records.mapped);
    u32 differing = 0;
    u32 misplaced = 0;
    u32 casters_opaque = 0;
    seen.assign(u64{3} * pair_count, 0u);
    for (u32 r = 0; r < expected_records; ++r) {
      u32 entry = 0;
      std::memcpy(&entry, got + u64{r} * gfx::k_cluster_build_record_bytes, sizeof(entry));
      REQUIRE(listed(entry));
      ++seen[entry];
      const u32 instance_index = entry_instance(entry);
      const bool caster = entry >= caster_base;
      // Inside the instance's run, and on the right side of it: drawn first, casters after.
      const u32 first = first_out()[instance_index];
      const u32 drawn = drawn_out()[instance_index];
      const u32 casters = casters_out()[instance_index];
      if (caster ? (r < first + drawn || r >= first + drawn + casters)
                 : (r < first || r >= first + drawn)) {
        ++misplaced;
      }
      u32 geometry_word = 0;  // the base geometry index, with the flags in the top three bits
      std::memcpy(&geometry_word, got + u64{r} * gfx::k_cluster_build_record_bytes + 12, 4);
      if (caster) casters_opaque += (geometry_word >> 29) == gfx::k_cluster_geometry_opaque;
      const geometry::ClusterDesc& desc = mesh.clusters[entry_cluster(entry)];
      gfx::ClusterBuildInput in;
      in.cluster_id = entry;  // the visible index is the cluster id and the base geometry index
      in.triangle_count = desc.triangle_count;
      in.vertex_count = desc.vertex_count;
      in.opaque = !caster;
      // A deformed instance's positions come out of the pool, at the block this frame gave **that
      // visible entry** — the word `deform_slots[entry]` holds.
      in.vertices = instance_index == k_deformed
                        ? pool.address + u64{deform_slot_table[entry]} * sizeof(Vec3)
                        : vertices.address + u64{desc.vertex_offset} * sizeof(Vec3);
      in.indices = index8.address + u64{desc.triangle_offset} * 3;
      u8 expected[gfx::k_cluster_build_record_bytes];
      gfx::write_cluster_build_records(std::span<const gfx::ClusterBuildInput>(&in, 1), expected);
      for (u32 b = 0; b < gfx::k_cluster_build_record_bytes; ++b)
        differing += got[u64{r} * gfx::k_cluster_build_record_bytes + b] != expected[b];
    }
    CHECK(differing == 0);
    CHECK(misplaced == 0);
    CHECK(casters_opaque == 0);
    // Nothing past the records built was written.
    CHECK(got[u64{expected_records} * gfx::k_cluster_build_record_bytes] == 0xcd);
  }

  // One bottom-level record per instance, over that instance's run of CLAS addresses.
  void check_blas_records() {
    const auto* blas_bytes = static_cast<const u8*>(blas_records.mapped);
    for (u32 k = 0; k < k_instances; ++k) {
      u32 blas_count = 0;
      u32 blas_stride = 0;
      u64 blas_references = 0;
      const u8* record = blas_bytes + u64{k} * gfx::k_cluster_blas_record_bytes;
      std::memcpy(&blas_count, record, 4);
      std::memcpy(&blas_stride, record + 4, 4);
      std::memcpy(&blas_references, record + 8, 8);
      CHECK(blas_count == drawn_out()[k] + casters_out()[k]);
      CHECK(blas_stride == 8);
      CHECK(blas_references == k_fake_addresses + u64{first_out()[k]} * 8);
    }
  }

  ~RecordsFixture() {
    if (!device.valid()) return;
    gfx::destroy_compute_pipeline(device, bucket);
    gfx::destroy_compute_pipeline(device, ranges);
    gfx::destroy_compute_pipeline(device, emit);
    if (module != VK_NULL_HANDLE) gfx::destroy_shader_module(device, module);
    for (gfx::BufferResource* b :
         {&clusters, &vertices, &index8, &instances, &visible_buffer, &count_buffer,
          &caster_count_buffer, &slots, &instance_counts, &records, &record_count, &blas_records,
          &meshes, &deform_table, &deform_slots, &pool}) {
      gfx::destroy_buffer(device, *b);
    }
    device.destroy();
  }
};

}  // namespace

TEST_CASE("clas records: the shader writes the same build records as the CPU") {
  RecordsFixture f;
  if (!f.create()) return;
  // A capacity that holds the whole scene, which is what nothing dropped means.
  f.run(f.pair_count);

  // The CPU reference: the same clusters, grouped by instance, drawn before casters, in the order
  // the shader's per-instance atomics can produce (within an instance the order is arbitrary, so
  // the records are compared as a set of entries rather than position by position).
  u32 total = 0;
  for (u32 k = 0; k < k_instances; ++k) {
    CHECK(f.drawn_out()[k] == f.drawn_of(k));
    CHECK(f.casters_out()[k] == f.casters_of(k));
    CHECK(f.first_out()[k] == total);
    total += f.drawn_of(k) + f.casters_of(k);
  }
  const u32 expected_records = f.count + f.caster_count;
  CHECK(total == expected_records);
  CHECK(f.record_word(0) == expected_records);  // built
  CHECK(f.record_word(1) == expected_records);  // wanted
  CHECK(f.record_word(2) == 0);                 // instances that lost their drawn clusters
  CHECK(f.record_word(3) == 0);                 // instances that lost their casters

  Vector<u32> seen;
  f.check_records(expected_records, seen);
  u32 casters_built = 0;
  u32 unseen = 0;
  for (u32 e = 0; e < seen.size(); ++e) {
    const bool listed = e < f.count || (e >= f.caster_base && e < f.caster_base + f.caster_count);
    unseen += seen[e] != (listed ? 1u : 0u);
    casters_built += e >= f.caster_base ? seen[e] : 0u;
  }
  CHECK(unseen == 0);
  CHECK(casters_built == f.caster_count);
  f.check_blas_records();
  MESSAGE("records for " << f.count << " drawn and " << f.caster_count << " caster entries of "
                         << f.pair_count << " pairs over " << k_instances
                         << " instances (one deformed) match the CPU");
}

TEST_CASE("clas records: a set sized under the frame keeps whole instances, casters last") {
  RecordsFixture f;
  if (!f.create()) return;
  // Instance 0 draws its even clusters and casts its odd ones; instance 1 draws its even ones.
  const u32 drawn0 = f.drawn_of(0);
  const u32 drawn1 = f.drawn_of(1);
  const u32 casters0 = f.casters_of(0);
  REQUIRE(drawn0 > 0);
  REQUIRE(drawn1 > 0);
  REQUIRE(casters0 > 0);
  const u32 wanted = drawn0 + drawn1 + casters0;

  // 1. Room for every drawn cluster and one caster short of the casters: the casters are the
  //    whole instance's or none, so instance 0's casters go and every drawn cluster stays.
  f.run(drawn0 + drawn1 + casters0 - 1);
  CHECK(f.record_word(0) == drawn0 + drawn1);
  CHECK(f.record_word(1) == wanted);
  CHECK(f.record_word(2) == 0);
  CHECK(f.record_word(3) == 1);
  CHECK(f.drawn_out()[0] == drawn0);
  CHECK(f.drawn_out()[1] == drawn1);
  CHECK(f.casters_out()[0] == 0);
  Vector<u32> seen;
  f.check_records(drawn0 + drawn1, seen);
  f.check_blas_records();

  // 2. Room for one instance's drawn clusters and not the other's: the first instance that fits
  //    keeps all of its own, the one that does not keeps nothing, and the casters of instance 0
  //    still come after every drawn cluster, taking what is left if they fit in it.
  f.run(drawn0);
  CHECK(f.record_word(0) == drawn0);
  CHECK(f.record_word(1) == wanted);
  CHECK(f.record_word(2) == 1);  // instance 1's drawn clusters did not fit
  CHECK(f.record_word(3) == 1);  // and nothing is left for instance 0's casters
  CHECK(f.drawn_out()[0] == drawn0);
  CHECK(f.drawn_out()[1] == 0);
  CHECK(f.casters_out()[0] == 0);
  f.check_records(drawn0, seen);
  f.check_blas_records();

  // 3. Room for instance 1's drawn clusters but not instance 0's, which are more: a large instance
  //    that does not fit does not starve a smaller one behind it, and it loses its casters with
  //    its drawn clusters — a part of its shadow is not kept without the rest.
  REQUIRE(drawn1 <= drawn0);
  f.run(drawn1);
  CHECK(f.record_word(0) == drawn1);
  CHECK(f.record_word(2) == 1);
  CHECK(f.record_word(3) == 0);  // counted with the instance, not again as casters
  CHECK(f.drawn_out()[0] == 0);
  CHECK(f.casters_out()[0] == 0);
  CHECK(f.drawn_out()[1] == drawn1);
  f.check_records(drawn1, seen);
  f.check_blas_records();
  MESSAGE("capacity " << drawn0 + drawn1 + casters0 - 1 << ", " << drawn0 << ", " << drawn1
                      << " of " << wanted << " wanted: whole instances kept, casters first to go");
}
