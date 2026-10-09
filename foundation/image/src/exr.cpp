#include <foundation/image/exr.h>

#include <cstdlib>
#include <cstring>
#include <tinyexr.h>

namespace engine::image {

u16 half_from_f32(f32 value) noexcept {
  u32 f = 0;
  std::memcpy(&f, &value, sizeof(f));
  const u32 sign = (f >> 16) & 0x8000u;
  const u32 bits = f & 0x7fffffffu;
  if (bits >= 0x7f800000u) {  // infinity, or a NaN kept quiet and a NaN
    const u32 payload = bits > 0x7f800000u ? (0x200u | ((bits >> 13) & 0x3ffu)) : 0u;
    return static_cast<u16>(sign | 0x7c00u | payload);
  }
  // 65520 and above round to infinity (65520 is the tie between 65504 and 2^16, and 65504 is odd).
  if (bits >= 0x477ff000u) return static_cast<u16>(sign | 0x7c00u);
  if (bits < 0x38800000u) {  // under 2^-14, the smallest normal half: a subnormal or zero
    if (bits < 0x33000000u) return static_cast<u16>(sign);  // under 2^-25 (a tie at it, to 0)
    const u32 exponent = bits >> 23;                        // 102 .. 112
    const u32 mantissa = (bits & 0x7fffffu) | 0x800000u;
    const u32 shift = 126u - exponent;  // to units of 2^-24, the subnormal half's step
    u32 q = mantissa >> shift;
    const u32 rest = mantissa & ((1u << shift) - 1u);
    const u32 tie = 1u << (shift - 1u);
    if (rest > tie || (rest == tie && (q & 1u) != 0u)) ++q;  // may carry into the first normal
    return static_cast<u16>(sign | q);
  }
  // A normal half: the exponent rebiased (127 to 15) and the mantissa's top ten bits, rounded on
  // the thirteen below them; a carry walks into the exponent, which is what rounding up does.
  u32 h = (bits - 0x38000000u) >> 13;
  const u32 rest = bits & 0x1fffu;
  if (rest > 0x1000u || (rest == 0x1000u && (h & 1u) != 0u)) ++h;
  return static_cast<u16>(sign | h);
}

f32 f32_from_half(u16 half) noexcept {
  const u32 sign = (static_cast<u32>(half) & 0x8000u) << 16;
  const u32 exponent = (static_cast<u32>(half) >> 10) & 0x1fu;
  const u32 mantissa = static_cast<u32>(half) & 0x3ffu;
  u32 f = 0;
  if (exponent == 0) {
    if (mantissa != 0) {  // subnormal: mantissa * 2^-24, exact in a float
      const f32 v = static_cast<f32>(mantissa) * (1.0f / 16777216.0f);
      std::memcpy(&f, &v, sizeof(f));
    }
    f |= sign;
  } else if (exponent == 31) {
    f = sign | 0x7f800000u | (mantissa << 13);
  } else {
    f = sign | ((exponent + 112u) << 23) | (mantissa << 13);
  }
  f32 out = 0.0f;
  std::memcpy(&out, &f, sizeof(out));
  return out;
}

namespace {

void set_error(std::string* error, std::string text) {
  if (error != nullptr) *error = std::move(text);
}

// tinyexr's names are fixed 256-byte fields; every name written here is a short literal.
template <usize N>
void copy_name(char (&field)[N], const char* name) {
  const usize length = std::strlen(name);
  std::memcpy(field, name, length < N ? length + 1 : N);
}

// tinyexr's error strings are its own allocations.
void take_error(std::string* error, const char* prefix, const char* message) {
  set_error(error, std::string(prefix) + (message != nullptr ? message : "unknown error"));
  if (message != nullptr) FreeEXRErrorMessage(message);
}

// The file's channels, sorted by name (exr.h says why), and where each one is in the caller's
// interleaved pixel.
struct Layout {
  u32 count = 0;
  const char* names[4] = {};
  u32 source[4] = {};
};

bool layout_for(u32 channels, Layout& out) {
  out = Layout{};
  switch (channels) {
    case 1: out = Layout{1, {"Y"}, {0}}; return true;
    case 3: out = Layout{3, {"B", "G", "R"}, {2, 1, 0}}; return true;
    case 4: out = Layout{4, {"A", "B", "G", "R"}, {3, 2, 1, 0}}; return true;
    default: return false;
  }
}

}  // namespace

bool encode_exr(u32 width, u32 height, u32 channels, std::span<const u16> halves, Vector<u8>& out,
                const ExrOptions& options, std::string* error) {
  out.clear();
  Layout layout;
  if (!layout_for(channels, layout)) {
    set_error(error, "encode_exr: channels must be 1, 3 or 4");
    return false;
  }
  const u64 pixels = u64{width} * height;
  if (width == 0 || height == 0 || width > 0x7fffffffu || height > 0x7fffffffu ||
      halves.size() != pixels * channels) {
    set_error(error, "encode_exr: the pixel count and the dimensions disagree");
    return false;
  }
  // tinyexr takes one plane per channel, in the header's order.
  Vector<u16> planes(static_cast<u32>(pixels * layout.count));
  unsigned char* plane_pointers[4] = {};
  for (u32 c = 0; c < layout.count; ++c) {
    u16* plane = planes.data() + static_cast<usize>(c) * static_cast<usize>(pixels);
    for (u64 i = 0; i < pixels; ++i) {
      plane[i] = halves[static_cast<usize>(i * channels + layout.source[c])];
    }
    plane_pointers[c] = reinterpret_cast<unsigned char*>(plane);
  }

  EXRHeader header;
  InitEXRHeader(&header);
  EXRImage image;
  InitEXRImage(&image);
  image.num_channels = static_cast<int>(layout.count);
  image.images = plane_pointers;
  image.width = static_cast<int>(width);
  image.height = static_cast<int>(height);

  EXRChannelInfo infos[4] = {};
  int pixel_types[4] = {};
  int requested[4] = {};
  for (u32 c = 0; c < layout.count; ++c) {
    copy_name(infos[c].name, layout.names[c]);
    pixel_types[c] = TINYEXR_PIXELTYPE_HALF;
    requested[c] = TINYEXR_PIXELTYPE_HALF;
  }
  header.num_channels = static_cast<int>(layout.count);
  header.channels = infos;
  header.pixel_types = pixel_types;
  header.requested_pixel_types = requested;
  header.compression_type = TINYEXR_COMPRESSIONTYPE_ZIP;

  EXRAttribute white{};
  if (options.white_luminance > 0.0f) {
    copy_name(white.name, "whiteLuminance");
    copy_name(white.type, "float");
    white.value = reinterpret_cast<unsigned char*>(const_cast<f32*>(&options.white_luminance));
    white.size = static_cast<int>(sizeof(f32));
    header.num_custom_attributes = 1;
    header.custom_attributes = &white;
  }

  unsigned char* memory = nullptr;
  const char* message = nullptr;
  const usize size = SaveEXRImageToMemory(&image, &header, &memory, &message);
  if (size == 0 || memory == nullptr) {
    take_error(error, "encode_exr: ", message);
    return false;
  }
  out.resize(static_cast<u32>(size));
  std::memcpy(out.data(), memory, size);
  std::free(memory);
  return true;
}

io::Status write_exr(std::string_view native_path, u32 width, u32 height, u32 channels,
                     std::span<const u16> halves, const ExrOptions& options) {
  Vector<u8> bytes;
  if (!encode_exr(width, height, channels, halves, bytes, options)) {
    return io::Status::InvalidArgument;
  }
  return io::write_file(
      native_path, std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

bool decode_exr(std::span<const u8> bytes, ExrImage& out, std::string* error) {
  out = ExrImage{};
  EXRVersion version;
  if (ParseEXRVersionFromMemory(&version, bytes.data(), bytes.size()) != TINYEXR_SUCCESS) {
    set_error(error, "decode_exr: not an OpenEXR file");
    return false;
  }
  if (version.tiled != 0 || version.multipart != 0 || version.non_image != 0) {
    set_error(error, "decode_exr: only single-part scanline images are read");
    return false;
  }
  EXRHeader header;
  InitEXRHeader(&header);
  const char* message = nullptr;
  if (ParseEXRHeaderFromMemory(&header, &version, bytes.data(), bytes.size(), &message) !=
      TINYEXR_SUCCESS) {
    take_error(error, "decode_exr: ", message);
    return false;
  }
  // Which of the file's channels is R, G, B, A or Y; every one is read as half.
  int slot_of[4] = {-1, -1, -1, -1};  // R, G, B, A
  int luminance = -1;
  bool known = true;
  for (int c = 0; c < header.num_channels; ++c) {
    const char* name = header.channels[c].name;
    if (std::strcmp(name, "R") == 0) {
      slot_of[0] = c;
    } else if (std::strcmp(name, "G") == 0) {
      slot_of[1] = c;
    } else if (std::strcmp(name, "B") == 0) {
      slot_of[2] = c;
    } else if (std::strcmp(name, "A") == 0) {
      slot_of[3] = c;
    } else if (std::strcmp(name, "Y") == 0) {
      luminance = c;
    } else {
      known = false;
    }
    // A channel is read as it is stored, and a FLOAT one rounded to half below.
    if (header.pixel_types[c] == TINYEXR_PIXELTYPE_UINT) known = false;
    header.requested_pixel_types[c] = header.pixel_types[c];
  }
  const bool rgb = slot_of[0] >= 0 && slot_of[1] >= 0 && slot_of[2] >= 0 && luminance < 0;
  const bool gray = luminance >= 0 && header.num_channels == 1;
  if (!known || (!rgb && !gray)) {
    FreeEXRHeader(&header);
    set_error(error, "decode_exr: the channels are not Y, or R, G and B with or without A");
    return false;
  }
  for (int i = 0; i < header.num_custom_attributes; ++i) {
    const EXRAttribute& a = header.custom_attributes[i];
    if (std::strcmp(a.name, "whiteLuminance") == 0 && std::strcmp(a.type, "float") == 0 &&
        a.size == static_cast<int>(sizeof(f32)) && a.value != nullptr) {
      std::memcpy(&out.white_luminance, a.value, sizeof(f32));
    }
  }
  EXRImage image;
  InitEXRImage(&image);
  if (LoadEXRImageFromMemory(&image, &header, bytes.data(), bytes.size(), &message) !=
      TINYEXR_SUCCESS) {
    FreeEXRHeader(&header);
    take_error(error, "decode_exr: ", message);
    return false;
  }
  const u32 channels = gray ? 1u : (slot_of[3] >= 0 ? 4u : 3u);
  out.width = static_cast<u32>(image.width);
  out.height = static_cast<u32>(image.height);
  out.channels = channels;
  const u64 pixels = u64{out.width} * out.height;
  out.halves.resize(static_cast<u32>(pixels * channels));
  for (u32 k = 0; k < channels; ++k) {
    const int file_channel = gray ? luminance : slot_of[k];
    if (header.requested_pixel_types[file_channel] == TINYEXR_PIXELTYPE_FLOAT) {
      const f32* plane = reinterpret_cast<const f32*>(image.images[file_channel]);
      for (u64 i = 0; i < pixels; ++i) {
        out.halves[static_cast<u32>(i * channels + k)] = half_from_f32(plane[i]);
      }
      continue;
    }
    const u16* plane = reinterpret_cast<const u16*>(image.images[file_channel]);
    for (u64 i = 0; i < pixels; ++i) {
      out.halves[static_cast<u32>(i * channels + k)] = plane[i];
    }
  }
  FreeEXRImage(&image);
  FreeEXRHeader(&header);
  return true;
}

bool read_exr(std::string_view native_path, ExrImage& out, std::string* error) {
  std::string bytes;
  const io::Status status = io::read_file(native_path, bytes);
  if (status != io::Status::Ok) {
    set_error(error, "cannot read " + std::string(native_path) + ": " + io::status_name(status));
    return false;
  }
  return decode_exr(std::span<const u8>(reinterpret_cast<const u8*>(bytes.data()), bytes.size()),
                    out, error);
}

}  // namespace engine::image
