// The editable glTF document (gltf_document.h): the order-keeping JSON model, the writer that
// must match Python's json.dumps(indent=2) byte for byte, and the buffer's accessors.

#include <domain/assets/gltf_document.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

using namespace engine;
using namespace engine::assets;

TEST_CASE("python_float_repr matches Python's repr") {
  // Each expectation is what CPython 3 prints for repr(x).
  CHECK(python_float_repr(0.0) == "0.0");
  CHECK(python_float_repr(-0.0) == "-0.0");
  CHECK(python_float_repr(1.0) == "1.0");
  CHECK(python_float_repr(-2.0) == "-2.0");
  CHECK(python_float_repr(0.1) == "0.1");
  CHECK(python_float_repr(100.0) == "100.0");
  CHECK(python_float_repr(0.75) == "0.75");
  CHECK(python_float_repr(0.0001) == "0.0001");
  CHECK(python_float_repr(0.00001) == "1e-05");
  CHECK(python_float_repr(1.5e-7) == "1.5e-07");
  CHECK(python_float_repr(1234567890123456.0) == "1234567890123456.0");
  CHECK(python_float_repr(1e16) == "1e+16");
  CHECK(python_float_repr(1.5e16) == "1.5e+16");
  CHECK(python_float_repr(1e100) == "1e+100");
  CHECK(python_float_repr(123456789012345678.0) == "1.2345678901234568e+17");
  CHECK(python_float_repr(static_cast<f64>(0.1f)) == "0.10000000149011612");
  CHECK(python_float_repr(static_cast<f64>(1.0000001f)) == "1.0000001192092896");
  CHECK(python_float_repr(7.3292660829338274e-09) == "7.3292660829338274e-09");
  CHECK(python_float_repr(0.10113268608414239) == "0.10113268608414239");
  CHECK(python_float_repr(-0.026648065075278282) == "-0.026648065075278282");
}

TEST_CASE("a document keeps its key order and writes as json.dumps(indent=2)") {
  const std::string text =
      "{\"b\": 1, \"a\": [1.0, -0, 2.5e-05, 1E2, \"x\\u00e9\\ud83d\\ude00\\n\\u007f/\"],"
      " \"e\": {}, \"f\": [], \"g\": {\"z\": null, \"y\": true, \"x\": false}, "
      "\"big\": 18446744073709551615}";
  DocValue doc;
  std::string error;
  REQUIRE_MESSAGE(parse_document_json(text, doc, &error), error);
  std::string out;
  REQUIRE(write_document_json(doc, out));
  const std::string expected =
      "{\n"
      "  \"b\": 1,\n"
      "  \"a\": [\n"
      "    1.0,\n"
      "    0,\n"
      "    2.5e-05,\n"
      "    100.0,\n"
      "    \"x\\u00e9\\ud83d\\ude00\\n\\u007f/\"\n"
      "  ],\n"
      "  \"e\": {},\n"
      "  \"f\": [],\n"
      "  \"g\": {\n"
      "    \"z\": null,\n"
      "    \"y\": true,\n"
      "    \"x\": false\n"
      "  },\n"
      "  \"big\": 18446744073709551615\n"
      "}";
  CHECK(out == expected);

  // A set on a present key keeps its place; a new key goes last, as a Python dict does.
  doc.set("a", DocValue::make_int(7));
  doc.set("new", DocValue::make_string("v"));
  REQUIRE(doc.keys.size() == 7);
  CHECK(doc.keys[1] == "a");
  CHECK(doc.keys[6] == "new");

  // CRLF line breaks, and the writer is idempotent over its own output.
  std::string crlf;
  REQUIRE(write_document_json(doc, crlf, "\r\n"));
  DocValue again;
  REQUIRE(parse_document_json(crlf, again));
  std::string twice;
  REQUIRE(write_document_json(again, twice, "\r\n"));
  CHECK(twice == crlf);
  CHECK(crlf.find("\r\n  \"new\": \"v\"\r\n}") != std::string::npos);
}

TEST_CASE("the parser refuses what JSON does not allow") {
  DocValue doc;
  CHECK_FALSE(parse_document_json("{\"a\": 1,}", doc));
  CHECK_FALSE(parse_document_json("[1 2]", doc));
  CHECK_FALSE(parse_document_json("\"\\ud800\"", doc));
  CHECK_FALSE(parse_document_json("01", doc));
  CHECK_FALSE(parse_document_json("{} x", doc));
  std::string deep(300, '[');
  CHECK_FALSE(parse_document_json(deep, doc));
  CHECK(parse_document_json("\xEF\xBB\xBF{\"a\": 2}", doc));
}

TEST_CASE("an appended accessor reads back, with its min and max, after the existing buffer") {
  test::TempDir tmp("gltf_document_append");
  REQUIRE(tmp.ok());
  // A .gltf with a 6-byte buffer, so the append has to pad to a four-byte boundary.
  {
    std::ofstream bin(tmp.file("in.bin"), std::ios::binary);
    const unsigned char bytes[6] = {1, 2, 3, 4, 5, 6};
    bin.write(reinterpret_cast<const char*>(bytes), 6);
    std::ofstream gltf(tmp.file("in.gltf"), std::ios::binary);
    gltf << "{\"asset\": {\"version\": \"2.0\"}, \"buffers\": [{\"uri\": \"in.bin\", "
            "\"byteLength\": 6}], \"bufferViews\": [{\"buffer\": 0, \"byteLength\": 6}], "
            "\"accessors\": [{\"bufferView\": 0, \"componentType\": 5123, \"count\": 3, "
            "\"type\": \"SCALAR\"}]}";
  }
  GltfDocument doc;
  std::string error;
  REQUIRE_MESSAGE(read_gltf_document(tmp.file("in.gltf"), doc, &error), error);
  CHECK_FALSE(doc.from_glb);
  REQUIRE(doc.bin.size() == 6);
  Vector<f64> values;
  u32 components = 0;
  REQUIRE(read_accessor(doc, 0, values, components, &error));
  REQUIRE(values.size() == 3);
  CHECK(values[0] == 513.0);  // 0x0201, little-endian
  CHECK(values[2] == 1541.0);

  const f32 rows[6] = {0.5f, -1.0f, 2.0f, -0.25f, 3.0f, 0.1f};
  const u32 index = append_float_accessor(doc, std::span<const f32>(rows, 6), 3);
  CHECK(index == 1);
  CHECK(doc.bin.size() == 8 + 24);
  CHECK(doc.bin[6] == 0);
  CHECK(doc.bin[7] == 0);
  REQUIRE(read_accessor(doc, 1, values, components, &error));
  CHECK(components == 3);
  CHECK(values[4] == 3.0);
  const DocValue& accessor = doc.json.find("accessors")->items[1];
  REQUIRE(accessor.keys.size() == 6);
  CHECK(accessor.keys[0] == "bufferView");
  CHECK(accessor.keys[5] == "max");
  CHECK(accessor.find("min")->items[0].real == -0.25);
  CHECK(accessor.find("max")->items[2].real == static_cast<f64>(2.0f));
  CHECK(doc.json.find("bufferViews")->items[1].find("byteOffset")->text == "8");

  REQUIRE_MESSAGE(write_gltf_document(doc, tmp.path(), "out", "\r\n", &error), error);
  std::ifstream in(tmp.file("out.gltf"), std::ios::binary);
  const std::string written((std::istreambuf_iterator<char>(in)), {});
  CHECK(written.find("\"buffers\": [\r\n    {\r\n      \"byteLength\": 32,\r\n      \"uri\": "
                     "\"out.bin\"\r\n    }\r\n  ]") != std::string::npos);
  CHECK(written.substr(written.size() - 3) == "}\r\n");
  CHECK(std::filesystem::file_size(tmp.file("out.bin")) == 32);
}
