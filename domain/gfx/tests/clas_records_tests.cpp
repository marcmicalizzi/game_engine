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
// The second case holds the **capacity** rule (docs/subsystems/renderer.md, "Past the capacity, by
// size on screen"): the set is sized by the frame, and a frame that wants more than it holds keeps
// whole instances by a prefix of their keep keys, `gfx::rt_keep`'s rule. The shader needs no ray
// tracing feature, so both run on any device with a driver.
#include "raster_path.h"

#include <domain/geometry/cluster.h>
#include <domain/gfx/backend/vulkan/vulkan.h>
#include <domain/gfx/cluster_acceleration.h>
#include <domain/gfx/cluster_cull.h>
#include <domain/gfx/device.h>
#include <domain/gfx/frame.h>
#include <domain/gfx/render_graph.h>
#include <domain/gfx/rt_keep.h>

#include <doctest/doctest.h>

#include <cstring>
#include <shaders/clas_records.spv.h>
#include <shaders/tlas_references.spv.h>
#include <string>
#include <vector>

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

// The ranges' seven passes (R106, R174), as the renderer records them: the scan over blocks of
// instances (`ranges_count_main`, `ranges_main`), then — dispatched from words `ranges_main` writes
// into `instance_counts`, nothing unless the frame is past its capacity — the cut's three passes
// and the kept instances' scan. `cached` takes the variants a frame with the cluster cache
// records. The keep block is this object's, written and cleared before every run as the
// renderer's "keep clear" pass does; `eye` and `mesh_bounds` are what it names.
struct RangesPasses {
  gfx::ComputePipeline count, ranges, cut, ties, fine, kept_count, kept;
  gfx::BufferResource block, tables;  // the ClusterKeepTables; the cut words, then the histograms
  gfx::FrameEye eye;
  u64 mesh_bounds = 0;

  bool create(const gfx::Device& device, gfx::ShaderModuleHandle module, bool cached,
              std::string* error) {
    const u32 push = sizeof(gfx::ClusterRecordParams);
    const char* names[7] = {cached ? "ranges_count_cached_main" : "ranges_count_main",
                            cached ? "ranges_cached_main" : "ranges_main",
                            "keep_cut_main",
                            "keep_ties_main",
                            "keep_fine_main",
                            cached ? "ranges_kept_count_cached_main" : "ranges_kept_count_main",
                            cached ? "ranges_kept_cached_main" : "ranges_kept_main"};
    gfx::ComputePipeline* outs[7] = {&count, &ranges, &cut, &ties, &fine, &kept_count, &kept};
    for (u32 k = 0; k < 7; ++k) {
      if (!gfx::create_compute_pipeline(device, module, names[k], {}, push, *outs[k], error))
        return false;
    }
    constexpr gfx::BufferUsage k_usage = gfx::BufferUsage::Storage |
                                         gfx::BufferUsage::ShaderDeviceAddress |
                                         gfx::BufferUsage::TransferDst;
    return gfx::create_buffer(device, sizeof(gfx::ClusterKeepTables), k_usage, false, block,
                              error) &&
           gfx::create_buffer(device, 64 + gfx::k_rt_keep_histogram_bytes, k_usage, false, tables,
                              error);
  }
  void destroy(const gfx::Device& device) {
    for (gfx::ComputePipeline* p : {&count, &ranges, &cut, &ties, &fine, &kept_count, &kept})
      gfx::destroy_compute_pipeline(device, *p);
    gfx::destroy_buffer(device, block);
    gfx::destroy_buffer(device, tables);
  }
  // The keep block written and its tables cleared, then a barrier before each of the seven;
  // `instance_counts` is the buffer `params` names.
  void record(gfx::CommandList cb, const gfx::ClusterRecordParams& given,
              const gfx::BufferResource& instance_counts) const {
    gfx::ClusterKeepTables keep;
    keep.eye = eye;
    keep.mesh_bounds = mesh_bounds;
    keep.keys = instance_counts.address + gfx::cluster_ranges_keys_offset(given.instance_count);
    keep.cut = tables.address;
    keep.histograms = tables.address + 64;
    keep.slot_count = given.instance_count;
    cb.update_buffer(block.buffer, 0, sizeof(keep), &keep);
    cb.fill_buffer(tables.buffer, 0, tables.size, 0u);
    gfx::ClusterRecordParams params = given;
    params.keep = block.address;
    const u32 groups = gfx::cluster_ranges_groups(params.instance_count);
    const gfx::ComputePipeline* passes[7] = {&count, &ranges,     &cut, &ties,
                                             &fine,  &kept_count, &kept};
    for (u32 p = 0; p < 7; ++p) {
      full_barrier(cb);
      cb.bind_pipeline(gfx::BindPoint::Compute, passes[p]->pipeline);
      cb.push_constants(passes[p]->layout, gfx::ShaderStage::Compute, 0, sizeof(params), &params);
      if (p < 2) {
        cb.dispatch(groups, 1, 1);
      } else {
        cb.dispatch_indirect(instance_counts.buffer,
                             p == 2 || p == 4
                                 ? gfx::cluster_ranges_drop_args(params.instance_count)
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
  // The caster policy's `traced = false` (R110): the instance flagged `k_instance_untraced`, or
  // ~0u for none, and whether the frame is traced-primary (`k_cluster_records_untraced_drawn`).
  u32 untraced = ~0u;
  bool untraced_drawn = false;
  gfx::BufferResource clusters, vertices, index8, instances, visible_buffer, count_buffer,
      caster_count_buffer, slots, instance_counts, records, record_count, blas_records, meshes,
      deform_table, deform_slots, pool;
  gfx::ShaderModuleHandle module = {};
  gfx::ComputePipeline bucket, emit;
  RangesPasses ranges;
  bool ready = false;

  u32 entry_instance(u32 entry) const { return list[u64{entry} * 2]; }
  u32 entry_cluster(u32 entry) const { return list[u64{entry} * 2 + 1]; }

  bool create(bool deformed_casters = false, u32 untraced_instance = ~0u) {
    if (!gfx_test::open_device(device)) return false;
    untraced = untraced_instance;
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
    if (untraced < k_instances) instance_table[untraced].flags |= gfx::k_instance_untraced;
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
    params.mode = gfx::cluster_records_mode(1, false, untraced_drawn);
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
  // Whether the records take instance `k`'s drawn entries and its casters (R110): an untraced
  // instance's drawn ones in a traced-primary frame alone, its casters never.
  bool takes_drawn(u32 k) const { return k != untraced || untraced_drawn; }
  bool takes_casters(u32 k) const { return k != untraced; }
  // The drawn entries and casters the list holds for instance `k`, as the records take them.
  u32 drawn_of(u32 k) const {
    if (!takes_drawn(k)) return 0;
    u32 n = 0;
    for (u32 i = 0; i < count; ++i)
      n += visible[i * 2] == k ? 1u : 0u;
    return n;
  }
  u32 casters_of(u32 k) const {
    if (!takes_casters(k)) return 0;
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
    if (entry < count) return takes_drawn(entry_instance(entry));
    return entry >= caster_base && entry < caster_base + caster_count && refused[entry] == 0 &&
           takes_casters(entry_instance(entry));
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

TEST_CASE(
    "clas records: an untraced instance is built as drawn entries in a traced picture alone") {
  // **The caster policy's two contracts** (renderer.md, "The caster policy"; R110). The deformed
  // instance is flagged `k_instance_untraced` and given casters in the list (which the cull never
  // writes for it; the records must refuse them anyway). In a frame whose picture is rasterized it
  // takes nothing — no slot, no count, no record, the other instance's records byte for byte as
  // without it — and in a traced-primary frame its drawn entries are built from their pool blocks
  // like any deformed instance's, and its casters still are not.
  for (const bool primary : {false, true}) {
    CAPTURE(primary);
    RecordsFixture f;
    if (!f.create(true, k_deformed)) return;
    f.untraced_drawn = primary;
    REQUIRE(f.casters_of(0) > 0);
    f.run(f.pair_count);
    u32 total = 0;
    for (u32 k = 0; k < k_instances; ++k) {
      CHECK(f.drawn_out()[k] == f.drawn_of(k));
      CHECK(f.casters_out()[k] == f.casters_of(k));
      CHECK(f.first_out()[k] == total);
      total += f.drawn_of(k) + f.casters_of(k);
    }
    CHECK(f.casters_out()[k_deformed] == 0);
    CHECK((f.drawn_out()[k_deformed] > 0) == primary);
    CHECK(f.record_word(0) == total);
    CHECK(f.record_word(1) == total);  // what it does not take, it does not want
    Vector<u32> seen;
    f.check_records(total, seen);
    u32 unseen = 0;
    for (u32 e = 0; e < seen.size(); ++e)
      unseen += seen[e] != (f.listed(e) ? 1u : 0u);
    CHECK(unseen == 0);
    f.check_blas_records();
    MESSAGE(std::string(primary ? "traced picture: " : "rasterized picture: ")
            << f.drawn_out()[k_deformed] << " drawn entries of the untraced instance built, "
            << total << " records in all");
  }
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

  // 2. and 3. Room for one instance's drawn clusters and not both: the fixture names no spheres, so
  //    both take the top size and their placements' hashes order them (renderer.md, "Past the
  //    capacity, by size on screen"). What each keeps is `gfx::rt_keep`'s with the keys the frame
  //    wrote: whole instances, a dropped instance's casters with it, and instance 0's casters after
  //    every drawn cluster. With the larger keyed first and past the capacity alone, nothing is
  //    kept: the prefix's price, which first-fit did not pay.
  for (const u32 capacity : {drawn0, drawn1}) {
    f.run(capacity);
    const u32* keys = f.drawn_out() + gfx::cluster_ranges_keys_offset(k_instances) / sizeof(u32);
    CHECK(keys[0] == gfx::k_rt_keep_q_max);
    CHECK(keys[2] == gfx::k_rt_keep_q_max);
    const gfx::RtKeepItem items[2] = {{drawn0, casters0, false, {keys[0], keys[1]}},
                                      {drawn1, 0, false, {keys[2], keys[3]}}};
    u32 kept_drawn[2] = {};
    u32 kept_casters[2] = {};
    const gfx::RtKeepResult want =
        gfx::rt_keep(items, capacity, 0, false, kept_drawn, kept_casters);
    CHECK(f.record_word(0) == want.words.built);
    CHECK(f.record_word(1) == wanted);
    CHECK(f.record_word(2) == want.words.dropped_drawn);
    CHECK(f.record_word(3) == want.words.dropped_casters);
    CHECK(want.words.dropped_drawn >= 1);
    CHECK(f.drawn_out()[0] == kept_drawn[0]);
    CHECK(f.drawn_out()[1] == kept_drawn[1]);
    CHECK(f.casters_out()[0] == kept_casters[0]);
    f.check_records(want.words.built, seen);
    f.check_blas_records();
  }
  MESSAGE("capacity " << drawn0 + drawn1 + casters0 - 1 << ", " << drawn0 << ", " << drawn1
                      << " of " << wanted << " wanted: whole instances kept, casters first to go");
}

namespace {

// **The capacity rule on the CPU** is `gfx::rt_keep` (rt_keep.h; renderer.md, "Past the capacity,
// by size on screen"): every instance keyed by its placement and the eye, each kind (built this
// frame, or referenced from the cluster cache) spent as a prefix of the keys against its own
// capacity, equal keys kept or dropped together. The case writes from it what the shader writes:
// the kept counts, each instance's first record, its bottom-level record and the six count words.
struct RangesCase {
  u32 n = 0;
  Vector<u32> drawn, casters;
  Vector<u8> cached;  // per instance: referenced from the cache
  // Per instance: its placement, which its keep key is made from; the cached ones are of mesh 0,
  // the others of mesh 1, whose spheres the shader reads as the meshes' bounds.
  Vector<gfx::InstanceDesc> placements;
  Vec4 spheres[2] = {Vec4(0.0f, 0.0f, 0.0f, 1.5f), Vec4(0.0f, 0.0f, 0.0f, 1.5f)};
  gfx::FrameEye eye;
  u32 capacity = 0;
  u32 reference_capacity = 0;
  u64 references = 0;  // the cache's reference array, or 0 for no cache
  // `gfx::k_cluster_records_drawn_first` (R143): every drawn structure before any caster.
  bool drawn_first = false;

  u64 wanted(bool of_cached) const {
    u64 w = 0;
    for (u32 i = 0; i < n; ++i)
      w += (cached[i] != 0) == of_cached ? u64{drawn[i]} + casters[i] : 0u;
    return w;
  }
  gfx::RtKeepKey key(u32 i) const {
    return gfx::rt_keep_key(placements[i], eye, spheres[placements[i].mesh]);
  }

  void reference(Vector<u32>& counts, Vector<u64>& blas, u32 words[6]) const {
    std::vector<gfx::RtKeepItem> items(n);
    for (u32 i = 0; i < n; ++i)
      items[i] = {drawn[i], casters[i], cached[i] != 0, key(i)};
    std::vector<u32> d(n), c(n);
    const gfx::RtKeepResult kept =
        gfx::rt_keep(items, capacity, reference_capacity, drawn_first, d, c);
    counts.assign(u64{3} * n, 0u);  // a u64 count: (u32, u32) would read as two iterators
    blas.assign(2 * n, u64{0});
    u32 total[2] = {0, 0};
    for (u32 i = 0; i < n; ++i) {
      // An instance that lost its drawn structures lost its counts and the mark with them.
      const bool lost = drawn[i] != 0 && d[i] == 0;
      const u32 kind = cached[i] != 0 && !lost ? 1u : 0u;
      counts[i] = d[i];
      counts[n + i] = c[i];
      counts[2 * n + i] = total[kind];
      blas[2 * i] = u64{d[i] + c[i]} | (u64{8} << 32);
      blas[2 * i + 1] = (kind != 0 ? references : k_fake_addresses) + u64{total[kind]} * 8;
      total[kind] += d[i] + c[i];
    }
    words[0] = kept.words.built;
    words[1] = kept.words.wanted;
    words[2] = kept.words.dropped_drawn;
    words[3] = kept.words.dropped_casters;
    words[4] = kept.words.references_kept;
    words[5] = kept.words.references_wanted;
  }
};

// Runs `c` through the seven passes — the cached variants with a minimal cache when `c.references`
// is not zero: instances of mesh 0 rigid are cached, every other is not — on `passes` (made here
// when null, so a caller can run several frames through one keep block) and says how many of the
// kept counts, bottom-level records, count words and keep keys differ from the rule's; `over` says
// whether the frame was past its capacity.
u32 run_ranges(const gfx::Device& device, gfx::ShaderModuleHandle module, const RangesCase& c,
               std::string& label, bool& over, RangesPasses* shared = nullptr) {
  std::string error;
  const bool cached = c.references != 0;
  RangesPasses own;
  RangesPasses& passes = shared != nullptr ? *shared : own;
  if (shared == nullptr) REQUIRE_MESSAGE(passes.create(device, module, cached, &error), error);
  constexpr gfx::BufferUsage k_address =
      gfx::BufferUsage::Storage | gfx::BufferUsage::ShaderDeviceAddress;
  gfx::BufferResource counts, blas, words, instances, bounds, bases, scratch, args, tables;
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
  REQUIRE(gfx::upload_buffer(device, c.placements.data(),
                             c.placements.size() * sizeof(gfx::InstanceDesc), k_address, instances,
                             &error));
  REQUIRE(gfx::upload_buffer(device, c.spheres, sizeof(c.spheres), k_address, bounds, &error));
  gfx::ClusterRecordParams params{};
  params.instances = instances.address;
  params.instance_counts = counts.address;
  params.record_count = words.address;
  params.blas_records = blas.address;
  params.clas_addresses = k_fake_addresses;
  params.instance_count = c.n;
  params.mode = gfx::cluster_records_mode(1, false, false, c.drawn_first);
  params.capacity = c.capacity;
  passes.eye = c.eye;
  passes.mesh_bounds = bounds.address;
  if (cached) {
    const u32 mesh_bases[2] = {0u, gfx::k_cluster_cache_empty};
    Vector<u32> zeros(gfx::k_cluster_cache_scratch_words + gfx::k_cluster_cache_args_words, 0u);
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
  // The drop words' state (clas_records.slang, `k_drop_state`): past the capacity.
  const u32 state = got[gfx::cluster_ranges_drop_args(c.n) / sizeof(u32) + 6];
  over = (state & 1u) != 0;
  // Past the capacity the keys are written, and to the bit the CPU's (`gfx::rt_keep_key`).
  u32 keys_differing = 0;
  if (over) {
    const u32* keys = got + gfx::cluster_ranges_keys_offset(c.n) / sizeof(u32);
    for (u32 i = 0; i < c.n; ++i) {
      if (c.drawn[i] + c.casters[i] == 0) continue;
      const gfx::RtKeepKey want = c.key(i);
      keys_differing += keys[2 * i] != want.q || keys[2 * i + 1] != want.h ? 1u : 0u;
    }
  }
  differing += keys_differing;
  const auto* got_words = static_cast<const u32*>(words.mapped);
  label = "kept " + std::to_string(got_words[0]) + "/" + std::to_string(got_words[4]) + " of " +
          std::to_string(got_words[1]) + "/" + std::to_string(got_words[5]) + ", dropped " +
          std::to_string(got_words[2]) + " drawn and " + std::to_string(got_words[3]) +
          " casters (the rule: " + std::to_string(expected_words[0]) + "/" +
          std::to_string(expected_words[4]) + ", " + std::to_string(expected_words[2]) + ", " +
          std::to_string(expected_words[3]) + "), " + std::to_string(keys_differing) +
          " keys not the CPU's";
  for (gfx::BufferResource* b :
       {&counts, &blas, &words, &instances, &bounds, &bases, &scratch, &args, &tables})
    gfx::destroy_buffer(device, *b);
  if (shared == nullptr) passes.destroy(device);
  return differing;
}

// Counts from a fixed generator, many of them zero, and placements from a few dozen: twelve
// translations (cells about the eye's) times five linear parts, so many instances share a
// placement exactly (equal keys) and many share a distance but not a 3x4 (one size, other hashes).
RangesCase random_case(u32 n, u32 seed, bool with_cache) {
  RangesCase c;
  c.n = n;
  u32 state = seed;
  const auto next = [&](u32 bound) {
    state = state * 1664525u + 1013904223u;
    return (state >> 8) % bound;
  };
  c.eye.cell = {1, 0, -2};
  c.eye.local = Vec3(10.5f, 1.75f, 33.25f);
  c.drawn.resize(n);
  c.casters.resize(n);
  c.cached.resize(n);
  c.placements.resize(n);
  for (u32 i = 0; i < n; ++i) {
    c.drawn[i] = next(4) == 0 ? 0u : next(40);
    c.casters[i] = next(3) == 0 ? next(12) : 0u;
    c.cached[i] = with_cache && next(3) == 0 ? u8{1} : u8{0};
    const u32 t = next(12);
    const u32 r = next(5);
    gfx::InstanceDesc& p = c.placements[i];
    const f32 s = 1.0f + 0.25f * static_cast<f32>(r);
    p.rows[0] = Vec4(s, 0.1f * static_cast<f32>(r), 0.0f, 3.5f * static_cast<f32>(t));
    p.rows[1] = Vec4(0.0f, s, 0.2f * static_cast<f32>(r), 1.25f);
    p.rows[2] = Vec4(-0.1f * static_cast<f32>(r), 0.0f, s, 60.0f - 4.75f * static_cast<f32>(t));
    p.cell = {1 + static_cast<i32>(t % 3) - 1, 0, -2 + static_cast<i32>(t / 3) - 1};
    p.scale_max = s;
    p.mesh = c.cached[i] != 0 ? 0u : 1u;
    p.deform = gfx::k_invalid_deform;
  }
  if (with_cache) c.references = 0x40000000ull;
  return c;
}

}  // namespace

TEST_CASE("clas records: past the capacity the cut keeps what rt_keep keeps") {
  // **The capacity rule as a cut** (R174; renderer.md, "Past the capacity, by size on screen").
  // Past its capacity a frame keys every instance and spends each capacity as a prefix of the keys:
  // the ranges key and add into a level-1 histogram, one group finds the crossing bin, the ties
  // pass adds the crossing bin's items at their hash, one group finds the cut, and the kept passes
  // write what is keyed below it. Here against `gfx::rt_keep` word for word — the kept counts,
  // each instance's first record, its bottom-level record, the count words — and the keep keys to
  // the bit against `gfx::rt_keep_key` given the same spheres: over 1,337, 5,003 and 70,001
  // instances (blocks short and whole), with and without the cache's references beside the
  // frame's own, both layouts of the key, and capacities from room to spare to almost none, either
  // kind or both short. The placements are few, so equal keys and one size with several hashes
  // straddle the capacities. With drawn first, a capacity of exactly the drawn total keeps every
  // drawn structure and no caster (R143).
  gfx::Device device;
  if (!gfx_test::open_device(device)) return;
  std::string error;
  gfx::ShaderModuleHandle module = gfx::create_shader_module(
      device, shaders::k_clas_records_spirv, shaders::k_clas_records_spirv_size, &error);
  REQUIRE(module.valid());
  u32 differing = 0;
  u32 cases = 0;
  for (const u32 n : {1337u, 5003u, 70001u}) {
    for (const bool with_cache : {false, true}) {
      for (const bool drawn_first : {false, true}) {
        RangesCase c = random_case(n, 777u + n + (with_cache ? 1u : 0u), with_cache);
        c.drawn_first = drawn_first;
        const u32 built = static_cast<u32>(c.wanted(false));
        const u32 refs = static_cast<u32>(c.wanted(true));
        u32 drawn_total = 0;
        for (u32 i = 0; i < n; ++i)
          drawn_total += c.cached[i] == 0 ? c.drawn[i] : 0u;
        const u32 capacities[7][2] = {{built + 5, refs + 5},  {built, refs},
                                      {built - 1, refs + 5},  {built - built / 4, refs + 5},
                                      {built + 5, refs / 2},  {built / 2, refs / 3},
                                      {drawn_total, refs + 5}};
        for (const auto& capacity : capacities) {
          c.capacity = capacity[0];
          c.reference_capacity = capacity[1];
          std::string label;
          bool over = false;
          const u32 here = run_ranges(device, module, c, label, over);
          CHECK_MESSAGE(here == 0, n << " instances" << (with_cache ? ", the cache" : "")
                                     << (drawn_first ? ", drawn first" : "") << ", capacity "
                                     << capacity[0] << "/" << capacity[1] << ": " << label);
          CHECK(over == (capacity[0] < built || (with_cache && capacity[1] < refs)));
          if (n == 70001u && capacity[0] == built / 2) MESSAGE(n << " instances: " << label);
          differing += here;
          ++cases;
        }
        // Drawn first at exactly the drawn total: every drawn structure, no caster.
        if (drawn_first) {
          c.capacity = drawn_total;
          c.reference_capacity = refs + 5;
          Vector<u32> counts;
          Vector<u64> blas;
          u32 words[6] = {};
          c.reference(counts, blas, words);
          CHECK(words[0] == drawn_total);
          CHECK(words[2] == 0);
        }
      }
    }
  }
  CHECK(differing == 0);
  MESSAGE(cases << " cases at or past the capacity: the cut writes rt_keep's words in every one");
  gfx::destroy_shader_module(device, module);
  device.destroy();
}

TEST_CASE("clas records: two orders of the same instances keep the same instances") {
  // The point of the rule (R174): the same placements listed in another order keep the same
  // placements, at one capacity. 5,003 instances with the cache, reversed and then rotated by a
  // third, at a capacity that drops: each instance's kept counts follow it, and the count words
  // are equal.
  gfx::Device device;
  if (!gfx_test::open_device(device)) return;
  std::string error;
  gfx::ShaderModuleHandle module = gfx::create_shader_module(
      device, shaders::k_clas_records_spirv, shaders::k_clas_records_spirv_size, &error);
  REQUIRE(module.valid());
  RangesCase base = random_case(5003u, 2026u, true);
  base.capacity = static_cast<u32>(base.wanted(false) / 2);
  base.reference_capacity = static_cast<u32>(base.wanted(true) / 2);
  Vector<u32> base_counts;
  Vector<u64> base_blas;
  u32 base_words[6] = {};
  base.reference(base_counts, base_blas, base_words);
  CHECK(base_words[2] > 0);  // the capacity drops
  for (u32 order = 0; order < 2; ++order) {
    RangesCase c = random_case(5003u, 2026u, true);
    c.capacity = base.capacity;
    c.reference_capacity = base.reference_capacity;
    for (u32 i = 0; i < c.n; ++i) {
      const u32 from = order == 0 ? c.n - 1 - i : (i + c.n / 3) % c.n;
      c.drawn[i] = base.drawn[from];
      c.casters[i] = base.casters[from];
      c.cached[i] = base.cached[from];
      c.placements[i] = base.placements[from];
    }
    std::string label;
    bool over = false;
    CHECK_MESSAGE(run_ranges(device, module, c, label, over) == 0, label);
    CHECK(over);
    Vector<u32> counts;
    Vector<u64> blas;
    u32 words[6] = {};
    c.reference(counts, blas, words);
    CHECK(std::memcmp(words, base_words, sizeof(words)) == 0);
    u32 moved = 0;
    for (u32 i = 0; i < c.n; ++i) {
      const u32 from = order == 0 ? c.n - 1 - i : (i + c.n / 3) % c.n;
      moved +=
          counts[i] != base_counts[from] || counts[c.n + i] != base_counts[c.n + from] ? 1u : 0u;
    }
    CHECK(moved == 0);
  }
  gfx::destroy_shader_module(device, module);
  device.destroy();
}

TEST_CASE("clas records: consecutive frames past and within the capacity cut from cleared tables") {
  // The cut's tables are cleared every frame (renderer.md, "The cut's tables are cleared every
  // frame"): four frames through one keep block — past, past at another capacity, fitting, past —
  // each writes rt_keep's words, so nothing one frame added is read by the next.
  gfx::Device device;
  if (!gfx_test::open_device(device)) return;
  std::string error;
  gfx::ShaderModuleHandle module = gfx::create_shader_module(
      device, shaders::k_clas_records_spirv, shaders::k_clas_records_spirv_size, &error);
  REQUIRE(module.valid());
  RangesPasses passes;
  REQUIRE_MESSAGE(passes.create(device, module, true, &error), error);
  RangesCase c = random_case(3001u, 99u, true);
  const u32 built = static_cast<u32>(c.wanted(false));
  const u32 refs = static_cast<u32>(c.wanted(true));
  const u32 frames[4][2] = {
      {built / 2, refs / 2}, {built / 3, refs + 1}, {built, refs}, {built - 1, refs / 4}};
  for (const auto& frame : frames) {
    c.capacity = frame[0];
    c.reference_capacity = frame[1];
    std::string label;
    bool over = false;
    CHECK_MESSAGE(run_ranges(device, module, c, label, over, &passes) == 0,
                  "capacity " << frame[0] << "/" << frame[1] << ": " << label);
    CHECK(over == (frame[0] < built || frame[1] < refs));
  }
  passes.destroy(device);
  gfx::destroy_shader_module(device, module);
  device.destroy();
}

TEST_CASE("cluster set: what a set reserves does not follow its totals") {
  // **What the frame's own set reserves** (R143; docs/experiments/rt-small-instances-2026-11-10.md,
  // "What a structure of the frame's own set reserves"). Every set here is created for clusters of
  // 124 triangles and 64 vertices each, and a forest's clusters average a quarter of those
  // triangles: its leaves are cards, four vertices to two triangles, so a cluster is full at 64
  // vertices and about 30 triangles. The size query takes the totals as well as the count
  // (`ClusterSetLimits::max_total_*`); this says what the driver does with them. On the RTX 5090
  // at driver 610 it does nothing: 6,144 bytes a cluster, 8,832 with micromaps, at a half and a
  // quarter of either total as at the whole, so a set sized by the triangles its frames build
  // would reserve what it reserves now. No number is pinned — it is the driver's — and the one
  // thing held is that fewer never reserve more; the messages say when a driver starts to follow.
  gfx::Device device;
  if (!gfx_test::open_device(device)) return;
  gfx::ClusterAsProperties props;
  if (!gfx::cluster_as_properties(device, props)) {
    MESSAGE("no cluster acceleration structures");
    device.destroy();
    return;
  }
  constexpr u32 k_clusters = 65536;
  u64 full = 0;
  for (const bool micromaps : {false, true}) {
    for (const u32 quarter : {4u, 2u, 1u}) {  // the share of the totals, in quarters
      for (const bool vertices_too : {false, true}) {
        if (quarter == 4 && vertices_too) continue;
        gfx::ClusterSetLimits limits;
        limits.max_clusters = k_clusters;
        limits.max_geometry_index = k_clusters - 1;
        limits.opacity_micromaps = micromaps;
        limits.max_total_triangles = k_clusters * 124u / 4u * quarter;
        if (vertices_too) limits.max_total_vertices = k_clusters * 64u / 4u * quarter;
        gfx::ClusterBuildSizes sizes;
        const bool ok = gfx::cluster_set_build_sizes(device, limits, sizes);
        if (!ok) {
          MESSAGE("the driver gave no size at " << quarter << "/4 of the triangles");
          continue;
        }
        if (quarter == 4) full = sizes.data_bytes;
        MESSAGE(std::string(micromaps ? "with micromaps, " : "plain, ")
                << quarter << "/4 of the triangles"
                << std::string(vertices_too ? " and of the vertices" : "") << ": "
                << sizes.data_bytes / k_clusters << " bytes a cluster of storage, "
                << sizes.scratch_bytes / k_clusters << " of scratch");
        CHECK(sizes.data_bytes <= full);  // fewer triangles never reserve more
      }
    }
  }
  device.destroy();
}

TEST_CASE("cluster set: what a set reserves follows its per-cluster most") {
  // **What a smaller per-cluster most would buy** (T129; docs/experiments/
  // memory-commit-accounting.md, "The traced chain's storage"). The totals buy nothing (above), so
  // the one lever left in implicit mode is the per-cluster most a set is created with: a second set
  // for the clusters of meshes that never pass a smaller one (a forest's leaf cards: 64 vertices,
  // about 30 triangles). This asks the driver what a cluster reserves at each most; no number is
  // pinned — it is the driver's — and the one thing held is that a smaller most never reserves
  // more.
  gfx::Device device;
  if (!gfx_test::open_device(device)) return;
  gfx::ClusterAsProperties props;
  if (!gfx::cluster_as_properties(device, props)) {
    MESSAGE("no cluster acceleration structures");
    device.destroy();
    return;
  }
  constexpr u32 k_clusters = 65536;
  for (const bool micromaps : {false, true}) {
    u64 previous = 0;
    for (const u32 triangles : {124u, 96u, 64u, 48u, 32u, 16u}) {
      for (const u32 vertices : {64u, 32u}) {
        gfx::ClusterSetLimits limits;
        limits.max_clusters = k_clusters;
        limits.max_geometry_index = k_clusters - 1;
        limits.opacity_micromaps = micromaps;
        limits.max_triangles_per_cluster = triangles;
        limits.max_vertices_per_cluster = vertices;
        gfx::ClusterBuildSizes sizes;
        if (!gfx::cluster_set_build_sizes(device, limits, sizes)) {
          MESSAGE("the driver gave no size at " << triangles << " triangles, " << vertices
                                                << " vertices");
          continue;
        }
        MESSAGE(std::string(micromaps ? "with micromaps, " : "plain, ")
                << triangles << " triangles and " << vertices << " vertices a cluster: "
                << sizes.data_bytes / k_clusters << " bytes a cluster of storage, "
                << sizes.scratch_bytes / k_clusters << " of scratch");
        if (vertices == 64u) {
          if (previous != 0) CHECK(sizes.data_bytes <= previous);  // a smaller most, never more
          previous = sizes.data_bytes;
        }
      }
    }
  }
  device.destroy();
}

TEST_CASE("tlas references: the bottom-level build is over the instances that reference anything") {
  // **The bottom-level build is over what the frame drew** (R143; renderer.md, "The bottom-level
  // build is over what the frame drew"). The ranges write a bottom-level record per instance slot
  // and the build was over all of them: `blas_compact_main` copies the records that reference
  // anything to the front of a second array, counts them, and says where each instance's went;
  // `tlas_references_main` then takes an instance's address from that place. 5,000 instances, one
  // in nine with a record: the count is theirs, the compacted records are theirs — each once, in
  // whatever order the atomic gave — every instance's top-level reference is the address of its own
  // record's structure, an instance with none is inactive, and a static form's keeps its mesh's.
  gfx::Device device;
  std::string error;
  if (!gfx_test::open_device(device)) return;
  constexpr u32 k_count = 5000;
  constexpr u32 k_words = static_cast<u32>(gfx::k_instance_record_bytes / 4);
  const auto has = [](u32 i) { return i % 9 == 4; };
  const auto is_static = [](u32 i) { return i % 50 == 7; };
  Vector<gfx::InstanceDesc> instances(k_count);
  Vector<u64> blas;
  blas.resize(2 * k_count);  // zeroed: a (u64, u64) constructor would read as two iterators
  Vector<u32> records(u64{k_count} * k_words, u32{0});
  u32 expected_built = 0;
  for (u32 i = 0; i < k_count; ++i) {
    instances[i].mesh = i % 3;
    instances[i].flags = is_static(i) ? gfx::k_instance_static_rt : 0u;
    records[u64{i} * k_words + 12] = i | (0xffu << 24);
    if (has(i)) {
      blas[2 * i] = u64{i % 5 + 1} | (u64{8} << 32);  // count, stride
      blas[2 * i + 1] = 0x100000u + u64{i} * 64u;     // references
      ++expected_built;
    }
  }
  const u64 static_addresses[3] = {0x7000000u, 0x7000100u, 0x7000200u};
  constexpr gfx::BufferUsage k_address =
      gfx::BufferUsage::Storage | gfx::BufferUsage::ShaderDeviceAddress;
  gfx::BufferResource instance_buffer, record_buffer, address_buffer, blas_buffer, dense_buffer,
      place_buffer, count_buffer, static_buffer;
  REQUIRE(gfx::upload_buffer(device, instances.data(), instances.size() * sizeof(gfx::InstanceDesc),
                             k_address, instance_buffer, &error));
  REQUIRE(gfx::create_buffer(device, records.size() * sizeof(u32), k_address, true, record_buffer,
                             &error));
  std::memcpy(record_buffer.mapped, records.data(), records.size() * sizeof(u32));
  REQUIRE(gfx::upload_buffer(device, blas.data(), blas.size() * sizeof(u64), k_address, blas_buffer,
                             &error));
  REQUIRE(gfx::upload_buffer(device, static_addresses, sizeof(static_addresses), k_address,
                             static_buffer, &error));
  REQUIRE(gfx::create_buffer(device, gfx::k_cluster_blas_record_bytes * k_count, k_address, true,
                             dense_buffer, &error));
  REQUIRE(gfx::create_buffer(device, sizeof(u32) * k_count, k_address, true, place_buffer, &error));
  REQUIRE(
      gfx::create_buffer(device, sizeof(u64) * k_count, k_address, true, address_buffer, &error));
  REQUIRE(gfx::create_buffer(device, sizeof(u32) * 4, k_address, true, count_buffer, &error));
  std::memset(dense_buffer.mapped, 0xcd, gfx::k_cluster_blas_record_bytes * k_count);
  std::memset(place_buffer.mapped, 0xcd, sizeof(u32) * k_count);
  std::memset(count_buffer.mapped, 0, sizeof(u32) * 4);  // the ranges clear it every frame
  const gfx::ShaderModuleHandle module = gfx::create_shader_module(
      device, shaders::k_tlas_references_spirv, shaders::k_tlas_references_spirv_size, &error);
  REQUIRE(module.valid());
  gfx::ComputePipeline compact, references;
  REQUIRE_MESSAGE(gfx::create_compute_pipeline(device, module, "blas_compact_main", {},
                                               sizeof(gfx::TlasReferenceParams), compact, &error),
                  error);
  REQUIRE_MESSAGE(
      gfx::create_compute_pipeline(device, module, "tlas_references_main", {},
                                   sizeof(gfx::TlasReferenceParams), references, &error),
      error);
  gfx::TlasReferenceParams params{};
  params.addresses = address_buffer.address;
  params.records = record_buffer.address;
  params.count = k_count;
  params.instances = instance_buffer.address;
  params.static_addresses = static_buffer.address;
  params.blas_records = blas_buffer.address;
  params.blas_dense = dense_buffer.address;
  params.blas_slots = place_buffer.address;
  params.blas_count = count_buffer.address;
  const u32 groups =
      (k_count + gfx::k_tlas_references_workgroup - 1) / gfx::k_tlas_references_workgroup;
  REQUIRE(gfx::submit_immediate(
      device,
      [&](gfx::CommandList cb) {
        cb.bind_pipeline(gfx::BindPoint::Compute, compact.pipeline);
        cb.push_constants(compact.layout, gfx::ShaderStage::Compute, 0, sizeof(params), &params);
        cb.dispatch(groups, 1, 1);
      },
      &error));
  const u32 built = *static_cast<const u32*>(count_buffer.mapped);
  CHECK(built == expected_built);
  // The build: each compacted record's structure at an address that names its references, which is
  // how the check below tells whose it is.
  const auto* dense = static_cast<const u64*>(dense_buffer.mapped);
  auto* addresses = static_cast<u64*>(address_buffer.mapped);
  for (u32 place = 0; place < built && place < k_count; ++place)
    addresses[place] = dense[2 * place + 1] + 0x40000000u;
  REQUIRE(gfx::submit_immediate(
      device,
      [&](gfx::CommandList cb) {
        cb.bind_pipeline(gfx::BindPoint::Compute, references.pipeline);
        cb.push_constants(references.layout, gfx::ShaderStage::Compute, 0, sizeof(params), &params);
        cb.dispatch(groups, 1, 1);
      },
      &error));
  const auto* places = static_cast<const u32*>(place_buffer.mapped);
  const auto* out = static_cast<const u32*>(record_buffer.mapped);
  Vector<u8> taken(k_count, u8{0});
  u32 wrong_place = 0;
  u32 wrong_record = 0;
  u32 wrong_reference = 0;
  for (u32 i = 0; i < k_count; ++i) {
    u64 reference = 0;
    std::memcpy(&reference, out + u64{i} * k_words + 14, sizeof(reference));
    u64 expected = 0;
    if (has(i)) {
      const u32 place = places[i];
      if (place >= built || taken[place] != 0) {
        ++wrong_place;
      } else {
        taken[place] = 1;
        wrong_record += dense[2 * place] != blas[2 * i] || dense[2 * place + 1] != blas[2 * i + 1];
      }
      expected = blas[2 * i + 1] + 0x40000000u;
    } else {
      wrong_place += places[i] != gfx::k_cluster_blas_none ? 1u : 0u;
    }
    if (is_static(i)) expected = static_addresses[i % 3];
    wrong_reference += reference != expected ? 1u : 0u;
  }
  MESSAGE(k_count << " instances, " << built << " with a record: " << wrong_place << " places, "
                  << wrong_record << " compacted records and " << wrong_reference
                  << " top-level references differ");
  CHECK(wrong_place == 0);
  CHECK(wrong_record == 0);
  CHECK(wrong_reference == 0);
  gfx::destroy_compute_pipeline(device, compact);
  gfx::destroy_compute_pipeline(device, references);
  gfx::destroy_shader_module(device, module);
  for (gfx::BufferResource* b : {&instance_buffer, &record_buffer, &address_buffer, &blas_buffer,
                                 &dense_buffer, &place_buffer, &count_buffer, &static_buffer})
    gfx::destroy_buffer(device, *b);
  device.destroy();
}

TEST_CASE("tlas references: a record's mask is its instance's ray mask, and a hole's stays 0") {
  // **The ray mask** (gfx.md, "The ray mask"; R110). `tlas_references.slang` writes each live
  // top-level record's mask from its instance's flags every frame — every bit but the caster bit
  // for an instance the caster policy leaves untraced, 0xff for every other — and leaves a record
  // whose mask is 0, a dynamic scene's hole, as it is. Every flag word from 0 to 255 is an
  // instance here, against `gfx::instance_ray_mask` on the CPU, which is what pins the shader's
  // constants (shaders/scene.slang) to the C++ ones; the custom index beside the mask is kept.
  gfx::Device device;
  std::string error;
  if (!gfx_test::open_device(device)) return;
  static_assert(gfx::instance_ray_mask(0u) == 0xffu);
  static_assert(gfx::instance_ray_mask(gfx::k_instance_untraced) == 0xfeu);
  static_assert((gfx::instance_ray_mask(gfx::k_instance_untraced) & gfx::k_ray_mask_caster) == 0);
  constexpr u32 k_count = 256;
  constexpr u32 k_words = static_cast<u32>(gfx::k_instance_record_bytes / 4);
  Vector<gfx::InstanceDesc> instances(k_count);
  Vector<u32> records(u64{k_count} * k_words, u32{0});
  Vector<u64> addresses(k_count, u64{0});
  const auto hole = [](u32 i) { return i % 7 == 3; };
  for (u32 i = 0; i < k_count; ++i) {
    instances[i].flags = i;
    instances[i].mesh = 0;
    addresses[i] = 0x10000u + u64{i} * 0x100u;
    records[u64{i} * k_words + 12] = ((i * 977u) & 0xffffffu) | (hole(i) ? 0u : 0xffu << 24);
  }
  constexpr gfx::BufferUsage k_address =
      gfx::BufferUsage::Storage | gfx::BufferUsage::ShaderDeviceAddress;
  gfx::BufferResource instance_buffer, record_buffer, address_buffer;
  REQUIRE(gfx::upload_buffer(device, instances.data(), instances.size() * sizeof(gfx::InstanceDesc),
                             k_address, instance_buffer, &error));
  REQUIRE(gfx::create_buffer(device, records.size() * sizeof(u32), k_address, true, record_buffer,
                             &error));
  std::memcpy(record_buffer.mapped, records.data(), records.size() * sizeof(u32));
  REQUIRE(gfx::upload_buffer(device, addresses.data(), addresses.size() * sizeof(u64), k_address,
                             address_buffer, &error));
  const gfx::ShaderModuleHandle module = gfx::create_shader_module(
      device, shaders::k_tlas_references_spirv, shaders::k_tlas_references_spirv_size, &error);
  REQUIRE(module.valid());
  gfx::ComputePipeline pass;
  REQUIRE_MESSAGE(gfx::create_compute_pipeline(device, module, "tlas_references_main", {},
                                               sizeof(gfx::TlasReferenceParams), pass, &error),
                  error);
  gfx::TlasReferenceParams params{};
  params.addresses = address_buffer.address;
  params.records = record_buffer.address;
  params.count = k_count;
  params.instances = instance_buffer.address;
  REQUIRE(gfx::submit_immediate(
      device,
      [&](gfx::CommandList cb) {
        cb.bind_pipeline(gfx::BindPoint::Compute, pass.pipeline);
        cb.push_constants(pass.layout, gfx::ShaderStage::Compute, 0, sizeof(params), &params);
        cb.dispatch(
            (k_count + gfx::k_tlas_references_workgroup - 1) / gfx::k_tlas_references_workgroup, 1,
            1);
      },
      &error));
  const auto* out = static_cast<const u32*>(record_buffer.mapped);
  u32 wrong_mask = 0;
  u32 wrong_index = 0;
  u32 wrong_reference = 0;
  u32 untraced = 0;
  for (u32 i = 0; i < k_count; ++i) {
    const u32 word = out[u64{i} * k_words + 12];
    const u32 expected = hole(i) ? 0u : gfx::instance_ray_mask(i);
    wrong_mask += (word >> 24) != expected ? 1u : 0u;
    wrong_index += (word & 0xffffffu) != ((i * 977u) & 0xffffffu) ? 1u : 0u;
    u64 reference = 0;
    std::memcpy(&reference, out + u64{i} * k_words + 14, sizeof(reference));
    wrong_reference += reference != addresses[i] ? 1u : 0u;
    untraced += !hole(i) && (i & gfx::k_instance_untraced) != 0 ? 1u : 0u;
  }
  MESSAGE(k_count << " records, " << untraced << " live and untraced: " << wrong_mask << " masks, "
                  << wrong_index << " custom indices and " << wrong_reference
                  << " references differ");
  CHECK(untraced > 0);
  CHECK(wrong_mask == 0);
  CHECK(wrong_index == 0);
  CHECK(wrong_reference == 0);
  gfx::destroy_compute_pipeline(device, pass);
  gfx::destroy_shader_module(device, module);
  for (gfx::BufferResource* b : {&instance_buffer, &record_buffer, &address_buffer})
    gfx::destroy_buffer(device, *b);
  device.destroy();
}
