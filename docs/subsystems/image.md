# image (foundation)

**Purpose.** Dependency-free image encoding for captures, test fixtures, and the screenshots agents look at. `encode_png` writes a PNG whose image data is a zlib stream of stored (uncompressed) deflate blocks, so every decoder reads it and nothing has to be linked: output is the raw size plus about 1/64. `write_png` does the same to a file through `io::write_file`. `crc32` and `adler32` are exposed for tests and other containers.

**Why stored blocks.** A 4K RGBA capture becomes a 33 MB PNG in the time of a memcpy and two checksums, which is what a capture path wants; disk size does not matter for a screenshot a test or an agent reads once. Real compression, decoding, and EXR for HDR captures arrive with the capture API's consumers (docs/plan/06-tooling.md).

**Public API.** `foundation/image/png.h`: `encode_png(width, height, channels, pixels, out)` for 1 to 4 channels at 8 bits, `write_png(path, ...)`, `crc32`, `adler32`. `io::Status::InvalidArgument` is returned when the dimensions and byte count disagree.

**Depends on.** `base`, `containers`, `io`.

**Testing.** `tools/dev.ps1 test -Filter image`: reference checksum values, a small RGBA image walked chunk by chunk with every CRC verified and the stored stream inflated by hand, a 300×300 gray image spanning several blocks, RGB, argument rejection, and a file round trip. `domain/gfx`'s capture test encodes a GPU capture and `engine-view --capture` writes real frames.
