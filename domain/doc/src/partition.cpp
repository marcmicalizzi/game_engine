#include <core/containers/flat_map.h>
#include <core/containers/flat_set.h>
#include <core/schema/type_info.h>
#include <domain/doc/partition.h>

#include <cmath>
#include <limits>

namespace engine::doc {

namespace {

void append_i32(std::string& out, i32 value) {
  if (value < 0) {
    out.push_back('-');
    // -2147483648 has no positive counterpart; widen before negating.
    out.append(std::to_string(-static_cast<i64>(value)));
    return;
  }
  out.append(std::to_string(value));
}

// One decimal integer with exactly one spelling: an optional '-', then digits with no leading
// zero unless the number is zero itself.
bool parse_i32(std::string_view text, i32& out) {
  if (text.empty()) return false;
  const bool negative = text.front() == '-';
  if (negative) text.remove_prefix(1);
  if (text.empty() || text.size() > 10) return false;
  if (text.size() > 1 && text.front() == '0') return false;
  if (negative && text == "0") return false;
  i64 value = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') return false;
    value = value * 10 + (c - '0');
  }
  if (negative) value = -value;
  if (value < std::numeric_limits<i32>::min() || value > std::numeric_limits<i32>::max())
    return false;
  out = static_cast<i32>(value);
  return true;
}

bool number(const JsonValue* value, f64& out) {
  return value != nullptr && value->get_f64(out) && std::isfinite(out);
}

// [x, y] or [x, y, z]; three components are projected onto the horizontal plane (x, z).
bool read_array(const JsonValue& value, f64& out_x, f64& out_y) {
  const JsonValue::Array& a = value.as_array();
  if (a.size() == 2) return number(&a[0], out_x) && number(&a[1], out_y);
  if (a.size() == 3) return number(&a[0], out_x) && number(&a[2], out_y);
  return false;
}

bool read_object(const JsonValue& value, f64& out_x, f64& out_y) {
  if (!number(value.find("x"), out_x)) return false;
  const JsonValue* z = value.find("z");
  return number(z != nullptr ? z : value.find("y"), out_y);
}

bool read_point(const JsonValue& value, f64& out_x, f64& out_y) {
  if (value.is_array()) return read_array(value, out_x, out_y);
  if (value.is_object()) return read_object(value, out_x, out_y);
  return false;
}

}  // namespace

std::string tile_file_name(TileCoord tile) {
  std::string name;
  name.reserve(24);
  append_i32(name, tile.x);
  name.push_back('_');
  append_i32(name, tile.y);
  name.append(".json");
  return name;
}

bool parse_tile_file_name(std::string_view name, TileCoord& out) {
  if (name.size() > 5 && name.substr(name.size() - 5) == ".json")
    name = name.substr(0, name.size() - 5);
  // The separator is the '_' that is not part of a sign, so a negative y is found from the end.
  const usize sep = name.find('_', 1);
  if (sep == std::string_view::npos) return false;
  TileCoord tile;
  if (!parse_i32(name.substr(0, sep), tile.x)) return false;
  if (!parse_i32(name.substr(sep + 1), tile.y)) return false;
  out = tile;
  return true;
}

std::string_view position_property(const ObjectRecord& record, const LayerPartition& partition) {
  if (!partition.property.empty()) return partition.property;
  if (record.type.empty()) return {};
  const schema::TypeInfo* info = schema::Registry::global().find(record.type);
  if (info == nullptr || info->kind != schema::Kind::Struct) return {};
  // A type opts in by declaring one of these; `position` wins when a type has both.
  if (info->find_field("position") != nullptr) return "position";
  if (info->find_field("transform") != nullptr) return "transform";
  return {};
}

bool read_position(const JsonValue& value, f64& out_x, f64& out_y) {
  if (read_point(value, out_x, out_y)) return true;
  if (!value.is_object()) return false;
  // A transform: the position is one member in.
  for (const char* member : {"position", "translation"}) {
    if (const JsonValue* inner = value.find(member)) {
      if (read_point(*inner, out_x, out_y)) return true;
    }
  }
  return false;
}

bool tile_of(const ObjectRecord& record, const LayerPartition& partition, TileCoord& out) {
  if (!(partition.tile_size > 0)) return false;
  const std::string_view property = position_property(record, partition);
  if (property.empty()) return false;
  const JsonValue* value = record.properties.find_value(property);
  if (value == nullptr) return false;
  f64 x = 0, y = 0;
  if (!read_position(*value, x, y)) return false;
  const f64 tx = std::floor(x / partition.tile_size);
  const f64 ty = std::floor(y / partition.tile_size);
  const f64 low = static_cast<f64>(std::numeric_limits<i32>::min());
  const f64 high = static_cast<f64>(std::numeric_limits<i32>::max());
  if (tx < low || tx > high || ty < low || ty > high) return false;
  out.x = static_cast<i32>(tx);
  out.y = static_cast<i32>(ty);
  return true;
}

LayerIndex build_layer_index(const Layer& layer) {
  LayerIndex index;
  index.name = layer.name();
  index.role = layer.role();
  index.partition = layer.partition();
  if (!layer.partitioned()) return index;

  // Records arrive in id order from the layer's FlatMap, so each tile's list comes out sorted
  // without a second pass, and so does `untiled`.
  FlatMap<TileCoord, Vector<ObjectId>> tiles;
  for (auto [id, record] : layer.records()) {
    TileCoord tile;
    if (tile_of(record, index.partition, tile)) {
      tiles[tile].push_back(id);
    } else {
      index.untiled.push_back(id);
    }
  }
  index.tiles.reserve(tiles.size());
  bool first = true;
  for (auto [tile, ids] : tiles) {  // FlatMap iterates in (x, y) order
    TileRef ref;
    ref.x = tile.x;
    ref.y = tile.y;
    ref.file = tile_file_name(tile);
    ref.objects = std::move(ids);
    index.tiles.push_back(std::move(ref));
    index.min_x = first ? tile.x : (tile.x < index.min_x ? tile.x : index.min_x);
    index.min_y = first ? tile.y : (tile.y < index.min_y ? tile.y : index.min_y);
    index.max_x = first ? tile.x : (tile.x > index.max_x ? tile.x : index.max_x);
    index.max_y = first ? tile.y : (tile.y > index.max_y ? tile.y : index.max_y);
    first = false;
  }
  return index;
}

u32 tile_count(const Layer& layer) {
  if (!layer.partitioned()) return 0;
  FlatSet<TileCoord> tiles;
  for (auto [id, record] : layer.records()) {
    TileCoord tile;
    if (tile_of(record, layer.partition(), tile)) tiles.insert(tile);
  }
  return tiles.size();
}

}  // namespace engine::doc
