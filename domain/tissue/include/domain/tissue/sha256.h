#pragma once

// SHA-256 (FIPS 180-4), for the tissue interchange's block hashes (docs/subsystems/tissue.md).
//
// Why SHA-256 and not core/hash's `hash_bytes`. The blocks are written by the authoring side — a
// Blender script — and the hash is the chain of custody between the array it saved and the bytes
// the engine reads: `hashlib.sha256(array.tobytes()).hexdigest()` is one line there, and every
// report in the handoff packets already names its files by SHA-256. `hash_bytes` is a fast table
// hash with no reference implementation outside this tree, which is the wrong property for a hash
// two tools must agree on. It lives here rather than in core/hash because a capability does not
// edit core (ADR-0027); if a second module wants it, it moves down with a note.
//
// A straightforward implementation, not a fast one: the blocks of a tissue definition are a few
// megabytes, hashed once at import and once at load.

#include <core/base/types.h>

#include <array>
#include <span>
#include <string>
#include <string_view>

namespace engine::tissue {

using Sha256Digest = std::array<u8, 32>;

Sha256Digest sha256(std::span<const u8> bytes) noexcept;

// The digest as 64 lower-case hex digits, the spelling `hexdigest()` gives and the interchange
// uses.
std::string sha256_hex(std::span<const u8> bytes);
std::string to_hex(const Sha256Digest& digest);

// Streaming, for data that is not one contiguous span (the topology hash widens indices as it
// goes).
class Sha256 {
 public:
  Sha256() noexcept;
  void update(std::span<const u8> bytes) noexcept;
  Sha256Digest finish() noexcept;

 private:
  void block(const u8* data) noexcept;
  u32 state_[8];
  u8 buffer_[64];
  u64 length_ = 0;  // bytes consumed so far
  u32 buffered_ = 0;
};

}  // namespace engine::tissue
