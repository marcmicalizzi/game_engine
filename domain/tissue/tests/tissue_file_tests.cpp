// The container and the interchange (docs/subsystems/tissue.md, "The container" and "The
// interchange"): a definition round-trips through both; a container of a newer build still loads,
// skipping what it does not know and saying so; and every damage the hashes exist to catch is
// caught.
#include "fixture.h"

#include <core/json/json.h>
#include <core/schema/json_reflect.h>
#include <domain/tissue/sha256.h>
#include <domain/tissue/tissue_file.h>
#include <foundation/io/vfs.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <cstring>
#include <string>

using namespace engine;
using namespace engine::tissue;

namespace {

Vector<u8> encoded(const TissueFile& file) {
  Vector<u8> bytes;
  std::string error;
  const bool ok = encode_tissue_file(file, bytes, &error);
  INFO(error);
  REQUIRE(ok);
  return bytes;
}

bool same_blocks(const TissueFile& a, const TissueFile& b) {
  if (a.blocks.size() != b.blocks.size()) return false;
  for (u32 i = 0; i < a.blocks.size(); ++i) {
    if (a.blocks[i].name != b.blocks[i].name || a.blocks[i].kind != b.blocks[i].kind ||
        a.blocks[i].count != b.blocks[i].count || a.blocks[i].bytes != b.blocks[i].bytes)
      return false;
  }
  return true;
}

// Rewrites the container's content hash after a deliberate edit, so the edit is what gets caught.
void rehash(Vector<u8>& bytes) {
  TissueFileHeader header;
  std::memcpy(&header, bytes.data(), sizeof(header));
  header.content_hash = hash_bytes(bytes.data() + sizeof(header), bytes.size() - sizeof(header));
  std::memcpy(bytes.data(), &header, sizeof(header));
}

}  // namespace

TEST_CASE("tissue file: a definition and its blocks round-trip through the container") {
  const fixture::Built built = fixture::make();
  const Vector<u8> bytes = encoded(built.file);
  TissueFile back;
  std::string error;
  REQUIRE(read_tissue_file_memory(bytes, back, &error));
  CHECK(back.warnings.empty());
  CHECK(same_blocks(back, built.file));
  CHECK(back.definition.name == built.file.definition.name);
  CHECK(back.definition.regions.size() == 1);
  CHECK(back.definition.bindings.front().normal_mode == "limit-interpolated");
  // The container's table carries each block's SHA-256 and no file names.
  for (const Block& entry : back.definition.blocks) {
    CHECK(entry.file.empty());
    CHECK(entry.sha256 == sha256_hex(back.find(entry.name)->bytes));
  }
  // Writing what was read gives the same bytes: the definition's JSON is canonical.
  CHECK(encoded(back) == bytes);

  TissueFileInfo info;
  REQUIRE(read_tissue_file_info(bytes, info, &error));
  CHECK(info.sections.size() == built.file.blocks.size() + 2);
  CHECK(info.sections[0].kind_name == "Definition");
  CHECK(info.sections[1].kind_name == "Names");
  for (const TissueFileSectionInfo& s : info.sections) {
    CHECK(s.known);
    CHECK(s.offset % k_tissue_file_alignment == 0);
  }
}

TEST_CASE("tissue file: a kind this build does not know is skipped, with a warning") {
  // What a newer writer's container looks like to this build: a section of kind 200 and a table
  // entry naming it, and a definition field nobody here has heard of.
  const fixture::Built built = fixture::make();
  Vector<u8> bytes = encoded(built.file);
  TissueFileHeader header;
  std::memcpy(&header, bytes.data(), sizeof(header));
  // Relabel the cable block's section as kind 200, as a future element kind would be.
  u32 relabelled = 0;
  TissueFileInfo info;
  REQUIRE(read_tissue_file_info(bytes, info));
  for (u32 i = 0; i < header.section_count; ++i) {
    if (info.sections[i].name != "slab.cables") continue;
    TissueFileSection s;
    std::memcpy(&s, bytes.data() + sizeof(header) + i * sizeof(TissueFileSection), sizeof(s));
    s.kind = 200;
    std::memcpy(bytes.data() + sizeof(header) + i * sizeof(TissueFileSection), &s, sizeof(s));
    ++relabelled;
  }
  REQUIRE(relabelled == 1);
  rehash(bytes);
  TissueFile back;
  std::string error;
  REQUIRE(read_tissue_file_memory(bytes, back, &error));
  CHECK(back.find("slab.cables") == nullptr);
  bool said_so = false;
  for (const std::string& w : back.warnings)
    said_so = said_so || w.find("slab.cables") != std::string::npos;
  CHECK(said_so);
  // Everything else is there.
  CHECK(back.find("slab.tets") != nullptr);
  CHECK(back.blocks.size() == built.file.blocks.size() - 1);
}

TEST_CASE(
    "tissue file: an unknown definition field and an unknown block kind name are warned about") {
  const fixture::Built built = fixture::make();
  // Build the container by hand around an edited definition: one field added to a region, and one
  // table entry of a kind name from the future.
  JsonValue json = schema::to_json(built.file.definition);
  json["regions"][0].set("rod_elements", JsonValue("a kind from a later build"));
  JsonValue future = JsonValue::object();
  future.set("name", JsonValue("slab.rods"));
  future.set("kind", JsonValue("RodElements"));
  future.set("count", JsonValue(u64{0}));
  future.set("bytes", JsonValue(u64{0}));
  future.set("sha256", JsonValue(sha256_hex({})));
  future.set("file", JsonValue(""));
  json["blocks"].push_back(std::move(future));
  const std::string text = write_json(json, JsonWriteOptions{.pretty = false});
  // Swap the definition section's bytes for the edited text: re-encode with the original, then
  // splice, since the definition is the first payload.
  Vector<u8> bytes = encoded(built.file);
  TissueFileInfo info;
  REQUIRE(read_tissue_file_info(bytes, info));
  Vector<u8> spliced(bytes.begin(), bytes.begin() + static_cast<u32>(info.sections[0].offset));
  for (const char c : text)
    spliced.push_back(static_cast<u8>(c));
  while (spliced.size() % k_tissue_file_alignment != 0)
    spliced.push_back(0);
  const u64 shift = spliced.size() - info.sections[1].offset;
  for (u32 i = static_cast<u32>(info.sections[1].offset); i < bytes.size(); ++i)
    spliced.push_back(bytes[i]);
  TissueFileHeader header;
  std::memcpy(&header, spliced.data(), sizeof(header));
  header.total_bytes = spliced.size();
  std::memcpy(spliced.data(), &header, sizeof(header));
  for (u32 i = 0; i < header.section_count; ++i) {
    TissueFileSection s;
    std::memcpy(&s, spliced.data() + sizeof(header) + i * sizeof(s), sizeof(s));
    if (i == 0)
      s.element_count = text.size();
    else
      s.offset += shift;
    std::memcpy(spliced.data() + sizeof(header) + i * sizeof(s), &s, sizeof(s));
  }
  rehash(spliced);
  TissueFile back;
  std::string error;
  const bool ok = read_tissue_file_memory(spliced, back, &error);
  INFO(error);
  REQUIRE(ok);
  bool field = false;
  bool kind = false;
  for (const std::string& w : back.warnings) {
    field = field || w.find("rod_elements") != std::string::npos;
    kind = kind || w.find("slab.rods") != std::string::npos;
  }
  CHECK(field);
  CHECK(kind);
  CHECK(same_blocks(back, built.file));
}

TEST_CASE("tissue file: damage is refused") {
  const fixture::Built built = fixture::make();
  const Vector<u8> bytes = encoded(built.file);
  TissueFile back;
  std::string error;

  Vector<u8> flipped = bytes;
  flipped[flipped.size() - 5] ^= 0x01u;
  CHECK_FALSE(read_tissue_file_memory(flipped, back, &error));
  CHECK(error.find("content hash") != std::string::npos);

  // A block altered and the content hash repaired: the block's own SHA-256 still catches it.
  TissueFileInfo info;
  REQUIRE(read_tissue_file_info(bytes, info));
  Vector<u8> altered = bytes;
  for (const TissueFileSectionInfo& s : info.sections)
    if (s.name == "slab.nodes") altered[static_cast<u32>(s.offset) + 3] ^= 0x10u;
  rehash(altered);
  CHECK_FALSE(read_tissue_file_memory(altered, back, &error));
  CHECK(error.find("slab.nodes") != std::string::npos);

  Vector<u8> truncated(bytes.begin(), bytes.end() - 16);
  CHECK_FALSE(read_tissue_file_memory(truncated, back, &error));
  Vector<u8> wrong_magic = bytes;
  wrong_magic[0] = 'X';
  CHECK_FALSE(read_tissue_file_memory(wrong_magic, back, &error));
  Vector<u8> wrong_version = bytes;
  wrong_version[4] = 9;
  CHECK_FALSE(read_tissue_file_memory(wrong_version, back, &error));
  CHECK(error.find("version") != std::string::npos);
}

TEST_CASE("tissue file: the interchange round-trips, and every inconsistency is refused") {
  engine::test::TempDir tmp("tissue_interchange");
  const fixture::Built built = fixture::make();
  std::string error;
  REQUIRE(export_interchange(built.file, tmp.path(), "slab", &error));
  const std::string json = tmp.file("slab.json");
  TissueFile imported;
  const bool ok = import_interchange(json, imported, &error);
  INFO(error);
  REQUIRE(ok);
  CHECK(same_blocks(imported, built.file));
  // The table names the files the export wrote.
  for (const Block& entry : imported.definition.blocks)
    CHECK(io::exists(tmp.file(entry.file)));
  // Import, then write a container: the same bytes as writing the original.
  CHECK(encoded(imported) == encoded(built.file));

  // Refusals, each on a fresh export with one thing wrong.
  const auto edit = [&](auto&& change) {
    engine::test::TempDir broken("tissue_interchange_broken");
    REQUIRE(export_interchange(built.file, broken.path(), "slab", &error));
    change(broken);
    TissueFile out;
    const bool accepted = import_interchange(broken.file("slab.json"), out, &error);
    return accepted ? std::string() : error;
  };
  // A block file one byte short.
  CHECK(edit([&](engine::test::TempDir& d) {
          std::string bytes;
          io::read_file(d.file("slab.slab.nodes.bin"), bytes);
          bytes.pop_back();
          io::write_file(d.file("slab.slab.nodes.bin"), bytes);
        }).find("slab.nodes") != std::string::npos);
  // A byte changed: the SHA-256 catches it.
  CHECK(edit([&](engine::test::TempDir& d) {
          std::string bytes;
          io::read_file(d.file("slab.slab.tets.bin"), bytes);
          bytes[0] = static_cast<char>(bytes[0] ^ 1);
          io::write_file(d.file("slab.slab.tets.bin"), bytes);
        }).find("SHA-256") != std::string::npos);
  // The JSON's format, a count that does not match the bytes, a duplicate name, an unknown field.
  const auto json_edit = [&](auto&& change) {
    return edit([&](engine::test::TempDir& d) {
      std::string text;
      io::read_file(d.file("slab.json"), text);
      JsonValue value;
      REQUIRE(parse_json(text, value).ok);
      change(value);
      io::write_file(d.file("slab.json"), write_json(value));
    });
  };
  CHECK(json_edit([](JsonValue& v) {
          v.set("format", JsonValue("engine.tissue.v9"));
        }).find("format") != std::string::npos);
  CHECK(json_edit([](JsonValue& v) {
          v["blocks"][0].set("count", JsonValue(u64{1}));
        }).find("says") != std::string::npos);
  CHECK(json_edit([](JsonValue& v) {
          JsonValue copy = v["blocks"][0];
          v["blocks"].push_back(std::move(copy));
        }).find("two blocks") != std::string::npos);
  CHECK(!json_edit([](JsonValue& v) { v.set("surprise", JsonValue(true)); }).empty());
  CHECK(json_edit([](JsonValue& v) {
          v["blocks"][0].set("kind", JsonValue("Names"));
        }).find("kind") != std::string::npos);
}

TEST_CASE("tissue file: a written container reads back from disk") {
  engine::test::TempDir tmp("tissue_container");
  const fixture::Built built = fixture::make();
  std::string error;
  const std::string path = tmp.file("slab.tissue");
  REQUIRE(write_tissue_file(path, built.file, &error));
  TissueFile back;
  REQUIRE(read_tissue_file(path, back, &error));
  CHECK(same_blocks(back, built.file));
  CHECK_FALSE(read_tissue_file(tmp.file("missing.tissue"), back, &error));
}
