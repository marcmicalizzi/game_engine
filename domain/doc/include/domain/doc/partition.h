#pragma once

// Tile partitioning of layer files (docs/plan/03-data-model.md §3.7, §3.2).
//
// One fixed tile grid is the unit of the document's layer files, of streaming, of nav tiles,
// and of agent edit leases. Here it is the file layout: a layer that carries a LayerPartition
// is written as
//
//     layers/<layer>/index.json          which tile holds each record, and the grid
//     layers/<layer>/tiles/<x>_<y>.json  a LayerFile restricted to that tile's records
//     layers/<layer>/untiled.json        the records with no position
//
// A record's tile is a function of the record alone — its position property, floored by the
// tile size — so it never depends on the other layers, on load order, or on what was on disk
// before. That is what makes the form canonical: the same document content always writes the
// same set of files, with the same bytes.
//
// Which property carries the position is either named by the partition setting (the
// reproducible form, since it depends on nothing outside the document) or, when it is not,
// taken from the record type's first `position` or `transform` field as the schema registry
// reports it, which is how a type opts in. Tiles are columns, so the grid is the horizontal
// plane: x and z of a three-component position, y being up as core/math has it.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/ids/id128.h>
#include <core/json/json_value.h>
#include <domain/doc/document.h>

#include <schemas/doc.h>
#include <string>
#include <string_view>

namespace engine::doc {

// `TileCoord`, a cell of the layer's grid, is declared in document.h: the document's tile index
// (`Document::ids_in_tile`) is keyed by it.

// "<x>_<y>.json", the decimal coordinates with a leading '-' where they are negative.
std::string tile_file_name(TileCoord tile);
// Parses that name back. False for anything else, including a coordinate with a redundant sign
// or leading zero, because a tile has exactly one spelling.
bool parse_tile_file_name(std::string_view name, TileCoord& out);

// The property carrying `record`'s world position: `partition.property` when it names one, and
// otherwise the record type's first `position` or `transform` field. Empty when neither the
// setting nor the type offers one, which puts the record in untiled.json.
std::string_view position_property(const ObjectRecord& record, const LayerPartition& partition);
// The same rule for a record of type `type` (a qualified schema name): what a reader that holds a
// composed object rather than one layer's record asks — the lease and role checks of plan 06
// §6.5, which place an object by its composed position (domain/protocol/policy.h).
std::string_view position_property(std::string_view type, const LayerPartition& partition);

// The tile a position value falls in under a grid of `tile_size`: `read_position`, floored by
// the tile size. False when the value is not a position, the size is not positive, or the tile
// leaves i32. `tile_of` is this over the record's own position property.
bool tile_of_position(const JsonValue& value, f64 tile_size, TileCoord& out);

// Reads a position out of a property value and projects it onto the horizontal plane. Accepts
// [x, y] and [x, y, z], an object with numeric `x` and `y` (and `z`), and a transform object
// with a `position` or `translation` member holding either of those. False when the value is
// not a position or is not finite.
bool read_position(const JsonValue& value, f64& out_x, f64& out_y);

// The tile `record` belongs to under `partition`. False when it has no usable position, or one
// so far out that its tile coordinates leave i32: it belongs in untiled.json either way.
bool tile_of(const ObjectRecord& record, const LayerPartition& partition, TileCoord& out);

// The canonical index of `layer` as it stands: occupied tiles in (x, y) order, each tile's
// records in id order, the untiled records in id order, and the bounds over the occupied
// tiles. An empty tile has no entry and no file.
LayerIndex build_layer_index(const Layer& layer);

// Occupied tiles, without building the index. Zero for a layer with no partition.
u32 tile_count(const Layer& layer);

}  // namespace engine::doc
