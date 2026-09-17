#pragma once

// Deflate (RFC 1951) and its zlib wrapper (RFC 1950), written here rather than linked: the
// PNG encoder is the only compressor the engine needs, and a capture path wants a compressor
// whose cost it can predict. Three levels, one matcher:
//
//   Stored   no compression at all. Stored blocks of 65535 bytes, a memcpy plus framing; the
//            output the encoder produced before there was a compressor, byte for byte.
//   Fast     LZ77 with a short hash chain, greedy matches, fixed Huffman codes. Several
//            hundred MB/s of input on a render-like image.
//   Default  the same matcher with a longer chain and lazy matching, and a dynamic Huffman
//            code built from each block's own symbol frequencies.
//
// Blocks are at most 256 KiB of input or 65535 symbols, and each one is emitted whichever of
// stored, fixed, and dynamic is smallest, so incompressible input costs five bytes per 64 KiB
// instead of growing. The LZ77 window spans block boundaries, as zlib's does.
//
// Not here: an inflater (decoding goes through stb_image in `decode.h`; the tests carry a
// small reference inflater), gzip framing, and presets above `Default` - a capture is written
// once and read once, so the last few percent of ratio is not worth the time it costs.

#include <core/base/types.h>
#include <core/containers/vector.h>

#include <span>

namespace engine::image {

// How hard the compressor works. The enum is the level knob for `deflate`, `zlib_compress`,
// and `encode_png`; every level produces a stream every decoder reads.
enum class Compression : u8 {
  Stored,   // stored blocks only: no matching, no Huffman, output is the input plus framing
  Fast,     // short chain, greedy matching, fixed Huffman codes
  Default,  // longer chain, lazy matching, dynamic Huffman codes per block
};

// Raw deflate stream (RFC 1951), replacing whatever `out` held. The stream always ends with a
// BFINAL block, so it is complete on return. `input` must be smaller than 4 GiB, which the
// `Vector` holding the result requires anyway.
void deflate(std::span<const u8> input, Compression level, Vector<u8>& out);

// The same stream inside a zlib container (RFC 1950): the two-byte header PNG's IDAT expects,
// then the deflate stream, then the big-endian adler32 of `input`.
void zlib_compress(std::span<const u8> input, Compression level, Vector<u8>& out);

}  // namespace engine::image
