#include <foundation/image/decode.h>

// stb_image (public domain / MIT, third_party/LICENSES.md) is compiled here, in the one
// translation unit that needs it: PNG, JPEG, TGA, and BMP decoded from memory only, because
// file reads belong to io::read_file. Failure strings stay on -- they are what callers report
// -- and are made thread-local so two jobs decoding at once cannot swap messages. The include
// directory is marked SYSTEM and the warning state is pushed around the header: the engine's
// warning policy is for engine code.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_TGA
#define STBI_ONLY_BMP
#define STBI_NO_STDIO
#define STBI_THREAD_LOCAL thread_local
#if defined(_MSC_VER)
#pragma warning(push, 0)
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wdouble-promotion"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wcast-align"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#include <stb_image.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <cstring>
#include <limits>

namespace engine::image {

namespace {

// One stb call's worth of input: the decoder takes an int length, and an image whose pixels do
// not fit a Vector's u32 size is refused before anything is allocated.
constexpr usize k_max_input = static_cast<usize>(std::numeric_limits<int>::max());

void fail(std::string* error, std::string_view what) {
  if (error != nullptr) *error = std::string(what);
}

// stb's reason for the last failure; never null, so callers always get a message.
void fail_stb(std::string* error, std::string_view what) {
  if (error == nullptr) return;
  const char* reason = stbi_failure_reason();
  *error = std::string(what) + (reason != nullptr ? reason : "unknown error");
}

bool check_input(std::span<const u8> bytes, std::string_view what, std::string* error) {
  if (bytes.empty()) {
    fail(error, std::string(what) + "no data");
    return false;
  }
  if (bytes.size() > k_max_input) {
    fail(error, std::string(what) + "input larger than 2 GiB");
    return false;
  }
  return true;
}

}  // namespace

bool probe_image(std::span<const u8> bytes, ImageInfo& out, std::string* error) {
  out = ImageInfo{};
  if (!check_input(bytes, "probe_image: ", error)) return false;

  int width = 0;
  int height = 0;
  int channels = 0;
  if (stbi_info_from_memory(bytes.data(), static_cast<int>(bytes.size()), &width, &height,
                            &channels) == 0) {
    fail_stb(error, "probe_image: ");
    return false;
  }
  if (width <= 0 || height <= 0 || channels < 1 || channels > 4) {
    fail(error, "probe_image: implausible header");
    return false;
  }
  out.width = static_cast<u32>(width);
  out.height = static_cast<u32>(height);
  out.channels = static_cast<u32>(channels);
  return true;
}

bool decode_image(std::span<const u8> bytes, Image& out, u32 desired_channels, std::string* error) {
  out = Image{};
  if (desired_channels > 4) {
    fail(error, "decode_image: desired_channels must be 0 (as stored) or 1 to 4");
    return false;
  }
  if (!check_input(bytes, "decode_image: ", error)) return false;

  int width = 0;
  int height = 0;
  int file_channels = 0;
  stbi_uc* pixels =
      stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &width, &height,
                            &file_channels, static_cast<int>(desired_channels));
  if (pixels == nullptr) {
    fail_stb(error, "decode_image: ");
    return false;
  }

  if (width <= 0 || height <= 0 || file_channels < 1 || file_channels > 4) {
    stbi_image_free(pixels);
    fail(error, "decode_image: implausible decoded image");
    return false;
  }
  const u32 channels = desired_channels != 0 ? desired_channels : static_cast<u32>(file_channels);
  const u64 total = u64{static_cast<u32>(width)} * static_cast<u32>(height) * channels;
  if (total > u64{Vector<u8>::max_size()}) {
    stbi_image_free(pixels);
    fail(error, "decode_image: decoded image is too large to hold");
    return false;
  }

  // Copy into the engine's allocator and hand stb's buffer straight back.
  out.pixels.resize(static_cast<u32>(total));
  std::memcpy(out.pixels.data(), pixels, static_cast<usize>(total));
  stbi_image_free(pixels);
  out.width = static_cast<u32>(width);
  out.height = static_cast<u32>(height);
  out.channels = channels;
  return true;
}

io::Status read_image(std::string_view native_path, Image& out, u32 desired_channels,
                      std::string* error) {
  out = Image{};
  if (desired_channels > 4) {
    fail(error, "read_image: desired_channels must be 0 (as stored) or 1 to 4");
    return io::Status::InvalidArgument;
  }
  std::string data;
  const io::Status status = io::read_file(native_path, data);
  if (status != io::Status::Ok) {
    fail(error, "read_image: " + std::string(native_path) + ": " + io::status_name(status));
    return status;
  }
  const auto bytes = std::span<const u8>(reinterpret_cast<const u8*>(data.data()), data.size());
  if (!decode_image(bytes, out, desired_channels, error)) return io::Status::IoError;
  return io::Status::Ok;
}

}  // namespace engine::image
