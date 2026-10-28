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
#include <domain/gfx/backend/vulkan/vulkan.h>
#include <domain/gfx/cluster_acceleration.h>
#include <domain/gfx/cluster_cull.h>
#include <domain/gfx/device.h>
#include <domain/gfx/frame.h>
#include <domain/gfx/render_graph.h>

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

// Everything before a command after it, which is all these tests ask of a barrier.
void full_barrier(gfx::CommandList cb) {
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
  vkCmdPipelineBarrier2(gfx::vk::native(cb), &dependency);
}

// The ranges' five passes (R106), as the renderer records them: the scan over blocks of instances
// (`ranges_count_main`, `ranges_main`), then — dispatched from words `ranges_main` writes into
// `instance_counts`, nothing unless the frame is past its capacity — the walk and the kept
// instances' scan. `cached` takes the variants a frame with the cluster cache records.
struct RangesPasses {
  gfx::ComputePipeline count, ranges, drop, kept_count, kept;

  bool create(const gfx::Device& device, gfx::ShaderModuleHandle module, bool cached,
              std::string* error) {
    const u32 push = sizeof(gfx::ClusterRecordParams);
    const char* names[5] = {cached ? "ranges_count_cached_main" : "ranges_count_main",
                            cached ? "ranges_cached_main" : "ranges_main",
                            cached ? "ranges_drop_cached_main" : "ranges_drop_main",
                            cached ? "ranges_kept_count_cached_main" : "ranges_kept_count_main",
                            cached ? "ranges_kept_cached_main" : "ranges_kept_main"};
    gfx::ComputePipeline* outs[5] = {&count, &ranges, &drop, &kept_count, &kept};
    for (u32 k = 0; k < 5; ++k) {
      if (!gfx::create_compute_pipeline(device, module, names[k], {}, push, *outs[k], error))
        return false;
    }
    return true;
  }
  void destroy(const gfx::Device& device) {
    for (gfx::ComputePipeline* p : {&count, &ranges, &drop, &kept_count, &kept})
      gfx::destroy_compute_pipeline(device, *p);
  }
  // A barrier before each of the five; `instance_counts` is the buffer `params` names.
  void record(gfx::CommandList cb, const gfx::ClusterRecordParams& params,
              const gfx::BufferResource& instance_counts) const {
    const u32 groups = gfx::cluster_ranges_groups(params.instance_count);
    const gfx::ComputePipeline* passes[5] = {&count, &ranges, &drop, &kept_count, &kept};
    for (u32 p = 0; p < 5; ++p) {
      full_barrier(cb);
      cb.bind_pipeline(gfx::BindPoint::Compute, passes[p]->pipeline);
      cb.push_constants(passes[p]->layout, gfx::ShaderStage::Compute, 0, sizeof(params), &params);
      if (p < 2) {
        cb.dispatch(groups, 1, 1);
      } else {
        cb.dispatch_indirect(instance_counts.buffer,
                             p == 2 ? gfx::cluster_ranges_drop_args(params.instance_count)
                                    : gfx::cluster_ranges_kept_args(params.instance_count));
      }
    }
  }
};

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
  // With deformed casters: the caster entries the pool gave no block, which are built not at all.
  Vector<u8> refused;
  gfx::BufferResource clusters, vertices, index8, instances, visible_buffer, count_buffer,
      caster_count_buffer, slots, instance_counts, records, record_count, blas_records, meshes,
      deform_table, deform_slots, pool;
  gfx::ShaderModuleHandle module = {};
  gfx::ComputePipeline bucket, emit;
  RangesPasses ranges;
  bool ready = false;

  u32 entry_instance(u32 entry) const { return list[u64{entry} * 2]; }
  u32 entry_cluster(u32 entry) const { return list[u64{entry} * 2 + 1]; }

  bool create(bool deformed_casters = false) {
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
    // instance is never cone-tested, so it has casters only outside the frustum: with
    // `deformed_casters`, its odd clusters too, every other one of which the pool gave a block
    // (renderer.md, "Shadows": the others are built not at all).
    for (u32 i = 1; i < cluster_count; i += 2) {
      caster_entries.push_back(0);
      caster_entries.push_back(i);
      if (deformed_casters) {
        caster_entries.push_back(k_deformed);
        caster_entries.push_back(i);
      }
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
    refused.assign(u64{3} * pair_count, u8{0});
    u32 deformed_casters_seen = 0;
    for (u32 j = 0; j < caster_count; ++j) {
      if (caster_entries[j * 2] != k_deformed) continue;
      const u32 entry = caster_base + j;
      if (deformed_casters_seen++ % 2 == 0) {
        deform_slot_table[entry] =
            k_pool_bias + mesh.clusters[caster_entries[j * 2 + 1]].vertex_offset;
      } else {
        refused[entry] = 1;
      }
    }
    constexpr gfx::BufferUsage k_storage = gfx::BufferUsage::Storage;
    constexpr gfx::BufferUsage k_address = k_storage | gfx::BufferUsage::ShaderDeviceAddress;
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
    REQUIRE(
        gfx::create_buffer(device, sizeof(u32) * gfx::cluster_instance_count_words(k_instances),
                           k_address | gfx::BufferUsage::TransferDst | gfx::BufferUsage::Indirect,
                           true, instance_counts, &error));
    REQUIRE(gfx::create_buffer(device, gfx::k_cluster_build_record_bytes * pair_count, k_address,
                               true, records, &error));
    REQUIRE(gfx::create_buffer(device, sizeof(u32) * gfx::k_cluster_record_count_words, k_address,
                               true, record_count, &error));
    REQUIRE(gfx::create_buffer(device, gfx::k_cluster_blas_record_bytes * k_instances, k_address,
                               true, blas_records, &error));
    module = gfx::create_shader_module(device, shaders::k_clas_records_spirv,
                                       shaders::k_clas_records_spirv_size, &error);
    REQUIRE(module.valid());
    const u32 push = sizeof(gfx::ClusterRecordParams);
    REQUIRE_MESSAGE(
        gfx::create_compute_pipeline(device, module, "records_main", {}, push, bucket, &error),
        error);
    REQUIRE_MESSAGE(ranges.create(device, module, false, &error), error);
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
    // One view: the run is the whole list and its count word is the first; past the capacity, the
    // drop order's scan.
    params.mode = gfx::cluster_records_mode(1, false, true);
    params.capacity = capacity;
    REQUIRE(gfx::submit_immediate(
        device,
        [&](gfx::CommandList cb) {
          cb.fill_buffer(instance_counts.buffer, 0, gfx::k_whole_size, 0);
          // The bucketing pass covers the drawn run and the caster run; the ranges the blocks of
          // instances; the emit pass the slots.
          const gfx::ComputePipeline* passes[2] = {&bucket, &emit};
          const u32 groups[2] = {(2 * pair_count + 63) / 64, (pair_count + 63) / 64};
          for (u32 p = 0; p < 2; ++p) {
            if (p == 1) ranges.record(cb, params, instance_counts);
            full_barrier(cb);
            cb.bind_pipeline(gfx::BindPoint::Compute, passes[p]->pipeline);
            cb.push_constants(passes[p]->layout, gfx::ShaderStage::Compute, 0, sizeof(params),
                              &params);
            cb.dispatch(groups[p], 1, 1);
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
      n += caster_entries[i * 2] == k && refused[caster_base + i] == 0 ? 1u : 0u;
    return n;
  }
  u32 refused_count() const {
    u32 n = 0;
    for (const u8 r : refused)
      n += r;
    return n;
  }
  // An entry the records hold: a counted drawn one, or a counted caster the pool did not refuse.
  bool listed(u32 entry) const {
    return entry < count ||
           (entry >= caster_base && entry < caster_base + caster_count && refused[entry] == 0);
  }

  // Every record the set holds, checked against the CPU writer: byte-identical for the entry its
  // cluster id names, inside its instance's run, drawn clusters before casters, each listed entry
  // at most once. Returns how many of each were seen through `drawn_seen` and `casters_seen`.
  void check_records(u32 expected_records, Vector<u32>& seen) {
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
    ranges.destroy(device);
    gfx::destroy_compute_pipeline(device, emit);
    if (module.valid()) gfx::destroy_shader_module(device, module);
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

TEST_CASE("clas records: a deformed caster reads its block, and one with none is not built") {
  // **The pool over the caster run** (renderer.md, "Shadows"; roadmap R84). Outside the frustum a
  // deformed instance is a caster too, and the caster run is allocated pool blocks like a drawn
  // run: a caster with a block is built from it, exactly as a drawn entry is, and one the budget
  // refused (`k_no_pool_slot`) is **not built at all** — its rest pose would cast where its pose
  // does not, and a wrong shadow is worse than a missing one. A drawn entry with no block still
  // draws its rest pose; the rule is the casters' alone.
  RecordsFixture f;
  if (!f.create(true)) return;
  REQUIRE(f.casters_of(k_deformed) > 0);
  REQUIRE(f.refused_count() > 0);
  f.run(f.pair_count);
  u32 total = 0;
  for (u32 k = 0; k < k_instances; ++k) {
    CHECK(f.drawn_out()[k] == f.drawn_of(k));
    CHECK(f.casters_out()[k] == f.casters_of(k));
    CHECK(f.first_out()[k] == total);
    total += f.drawn_of(k) + f.casters_of(k);
  }
  const u32 expected_records = f.count + f.caster_count - f.refused_count();
  CHECK(total == expected_records);
  CHECK(f.record_word(0) == expected_records);
  CHECK(f.record_word(1) == expected_records);  // a refused caster is not wanted either
  Vector<u32> seen;
  f.check_records(expected_records, seen);
  u32 unseen = 0;
  u32 refused_built = 0;
  for (u32 e = 0; e < seen.size(); ++e) {
    unseen += seen[e] != (f.listed(e) ? 1u : 0u);
    refused_built += f.refused[e] != 0 ? seen[e] : 0u;
  }
  CHECK(unseen == 0);
  CHECK(refused_built == 0);
  f.check_blas_records();
  MESSAGE(f.casters_of(k_deformed)
          << " deformed casters built from their blocks, " << f.refused_count()
          << " without one left out, beside " << f.casters_of(0) << " rigid casters");
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

namespace {

// **The capacity rule on the CPU**, as `drop_serial` in clas_records.slang writes it and as it was
// the whole of the ranges until R106: every instance kept when both kinds fit; otherwise a rank of
// the order at a time, its drawn clusters instance by instance — a whole instance or none, its
// casters going with its drawn clusters — then its casters from what is left, each kind (built
// this frame, or referenced from the cluster cache) against its own capacity; an instance the
// order does not name keeps nothing. Writes the kept counts, each instance's first record, its
// bottom-level record and the six count words, as the shader does.
struct RangesCase {
  u32 n = 0;
  Vector<u32> drawn, casters;
  Vector<u8> cached;  // per instance: referenced from the cache
  Vector<u32> order;  // {instance, rank} pairs, or empty for instance order
  u32 capacity = 0;
  u32 reference_capacity = 0;
  u64 references = 0;  // the cache's reference array, or 0 for no cache

  u64 wanted(bool of_cached) const {
    u64 w = 0;
    for (u32 i = 0; i < n; ++i)
      w += (cached[i] != 0) == of_cached ? u64{drawn[i]} + casters[i] : 0u;
    return w;
  }

  void reference(Vector<u32>& counts, Vector<u64>& blas, u32 words[6]) const {
    Vector<u32> d = drawn;
    Vector<u32> c = casters;
    Vector<u8> dm = cached;  // the marks the counts carry, each word its own
    Vector<u8> cm = cached;
    const u32 wanted_built = static_cast<u32>(wanted(false));
    const u32 wanted_cached = static_cast<u32>(wanted(true));
    u32 dropped_drawn = 0;
    u32 dropped_casters = 0;
    if (wanted_built > capacity || wanted_cached > reference_capacity) {
      u32 left[2] = {capacity, reference_capacity};
      const auto take = [&](bool kind, u32 value) {
        if (value > left[kind]) return false;
        left[kind] -= value;
        return true;
      };
      const u32 m = order.empty() ? n : static_cast<u32>(order.size() / 2);
      const auto entry = [&](u32 k) { return order.empty() ? k : order[2 * k]; };
      const auto rank = [&](u32 k) { return order.empty() ? 0u : order[2 * k + 1]; };
      Vector<u8> visited(n, u8{0});
      u32 begin = 0;
      while (begin < m) {
        u32 end = begin;
        while (end < m && rank(end) == rank(begin)) {
          const u32 i = entry(end);
          if (i < n) {
            if (!take(dm[i] != 0, d[i])) {
              d[i] = c[i] = 0;
              dm[i] = cm[i] = 0;
              ++dropped_drawn;
            }
            visited[i] = 1;
          }
          ++end;
        }
        for (u32 k = begin; k < end; ++k) {
          const u32 i = entry(k);
          if (i >= n) continue;
          if (!take(cm[i] != 0, c[i])) {
            c[i] = 0;
            cm[i] = 0;
            ++dropped_casters;
          }
        }
        begin = end;
      }
      for (u32 i = 0; i < n; ++i) {
        if (visited[i] == 0 && d[i] + c[i] != 0) {
          d[i] = c[i] = 0;
          dm[i] = cm[i] = 0;
          ++dropped_drawn;
        }
      }
    }
    counts.assign(u64{3} * n, 0u);  // a u64 count: (u32, u32) would read as two iterators
    blas.assign(2 * n, u64{0});
    u32 total[2] = {0, 0};
    for (u32 i = 0; i < n; ++i) {
      const bool kind = dm[i] != 0 || cm[i] != 0;
      counts[i] = d[i];
      counts[n + i] = c[i];
      counts[2 * n + i] = total[kind];
      blas[2 * i] = u64{d[i] + c[i]} | (u64{8} << 32);
      blas[2 * i + 1] = (kind ? references : k_fake_addresses) + u64{total[kind]} * 8;
      total[kind] += d[i] + c[i];
    }
    words[0] = total[0];
    words[1] = wanted_built;
    words[2] = dropped_drawn;
    words[3] = dropped_casters;
    words[4] = total[1];
    words[5] = wanted_cached;
  }
};

// Runs `c` through the five passes — the cached variants with a minimal cache when `c.references`
// is not zero: instances of mesh 0 rigid are cached, every other is not — and says how many of the
// kept counts, bottom-level records and count words differ from the rule's, and through `walked`
// whether a frame past its capacity took the scan (1) or its one-thread fallback (2); 0 if it fit,
// or if `scan` is false and the frame took the one-thread rule in `ranges_main`.
u32 run_ranges(const gfx::Device& device, gfx::ShaderModuleHandle module, const RangesCase& c,
               std::string& label, u32& walked, bool scan = true) {
  std::string error;
  const bool cached = c.references != 0;
  RangesPasses passes;
  REQUIRE_MESSAGE(passes.create(device, module, cached, &error), error);
  constexpr gfx::BufferUsage k_address =
      gfx::BufferUsage::Storage | gfx::BufferUsage::ShaderDeviceAddress;
  gfx::BufferResource counts, blas, words, order, instances, bases, scratch, args, tables;
  REQUIRE(gfx::create_buffer(device, sizeof(u32) * gfx::cluster_instance_count_words(c.n),
                             k_address | gfx::BufferUsage::Indirect, true, counts, &error));
  REQUIRE(gfx::create_buffer(device, gfx::k_cluster_blas_record_bytes * c.n, k_address, true, blas,
                             &error));
  REQUIRE(gfx::create_buffer(device, sizeof(u32) * gfx::k_cluster_record_count_words, k_address,
                             true, words, &error));
  std::memset(counts.mapped, 0, sizeof(u32) * gfx::cluster_instance_count_words(c.n));
  std::memcpy(counts.mapped, c.drawn.data(), sizeof(u32) * c.n);
  std::memcpy(static_cast<u32*>(counts.mapped) + c.n, c.casters.data(), sizeof(u32) * c.n);
  std::memset(blas.mapped, 0xcd, gfx::k_cluster_blas_record_bytes * c.n);
  std::memset(words.mapped, 0xcd, sizeof(u32) * gfx::k_cluster_record_count_words);
  gfx::ClusterRecordParams params{};
  params.instance_counts = counts.address;
  params.record_count = words.address;
  params.blas_records = blas.address;
  params.clas_addresses = k_fake_addresses;
  params.instance_count = c.n;
  params.mode = gfx::cluster_records_mode(1, false, scan);
  params.capacity = c.capacity;
  if (!c.order.empty()) {
    Vector<u32> laid(2 + c.order.size(), 0u);
    laid[0] = static_cast<u32>(c.order.size() / 2);
    std::memcpy(laid.data() + 2, c.order.data(), c.order.size() * sizeof(u32));
    REQUIRE(gfx::upload_buffer(device, laid.data(), laid.size() * sizeof(u32), k_address, order,
                               &error));
    params.drop_order = order.address;
  }
  if (cached) {
    Vector<gfx::InstanceDesc> table(c.n);
    for (u32 i = 0; i < c.n; ++i) {
      table[i].mesh = c.cached[i] != 0 ? 0u : 1u;
      table[i].deform = gfx::k_invalid_deform;
    }
    const u32 mesh_bases[2] = {0u, gfx::k_cluster_cache_empty};
    Vector<u32> zeros(gfx::k_cluster_cache_scratch_words + gfx::k_cluster_cache_args_words, 0u);
    REQUIRE(gfx::upload_buffer(device, table.data(), table.size() * sizeof(gfx::InstanceDesc),
                               k_address, instances, &error));
    REQUIRE(gfx::upload_buffer(device, mesh_bases, sizeof(mesh_bases), k_address, bases, &error));
    REQUIRE(gfx::upload_buffer(device, zeros.data(),
                               gfx::k_cluster_cache_scratch_words * sizeof(u32), k_address, scratch,
                               &error));
    REQUIRE(gfx::upload_buffer(device, zeros.data(), gfx::k_cluster_cache_args_words * sizeof(u32),
                               k_address, args, &error));
    gfx::ClusterCacheTables block{};
    block.bases = bases.address;
    block.references = c.references;
    block.scratch = scratch.address;
    block.args = args.address;
    block.mesh_count = 2;
    block.reference_capacity = c.reference_capacity;
    REQUIRE(gfx::create_buffer(device, sizeof(block), k_address, true, tables, &error));
    std::memcpy(tables.mapped, &block, sizeof(block));
    params.instances = instances.address;
    params.cache = tables.address;
  }
  REQUIRE(gfx::submit_immediate(
      device, [&](gfx::CommandList cb) { passes.record(cb, params, counts); }, &error));
  Vector<u32> expected;
  Vector<u64> expected_blas;
  u32 expected_words[6] = {};
  c.reference(expected, expected_blas, expected_words);
  u32 differing = 0;
  const auto* got = static_cast<const u32*>(counts.mapped);
  for (u32 w = 0; w < expected.size(); ++w)
    differing += got[w] != expected[w];
  differing +=
      std::memcmp(blas.mapped, expected_blas.data(), gfx::k_cluster_blas_record_bytes * c.n) != 0;
  differing += std::memcmp(words.mapped, expected_words, sizeof(expected_words)) != 0;
  // The drop words' state (clas_records.slang, `k_drop_state`): past the capacity, and by the rule.
  const u32 state = got[gfx::cluster_ranges_drop_args(c.n) / sizeof(u32) + 6];
  walked = (state & 1u) == 0 ? 0u : (state & 2u) != 0 ? 2u : 1u;
  const auto* got_words = static_cast<const u32*>(words.mapped);
  label = "kept " + std::to_string(got_words[0]) + "/" + std::to_string(got_words[4]) + " of " +
          std::to_string(got_words[1]) + "/" + std::to_string(got_words[5]) + ", dropped " +
          std::to_string(got_words[2]) + " drawn and " + std::to_string(got_words[3]) +
          " casters (the rule: " + std::to_string(expected_words[0]) + "/" +
          std::to_string(expected_words[4]) + ", " + std::to_string(expected_words[2]) + ", " +
          std::to_string(expected_words[3]) + ")";
  for (gfx::BufferResource* b :
       {&counts, &blas, &words, &order, &instances, &bases, &scratch, &args, &tables})
    gfx::destroy_buffer(device, *b);
  passes.destroy(device);
  return differing;
}

// Counts from a fixed generator, many of them zero.
RangesCase random_case(u32 n, u32 seed, bool with_cache) {
  RangesCase c;
  c.n = n;
  u32 state = seed;
  const auto next = [&](u32 bound) {
    state = state * 1664525u + 1013904223u;
    return (state >> 8) % bound;
  };
  c.drawn.resize(n);
  c.casters.resize(n);
  c.cached.resize(n);
  for (u32 i = 0; i < n; ++i) {
    c.drawn[i] = next(4) == 0 ? 0u : next(40);
    c.casters[i] = next(3) == 0 ? next(12) : 0u;
    c.cached[i] = with_cache && next(3) == 0 ? u8{1} : u8{0};
  }
  if (with_cache) c.references = 0x40000000ull;
  return c;
}

// A drop order as the renderer lays one out: ranks rising, a rank's run of any length, every
// instance named at most once — here most of them, in a scrambled order, and a few entries past
// the instances, which name nothing.
void random_order(RangesCase& c, u32 seed) {
  u32 state = seed;
  const auto next = [&](u32 bound) {
    state = state * 22695477u + 1u;
    return (state >> 8) % bound;
  };
  Vector<u32> named;
  for (u32 i = 0; i < c.n; ++i) {
    if (next(10) != 0) named.push_back(i);
  }
  for (u32 k = named.size(); k > 1; --k)
    std::swap(named[k - 1], named[next(k)]);
  for (u32 extra = 0; extra < 3 && named.size() + 3 <= c.n; ++extra)
    named.push_back(c.n + extra * 7);  // past the instances: no instance, no items
  u32 rank = 0;
  for (u32 k = 0; k < named.size(); ++k) {
    if (next(40) == 0) rank += 1 + next(3);
    c.order.push_back(named[k]);
    c.order.push_back(rank);
  }
}

}  // namespace

TEST_CASE("clas records: the ranges over many instances are the one-thread rule's, word for word") {
  // The ranges are a device-wide scan (R106): `ranges_count_main` sums each block of 1,024
  // instances, a group a block, and `ranges_main`, over the same groups, has each group sum the
  // block sums for the totals and its own offset and write its block when the frame fits the set;
  // past the capacity three more passes, dispatched indirectly, walk the order and write what it
  // kept. Over 1,337 instances — two blocks, the second short, and four instances to a thread that
  // do not divide them — and over 70,001, 69 blocks, with counts from a fixed generator, many of
  // them zero, every capacity from room to spare to a third of what is wanted must write exactly
  // what the rule writes: the kept counts, each instance's first record, its bottom-level record,
  // and the count words. Here with no order: the instances in their own order, one rank.
  gfx::Device device;
  if (!gfx_test::open_device(device)) return;
  std::string error;
  gfx::ShaderModuleHandle module = gfx::create_shader_module(
      device, shaders::k_clas_records_spirv, shaders::k_clas_records_spirv_size, &error);
  REQUIRE(module.valid());
  u32 differing = 0;
  for (const u32 n : {1337u, 70001u}) {
    RangesCase c = random_case(n, 12345u, false);
    const u32 wanted = static_cast<u32>(c.wanted(false));
    for (const u32 capacity : {wanted + 100, wanted, wanted - 1, wanted / 3}) {
      c.capacity = capacity;
      // The scan, and — as a frame records when none before it dropped anything — the one thread.
      for (const bool scan : {true, false}) {
        std::string label;
        u32 walked = 0;
        const u32 here = run_ranges(device, module, c, label, walked, scan);
        CHECK_MESSAGE(here == 0, n << " instances, capacity " << capacity
                                   << (scan ? "" : ", one thread") << ": " << label);
        CHECK(walked == (scan && capacity < wanted ? 1u : 0u));
        differing += here;
      }
    }
    MESSAGE(n << " instances wanting " << wanted
              << " records: the blocks' scan and the walk write the rule's words at four "
                 "capacities");
  }
  CHECK(differing == 0);
  gfx::destroy_shader_module(device, module);
  device.destroy();
}

TEST_CASE("clas records: past the capacity the walk keeps what the rule keeps, rank by rank") {
  // **The capacity rule as a scan** (R106; renderer.md, "The ray tracing chain's memory"). Until
  // R106 a frame past its capacity walked every instance on one thread — 34 to 44 ms of the forest
  // proof of concept's chain — and now it lays the drop order out as one sequence of drawn and
  // caster items, takes whole blocks of it until the crossing, and walks item by item only blocks
  // that keep part of themselves. Here against the rule on the CPU, word for word: a drop order of
  // ranks rising in runs of every length, a tenth of the instances not named (they keep nothing)
  // and entries past the instances (which name nothing); the cluster cache's instances spending
  // their own capacity beside the frame's; capacities from room to spare to almost none, either
  // kind or both short; and an order whose ranks fall, which the scan does not take and the rule
  // walks on one thread as it always did.
  gfx::Device device;
  if (!gfx_test::open_device(device)) return;
  std::string error;
  gfx::ShaderModuleHandle module = gfx::create_shader_module(
      device, shaders::k_clas_records_spirv, shaders::k_clas_records_spirv_size, &error);
  REQUIRE(module.valid());
  u32 differing = 0;
  u32 cases = 0;
  for (const u32 n : {700u, 5003u, 70001u}) {
    for (const bool with_cache : {false, true}) {
      RangesCase c = random_case(n, 777u + n, with_cache);
      random_order(c, 31u * n + (with_cache ? 1u : 0u));
      const u32 built = static_cast<u32>(c.wanted(false));
      const u32 refs = static_cast<u32>(c.wanted(true));
      const u32 capacities[5][2] = {{built + 5, refs + 5},
                                    {built - built / 4, refs + 5},
                                    {built + 5, refs / 2},
                                    {built / 2, refs / 3},
                                    {7, 3}};
      for (const auto& capacity : capacities) {
        if (!with_cache && capacity[1] != refs + 5 && capacity[0] == built + 5) continue;
        c.capacity = capacity[0];
        c.reference_capacity = capacity[1];
        std::string label;
        u32 walked = 0;
        const u32 here = run_ranges(device, module, c, label, walked);
        CHECK_MESSAGE(here == 0, n << " instances" << std::string(with_cache ? " with a cache" : "")
                                   << ", capacity " << capacity[0] << "/" << capacity[1] << ": "
                                   << label);
        const bool over = capacity[0] < built || (with_cache && capacity[1] < refs);
        CHECK(walked == (over ? 1u : 0u));  // the scan, never the one-thread rule
        if (n == 70001u && capacity[0] == built / 2) MESSAGE(n << " instances: " << label);
        differing += here;
        ++cases;
      }
    }
  }
  // Ranks that fall: the scan's precondition broken, the one-thread rule takes the frame.
  {
    RangesCase c = random_case(3001u, 99u, true);
    random_order(c, 5u);
    std::swap(c.order[1], c.order[c.order.size() - 1]);  // the first entry's rank past the last's
    c.order[1] += 1000;
    c.capacity = static_cast<u32>(c.wanted(false) / 2);
    c.reference_capacity = static_cast<u32>(c.wanted(true) / 2);
    std::string label;
    u32 walked = 0;
    const u32 here = run_ranges(device, module, c, label, walked);
    CHECK_MESSAGE(here == 0, "falling ranks: " << label);
    CHECK(walked == 2u);  // the one-thread rule
    differing += here;
    ++cases;
  }
  CHECK(differing == 0);
  MESSAGE(cases << " cases past or at the capacity: the walk writes the rule's words in every one");
  gfx::destroy_shader_module(device, module);
  device.destroy();
}
