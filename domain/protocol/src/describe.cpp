// Plan 06 §6.6's `describe(object | tile | region, detail)` (docs/subsystems/protocol.md,
// "Describing an object, a tile or a region"): what an agent needs to see of a part of the
// document before it edits it, bounded in records and bytes, with a sentence made from templates
// over the counts so the same document gives the same words. Everything it reads it asks the
// document: the composed index for an object and its children, the tile index for a tile, never a
// scan of the layers.
#include <core/json/json.h>
#include <core/schema/json_reflect.h>
#include <core/schema/type_info.h>
#include <domain/doc/document.h>
#include <domain/protocol/rpc.h>
#include <domain/protocol/session.h>

#include <algorithm>
#include <cstdio>
#include <string>

namespace engine::protocol {

namespace {

constexpr u32 k_max_records = 10000;
constexpr u32 k_min_bytes = 1024;
constexpr u32 k_max_bytes = 4u << 20;
constexpr u64 k_max_region_tiles = 65536;
// An object's subtree is walked whole at Full to count it; past this many records the walk stops
// and the counts are what it reached (`bound` says "walk").
constexpr u32 k_walk_limit = 1u << 20;
// Types named in the sentence; the rest are "and N more types".
constexpr u32 k_sentence_types = 8;

std::string hex_of(Id128 id) {
  char hex[Id128::k_hex_length + 1];
  id.to_hex(hex);
  return std::string(hex, Id128::k_hex_length);
}

// The first `worldpos` field of the record's registered type that the record sets: where it is,
// by the engine's own word for that (ADR-0053), whatever the field is called.
bool world_position(const doc::Document& d, Id128 id, std::string_view type, WorldPos& out) {
  const schema::TypeInfo* info = schema::Registry::global().find(type);
  if (info == nullptr) return false;
  for (const schema::FieldInfo& f : info->fields) {
    const schema::TypeRef* t = &f.type;
    if (t->kind == schema::Kind::Optional && t->element != nullptr) t = t->element;
    if (t->kind != schema::Kind::WorldPos) continue;
    const JsonValue* v = d.property(id, f.name);
    if (v == nullptr || !v->is_array() || v->size() != 3) continue;
    f64 p[3] = {};
    if (!(*v)[0].get_f64(p[0]) || !(*v)[1].get_f64(p[1]) || !(*v)[2].get_f64(p[2])) continue;
    out = WorldPos(p[0], p[1], p[2]);
    return true;
  }
  return false;
}

std::string fixed(f64 v) {
  char text[48];
  std::snprintf(text, sizeof(text), "%.2f", v);
  return text;
}

struct Builder {
  const doc::Document& d;
  const DescribeParams& params;
  DescribeResult& out;
  FlatMap<std::string, u32> counts;

  void count(Id128 id) {
    ++out.total;
    const std::string type(d.type_of(id));
    if (u32* c = counts.find_value(type)) {
      ++*c;
    } else {
      counts.insert_or_assign(type, 1u);
    }
  }

  // Lists `id` when the detail asks for it and the bounds let it; counts it as omitted otherwise.
  // A described object itself is `always` listed, whatever the bounds: it is what was asked for.
  void offer(Id128 id, u32 depth, bool always = false) {
    if (!always && params.detail == DescribeDetail::Summary) {
      ++out.omitted;
      return;
    }
    if (!always && !out.bound.empty()) {
      ++out.omitted;
      return;
    }
    if (!always && out.records.size() >= params.max_records) {
      out.bound = "max_records";
      ++out.omitted;
      return;
    }
    DescribedRecord r;
    r.id = id;
    r.type = std::string(d.type_of(id));
    r.parent = d.parent_of(id);
    if (const JsonValue* name = d.property(id, "name"); name != nullptr && name->is_string())
      r.name = std::string(name->as_string());
    WorldPos p;
    if (world_position(d, id, r.type, p)) r.position = p;
    doc::TileCoord tile;
    if (d.object_tile(id, tile)) {
      r.tiled = true;
      r.tile_x = tile.x;
      r.tile_y = tile.y;
    }
    r.children = d.children(id).size();
    r.depth = depth;
    if (params.detail == DescribeDetail::Full) {
      doc::ResolvedObject resolved;
      if (d.resolve(id, resolved))
        for (auto [name, value] : resolved.properties)
          r.properties.insert_or_assign(std::string(name), *value);
    }
    const u32 bytes = static_cast<u32>(write_json(schema::to_json(r)).size());
    if (!always && out.bytes + bytes > params.max_bytes) {
      out.bound = "max_bytes";
      ++out.omitted;
      return;
    }
    out.bytes += bytes;
    out.records.push_back(std::move(r));
  }

  void finish_types() {
    for (const auto& [type, n] : counts) {
      DescribeTypeCount c;
      c.type = type;
      c.count = n;
      out.types.push_back(std::move(c));
    }
    std::sort(out.types.begin(), out.types.end(),
              [](const DescribeTypeCount& a, const DescribeTypeCount& b) {
                return a.count != b.count ? a.count > b.count : a.type < b.type;
              });
  }

  // "3 engine.world.Node, 1 engine.scene.MeshPlacement and 2 more types", over `types` from
  // `first` on (an object's own type is not among its contents).
  std::string type_list(const Vector<DescribeTypeCount>& types) const {
    std::string s;
    const u32 named = std::min<u32>(types.size(), k_sentence_types);
    for (u32 i = 0; i < named; ++i) {
      if (i > 0) s += i + 1 == named && named == types.size() ? " and " : ", ";
      s += std::to_string(types[i].count) + " " + types[i].type;
    }
    if (types.size() > named) s += " and " + std::to_string(types.size() - named) + " more types";
    return s;
  }

  std::string records_phrase(u32 n) const {
    return n == 1 ? std::string("1 live record") : std::to_string(n) + " live records";
  }

  std::string listed_sentence() const {
    if (out.omitted == 0) return {};
    const std::string listed = std::to_string(out.records.size());
    if (out.bound.empty()) return " Listed " + listed + " (detail Summary lists no others).";
    return " Listed " + listed + " of " + std::to_string(out.total) + "; " +
           std::to_string(out.omitted) + " left out by " + out.bound + ".";
  }
};

bool describe_object(Builder& b) {
  const doc::Document& d = b.d;
  const Id128 root = *b.params.id;
  b.out.scope = "object";
  if (!d.exists(root)) return false;
  // The object and its children; at Full its live subtree, depth first with children by id, an
  // order that is the document's alone.
  struct Item {
    Id128 id;
    u32 depth;
  };
  Vector<Item> stack;
  stack.push_back({root, 0});
  u32 visited = 0;
  while (!stack.empty()) {
    const Item item = stack.back();
    stack.pop_back();
    if (++visited > k_walk_limit) {
      if (b.out.bound.empty()) b.out.bound = "walk";
      break;
    }
    b.count(item.id);
    b.offer(item.id, item.depth, item.depth == 0);
    if (item.depth == 0 || b.params.detail == DescribeDetail::Full) {
      const Vector<Id128> children = d.children(item.id);
      for (u32 i = children.size(); i > 0; --i)
        stack.push_back({children[i - 1], item.depth + 1});
    }
  }
  b.finish_types();
  // The sentence: what the object is, where, under what, and what it holds.
  const DescribedRecord& self = b.out.records[0];
  std::string s = self.type;
  if (!self.name.empty()) s += " '" + self.name + "'";
  s += " " + hex_of(self.id);
  if (self.position.has_value())
    s += " at (" + fixed(self.position->x) + ", " + fixed(self.position->y) + ", " +
         fixed(self.position->z) + ")";
  if (self.tiled)
    s += " in tile (" + std::to_string(self.tile_x) + ", " + std::to_string(self.tile_y) + ")";
  s += self.parent.is_null()
           ? std::string(", a root")
           : ", under " + std::string(d.type_of(self.parent)) + " " + hex_of(self.parent);
  // Its contents by type: the counts without the object itself.
  Vector<DescribeTypeCount> contents;
  bool removed = false;
  for (const DescribeTypeCount& c : b.out.types) {
    DescribeTypeCount kept = c;
    if (!removed && kept.type == self.type) {
      removed = true;
      if (--kept.count == 0) continue;
    }
    contents.push_back(std::move(kept));
  }
  std::sort(contents.begin(), contents.end(),
            [](const DescribeTypeCount& x, const DescribeTypeCount& y) {
              return x.count != y.count ? x.count > y.count : x.type < y.type;
            });
  const u32 held = b.out.total - 1;
  const char* what = b.params.detail == DescribeDetail::Full ? " in its subtree" : " as children";
  if (held == 0) {
    s += self.children == 0 ? ", with no children." : ".";
  } else {
    s += ", with " + b.records_phrase(held) + what + ": " + b.type_list(contents) + ".";
  }
  b.out.summary = s + b.listed_sentence();
  return true;
}

bool describe_tiles(Builder& b, const TileRange& range, bool one) {
  const doc::Document& d = b.d;
  b.out.scope = one ? "tile" : "region";
  // Every live record in the rectangle, tile by tile, row by row, ids in order within a tile. A
  // rectangle wider than the document has occupied tiles is answered from the live set instead,
  // so an empty corner of a large region costs nothing per tile; the order is the same.
  const u64 width = static_cast<u64>(static_cast<i64>(range.x1) - range.x0 + 1);
  const u64 height = static_cast<u64>(static_cast<i64>(range.y1) - range.y0 + 1);
  struct Entry {
    doc::TileCoord tile;
    Id128 id;
  };
  Vector<Entry> entries;
  if (width * height <= d.occupied_tiles()) {
    for (i32 y = range.y0; y <= range.y1; ++y)
      for (i32 x = range.x0; x <= range.x1; ++x)
        for (const Id128 id : d.ids_in_tile({x, y}))
          entries.push_back({{x, y}, id});
  } else {
    for (const Id128 id : d.objects()) {
      doc::TileCoord t;
      if (!d.object_tile(id, t) || t.x < range.x0 || t.x > range.x1 || t.y < range.y0 ||
          t.y > range.y1)
        continue;
      entries.push_back({t, id});
    }
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& c) {
      if (a.tile.y != c.tile.y) return a.tile.y < c.tile.y;
      if (a.tile.x != c.tile.x) return a.tile.x < c.tile.x;
      return a.id < c.id;
    });
  }
  for (u32 i = 0; i < entries.size(); ++i) {
    if (i == 0 || !(entries[i].tile == entries[i - 1].tile)) ++b.out.occupied_tiles;
    b.count(entries[i].id);
    b.offer(entries[i].id, 0);
  }
  b.finish_types();
  const auto coord = [](i32 x, i32 y) {
    return "(" + std::to_string(x) + ", " + std::to_string(y) + ")";
  };
  std::string s = one ? "Tile " + coord(range.x0, range.y0)
                      : "Tiles " + coord(range.x0, range.y0) + " to " + coord(range.x1, range.y1) +
                            ", " + std::to_string(b.out.occupied_tiles) + " of " +
                            std::to_string(width * height) + " occupied,";
  if (b.out.total == 0) {
    s += " hold no live records.";
    if (one) s = "Tile " + coord(range.x0, range.y0) + " holds no live records.";
  } else {
    s += (one ? " holds " : " hold ") + b.records_phrase(b.out.total) + ": " +
         b.type_list(b.out.types) + ".";
  }
  b.out.summary = s + b.listed_sentence();
  return true;
}

bool doc_describe(Context& ctx, const DescribeParams& params, DescribeResult& out,
                  RpcError& error) {
  Session* s = ctx.sessions->require(params.session, error);
  if (s == nullptr) return false;
  const u32 named = (params.id.has_value() ? 1u : 0u) + (params.tile.has_value() ? 1u : 0u) +
                    (params.region.has_value() ? 1u : 0u);
  if (named != 1) {
    error = make_error(codes::k_invalid_argument,
                       "describe takes exactly one of id, tile and region; this call named " +
                           std::to_string(named));
    return false;
  }
  if (params.max_records < 1 || params.max_records > k_max_records) {
    error = make_error(codes::k_invalid_argument,
                       "max_records must be 1 to " + std::to_string(k_max_records));
    return false;
  }
  if (params.max_bytes < k_min_bytes || params.max_bytes > k_max_bytes) {
    error =
        make_error(codes::k_invalid_argument, "max_bytes must be " + std::to_string(k_min_bytes) +
                                                  " to " + std::to_string(k_max_bytes));
    return false;
  }
  out.detail = params.detail;
  Builder b{s->document(), params, out, {}};
  if (params.id.has_value()) {
    if (!describe_object(b)) {
      error = make_error(codes::k_not_found, "no live object " + hex_of(*params.id));
      return false;
    }
    return true;
  }
  TileRange range;
  if (params.tile.has_value()) {
    range.x0 = range.x1 = params.tile->x;
    range.y0 = range.y1 = params.tile->y;
  } else {
    range = *params.region;
    if (range.x1 < range.x0 || range.y1 < range.y0) {
      error = make_error(codes::k_invalid_argument, "region must have x0 <= x1 and y0 <= y1");
      return false;
    }
    const u64 tiles = static_cast<u64>(static_cast<i64>(range.x1) - range.x0 + 1) *
                      static_cast<u64>(static_cast<i64>(range.y1) - range.y0 + 1);
    if (tiles > k_max_region_tiles) {
      error = make_error(codes::k_invalid_argument,
                         "region covers " + std::to_string(tiles) + " tiles; at most " +
                             std::to_string(k_max_region_tiles) + " (describe a smaller one)");
      return false;
    }
  }
  return describe_tiles(b, range, params.tile.has_value());
}

}  // namespace

void add_query_methods(Dispatcher& d);  // query.cpp

// doc.describe and doc.query: the document's introspection methods, both read-only.
void add_introspection_methods(Dispatcher& d) {
  add_query_methods(d);
  d.add(read_only(method<DescribeParams, DescribeResult, &doc_describe>(
      "doc.describe",
      "Describe one object (with its children, or its subtree at Full), one tile or a rectangle "
      "of tiles at a detail (Summary, Standard, Full): counts by type, a sentence made from "
      "templates, and the records, bounded by max_records and max_bytes with what was left out "
      "counted.")));
}

}  // namespace engine::protocol
