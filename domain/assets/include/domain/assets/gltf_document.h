#pragma once

// A glTF file as a document to edit and write back, beside the importer (gltf.h), which reads one
// into a mesh and never writes. `engine-content morphs` (docs/subsystems/assets.md, "Editing a
// glTF document") is the first writer: it appends morph channels to an authored body and must
// leave every object it does not touch exactly as it was, so the model here is the file's own
// JSON, not the importer's flattened mesh.
//
// Why a JSON model of its own rather than `core/json`: `JsonValue`'s objects are sorted maps
// (the canonical writer's property), and a glTF document's key order is the authoring tool's. A
// document edited here keeps every object's keys in the order the file gave them, appends a new
// key at the end, keeps an integer as the integer it was, and keeps a float as the double it
// parses to.
//
// The writer is Python's `json.dumps(value, indent=2)` byte for byte — two-space indentation,
// one item a line with the `,` at the line's end, `": "` after a key, `[]`/`{}` for empty
// containers, every non-ASCII or control character escaped as a lowercase `\uXXXX` (surrogate pairs
// past the BMP), and a float as Python's `repr` (the shortest digits that round-trip; positional
// between 1e-4 and 1e16, `1e-05`/`1e+16` style outside, `.0` on a whole number). It is that writer
// because the documents it edits come out of Python (Blender's exporter, the authoring scripts, the
// forest proof of concept's v27 recipe), and the acceptance test of the first job was byte identity
// with a Python-written file; a canonical sorted writer would turn every edit into a rewrite of the
// whole file.

#include <core/base/types.h>
#include <core/containers/vector.h>

#include <span>
#include <string>
#include <string_view>

namespace engine::assets {

// One JSON value with its object's key order kept. Numbers keep their kind: an integer token is
// kept as its text (so a 64-bit id or a hash survives unchanged), anything with a fraction or an
// exponent is a double.
struct DocValue {
  enum class Kind : u8 { null, boolean, integer, real, string, array, object };

  Kind kind = Kind::null;
  bool boolean = false;
  f64 real = 0.0;
  std::string text;          // the integer's digits, or the string's UTF-8 bytes
  Vector<std::string> keys;  // an object's keys in order, parallel to `items`
  Vector<DocValue> items;    // an array's elements or an object's values

  static DocValue make_null() { return DocValue{}; }
  static DocValue make_bool(bool v);
  static DocValue make_int(i64 v);
  static DocValue make_real(f64 v);
  static DocValue make_string(std::string_view v);
  static DocValue make_array();
  static DocValue make_object();

  bool is_object() const noexcept { return kind == Kind::object; }
  bool is_array() const noexcept { return kind == Kind::array; }
  bool is_number() const noexcept { return kind == Kind::integer || kind == Kind::real; }
  bool is_string() const noexcept { return kind == Kind::string; }

  // An object's member, or nullptr when absent or not an object.
  const DocValue* find(std::string_view key) const noexcept;
  DocValue* find(std::string_view key) noexcept;
  // Sets an object's member in place when present, and appends it at the end when not — the
  // order a Python dict gives `d[key] = value`.
  DocValue& set(std::string_view key, DocValue value);
  // Appends to an array.
  DocValue& push_back(DocValue value);

  // A number as a double (an integer's text converted), false when not a number.
  bool get_f64(f64& out) const noexcept;
  // A number with an integral value that fits, false otherwise.
  bool get_i64(i64& out) const noexcept;
};

// Parses JSON text (a UTF-8 byte-order mark is skipped). False with a message naming the byte
// offset on malformed input, a nesting deeper than 256, or a lone UTF-16 surrogate escape.
bool parse_document_json(std::string_view text, DocValue& out, std::string* error = nullptr);

// Python's `json.dumps(value, indent=2)`, with `newline` at every line break the indentation makes
// (Python's text-mode write on Windows turns them into "\r\n"). No trailing newline. False when a
// float is NaN or infinite, which JSON cannot hold.
bool write_document_json(const DocValue& value, std::string& out, std::string_view newline = "\n");

// Python's `repr(float)`: the shortest decimal that round-trips, positional for a decimal
// exponent from -4 up to 15 and scientific (`1e-05`, `1.5e+16`) outside it, `.0` on a whole
// number, "-0.0" for negative zero.
std::string python_float_repr(f64 value);

// A glTF document with its one buffer: a `.gltf` and the file its `buffers[0].uri` names, or a
// `.glb`'s JSON chunk and BIN chunk. A document with more than one buffer, or a buffer held in a
// data URI, is refused: nothing that writes one has needed it.
struct GltfDocument {
  DocValue json;
  // Buffer 0: the whole file `buffers[0].uri` names, or the whole BIN chunk (its padding
  // included). Whole, because the Python recipe this replaced kept the whole file as the prefix
  // its output must start with.
  Vector<u8> bin;
  bool from_glb = false;
};

bool read_gltf_document(std::string_view path, GltfDocument& out, std::string* error = nullptr);

// An accessor's elements as doubles, row by row (`components` per element), through its buffer
// view's offset and stride. Float, and unsigned 8-, 16- and 32-bit components; a normalized
// accessor or a sparse one is refused.
bool read_accessor(const GltfDocument& doc, u32 accessor, Vector<f64>& out, u32& components,
                   std::string* error = nullptr);

// Appends `values` (rows of `components` floats, 1 for SCALAR or 3 for VEC3) to buffer 0 — padded
// with zeros to a four-byte boundary first — with a buffer view `{buffer, byteOffset,
// byteLength}` and an accessor `{bufferView, componentType, count, type, min, max}` after the
// existing ones, and returns the accessor's index. `min` and `max` are the per-component extremes
// of the floats as stored.
u32 append_float_accessor(GltfDocument& doc, std::span<const f32> values, u32 components);

// Writes `<dir>/<stem>.gltf` and `<dir>/<stem>.bin`, after setting `buffers` to the one entry
// `{"byteLength": n, "uri": "<stem>.bin"}`. The JSON is `write_document_json`'s with `newline`,
// followed by one more `newline`.
bool write_gltf_document(GltfDocument& doc, std::string_view dir, std::string_view stem,
                         std::string_view newline, std::string* error = nullptr);

}  // namespace engine::assets
