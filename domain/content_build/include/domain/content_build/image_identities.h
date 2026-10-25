#pragma once

// **What a load knows about its source images** (docs/subsystems/content_build.md, "One hash per
// image, and a pack's from its manifest"; roadmap W75). A built texture is addressed by
// `texture::texture_cache_key(hash_bytes(image), options)`, so finding one by an image named by
// path needs the image's hash, and until 2026-10-24 every reader took it by reading the file: the
// renderer's texture step once per mesh that named it, the GPU upload again, a baked tree's
// source hash a third time — 2.7 GB of the forest's 4.4 GB read before ready.
//
// This is one table for a load (or a content build), shared by every reader in it:
//
// - **A file is read and hashed at most once.** The first reader to ask hashes it; a second
//   reader asking meanwhile waits for that answer rather than reading the file again.
// - **A material pack's file is not read at all once it has been hashed under that pack.** A
//   pack is fetched and verified as a unit against its `pack.json`, which pins every map by its
//   SHA-256 (materials.md), and `materials::Pack::hash` is over that text — so a file the pack
//   names has the same bytes for as long as the pack has the same hash, and the hash those bytes
//   give is derived data: `<ddc>/textures/packs/<pack hash>.json`, written when a load hashed a
//   file the index did not hold. A pack edited in place without its manifest is not a pack; the
//   one place that reads such a file anyway — a build of its missing texture — checks the bytes
//   against the index and says so when they differ.
// - **Every other file is hashed as it is now**, once per load: a glTF's sibling PNG and a
//   recipe's own set are covered by nothing the load reads, so a repainted one still finds its
//   new texture or none (texture.md).
//
// Thread-safe: the per-mesh loads and the texture step's jobs ask concurrently. Nothing here
// decides from a timestamp.

#include <core/base/types.h>
#include <core/containers/hash_map.h>
#include <core/containers/vector.h>

#include <condition_variable>
#include <mutex>
#include <string>
#include <string_view>

namespace engine::materials {
struct Pack;
}

namespace engine::content_build {

// The pack index's file for a pack under a derived-data root: `<ddc>/textures/packs/<hash>.json`,
// the pack's `materials::Pack::hash` as sixteen lower-case hex digits.
std::string pack_index_path(std::string_view ddc, u64 pack_hash);

class ImageIdentities {
 public:
  // `ddc` empty keeps no pack index (every file is hashed once a load); `pack` null, or a pack
  // with no roles, likewise.
  ImageIdentities(std::string ddc, const materials::Pack* pack);
  ImageIdentities(const ImageIdentities&) = delete;
  ImageIdentities& operator=(const ImageIdentities&) = delete;

  // `hash_bytes` over the file at `path` (a native path, as the container's records resolve it),
  // and its size: from this load's table, from the pack's index, or by reading it once. False
  // with the reason when it cannot be read.
  bool identify(std::string_view path, u64& hash, u64& size, std::string& error);

  // The file's bytes, for a reader that must have them (a texture build): read now, counted, and
  // checked against an identity the table already held — a pack file that no longer hashes as its
  // index says is a warning, and the bytes' own hash is what `hash` returns and what the table
  // keeps from then on.
  bool read(std::string_view path, std::string& bytes, u64& hash, std::string& error);

  // Writes the pack index when this load hashed a pack file it did not hold (or found one that
  // moved). Best effort: a cache that cannot be written costs the next load the reads.
  void save();

  struct Counts {
    u32 from_index = 0;    // pack files answered from the pack's index, no read
    u32 hashed = 0;        // files read and hashed to answer `identify`
    u32 repeats = 0;       // `identify` calls answered by this load's table
    u32 reads = 0;         // source image files opened, by either call
    u64 read_bytes = 0;    // what those reads returned
    u32 pack_changed = 0;  // pack files whose bytes no longer gave their index's hash
  };
  Counts counts() const;

 private:
  struct Entry {
    std::string path;
    u64 hash = 0;
    u64 size = 0;
    bool known = false;    // hash and size are an answer
    bool pending = false;  // a reader is hashing it now
    bool pack = false;     // the pack names it
    bool indexed = false;  // the answer came from the pack index and no read has checked it
    std::string error;     // why it could not be read; with `known` false and `pending` false
  };
  u32 entry_of(const std::string& path);  // with the lock held
  void load_index();

  std::string ddc_;
  u64 pack_hash_ = 0;
  mutable std::mutex mutex_;
  std::condition_variable done_;
  Vector<Entry> entries_;
  HashMap<u64, u32> index_;  // hash_bytes(path) -> entry; a collision is compared by path
  bool dirty_ = false;
  Counts counts_;
};

}  // namespace engine::content_build
