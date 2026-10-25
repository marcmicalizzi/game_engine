// One hash per source image per load, and a material pack's from its index (image_identities.h;
// content_build.md, "One hash per image, and a pack's from its manifest"; roadmap W75).
#include <core/hash/hash.h>
#include <domain/content_build/generator_hosts.h>
#include <domain/content_build/image_identities.h>
#include <domain/materials/materials.h>
#include <foundation/io/vfs.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <string>

using namespace engine;

namespace {

bool write(const std::string& path, const std::string& text) {
  return io::write_file(path, text) == io::Status::Ok;
}

// A pack of one role whose five maps are the test's own files.
bool make_pack(const test::TempDir& tmp, materials::Pack& out) {
  if (io::make_directories(tmp.file("pack/bark")) != io::Status::Ok) return false;
  const char* maps[] = {"base_color", "normal", "roughness", "metallic", "height"};
  std::string text =
      R"({"format":"engine.materials.pack/1","name":"t","version":"1","roles":{"palm/bark":)"
      R"({"tile_m":0.5,"normal_convention":"opengl","maps":{)";
  for (u32 i = 0; i < 5; ++i) {
    if (!write(tmp.file(std::string("pack/bark/") + maps[i] + ".png"),
               std::string("bytes of ") + maps[i]))
      return false;
    text += std::string(i > 0 ? "," : "") + "\"" + maps[i] + "\":{\"file\":\"bark/" + maps[i] +
            ".png\"}";
  }
  text += "}}}}";
  std::string error;
  return materials::parse_pack(text, tmp.file("pack"), out, error);
}

}  // namespace

TEST_CASE("image identities: a file is read and hashed once however often it is asked for") {
  const test::TempDir tmp("image_identities_once");
  REQUIRE(tmp.ok());
  const std::string file = tmp.file("leaf.png");
  REQUIRE(write(file, "leaf bytes"));
  content_build::ImageIdentities identities("", nullptr);
  u64 hash = 0;
  u64 size = 0;
  std::string error;
  for (u32 i = 0; i < 3; ++i) {
    REQUIRE_MESSAGE(identities.identify(file, hash, size, error), error);
    CHECK(hash == hash_bytes("leaf bytes", 10));
    CHECK(size == 10);
  }
  const content_build::ImageIdentities::Counts counts = identities.counts();
  CHECK(counts.reads == 1);
  CHECK(counts.hashed == 1);
  CHECK(counts.repeats == 2);
  CHECK(counts.from_index == 0);
  // A file that is not there is an answer too, said once.
  CHECK_FALSE(identities.identify(tmp.file("missing.png"), hash, size, error));
  CHECK_FALSE(error.empty());
}

TEST_CASE("image identities: a pack's files are answered from its index without a read") {
  const test::TempDir tmp("image_identities_pack");
  REQUIRE(tmp.ok());
  materials::Pack pack;
  REQUIRE(make_pack(tmp, pack));
  const std::string ddc = tmp.file("ddc");
  const std::string base = pack.roles[0].set.base_color;
  u64 first = 0;
  u64 size = 0;
  std::string error;
  {
    content_build::ImageIdentities identities(ddc, &pack);
    REQUIRE_MESSAGE(identities.identify(base, first, size, error), error);
    CHECK(identities.counts().reads == 1);
    identities.save();
  }
  CHECK(io::exists(content_build::pack_index_path(ddc, pack.hash)));
  {
    // The next load: the same pack, the same answer, no read.
    content_build::ImageIdentities identities(ddc, &pack);
    u64 again = 0;
    REQUIRE_MESSAGE(identities.identify(base, again, size, error), error);
    CHECK(again == first);
    const content_build::ImageIdentities::Counts counts = identities.counts();
    CHECK(counts.reads == 0);
    CHECK(counts.from_index == 1);
    // A file the pack does not name is not the index's to answer: it is read.
    REQUIRE(write(tmp.file("other.png"), "other"));
    REQUIRE(identities.identify(tmp.file("other.png"), again, size, error));
    CHECK(identities.counts().reads == 1);
  }
  {
    // Another pack (its text differs, so its hash does): its own index, which knows nothing yet.
    materials::Pack moved = pack;
    moved.hash ^= 1u;
    content_build::ImageIdentities identities(ddc, &moved);
    u64 again = 0;
    REQUIRE(identities.identify(base, again, size, error));
    CHECK(identities.counts().reads == 1);
  }
  {
    // A pack file edited in place without its manifest: the index still answers what it holds —
    // that is the pack's contract — and the one reader that reads the bytes (a build) says so and
    // takes the bytes' hash from then on.
    REQUIRE(write(base, "repainted"));
    content_build::ImageIdentities identities(ddc, &pack);
    u64 indexed = 0;
    REQUIRE(identities.identify(base, indexed, size, error));
    CHECK(indexed == first);
    std::string bytes;
    u64 read_hash = 0;
    REQUIRE(identities.read(base, bytes, read_hash, error));
    CHECK(bytes == "repainted");
    CHECK(read_hash == hash_bytes("repainted", 9));
    CHECK(identities.counts().pack_changed == 1);
    u64 now = 0;
    REQUIRE(identities.identify(base, now, size, error));
    CHECK(now == read_hash);
  }
}

TEST_CASE("image identities: a generator's hash hook answers from the table, as its bytes would") {
  const test::TempDir tmp("image_identities_hosts");
  REQUIRE(tmp.ok());
  REQUIRE(write(tmp.file("leaf.png"), "leaf"));
  content_build::ImageIdentities identities("", nullptr);
  content_build::GeneratorHosts hosts(tmp.file("tree.flora.json"), {}, 0, nullptr);
  hosts.set_identities(&identities);
  const assets::MeshGeneratorHosts* hooks = hosts.hooks();
  REQUIRE(hooks->hash_image != nullptr);
  u64 hash = 0;
  std::string error;
  REQUIRE_MESSAGE(hooks->hash_image(hooks->context, "leaf.png", hash, error), error);
  CHECK(hash == hash_bytes("leaf", 4));
  REQUIRE(hooks->hash_image(hooks->context, "leaf.png", hash, error));
  CHECK(identities.counts().reads == 1);
  // And without a table the hook still answers, from the bytes.
  content_build::GeneratorHosts plain(tmp.file("tree.flora.json"), {}, 0, nullptr);
  u64 direct = 0;
  REQUIRE(plain.hooks()->hash_image(plain.hooks()->context, "leaf.png", direct, error));
  CHECK(direct == hash);
}
