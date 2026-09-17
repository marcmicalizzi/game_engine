// What the composed index buys (docs/subsystems/doc.md, docs/plan/03-data-model.md §3.2).
//
// The ".linear" benchmarks are the composition this module used to do: walk every layer for
// every question, and answer objects() and children() by resolving every id. They live here and
// in the tests, never in the API, so the comparison stays honest and the API has one way to ask.
//
// What the numbers say (docs/subsystems/doc.md): objects() and children() were quadratic and are
// not any more; resolve() and property() are a wash over a shallow stack, because a hash lookup
// into a large map costs about what the two FlatMap searches it replaces cost. Keep both halves
// in the table rather than reporting only the wins.
#include <core/ids/id128.h>
#include <domain/doc/document.h>
#include <domain/doc/partition.h>
#include <foundation/bench/bench.h>

#include <algorithm>
#include <schemas/provenance.h>
#include <string>

using namespace engine;
using namespace engine::doc;

namespace {

const char* const k_type = "engine.content.AssetProvenance";
constexpr u32 k_fanout = 8;     // children per parent
constexpr u32 k_probes = 1024;  // ids or parents visited per timed iteration

ObjectId id_of(u32 n) { return Id128::from_parts(1, n + 1); }

// A document of `count` objects in a two-layer stack: a base layer holding the tree, and a
// feature layer overriding one property of every fourth object, which is what makes composition
// more than a single lookup.
Document make_document(u32 count) {
  Document d;
  Layer base("base", LayerRole::Base);
  Layer feature("feature", LayerRole::Feature);
  for (u32 i = 0; i < count; ++i) {
    ObjectRecord r;
    r.id = id_of(i);
    r.type = k_type;
    r.parent = i < k_fanout ? ObjectId{} : id_of(i / k_fanout);
    r.properties.insert_or_assign("generator", JsonValue("base-gen"));
    r.properties.insert_or_assign(
        "position",
        JsonValue(JsonValue::Array{JsonValue(static_cast<f64>(i % 997) * 3.5), JsonValue(0.0),
                                   JsonValue(static_cast<f64>(i / 997) * 3.5)}));
    base.set(std::move(r));
    if (i % 4 != 0) continue;
    ObjectRecord o;
    o.id = id_of(i);
    o.properties.insert_or_assign("license", JsonValue("MIT"));
    feature.set(std::move(o));
  }
  d.add_layer(std::move(base));
  d.add_layer(std::move(feature));
  d.rebuild_index();
  return d;
}

// ---- the linear composition, kept out of the API ---------------------------------------------

bool linear_resolve(const Document& d, ObjectId id, ResolvedObject& out) {
  out = ResolvedObject{};
  out.id = id;
  bool defined = false;
  u32 defining = 0;
  for (u32 i = 0; i < d.layer_count(); ++i) {
    const ObjectRecord* r = d.layer(i).find(id);
    if (r != nullptr && !r->type.empty()) {
      defined = true;
      defining = i;
      out.type = r->type;
    }
  }
  if (!defined) return false;
  out.defining_layer = defining;
  for (u32 i = 0; i < d.layer_count(); ++i) {
    const ObjectRecord* r = d.layer(i).find(id);
    if (r == nullptr) continue;
    if (r->parent.has_value()) out.parent = *r->parent;
    if (r->deleted && i >= defining) out.deleted = true;
    for (auto [name, value] : r->properties)
      out.properties.insert_or_assign(std::string_view(name), &value);
  }
  return true;
}

const JsonValue* linear_property(const Document& d, ObjectId id, std::string_view name) {
  const JsonValue* found = nullptr;
  bool defined = false;
  for (u32 i = 0; i < d.layer_count(); ++i) {
    const ObjectRecord* r = d.layer(i).find(id);
    if (r == nullptr) continue;
    if (!r->type.empty()) defined = true;
    if (const JsonValue* v = r->properties.find_value(name)) found = v;
  }
  return defined ? found : nullptr;
}

Vector<ObjectId> linear_objects(const Document& d) {
  Vector<ObjectId> ids;
  for (u32 i = 0; i < d.layer_count(); ++i) {
    for (auto [id, record] : d.layer(i).records()) {
      if (!record.type.empty()) ids.push_back(id);
    }
  }
  std::sort(ids.begin(), ids.end());
  Vector<ObjectId> out;
  out.reserve(ids.size());
  for (u32 i = 0; i < ids.size(); ++i) {
    if (i > 0 && ids[i] == ids[i - 1]) continue;
    ResolvedObject r;
    if (linear_resolve(d, ids[i], r) && !r.deleted) out.push_back(ids[i]);
  }
  return out;
}

Vector<ObjectId> linear_children(const Document& d, ObjectId parent) {
  Vector<ObjectId> out;
  for (const ObjectId id : linear_objects(d)) {
    ResolvedObject r;
    if (linear_resolve(d, id, r) && r.parent == parent) out.push_back(id);
  }
  return out;
}

}  // namespace

// ---- resolve ---------------------------------------------------------------------------------

ENGINE_BENCH_ARGS(resolve_index, "doc.resolve.index", 1000, 100000) {
  const u32 count = static_cast<u32>(state.arg());
  const Document d = make_document(count);
  while (state.keep_running()) {
    u64 sum = 0;
    for (u32 i = 0; i < k_probes; ++i) {
      ResolvedObject r;
      if (d.resolve(id_of((i * 7919) % count), r)) sum += r.properties.size();
    }
    bench::keep(sum);
  }
  state.set_items(k_probes);
}

ENGINE_BENCH_ARGS(resolve_linear, "doc.resolve.linear", 1000, 100000) {
  const u32 count = static_cast<u32>(state.arg());
  const Document d = make_document(count);
  while (state.keep_running()) {
    u64 sum = 0;
    for (u32 i = 0; i < k_probes; ++i) {
      ResolvedObject r;
      if (linear_resolve(d, id_of((i * 7919) % count), r)) sum += r.properties.size();
    }
    bench::keep(sum);
  }
  state.set_items(k_probes);
}

// ---- one composed property -------------------------------------------------------------------
//
// resolve() composes a whole property map and allocates for it, which hides what the index does;
// property() is the same lookup without that, so this is where the layer walk shows up.

ENGINE_BENCH_ARGS(property_index, "doc.property.index", 1000, 100000) {
  const u32 count = static_cast<u32>(state.arg());
  const Document d = make_document(count);
  while (state.keep_running()) {
    u64 found = 0;
    for (u32 i = 0; i < k_probes; ++i)
      found += d.property(id_of((i * 7919) % count), "generator") != nullptr ? 1 : 0;
    bench::keep(found);
  }
  state.set_items(k_probes);
}

ENGINE_BENCH_ARGS(property_linear, "doc.property.linear", 1000, 100000) {
  const u32 count = static_cast<u32>(state.arg());
  const Document d = make_document(count);
  while (state.keep_running()) {
    u64 found = 0;
    for (u32 i = 0; i < k_probes; ++i)
      found += linear_property(d, id_of((i * 7919) % count), "generator") != nullptr ? 1 : 0;
    bench::keep(found);
  }
  state.set_items(k_probes);
}

// ---- objects ---------------------------------------------------------------------------------

ENGINE_BENCH_ARGS(objects_index, "doc.objects.index", 1000, 100000) {
  const u32 count = static_cast<u32>(state.arg());
  const Document d = make_document(count);
  while (state.keep_running())
    bench::keep(d.objects().size());
  state.set_items(count);
}

ENGINE_BENCH_ARGS(objects_linear, "doc.objects.linear", 1000, 100000) {
  const u32 count = static_cast<u32>(state.arg());
  const Document d = make_document(count);
  while (state.keep_running())
    bench::keep(linear_objects(d).size());
  state.set_items(count);
}

// ---- children --------------------------------------------------------------------------------

ENGINE_BENCH_ARGS(children_index, "doc.children.index", 1000, 100000) {
  const u32 count = static_cast<u32>(state.arg());
  const Document d = make_document(count);
  while (state.keep_running()) {
    u64 sum = 0;
    for (u32 i = 0; i < k_probes; ++i)
      sum += d.children(id_of((i * 7919) % count)).size();
    bench::keep(sum);
  }
  state.set_items(k_probes);
}

// One parent per iteration: the linear form walks the whole document for each one, so asking it
// for a thousand parents at 100k records takes minutes.
ENGINE_BENCH_ARGS(children_linear, "doc.children.linear", 1000, 100000) {
  const u32 count = static_cast<u32>(state.arg());
  const Document d = make_document(count);
  u32 probe = 0;
  while (state.keep_running()) {
    bench::keep(linear_children(d, id_of((probe++ * 7919) % count)).size());
  }
  state.set_items(1);
}

// ---- what maintaining the index costs ----------------------------------------------------------

// Setting a property on a record the edit layer already has: the precondition check, the value
// assignment, and the index maintenance, without the cost of growing the layer's FlatMap, which
// belongs to the container and would swamp the rest.
ENGINE_BENCH_ARGS(apply_set_property, "doc.apply.set_property", 1000, 100000) {
  const u32 count = static_cast<u32>(state.arg());
  Document d = make_document(count);
  d.set_edit_layer(1);
  u32 n = 0;
  for (u32 i = 0; i < k_probes; ++i)  // setup: every probed record exists in the edit layer
    d.apply(cmd_set(id_of((i * 7919) % count), "prompt_hash", JsonValue(u64{0})), nullptr, nullptr);
  while (state.keep_running()) {
    for (u32 i = 0; i < k_probes; ++i) {
      const ObjectId id = id_of((i * 7919) % count);
      d.apply(cmd_set(id, "prompt_hash", JsonValue(static_cast<u64>(n++))), nullptr, nullptr);
    }
  }
  state.set_items(k_probes);
}

// ---- partitioning ------------------------------------------------------------------------------

ENGINE_BENCH_ARGS(build_index_tiles, "doc.partition.build_index", 1000, 100000) {
  const u32 count = static_cast<u32>(state.arg());
  Document d = make_document(count);
  LayerPartition partition;
  partition.property = "position";
  partition.tile_size = 64;
  d.set_layer_partition(0, partition);
  const Document& document = d;
  while (state.keep_running())
    bench::keep(build_layer_index(document.layer(0)).tiles.size());
  state.set_items(count);
}
