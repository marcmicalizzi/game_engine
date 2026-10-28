// The deformed-vertex pool's allocator (`deform_alloc.slang`, roadmap R104) against the
// one-workgroup allocator it replaced (`deform_alloc_reference.slang`), and both against the same
// rule written on the CPU here.
//
// The allocator became three passes — the tiles' scans, the tiles' cursors, the blocks — because
// one workgroup walking hundreds of thousands of entries cost 10 to 15 ms of the forest's frame.
// What it must not change is the answer: for every visible list, every budget and every sequence
// of dispatches the frame makes (three camera runs over several views, then the cascades' run),
// the slot table and the record (`gfx::DeformAlloc`) are the same words. The cases are the four
// shapes the placement rule has — everything fits; the budget crossed early, so most of the cut
// is past the crossing; crossed late; and a cluster bigger than the room left followed by small
// ones that still have to be placed — plus a cut large enough for passes 1 and 3 to stride, the
// cascades' rest pose (by flag, by distance and, since R103, by reach per cascade, never a terrain
// level), and ten runs of the same input to the same bytes.
//
// Needs a device with buffer device addresses and 64-bit integers in shaders, which every device
// the engine opens has.
#include "raster_path.h"

#include <domain/geometry/cluster.h>
#include <domain/gfx/backend/vulkan/vulkan.h>
#include <domain/gfx/cluster_cull.h>
#include <domain/gfx/device.h>

#include <doctest/doctest.h>

#include <cstring>
#include <random>
#include <shaders/deform_alloc.spv.h>
#include <shaders/deform_alloc_reference.spv.h>
#include <string>
#include <vector>

using namespace engine;

namespace {

constexpr u32 k_garbage = 0xEEEEEEEEu;

// One dispatch of the frame's sequence: what `scene_renderer.cpp` gives each run.
struct Dispatch {
  u32 views = 1;
  u32 first_entry = 0;
  u32 count_offset = 0;  // bytes into the count buffer of view 0's word
  bool reset = false;
  f32 rest_beyond_m = -1.0f;
  f32 rest_padding_m[4] = {};  // the cascades' rest pose per view (R103)
};

struct Case {
  const char* name = "";
  u32 pair_count = 0;  // one view's run is at most this long, and runs are this far apart
  u32 budget = 0;
  std::vector<geometry::ClusterDesc> clusters;
  std::vector<gfx::InstanceDesc> instances;
  std::vector<u32> visible;  // u32x2 per entry of the whole list
  std::vector<u32> counts;   // four words per view block, as the draw arguments are laid out
  std::vector<Dispatch> dispatches;
};

struct Result {
  std::vector<u32> slots;
  gfx::DeformAlloc record{};
  bool operator==(const Result& o) const {
    return slots == o.slots && std::memcmp(&record, &o.record, sizeof(record)) == 0;
  }
};

// The rule on the CPU, from the reference shader's definition: tiles of 256 per view, an entry
// placed when cursor + exclusive + need fits, the cursor advanced by the largest fitted inclusive.
Result cpu_allocate(const Case& c) {
  Result r;
  r.slots.assign(c.visible.size() / 2, k_garbage);
  u32 cursor = 0;
  for (const Dispatch& d : c.dispatches) {
    if (d.reset) {
      r.record = {};
      cursor = 0;
    }
    for (u32 v = 0; v < d.views; ++v) {
      const u32 count = std::min(c.counts[(d.count_offset / 4) + v * 4], c.pair_count);
      const u32 base = d.first_entry + v * c.pair_count;
      for (u32 tile = 0; tile < count; tile += 256) {
        u32 inclusive = 0;
        u32 placed = 0;
        for (u32 i = tile; i < count && i < tile + 256; ++i) {
          const u32 entry = base + i;
          const gfx::InstanceDesc& inst = c.instances[c.visible[entry * 2]];
          u32 need = inst.deform != gfx::k_invalid_deform
                         ? c.clusters[c.visible[entry * 2 + 1]].vertex_count
                         : 0u;
          // A terrain level is never at rest; past its species' flag and the distance, an
          // instance whose reach is under its cascade's length is (R103).
          if (need != 0 && d.rest_beyond_m >= 0.0f && inst.terrain == 0) {
            if ((inst.flags & gfx::k_instance_shadow_rest) != 0) {
              need = 0;
            } else if (d.rest_beyond_m > 0.0f) {
              const f32 x = inst.rows[0].w, y = inst.rows[1].w, z = inst.rows[2].w;
              if (x * x + y * y + z * z > d.rest_beyond_m * d.rest_beyond_m) need = 0;
            }
            const f32 reach = inst.bounds_padding * inst.scale_max;
            if (need != 0 && v < 4 && reach > 0.0f && reach < d.rest_padding_m[v]) need = 0;
          }
          const u32 start = cursor + inclusive;
          inclusive += need;
          const bool fits = need != 0 && start + need <= c.budget;
          r.slots[entry] = fits ? start : gfx::k_no_pool_slot;
          if (need != 0) {
            if (fits) {
              ++r.record.entries;
              placed = inclusive;
            } else {
              ++r.record.overflow_entries;
              r.record.overflow_vertices += need;
            }
          }
        }
        cursor += placed;
      }
    }
  }
  r.record.cursor = cursor;
  return r;
}

void compute_barrier(gfx::CommandList cb, VkPipelineStageFlags2 src, VkAccessFlags2 src_access,
                     VkPipelineStageFlags2 dst, VkAccessFlags2 dst_access) {
  VkMemoryBarrier2 barrier{};
  barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
  barrier.srcStageMask = src;
  barrier.srcAccessMask = src_access;
  barrier.dstStageMask = dst;
  barrier.dstAccessMask = dst_access;
  VkDependencyInfo dep{};
  dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
  dep.memoryBarrierCount = 1;
  dep.pMemoryBarriers = &barrier;
  vkCmdPipelineBarrier2(gfx::vk::native(cb), &dep);
}

constexpr VkPipelineStageFlags2 k_cs = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
constexpr VkAccessFlags2 k_rw =
    VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;

struct Allocators {
  gfx::ShaderModuleHandle module;
  gfx::ShaderModuleHandle reference_module;
  gfx::ComputePipeline tiles, scan, place, reference;

  bool create(const gfx::Device& device, std::string* error) {
    module = gfx::create_shader_module(device, shaders::k_deform_alloc_spirv,
                                       shaders::k_deform_alloc_spirv_size, error);
    reference_module =
        gfx::create_shader_module(device, shaders::k_deform_alloc_reference_spirv,
                                  shaders::k_deform_alloc_reference_spirv_size, error);
    constexpr u32 k_bytes = sizeof(gfx::DeformAllocParams);
    return module.valid() && reference_module.valid() &&
           gfx::create_compute_pipeline(device, module, "deform_alloc_tiles_main", {}, k_bytes,
                                        tiles, error) &&
           gfx::create_compute_pipeline(device, module, "deform_alloc_scan_main", {}, k_bytes, scan,
                                        error) &&
           gfx::create_compute_pipeline(device, module, "deform_alloc_place_main", {}, k_bytes,
                                        place, error) &&
           gfx::create_compute_pipeline(device, reference_module, "deform_alloc_reference_main", {},
                                        k_bytes, reference, error);
  }
  void destroy(const gfx::Device& device) {
    for (gfx::ComputePipeline* p : {&tiles, &scan, &place, &reference})
      gfx::destroy_compute_pipeline(device, *p);
    gfx::destroy_shader_module(device, module);
    gfx::destroy_shader_module(device, reference_module);
  }
};

// Runs the case's dispatch sequence through one allocator and reads the slots and record back.
bool run_gpu(const gfx::Device& device, const Allocators& a, const Case& c, bool reference,
             Result& out, std::string* error) {
  constexpr gfx::BufferUsage k_storage = gfx::BufferUsage::Storage;
  constexpr gfx::BufferUsage k_scratch =
      gfx::BufferUsage::Storage | gfx::BufferUsage::ShaderDeviceAddress |
      gfx::BufferUsage::TransferSrc | gfx::BufferUsage::TransferDst;
  const u64 slot_bytes = c.visible.size() / 2 * sizeof(u32);
  u32 max_views = 1;
  for (const Dispatch& d : c.dispatches)
    max_views = std::max(max_views, d.views);
  const u64 block_bytes =
      u64{std::max(1u, gfx::deform_alloc_tile_capacity(max_views, c.pair_count))} * 8;
  gfx::FrameEye eye{};  // at the origin: an instance's distance is its translation's length
  gfx::BufferResource clusters, instances, visible, counts, eye_buffer, slots, alloc, blocks,
      readback;
  bool ok =
      gfx::upload_buffer(device, c.clusters.data(), c.clusters.size() * sizeof(c.clusters[0]),
                         k_storage, clusters, error) &&
      gfx::upload_buffer(device, c.instances.data(), c.instances.size() * sizeof(c.instances[0]),
                         k_storage, instances, error) &&
      gfx::upload_buffer(device, c.visible.data(), c.visible.size() * sizeof(u32), k_storage,
                         visible, error) &&
      gfx::upload_buffer(device, c.counts.data(), c.counts.size() * sizeof(u32), k_storage, counts,
                         error) &&
      gfx::upload_buffer(device, &eye, sizeof(eye), k_storage, eye_buffer, error) &&
      gfx::create_buffer(device, slot_bytes, k_scratch, false, slots, error) &&
      gfx::create_buffer(device, sizeof(gfx::DeformAlloc), k_scratch, false, alloc, error) &&
      gfx::create_buffer(device, block_bytes, k_scratch, false, blocks, error) &&
      gfx::create_buffer(device, slot_bytes + sizeof(gfx::DeformAlloc),
                         gfx::BufferUsage::TransferDst, true, readback, error);
  if (ok) {
    ok = gfx::submit_immediate(
        device,
        [&](gfx::CommandList cb) {
          cb.fill_buffer(slots.buffer, 0, slot_bytes, k_garbage);
          cb.fill_buffer(alloc.buffer, 0, sizeof(gfx::DeformAlloc), k_garbage);
          cb.fill_buffer(blocks.buffer, 0, block_bytes, k_garbage);
          compute_barrier(cb, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, k_cs,
                          k_rw);
          for (const Dispatch& d : c.dispatches) {
            gfx::DeformAllocParams p{};
            p.clusters = clusters.address;
            p.instances = instances.address;
            p.visible = visible.address;
            p.visible_counts = counts.address + d.count_offset;
            p.slots = slots.address;
            p.alloc = alloc.address;
            p.pool_vertices = c.budget;
            p.pair_count = c.pair_count;
            p.views = d.views;
            p.first_entry = d.first_entry;
            p.reset = d.reset ? 1u : 0u;
            p.rest_beyond_m = d.rest_beyond_m;
            for (u32 v = 0; v < 4; ++v)
              p.rest_padding_m[v] = d.rest_padding_m[v];
            p.eye = eye_buffer.address;
            p.blocks = blocks.address;
            p.groups = gfx::deform_alloc_group_count(d.views, c.pair_count);
            const auto dispatch = [&](const gfx::ComputePipeline& pipe, u32 groups) {
              cb.bind_pipeline(gfx::BindPoint::Compute, pipe.pipeline);
              cb.push_constants(pipe.layout, gfx::ShaderStage::Compute, 0, sizeof(p), &p);
              cb.dispatch(groups, 1, 1);
              compute_barrier(cb, k_cs, k_rw, k_cs, k_rw);
            };
            if (reference) {
              dispatch(a.reference, 1);
            } else {
              dispatch(a.tiles, p.groups);
              dispatch(a.scan, 1);
              dispatch(a.place, p.groups);
            }
          }
          compute_barrier(cb, k_cs, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                          VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
          cb.copy_buffer(slots.buffer, readback.buffer, gfx::BufferCopy{0, 0, slot_bytes});
          cb.copy_buffer(alloc.buffer, readback.buffer,
                         gfx::BufferCopy{0, slot_bytes, sizeof(gfx::DeformAlloc)});
        },
        error);
  }
  if (ok) {
    const auto* words = static_cast<const u8*>(readback.mapped);
    out.slots.resize(slot_bytes / sizeof(u32));
    std::memcpy(out.slots.data(), words, slot_bytes);
    std::memcpy(&out.record, words + slot_bytes, sizeof(gfx::DeformAlloc));
  }
  for (gfx::BufferResource* b :
       {&clusters, &instances, &visible, &counts, &eye_buffer, &slots, &alloc, &blocks, &readback})
    gfx::destroy_buffer(device, *b);
  return ok;
}

// A frame's population: `cluster_count` clusters of 1 to 255 vertices (with a few of zero), and
// `instance_count` instances of which about two in three are deformed; some deformed ones carry
// the cascades' rest flag, and every one stands somewhere within 200 m of the eye.
void make_population(std::mt19937& rng, u32 cluster_count, u32 instance_count, Case& c) {
  std::uniform_int_distribution<u32> verts(1, 255);
  std::uniform_int_distribution<u32> pick(0, 99);
  std::uniform_real_distribution<f32> place(-115.0f, 115.0f);
  for (u32 i = 0; i < cluster_count; ++i) {
    geometry::ClusterDesc d{};
    d.vertex_count = pick(rng) < 3 ? 0u : verts(rng);
    c.clusters.push_back(d);
  }
  for (u32 i = 0; i < instance_count; ++i) {
    gfx::InstanceDesc inst{};
    const u32 roll = pick(rng);
    inst.deform = roll < 66 ? i : gfx::k_invalid_deform;
    if (roll < 10) inst.flags |= gfx::k_instance_shadow_rest;
    // R103: a reach from rest of 0 to 8 m (a fifth state none), and a few terrain levels, which
    // the cascades never put at rest.
    inst.bounds_padding = pick(rng) < 20 ? 0.0f : static_cast<f32>(pick(rng)) * 0.04f;
    inst.scale_max = 1.0f + static_cast<f32>(pick(rng)) * 0.01f;
    if (roll >= 60 && roll < 66) inst.terrain = 1;
    inst.rows[0].w = place(rng);
    inst.rows[1].w = place(rng);
    inst.rows[2].w = place(rng);
    c.instances.push_back(inst);
  }
}

// The frame's dispatch sequence for `views` views: three camera runs, run-major, then one
// cascades' dispatch of `cascades` runs with the rest pose past `rest_m`. Each view's (and
// cascade's) count is drawn from `counts` (clamped by the shader to `pair_count`) and its
// entries filled with random pairs.
void make_frame(std::mt19937& rng, u32 views, u32 cascades, f32 rest_m,
                const std::vector<u32>& view_counts, Case& c) {
  const u32 runs = 3 * views + cascades;
  c.visible.assign(u64{runs} * c.pair_count * 2, 0);
  std::uniform_int_distribution<u32> inst(0, static_cast<u32>(c.instances.size()) - 1);
  std::uniform_int_distribution<u32> clus(0, static_cast<u32>(c.clusters.size()) - 1);
  for (u32 e = 0; e < runs * c.pair_count; ++e) {
    c.visible[e * 2] = inst(rng);
    c.visible[e * 2 + 1] = clus(rng);
  }
  // Count words: one block of four per (run, view) in the camera's layout, then the cascades'.
  c.counts.assign(u64{runs} * 4, 0);
  for (u32 i = 0; i < runs; ++i)
    c.counts[i * 4] = view_counts[i % view_counts.size()];
  for (u32 run = 0; run < 3; ++run) {
    Dispatch d;
    d.views = views;
    d.first_entry = run * views * c.pair_count;
    d.count_offset = run * views * 16;
    d.reset = run == 0;
    c.dispatches.push_back(d);
  }
  if (cascades > 0) {
    Dispatch d;
    d.views = cascades;
    d.first_entry = 3 * views * c.pair_count;
    d.count_offset = 3 * views * 16;
    d.rest_beyond_m = rest_m;
    // The cascades' rest by reach (R103): never the nearest, then coarser texels outwards.
    const f32 lengths[4] = {0.0f, 0.5f, 2.0f, 6.0f};
    for (u32 v = 0; v < 4; ++v)
      d.rest_padding_m[v] = rest_m >= 0.0f ? lengths[v] : 0.0f;
    c.dispatches.push_back(d);
  }
}

u64 total_need(const Case& c) {
  // What the frame asks for if the budget never refuses: the CPU rule with an unbounded budget.
  Case open = c;
  open.budget = ~0u;
  return cpu_allocate(open).record.cursor;
}

void check_case(const gfx::Device& device, const Allocators& a, const Case& c) {
  INFO("case: " << std::string(c.name));
  std::string error;
  const Result cpu = cpu_allocate(c);
  Result reference;
  Result scan;
  REQUIRE_MESSAGE(run_gpu(device, a, c, true, reference, &error), error);
  REQUIRE_MESSAGE(run_gpu(device, a, c, false, scan, &error), error);
  CHECK(reference == cpu);
  u32 mismatches = 0;
  u32 first = ~0u;
  for (u32 i = 0; i < scan.slots.size(); ++i) {
    if (scan.slots[i] != reference.slots[i]) {
      if (first == ~0u) first = i;
      ++mismatches;
    }
  }
  INFO("first mismatch at entry " << first << ": scan " << (first != ~0u ? scan.slots[first] : 0)
                                  << ", reference " << (first != ~0u ? reference.slots[first] : 0));
  CHECK(mismatches == 0);
  CHECK(scan.record.cursor == reference.record.cursor);
  CHECK(scan.record.entries == reference.record.entries);
  CHECK(scan.record.overflow_entries == reference.record.overflow_entries);
  CHECK(scan.record.overflow_vertices == reference.record.overflow_vertices);
  MESSAGE(std::string(c.name) << ": " << cpu.record.entries << " placed, "
                              << cpu.record.overflow_entries << " refused, cursor "
                              << cpu.record.cursor << " of " << c.budget);
}

}  // namespace

TEST_CASE("deform alloc: the device-wide scan hands out the one-workgroup allocator's blocks") {
  gfx::Device device;
  if (!gfx_test::open_device(device)) return;
  std::string error;
  Allocators a;
  REQUIRE_MESSAGE(a.create(device, &error), error);

  std::mt19937 rng(20261026u);
  Case base;
  base.pair_count = 3000;  // 12 tiles a view, the last one partial
  make_population(rng, 400, 300, base);
  // Three views of different cut sizes (one over the dispatch's bound, which the shader clamps),
  // and four cascades.
  make_frame(rng, 3, 4, 60.0f, {2900, 1, 3500, 0, 2048, 257, 3000}, base);
  const u64 asked = total_need(base);
  REQUIRE(asked > 1000);
  {
    // The cascades' rest by reach (R103) leaves entries out of what the frame asks for.
    Case posed = base;
    for (Dispatch& d : posed.dispatches)
      for (f32& length : d.rest_padding_m)
        length = 0.0f;
    CHECK(total_need(posed) > asked);
  }

  {
    Case c = base;
    c.name = "fits the budget";
    c.budget = static_cast<u32>(asked + 17);
    check_case(device, a, c);
    CHECK(cpu_allocate(c).record.overflow_entries == 0);
  }
  {
    Case c = base;
    c.name = "crosses early";
    c.budget = static_cast<u32>(asked / 10);
    check_case(device, a, c);
  }
  {
    Case c = base;
    c.name = "crosses late";
    c.budget = static_cast<u32>(asked - asked / 20);
    check_case(device, a, c);
  }
  {
    Case c = base;
    c.name = "no budget";
    c.budget = 0;
    check_case(device, a, c);
  }
  {
    // A cluster bigger than the room left, at a tile's last lane, then small ones that the next
    // tiles still place: the cursor advanced by what was placed, not by what was asked for.
    Case c;
    c.name = "a big cluster, then small ones";
    c.pair_count = 1024;
    geometry::ClusterDesc small{};
    small.vertex_count = 3;
    geometry::ClusterDesc big{};
    big.vertex_count = 255;
    c.clusters.push_back(small);
    c.clusters.push_back(big);
    gfx::InstanceDesc inst{};
    inst.deform = 0;
    c.instances.push_back(inst);
    c.visible.assign(u64{c.pair_count} * 2, 0);
    for (u32 e = 0; e < c.pair_count; ++e)
      c.visible[e * 2 + 1] = e == 255 ? 1u : 0u;
    c.counts = {1000, 0, 0, 0};
    c.dispatches.push_back(Dispatch{1, 0, 0, true, -1.0f});
    c.budget = 255 * 3 + 100;  // the first tile's 255 small ones fit, the big one does not
    check_case(device, a, c);
    const Result r = cpu_allocate(c);
    CHECK(r.slots[255] == gfx::k_no_pool_slot);
    CHECK(r.slots[256] == 255 * 3);  // the next tile starts where the placed ones ended
    CHECK(r.record.overflow_entries == 1 + (1000 - 256 - 100 / 3));
  }
  {
    // Large enough that passes 1 and 3 dispatch their full group count and stride over tiles.
    Case c;
    c.name = "a cut past the group count";
    c.pair_count = 600'000;
    make_population(rng, 2000, 5000, c);
    make_frame(rng, 1, 0, -1.0f, {600'000, 590'001, 12'345}, c);
    const u64 big_asked = total_need(c);
    c.budget = static_cast<u32>(big_asked / 2);
    check_case(device, a, c);
    c.name = "a cut past the group count, fitting";
    c.budget = static_cast<u32>(big_asked);
    check_case(device, a, c);
  }
  a.destroy(device);
}

TEST_CASE("deform alloc: the same input gives the same bytes, ten times") {
  gfx::Device device;
  if (!gfx_test::open_device(device)) return;
  std::string error;
  Allocators a;
  REQUIRE_MESSAGE(a.create(device, &error), error);
  std::mt19937 rng(7u);
  Case c;
  c.name = "repeat";
  c.pair_count = 50'000;
  make_population(rng, 1000, 2000, c);
  make_frame(rng, 2, 2, 0.0f, {49'000, 50'000, 31'337}, c);
  c.budget = static_cast<u32>(total_need(c) * 3 / 4);
  Result first;
  REQUIRE_MESSAGE(run_gpu(device, a, c, false, first, &error), error);
  CHECK(first == cpu_allocate(c));
  for (u32 i = 1; i < 10; ++i) {
    Result again;
    REQUIRE_MESSAGE(run_gpu(device, a, c, false, again, &error), error);
    CHECK(again == first);
  }
  a.destroy(device);
}
