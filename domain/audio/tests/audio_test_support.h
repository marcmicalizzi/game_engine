#pragma once

// Generated audio for the tests: waveforms, WAV and FLAC files written in memory. Nothing is read
// from disk and nothing is committed as a binary fixture (AGENTS.md): every clip a test decodes is
// written by the test that decodes it.
//
// Two kinds of sine. `reference_sine` uses std::sin in double and is for tests that compare the
// mix against an analytic expectation with a tolerance. `exact_sine` is built from the engine's
// own `sin_quarter` polynomial — +, * and a fixed order, no libm — and is what the determinism
// test mixes, because a clip generated with std::sin would make its pinned hash a property of the
// C library that generated it rather than of the mixer.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <domain/audio/format.h>
#include <domain/audio/spatial.h>

#include <cmath>
#include <cstring>
#include <span>
#include <string>

namespace engine::audio::test {

inline constexpr f64 k_pi_d = 3.14159265358979323846;

// sin(2 pi * cycles * i / frames) through `sin_quarter`, for any phase: fold the phase into the
// first quarter and put the sign back.
inline f32 exact_sine_at(u64 i, u64 period_frames) {
  const u64 phase = i % period_frames;  // [0, period)
  const u64 quarter = period_frames / 4u;
  // Exact for periods divisible by four; the tests use 48 and 96.
  const u64 q = phase / quarter;
  const u64 r = phase % quarter;
  const f32 t = static_cast<f32>(r) / static_cast<f32>(quarter);
  switch (q) {
    case 0: return sin_quarter(t);
    case 1: return sin_quarter(1.0f - t);
    case 2: return -sin_quarter(t);
    default: return -sin_quarter(1.0f - t);
  }
}

inline Vector<f32> exact_sine(u32 frames, u32 period_frames, f32 amplitude, u32 channels = 1) {
  Vector<f32> out;
  out.resize_exact(frames * channels);
  for (u32 i = 0; i < frames; ++i) {
    const f32 s = amplitude * exact_sine_at(i, period_frames);
    for (u32 c = 0; c < channels; ++c)
      out[i * channels + c] = s;
  }
  return out;
}

inline Vector<f32> reference_sine(u32 frames, f64 hz, f64 amplitude, u32 rate = k_sample_rate) {
  Vector<f32> out;
  out.resize_exact(frames);
  for (u32 i = 0; i < frames; ++i)
    out[i] = static_cast<f32>(
        amplitude * std::sin(2.0 * k_pi_d * hz * static_cast<f64>(i) / static_cast<f64>(rate)));
  return out;
}

// Deterministic noise in [-1, 1): a 64-bit LCG's top 24 bits, exactly representable.
inline Vector<f32> lcg_noise(u32 frames, u64 seed, f32 amplitude) {
  Vector<f32> out;
  out.resize_exact(frames);
  u64 state = seed;
  for (u32 i = 0; i < frames; ++i) {
    state = state * 6364136223846793005ull + 1442695040888963407ull;
    const i32 top = static_cast<i32>(state >> 40) - (1 << 23);  // [-2^23, 2^23)
    out[i] = amplitude * (static_cast<f32>(top) / 8388608.0f);
  }
  return out;
}

// ---- files --------------------------------------------------------------------------------------

inline void put_u16(Vector<u8>& out, u32 v) {
  out.push_back(static_cast<u8>(v & 0xFFu));
  out.push_back(static_cast<u8>((v >> 8) & 0xFFu));
}
inline void put_u32(Vector<u8>& out, u32 v) {
  put_u16(out, v & 0xFFFFu);
  put_u16(out, v >> 16);
}
inline void put_tag(Vector<u8>& out, const char* tag) {
  for (int i = 0; i < 4; ++i)
    out.push_back(static_cast<u8>(tag[i]));
}

// A canonical RIFF/WAVE file: 16-bit PCM.
inline Vector<u8> wav_s16(std::span<const i16> interleaved, u32 channels, u32 rate) {
  Vector<u8> out;
  const u32 data_bytes = static_cast<u32>(interleaved.size() * 2u);
  put_tag(out, "RIFF");
  put_u32(out, 36u + data_bytes);
  put_tag(out, "WAVE");
  put_tag(out, "fmt ");
  put_u32(out, 16);
  put_u16(out, 1);  // PCM
  put_u16(out, channels);
  put_u32(out, rate);
  put_u32(out, rate * channels * 2u);
  put_u16(out, channels * 2u);
  put_u16(out, 16);
  put_tag(out, "data");
  put_u32(out, data_bytes);
  for (const i16 s : interleaved)
    put_u16(out, static_cast<u16>(s));
  return out;
}

// A RIFF/WAVE file of 32-bit IEEE float samples (format tag 3).
inline Vector<u8> wav_f32(std::span<const f32> interleaved, u32 channels, u32 rate) {
  Vector<u8> out;
  const u32 data_bytes = static_cast<u32>(interleaved.size() * 4u);
  put_tag(out, "RIFF");
  put_u32(out, 36u + data_bytes);
  put_tag(out, "WAVE");
  put_tag(out, "fmt ");
  put_u32(out, 16);
  put_u16(out, 3);  // IEEE float
  put_u16(out, channels);
  put_u32(out, rate);
  put_u32(out, rate * channels * 4u);
  put_u16(out, channels * 4u);
  put_u16(out, 32);
  put_tag(out, "data");
  put_u32(out, data_bytes);
  for (const f32 s : interleaved) {
    u32 bits;
    std::memcpy(&bits, &s, 4);
    put_u32(out, bits);
  }
  return out;
}

// ---- a FLAC writer that does not compress -------------------------------------------------------
//
// FLAC's VERBATIM subframe stores samples as they are, so a valid stream needs no predictor, no
// Rice coder and no search — only the framing: the "fLaC" marker, a STREAMINFO block, and frames
// whose headers carry a CRC-8 and whose ends carry a CRC-16. That is enough to put a generated
// clip through dr_flac without committing a binary file.

class BitWriter {
 public:
  void put(u64 value, u32 bits) {
    for (u32 i = bits; i-- > 0;) {
      current_ = static_cast<u8>((current_ << 1) | ((value >> i) & 1u));
      if (++count_ == 8) flush_byte();
    }
  }
  void align() {
    while (count_ != 0)
      put(0, 1);
  }
  Vector<u8>& bytes() { return bytes_; }

 private:
  void flush_byte() {
    bytes_.push_back(current_);
    current_ = 0;
    count_ = 0;
  }
  Vector<u8> bytes_;
  u8 current_ = 0;
  u32 count_ = 0;
};

inline u8 crc8(const u8* data, usize size) {
  u8 crc = 0;
  for (usize i = 0; i < size; ++i) {
    crc = static_cast<u8>(crc ^ data[i]);
    for (int b = 0; b < 8; ++b)
      crc = static_cast<u8>((crc & 0x80u) != 0 ? (crc << 1) ^ 0x07u : crc << 1);
  }
  return crc;
}

inline u16 crc16(const u8* data, usize size) {
  u16 crc = 0;
  for (usize i = 0; i < size; ++i) {
    crc = static_cast<u16>(crc ^ (static_cast<u16>(data[i]) << 8));
    for (int b = 0; b < 8; ++b)
      crc = static_cast<u16>((crc & 0x8000u) != 0 ? (crc << 1) ^ 0x8005u : crc << 1);
  }
  return crc;
}

// 16-bit samples at 48 kHz, 1 or 2 channels (independent, no stereo decorrelation), frames of
// 4096 samples (the last one shorter), every subframe VERBATIM.
inline Vector<u8> flac_s16(std::span<const i16> interleaved, u32 channels) {
  constexpr u32 k_block = 4096;
  const u32 frames = static_cast<u32>(interleaved.size() / channels);
  BitWriter w;
  // "fLaC", then STREAMINFO as the last metadata block.
  w.put(0x664C6143u, 32);
  w.put(1, 1);  // last metadata block
  w.put(0, 7);  // STREAMINFO
  w.put(34, 24);
  w.put(k_block, 16);  // min block size
  w.put(k_block, 16);  // max block size
  w.put(0, 24);        // min frame size: unknown
  w.put(0, 24);        // max frame size: unknown
  w.put(k_sample_rate, 20);
  w.put(channels - 1u, 3);
  w.put(16 - 1, 5);  // bits per sample
  w.put(frames, 36);
  w.put(0, 64);  // MD5: zero means "not computed"
  w.put(0, 64);

  u32 frame_number = 0;
  for (u32 first = 0; first < frames; first += k_block, ++frame_number) {
    const u32 count = frames - first < k_block ? frames - first : k_block;
    const u32 header_start = w.bytes().size();
    w.put(0x3FFE, 14);        // sync
    w.put(0, 1);              // reserved
    w.put(0, 1);              // fixed block size
    w.put(7, 4);              // block size: 16-bit (count - 1) after the header
    w.put(10, 4);             // 48 kHz
    w.put(channels - 1u, 4);  // independent channels
    w.put(4, 3);              // 16 bits per sample
    w.put(0, 1);              // reserved
    // The frame number, UTF-8 coded; the tests stay under 128 frames, one byte.
    w.put(frame_number, 8);
    w.put(count - 1u, 16);
    w.put(crc8(w.bytes().data() + header_start, w.bytes().size() - header_start), 8);
    for (u32 c = 0; c < channels; ++c) {
      w.put(0, 1);  // padding
      w.put(1, 6);  // VERBATIM
      w.put(0, 1);  // no wasted bits
      for (u32 i = 0; i < count; ++i)
        w.put(static_cast<u16>(interleaved[(first + i) * channels + c]), 16);
    }
    w.align();
    w.put(crc16(w.bytes().data() + header_start, w.bytes().size() - header_start), 16);
  }
  return std::move(w.bytes());
}

// "0x" and sixteen hex digits: what a pinned hash is written as, since doctest's MESSAGE does not
// honour std::hex.
inline std::string hex64(u64 value) {
  static const char* digits = "0123456789abcdef";
  std::string out = "0x0000000000000000";
  for (u32 i = 0; i < 16; ++i)
    out[17u - i] = digits[(value >> (4u * i)) & 0xFu];
  return out;
}

inline Vector<i16> to_s16(std::span<const f32> samples) {
  Vector<i16> out;
  out.resize_exact(static_cast<u32>(samples.size()));
  for (usize i = 0; i < samples.size(); ++i) {
    f32 s = samples[i] * 32767.0f;
    s = s < -32768.0f ? -32768.0f : (s > 32767.0f ? 32767.0f : s);
    out[static_cast<u32>(i)] = static_cast<i16>(std::lround(s));
  }
  return out;
}

}  // namespace engine::audio::test
