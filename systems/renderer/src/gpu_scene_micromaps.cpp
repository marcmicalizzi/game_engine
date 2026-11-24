// The opacity micromaps' bake at load, and its parts read back from the derived-data root (roadmap
// R120, R207, R208; docs/subsystems/renderer.md, "Opacity micromaps" and "The bake is read back";
// systems/renderer/micromap_cache.h says what a part is keyed by and why it names the device).

#include <core/log/log.h>
#include <core/profiling/startup_trace.h>
#include <core/time/time.h>
#include <domain/gfx/opacity_micromap.h>
#include <foundation/io/vfs.h>
#include <systems/renderer/gpu_scene.h>
#include <systems/renderer/micromap_cache.h>

#include <algorithm>
#include <renderer_log.h>
#include <string>

namespace engine::renderer {

namespace {

// One part of the scene, as the scene lists it: a run of its masked clusters (`micromap_list_`'s
// items), their triangles and the key the run is stored under at the cap being tried.
struct ScenePart {
  u32 first_item = 0;
  u32 items = 0;
  u32 first_triangle = 0;
  u32 triangles = 0;
  u64 key = 0;
  u32 source = ~0u;  // where its triangles come from this round, and from where in it
  u32 first = 0;
  const u32* wanted = nullptr;
};

// What the store holds for this generation: each pack's header, and each part key's pack and
// place, the first pack that names a key winning.
struct StoreIndex {
  Vector<MicromapPackIndex> packs;
  Vector<MicromapPackData> loaded;  // per pack, its sections once read
  Vector<u8> state;                 // per pack: 0 unread, 1 read, 2 refused
  HashMap<u64, u64> parts;          // key -> pack << 32 | part
};

void index_store(const std::string& dir, u64 generation, StoreIndex& out, MicromapCacheReport& r,
                 Vector<std::string>& damaged) {
  Vector<io::DirEntry> entries;
  if (io::list_directory(dir, entries) != io::Status::Ok) return;
  for (const io::DirEntry& e : entries) {
    if (e.is_directory || e.name.size() < 4 || e.name.substr(e.name.size() - 4) != ".omm") continue;
    MicromapPackIndex index;
    if (!read_micromap_pack_index(io::join_path(dir, e.name), generation, index)) {
      // A pack in this generation's folder that is not one of its packs is of no use to any
      // load: removed at the next store.
      ++r.packs_refused;
      if (r.refused.empty()) r.refused = e.name + ": " + index.refused;
      damaged.push_back(index.path);
      continue;
    }
    const u32 p = static_cast<u32>(out.packs.size());
    for (u32 i = 0; i < index.parts.size(); ++i)
      out.parts.try_emplace(index.parts[i].key, (u64{p} << 32) | i);
    out.packs.push_back(std::move(index));
  }
  out.loaded.resize(static_cast<u32>(out.packs.size()));
  out.state.assign(static_cast<u32>(out.packs.size()), u8{0});
  r.packs = static_cast<u32>(out.packs.size());
}

// The sink of the bake of the missing parts: it stores them as one pack, then builds the scene's
// micromap from the stored parts it read and the sections the bake hands it, before the bake frees
// them.
struct Assemble {
  const gfx::Device* device = nullptr;
  const gfx::MicromapBakeInput* scene = nullptr;
  Vector<ScenePart>* parts = nullptr;
  Vector<MicromapPackView>* sources = nullptr;
  Vector<MicromapPackPart> baked;  // the pack this load stores: the missing parts' records
  Vector<u32> baked_first;         // per record, its first triangle in the bake's sections
  std::string root;
  u64 generation = 0;
  u64 budget = 0;
  u32 cap = 0;
  // Out.
  bool stored = false;
  u64 stored_bytes = 0;
  std::string path;
  std::string store_error;
  bool over_budget = false;
  bool restored = false;
  gfx::MicromapStats stats;
  std::string error;
  gfx::OpacityMicromapSet* out = nullptr;

  bool finish() {
    Vector<MicromapRun> runs;
    for (const ScenePart& p : *parts)
      runs.push_back(MicromapRun{p.source, p.first, p.triangles, p.wanted});
    MicromapAssembly assembly;
    assemble_micromaps(std::span<const MicromapPackView>(sources->data(), sources->size()),
                       std::span<const MicromapRun>(runs.data(), runs.size()), assembly);
    assembly.stats.level_cap = cap;
    if (budget != 0 && assembly.total_bytes > budget && cap > 0) {
      over_budget = true;
      return true;
    }
    stats = assembly.stats;
    restored = restore_assembled_micromaps(
        *device, scene->cluster_count, scene->items, scene->triangle_counts,
        std::span<const MicromapPackView>(sources->data(), sources->size()), assembly, *out,
        &error);
    return restored;
  }

  static void take(void* context, const gfx::MicromapSections& sections) {
    auto& self = *static_cast<Assemble*>(context);
    // The bake's sections are this round's last source; each missing part's triangles are a run
    // of them, in the order the bake was given the parts.
    for (u32 b = 0; b < self.baked.size(); ++b) {
      MicromapPackPart& record = self.baked[b];
      const u32 first = self.baked_first[b];
      for (u32 t = 0; t < record.triangle_count && first + t < sections.wanted.size(); ++t)
        ++record.wanted[sections.wanted[first + t]];
    }
    MicromapPackView bake;
    bake.parts = std::span<const MicromapPackPart>(self.baked.data(), self.baked.size());
    bake.indices = sections.indices;
    bake.triangles = sections.triangles;
    bake.data = sections.data;
    if (!self.root.empty()) {
      self.path = micromap_pack_path(self.root, self.generation, bake.parts);
      self.stored = write_micromap_pack(self.path, self.generation, bake, &self.store_error);
      if (self.stored) {
        io::FileInfo info;
        if (io::stat_file(self.path, info) == io::Status::Ok) self.stored_bytes = info.size;
      }
    }
    const u32 source = static_cast<u32>(self.sources->size());
    self.sources->push_back(bake);
    for (ScenePart& p : *self.parts) {
      if (p.source != ~0u) continue;
      for (u32 b = 0; b < self.baked.size(); ++b) {
        if (self.baked[b].key != p.key) continue;
        p.source = source;
        p.first = self.baked_first[b];
        p.wanted = self.baked[b].wanted;
      }
    }
    (void)self.finish();
    self.sources->pop_back();
  }
};

// **The scene's micromaps from the store** (R208): its parts found in the generation's packs are
// read, the rest baked in one dispatch and stored as a new pack, and one micromap is built from
// both. The budget is the bake's rule over the whole: while the assembled micromap is over it the
// cap is lowered a level and the parts are found or baked again at that cap.
bool micromaps_from_store(const gfx::Device& device, gfx::ShaderLibrary& shaders,
                          const gfx::BindlessSet& bindless, const gfx::MicromapBakeInput& in,
                          const gfx::Shader& shader, MicromapKeyState& state,
                          gfx::OpacityMicromapSet& out, i64 start, std::string* error) {
  MicromapCacheReport& r = state.report;
  u8 device_id[28];
  Vector<u64> digests;
  std::string why;
  if (!gfx::micromap_device_identity(device, device_id) ||
      !gfx::micromap_item_digests(device, shaders, bindless, in, digests, &why)) {
    r.unkeyed = "the geometry digest failed: " + why;
    return false;
  }
  const u64 generation = micromap_generation(device_id, shader.hash);
  r.generation = generation;
  const std::string dir = micromap_generation_dir(state.root, generation);
  // The scene's parts: runs of items naming one part, in the list's order.
  Vector<ScenePart> parts;
  const u32 item_count = static_cast<u32>(in.items.size() / 2);
  Vector<u32> locals(item_count, 0u);
  Vector<u64> materials(item_count, u64{0});
  u32 triangle = 0;
  for (u32 i = 0; i < item_count; ++i) {
    const u32 part = state.parts[2 * i];
    locals[i] = state.parts[2 * i + 1];
    materials[i] = state.material_digests[in.items[2 * i + 1]];
    if (parts.empty() || state.parts[2 * (parts.back().first_item)] != part) {
      ScenePart p;
      p.first_item = i;
      p.first_triangle = triangle;
      parts.push_back(p);
    }
    ++parts.back().items;
    parts.back().triangles += in.triangle_counts[i];
    triangle += in.triangle_counts[i];
  }
  r.parts = static_cast<u32>(parts.size());
  StoreIndex store;
  Vector<std::string> damaged;
  index_store(dir, generation, store, r, damaged);
  r.key_ms = static_cast<f64>(time::monotonic_ns() - start) / 1e6;
  bool stored = false;
  u32 cap = in.max_level;
  const u32 first_cap = cap;
  for (;;) {
    for (ScenePart& p : parts) {
      MicromapPartKeyInput key;
      key.generation = generation;
      key.level_cap = cap;
      key.target_texels = in.target_texels;
      key.locals = std::span<const u32>(locals.data() + p.first_item, p.items);
      key.triangle_counts = in.triangle_counts.subspan(p.first_item, p.items);
      key.materials = std::span<const u64>(materials.data() + p.first_item, p.items);
      key.digests = std::span<const u64>(digests.data() + p.first_item, p.items);
      p.key = micromap_part_key(key);
      p.source = ~0u;
      p.wanted = nullptr;
    }
    // Every pack that holds a part of the scene, read whole and checked; a pack that fails is
    // forgotten (its parts baked below) and removed at this load's store.
    Vector<MicromapPackView> sources;
    Vector<u32> source_of(static_cast<u32>(store.packs.size()), ~0u);
    u32 read = 0;
    for (ScenePart& p : parts) {
      const u64* at = store.parts.find_value(p.key);
      if (at == nullptr) continue;
      const u32 pack = static_cast<u32>(*at >> 32);
      const u32 index = static_cast<u32>(*at & 0xffffffffu);
      if (store.state[pack] == 0) {
        std::string refused;
        if (read_micromap_pack(store.packs[pack], store.loaded[pack], &refused)) {
          store.state[pack] = 1;
          r.read_bytes += store.packs[pack].file_bytes;
          ++r.packs_read;
        } else {
          store.state[pack] = 2;
          ++r.packs_refused;
          if (r.refused.empty()) r.refused = store.packs[pack].path + ": " + refused;
          damaged.push_back(store.packs[pack].path);
          ENGINE_LOG_WARN(log_renderer, "stored opacity micromaps refused",
                          log::field("path", std::string_view{store.packs[pack].path}),
                          log::field("reason", std::string_view{refused}));
        }
      }
      if (store.state[pack] != 1) continue;
      if (source_of[pack] == ~0u) {
        source_of[pack] = static_cast<u32>(sources.size());
        sources.push_back(store.loaded[pack].view());
      }
      p.source = source_of[pack];
      p.first = store.loaded[pack].first_triangle[index];
      p.wanted = store.loaded[pack].parts[index].wanted;
      ++read;
    }
    // The missing parts, each key once, baked in one dispatch over their items.
    Assemble job;
    job.device = &device;
    job.scene = &in;
    job.parts = &parts;
    job.sources = &sources;
    job.root = state.root;
    job.generation = generation;
    job.budget = in.budget_bytes;
    job.cap = cap;
    job.out = &out;
    Vector<u32> items;
    Vector<u32> counts;
    u32 baked_triangles = 0;
    for (const ScenePart& p : parts) {
      if (p.source != ~0u) continue;
      bool again = false;
      for (const MicromapPackPart& b : job.baked)
        again = again || b.key == p.key;
      if (again) continue;
      MicromapPackPart record;
      record.key = p.key;
      record.triangle_count = p.triangles;
      job.baked.push_back(record);
      job.baked_first.push_back(baked_triangles);
      baked_triangles += p.triangles;
      for (u32 i = p.first_item; i < p.first_item + p.items; ++i) {
        items.push_back(in.items[2 * i]);
        items.push_back(in.items[2 * i + 1]);
        counts.push_back(in.triangle_counts[i]);
      }
    }
    if (job.baked.empty()) {
      if (!job.finish() && !job.over_budget) {
        if (error != nullptr) *error = "opacity micromaps from the store: " + job.error;
        return false;
      }
    } else {
      gfx::MicromapBakeInput bake = in;
      bake.items = std::span<const u32>(items.data(), items.size());
      bake.triangle_counts = std::span<const u32>(counts.data(), counts.size());
      bake.max_level = cap;
      bake.budget_bytes = 0;  // the budget is the whole scene's, held by the assembly
      bake.build = false;
      gfx::MicromapSink sink;
      sink.take = &Assemble::take;
      sink.context = &job;
      gfx::OpacityMicromapSet unused;
      if (!gfx::bake_opacity_micromaps(device, shaders, bindless, bake, sink, unused, error))
        return false;
      r.baked += static_cast<u32>(job.baked.size());
      if (job.stored) {
        stored = true;
        r.stored = true;
        r.stored_bytes += job.stored_bytes;
        r.path = job.path;
      } else if (!job.store_error.empty()) {
        ENGINE_LOG_WARN(log_renderer, "opacity micromaps not stored",
                        log::field("path", std::string_view{job.path}),
                        log::field("reason", std::string_view{job.store_error}));
      }
      if (!job.restored && !job.over_budget) {
        if (error != nullptr) *error = "opacity micromaps from the store: " + job.error;
        return false;
      }
    }
    if (job.over_budget) {
      --cap;
      r.baked = 0;
      continue;
    }
    r.read = read;
    out.stats = job.stats;
    out.stats.level_cap = cap;
    out.stats.levels_dropped = first_cap - cap;
    out.stats.storage_bytes = out.storage.size;
    r.levels_dropped = first_cap - cap;
    if (r.levels_dropped != 0) {
      ENGINE_LOG_WARN(log_renderer, "opacity micromap budget lowered the level cap",
                      log::field("from", first_cap), log::field("to", cap),
                      log::field("budget_bytes", in.budget_bytes));
    }
    break;
  }
  touch_micromap_generation(state.root, generation);
  if (stored || !damaged.empty()) {
    // A pack this load stored over a damaged one of the same name is the store's, not the damage.
    Vector<std::string> remove;
    for (const std::string& path : damaged)
      if (path != r.path) remove.push_back(path);
    damaged = std::move(remove);
    MicromapPruneReport pruned;
    prune_micromap_store(state.root, generation,
                         std::span<const std::string>(damaged.data(), damaged.size()),
                         time::wall_unix_ms(), pruned);
    r.swept = pruned.swept;
    r.pruned_packs = pruned.packs;
    r.pruned_generations = pruned.generations;
  }
  if (r.path.empty()) r.path = dir;
  return true;
}

}  // namespace

// **The opacity micromaps' bake** (gfx/opacity_micromap.h; renderer.md, "Opacity micromaps";
// R120): once, over the alpha-tested clusters `upload_materials` listed, blocking as every upload
// of a scene's creation is — or, since R207 and R208, its parts read back from
// `<ddc>/micromaps/<generation>/` where a load before this one on this device baked them, and only
// the rest baked. Then the rigid forms' static structures, built at create before the shaders
// existed, are built again naming the micromaps, before any frame references them.
bool GpuScene::bake_micromaps(gfx::ShaderLibrary& shaders, std::string* error) {
  if (micromaps_.address != 0 || micromap_list_.empty()) return true;
  ENGINE_STARTUP_STAGE("opacity micromaps");
  const i64 start = time::monotonic_ns();
  const gfx::Device& device = *device_;
  gfx::MicromapBakeInput in;
  in.clusters = clusters.address;
  in.triangles = triangles.address;
  in.attributes = attributes.address;
  in.materials = materials.address;
  in.items = std::span<const u32>(micromap_list_.data(), micromap_list_.size());
  in.triangle_counts = std::span<const u32>(micromap_triangles_.data(), micromap_triangles_.size());
  in.cluster_count = cluster_count_;
  u32 limit_2 = 0;
  u32 limit_4 = 0;
  gfx::opacity_micromap_limits(device, limit_2, limit_4);
  in.max_level = std::min(micromap_key_.level, limit_4);
  in.budget_bytes = micromap_key_.budget_bytes;
  in.target_texels = micromap_key_.texels;

  MicromapCacheReport& report = micromap_key_.report;
  report.unkeyed = micromap_key_.unkeyed;
  u64 masked = 0;
  for (const u32 n : micromap_triangles_)
    masked += n;
  // The floor (R208): a scene this small bakes in less time than a read's fixed cost.
  if (report.unkeyed.empty() && !micromap_key_.root.empty() && masked < micromap_key_.store_min) {
    report.unkeyed = "fewer masked triangles (" + std::to_string(masked) +
                     ") than renderer.rt.micromap_store_min (" +
                     std::to_string(micromap_key_.store_min) + "): baked, not stored";
  }
  bool done = false;
  if (report.unkeyed.empty() && !micromap_key_.root.empty()) {
    const gfx::Shader* shader = shaders.get("opacity_micromap_bake");
    std::string failed;
    if (shader == nullptr) {
      report.unkeyed = "the bake shader did not load";
    } else if (!(done = micromaps_from_store(device, shaders, bindless_, in, *shader, micromap_key_,
                                             micromaps_, start, &failed)) &&
               !failed.empty()) {
      // A store that cannot be read or written is a cost, never a failed load: bake as before.
      report.unkeyed = failed;
      ENGINE_LOG_WARN(log_renderer, "opacity micromaps' store failed; baked instead",
                      log::field("reason", std::string_view{failed}));
    }
  }
  if (!done) {
    gfx::destroy_opacity_micromaps(device, micromaps_);
    if (!gfx::bake_opacity_micromaps(device, shaders, bindless_, in, micromaps_, error)) {
      return false;
    }
    // Every part baked, none stored.
    report.parts = 0;
    for (u32 i = 0; i < micromap_key_.parts.size(); i += 2)
      if (i == 0 || micromap_key_.parts[i] != micromap_key_.parts[i - 2]) ++report.parts;
    report.baked = std::max(report.parts, 1u);
    report.read = 0;
  }
  report.ms = static_cast<f64>(time::monotonic_ns() - start) / 1e6;
  if (done) micromaps_.stats.bake_ms = report.ms;
  micromap_list_ = Vector<u32>{};
  micromap_triangles_ = Vector<u32>{};
  micromap_key_.parts = Vector<u32>{};
  const gfx::MicromapStats& s = micromaps_.stats;
  u32 entries = 0;
  // Per level, `level:triangles wanting it`, `level:triangles/stored entries`, and
  // `level:unknown/micro-triangles` (R120 stage 3's histogram), as text for the log line.
  std::string wanted;
  std::string stored;
  std::string unknown;
  for (u32 l = 0; l <= gfx::k_micromap_max_level; ++l) {
    entries += s.entries_per_level[l];
    if (s.wanted_per_level[l] != 0)
      wanted += std::to_string(l) + ":" + std::to_string(s.wanted_per_level[l]) + " ";
    if (s.entries_per_level[l] != 0)
      stored += std::to_string(l) + ":" + std::to_string(s.entries_per_level[l]) + "/" +
                std::to_string(s.unique_per_level[l]) + " ";
    if (s.micro_per_level[l] != 0)
      unknown += std::to_string(l) + ":" + std::to_string(s.unknown_per_level[l]) + "/" +
                 std::to_string(s.micro_per_level[l]) + " ";
  }
  ENGINE_LOG_INFO(
      log_renderer, report.baked == 0 ? "opacity micromaps read" : "opacity micromaps baked",
      log::field("triangles", s.triangles), log::field("with_data", entries),
      log::field("unique_entries", s.unique_entries),
      log::field("dedup_mismatches", s.dedup_mismatches),
      log::field("fully_opaque", s.special_opaque),
      log::field("fully_transparent", s.special_transparent),
      log::field("fully_unknown", s.special_unknown), log::field("micro_unknown", s.micro_unknown),
      log::field("micro_unknown_filter", s.micro_unknown_filter),
      log::field("micro_known", s.micro_opaque + s.micro_transparent),
      log::field("level_cap", s.level_cap), log::field("levels_dropped", s.levels_dropped),
      log::field("wanted", std::string_view{wanted}),
      log::field("stored", std::string_view{stored}),
      log::field("unknown", std::string_view{unknown}), log::field("data_bytes", s.data_bytes),
      log::field("resident_bytes", micromap_bytes()), log::field("bake_ms", s.bake_ms),
      log::field("stage_ms", report.ms), log::field("key_ms", report.key_ms),
      log::field("generation", report.generation), log::field("parts", report.parts),
      log::field("parts_read", report.read), log::field("parts_baked", report.baked),
      log::field("packs_read", report.packs_read), log::field("read_bytes", report.read_bytes),
      log::field("written", report.stored), log::field("written_bytes", report.stored_bytes),
      log::field("pruned_packs", report.pruned_packs), log::field("swept", report.swept),
      log::field("unkeyed", std::string_view{report.unkeyed}));
  if (micromaps_.address != 0 && static_build_ && static_clusters_ > 0) {
    gfx::destroy_cluster_set(device, static_clas_);
    gfx::destroy_cluster_blas_set(device, static_blas_);
    if (!build_static_forms(error)) return false;
  }
  return true;
}

}  // namespace engine::renderer
