// The mip chain (docs/subsystems/texture.md, "The mip chain"): the sRGB tables, the filter
// kernel, and the separable resampler. Everything here is IEEE arithmetic in a fixed order — no C
// library call, no reduction whose order depends on a thread — so the chain is the same bytes on
// every compiler and for every thread count (ADR-0035).
#include <core/jobs/job_system.h>
#include <domain/texture/texture_build.h>

#include <cmath>
#include <cstring>
#include <utility>

namespace engine::texture {

namespace {

// sRGB code n to linear light, n = 0..255: the IEC 61966-2-1 piecewise curve evaluated in double
// precision and rounded to float, once, when this file was written (tools-free: a PowerShell loop
// over [Math]::Pow). Committed rather than computed because `std::pow` is the C library's and its
// last bit is not the same on MSVC and glibc; the test checks every entry against `std::pow` to
// within an ulp, which is the check that the table was typed right and not a dependency on the
// library.
constexpr f32 k_srgb_decode[256] = {
    0.0f,          0.000303527f,  0.000607054f, 0.000910581f,  0.001214108f,  0.001517635f,
    0.001821162f,  0.0021246888f, 0.002428216f, 0.0027317428f, 0.00303527f,   0.0033465358f,
    0.0036765074f, 0.004024717f,  0.004391442f, 0.0047769533f, 0.0051815165f, 0.0056053917f,
    0.006048833f,  0.0065120906f, 0.00699541f,  0.007499032f,  0.008023193f,  0.008568126f,
    0.009134059f,  0.009721218f,  0.010329823f, 0.010960094f,  0.011612245f,  0.012286488f,
    0.0129830325f, 0.013702083f,  0.014443844f, 0.015208514f,  0.015996294f,  0.016807375f,
    0.017641954f,  0.01850022f,   0.019382361f, 0.020288562f,  0.02121901f,   0.022173885f,
    0.023153367f,  0.024157632f,  0.02518686f,  0.026241222f,  0.027320892f,  0.02842604f,
    0.029556835f,  0.030713445f,  0.031896032f, 0.033104766f,  0.034339808f,  0.035601314f,
    0.03688945f,   0.038204372f,  0.039546236f, 0.0409152f,    0.04231141f,   0.04373503f,
    0.045186203f,  0.046665087f,  0.048171826f, 0.049706567f,  0.051269457f,  0.052860647f,
    0.054480277f,  0.05612849f,   0.05780543f,  0.059511237f,  0.061246052f,  0.063010015f,
    0.064803265f,  0.06662594f,   0.06847817f,  0.070360094f,  0.07227185f,   0.07421357f,
    0.07618538f,   0.07818742f,   0.08021982f,  0.08228271f,   0.08437621f,   0.08650046f,
    0.08865558f,   0.09084171f,   0.093058966f, 0.09530747f,   0.09758735f,   0.099898726f,
    0.10224173f,   0.104616486f,  0.107023105f, 0.10946171f,   0.11193243f,   0.114435375f,
    0.116970666f,  0.11953843f,   0.122138776f, 0.12477182f,   0.12743768f,   0.13013647f,
    0.13286832f,   0.13563333f,   0.13843161f,  0.14126329f,   0.14412847f,   0.14702727f,
    0.14995979f,   0.15292615f,   0.15592647f,  0.15896083f,   0.16202937f,   0.1651322f,
    0.1682694f,    0.17144111f,   0.1746474f,   0.17788842f,   0.18116425f,   0.18447499f,
    0.18782078f,   0.19120169f,   0.19461784f,  0.19806932f,   0.20155625f,   0.20507874f,
    0.20863687f,   0.21223076f,   0.2158605f,   0.2195262f,    0.22322796f,   0.22696587f,
    0.23074006f,   0.23455058f,   0.23839757f,  0.24228112f,   0.24620132f,   0.25015828f,
    0.2541521f,    0.25818285f,   0.26225066f,  0.2663556f,    0.2704978f,    0.2746773f,
    0.27889428f,   0.28314874f,   0.28744084f,  0.29177064f,   0.29613826f,   0.30054379f,
    0.3049873f,    0.30946892f,   0.31398872f,  0.31854677f,   0.3231432f,    0.3277781f,
    0.33245152f,   0.33716363f,   0.34191442f,  0.34670407f,   0.3515326f,    0.35640013f,
    0.3613068f,    0.3662526f,    0.3712377f,   0.37626213f,   0.38132602f,   0.38642943f,
    0.39157248f,   0.39675522f,   0.40197778f,  0.4072402f,    0.4125426f,    0.41788507f,
    0.42326766f,   0.4286905f,    0.43415365f,  0.43965718f,   0.4452012f,    0.4507858f,
    0.45641103f,   0.462077f,     0.4677838f,   0.47353148f,   0.47932017f,   0.48514995f,
    0.49102086f,   0.49693298f,   0.5028865f,   0.50888133f,   0.5149177f,    0.52099556f,
    0.5271151f,    0.5332764f,    0.5394795f,   0.54572445f,   0.55201143f,   0.5583404f,
    0.5647115f,    0.57112485f,   0.57758045f,  0.58407843f,   0.59061885f,   0.59720176f,
    0.60382736f,   0.61049557f,   0.6172066f,   0.6239604f,    0.63075715f,   0.63759685f,
    0.6444797f,    0.65140563f,   0.65837485f,  0.6653873f,    0.67244315f,   0.6795425f,
    0.6866853f,    0.69387174f,   0.7011019f,   0.70837575f,   0.7156935f,    0.7230551f,
    0.73046076f,   0.7379104f,    0.7454042f,   0.7529422f,    0.7605245f,    0.76815116f,
    0.7758222f,    0.7835378f,    0.7912979f,   0.7991027f,    0.80695224f,   0.8148466f,
    0.82278574f,   0.8307699f,    0.838799f,    0.8468732f,    0.8549926f,    0.8631572f,
    0.8713671f,    0.8796224f,    0.8879231f,   0.8962694f,    0.9046612f,    0.91309863f,
    0.92158186f,   0.9301109f,    0.9386857f,   0.9473065f,    0.9559733f,    0.9646863f,
    0.9734453f,    0.9822506f,    0.9911021f,   1.0f,
};

// The linear value at which the sRGB encoding crosses from code n to code n + 1: the same curve at
// (n + 0.5) / 255, so that encoding by "how many thresholds lie at or below the value" is exactly
// round-half-up of 255 times the encoded value, up to the table's float rounding. Taken the same
// way as the table above.
constexpr f32 k_srgb_threshold[255] = {
    0.0001517635f, 0.0004552905f, 0.0007588175f, 0.0010623444f, 0.0013658714f, 0.0016693984f,
    0.0019729254f, 0.0022764525f, 0.0025799794f, 0.0028835062f, 0.0031883009f, 0.0035092593f,
    0.003848315f,  0.004205748f,  0.004581833f,  0.0049768374f, 0.005391024f,  0.0058246506f,
    0.0062779696f, 0.0067512277f, 0.0072446684f, 0.0077585303f, 0.0082930485f, 0.008848453f,
    0.0094249705f, 0.010022826f,  0.010642237f,  0.011283421f,  0.0119465925f, 0.01263196f,
    0.013339732f,  0.014070112f,  0.014823303f,  0.015599503f,  0.01639891f,   0.017221715f,
    0.018068114f,  0.018938294f,  0.019832443f,  0.020750744f,  0.021693382f,  0.022660539f,
    0.02365239f,   0.024669115f,  0.025710888f,  0.026777882f,  0.02787027f,   0.02898822f,
    0.030131903f,  0.03130148f,   0.032497123f,  0.03371899f,   0.034967244f,  0.036242045f,
    0.037543554f,  0.038871925f,  0.04022732f,   0.041609887f,  0.043019786f,  0.044457164f,
    0.04592217f,   0.047414962f,  0.048935685f,  0.050484486f,  0.052061506f,  0.053666897f,
    0.055300802f,  0.05696336f,   0.058654718f,  0.060375012f,  0.062124383f,  0.063902974f,
    0.06571092f,   0.06754835f,   0.06941541f,   0.071312234f,  0.073238954f,  0.07519571f,
    0.07718261f,   0.07919982f,   0.08124744f,   0.083325624f,  0.08543449f,   0.087574154f,
    0.08974477f,   0.09194644f,   0.0941793f,    0.096443474f,  0.098739095f,  0.10106627f,
    0.10342513f,   0.105815805f,  0.1082384f,    0.110693045f,  0.11317986f,   0.11569897f,
    0.11825048f,   0.12083452f,   0.1234512f,    0.12610064f,   0.12878296f,   0.13149826f,
    0.13424668f,   0.1370283f,    0.13984327f,   0.14269169f,   0.14557366f,   0.14848931f,
    0.15143873f,   0.15442206f,   0.15743938f,   0.16049083f,   0.1635765f,    0.16669649f,
    0.16985093f,   0.17303991f,   0.17626357f,   0.17952198f,   0.18281525f,   0.1861435f,
    0.18950683f,   0.19290535f,   0.19633915f,   0.19980834f,   0.20331304f,   0.20685335f,
    0.21042934f,   0.21404114f,   0.21768884f,   0.22137256f,   0.2250924f,    0.22884843f,
    0.23264076f,   0.2364695f,    0.24033478f,   0.24423663f,   0.2481752f,    0.25215057f,
    0.25616285f,   0.26021212f,   0.26429847f,   0.26842204f,   0.2725829f,    0.2767811f,
    0.2810168f,    0.2852901f,    0.28960103f,   0.29394972f,   0.2983363f,    0.3027608f,
    0.30722335f,   0.31172404f,   0.31626296f,   0.32084018f,   0.32545584f,   0.33010998f,
    0.33480275f,   0.33953416f,   0.34430438f,   0.34911346f,   0.3539615f,    0.35884857f,
    0.36377478f,   0.36874023f,   0.37374496f,   0.37878913f,   0.38387278f,   0.388996f,
    0.3941589f,    0.39936152f,   0.40460402f,   0.40988642f,   0.41520882f,   0.42057136f,
    0.42597404f,   0.43141702f,   0.43690035f,   0.44242412f,   0.44798842f,   0.4535933f,
    0.45923892f,   0.4649253f,    0.47065252f,   0.4764207f,    0.48222992f,   0.48808023f,
    0.49397177f,   0.49990454f,   0.5058787f,    0.5118943f,    0.5179514f,    0.5240501f,
    0.5301905f,    0.5363727f,    0.54259676f,   0.5488627f,    0.55517066f,   0.5615207f,
    0.5679129f,    0.5743473f,    0.58082414f,   0.58734334f,   0.593905f,     0.6005092f,
    0.6071561f,    0.6138457f,    0.6205781f,    0.62735337f,   0.6341716f,    0.6410329f,
    0.64793724f,   0.6548848f,    0.66187567f,   0.6689098f,    0.67598736f,   0.68310845f,
    0.6902731f,    0.69748133f,   0.7047334f,    0.71202916f,   0.7193688f,    0.72675246f,
    0.73418003f,   0.7416518f,    0.7491677f,    0.7567278f,    0.7643323f,    0.7719811f,
    0.7796744f,    0.7874123f,    0.79519475f,   0.8030219f,    0.81089383f,   0.8188105f,
    0.8267722f,    0.8347788f,    0.8428305f,    0.8509273f,    0.8590692f,    0.8672565f,
    0.87548906f,   0.88376707f,   0.89209056f,   0.9004596f,    0.9088742f,    0.91733456f,
    0.9258406f,    0.9343926f,    0.94299036f,   0.95163417f,   0.96032405f,   0.96906f,
    0.97784215f,   0.98667055f,   0.99554527f,
};

constexpr f32 clamp01(f32 v) noexcept { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

// Round-half-up of a value already in [0, 255.5).
inline u8 round_u8(f32 scaled) noexcept { return static_cast<u8>(static_cast<u32>(scaled + 0.5f)); }

inline u8 unorm8(f32 v) noexcept { return round_u8(clamp01(v) * 255.0f); }

// Level 0's texels as floats, through one lookup per channel byte: sRGB colour to linear light,
// data to 0..1, a normal map's xyz to -1..1. Built per build rather than per tap.
struct ChannelTables {
  f32 table[4][256];
};

void make_tables(MipMode mode, ChannelTables& out) noexcept {
  for (u32 v = 0; v < 256; ++v) {
    const f32 unit = static_cast<f32>(v) / 255.0f;
    for (u32 c = 0; c < 4; ++c) {
      f32 value = unit;
      if (c < 3 && mode == MipMode::srgb) value = k_srgb_decode[v];
      if (c < 3 && mode == MipMode::normal) value = unit * 2.0f - 1.0f;
      out.table[c][v] = value;
    }
  }
}

// One source texel's taps for one destination texel: the clamped source index and its weight.
// Taps of a destination are contiguous; `first[d]` is where they start and `first[d + 1]` where
// they end.
struct Taps {
  Vector<u32> first;
  Vector<u32> index;
  Vector<f32> weight;
};

// The taps of a resampling from `src` texels to `dst` along one axis. At a ratio of one the axis is
// copied; otherwise the kernel is stretched to the ratio, evaluated at every source texel center
// inside its support, clamped to the edge (a tap past the edge lands on the edge texel, as a
// clamp-to-edge sampler would read it), and normalized so a constant image stays constant.
void make_taps(u32 src, u32 dst, Taps& out) {
  out.first.clear();
  out.index.clear();
  out.weight.clear();
  out.first.reserve(dst + 1);
  if (src == dst) {
    for (u32 d = 0; d < dst; ++d) {
      out.first.push_back(d);
      out.index.push_back(d);
      out.weight.push_back(1.0f);
    }
    out.first.push_back(dst);
    return;
  }
  const f32 scale = static_cast<f32>(src) / static_cast<f32>(dst);
  const f32 radius = 2.0f * scale;
  const i32 last = static_cast<i32>(src) - 1;
  for (u32 d = 0; d < dst; ++d) {
    out.first.push_back(out.index.size());
    const f32 center = (static_cast<f32>(d) + 0.5f) * scale;
    const i32 lo = static_cast<i32>(std::floor(center - radius - 0.5f));
    const i32 hi = static_cast<i32>(std::floor(center + radius - 0.5f)) + 1;
    const u32 begin = out.index.size();
    f32 sum = 0.0f;
    for (i32 i = lo; i <= hi; ++i) {
      const f32 x = (static_cast<f32>(i) + 0.5f - center) / scale;
      const f32 w = mip_kernel(x);
      if (w == 0.0f) continue;
      const i32 clamped = i < 0 ? 0 : (i > last ? last : i);
      out.index.push_back(static_cast<u32>(clamped));
      out.weight.push_back(w);
      sum += w;
    }
    for (u32 t = begin; t < out.index.size(); ++t)
      out.weight[t] = out.weight[t] / sum;
  }
  out.first.push_back(out.index.size());
}

// Runs `body(begin, end)` over rows [0, count), on the pool when there is one. Each row is written
// by exactly one call, so the result does not depend on how the rows were split.
template <class F>
void for_rows(jobs::JobSystem* pool, u32 count, F&& body) {
  if (pool == nullptr || count < 16) {
    body(0u, count);
    return;
  }
  pool->parallel_for(jobs::Pool::Performance, count, 8,
                     [&body](u32 begin, u32 end) { body(begin, end); });
}

// One separable resampling of a 4-channel float image `fetch(x, y)` of `sw` x `sh` into `dst`
// (`dw` x `dh` x 4 floats): horizontally into `scratch` (`dw` x `sh` x 4), then vertically.
template <class Fetch>
void resample(const Fetch& fetch, u32 sw, u32 sh, u32 dw, u32 dh, Vector<f32>& scratch,
              Vector<f32>& dst, jobs::JobSystem* pool) {
  Taps horizontal;
  Taps vertical;
  make_taps(sw, dw, horizontal);
  make_taps(sh, dh, vertical);
  scratch.resize(dw * sh * 4);
  dst.resize(dw * dh * 4);
  f32* mid = scratch.data();
  for_rows(pool, sh, [&](u32 begin, u32 end) {
    for (u32 y = begin; y < end; ++y) {
      f32* row = mid + static_cast<usize>(y) * dw * 4;
      for (u32 x = 0; x < dw; ++x) {
        f32 acc[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        for (u32 t = horizontal.first[x]; t < horizontal.first[x + 1]; ++t) {
          const f32 w = horizontal.weight[t];
          f32 texel[4];
          fetch(horizontal.index[t], y, texel);
          for (u32 c = 0; c < 4; ++c)
            acc[c] += w * texel[c];
        }
        for (u32 c = 0; c < 4; ++c)
          row[x * 4 + c] = acc[c];
      }
    }
  });
  f32* out = dst.data();
  for_rows(pool, dh, [&](u32 begin, u32 end) {
    for (u32 y = begin; y < end; ++y) {
      f32* row = out + static_cast<usize>(y) * dw * 4;
      for (u32 x = 0; x < dw * 4; ++x)
        row[x] = 0.0f;
      for (u32 t = vertical.first[y]; t < vertical.first[y + 1]; ++t) {
        const f32 w = vertical.weight[t];
        const f32* src = mid + static_cast<usize>(vertical.index[t]) * dw * 4;
        for (u32 x = 0; x < dw * 4; ++x)
          row[x] += w * src[x];
      }
    }
  });
}

// A normal map's filtered vectors go back on the unit sphere before they are stored or filtered
// again. A vector that filtered to nothing (opposite normals averaged away) becomes +z, the flat
// normal, rather than a division by zero.
void renormalize(f32* texel) noexcept {
  const f32 length_sq = texel[0] * texel[0] + texel[1] * texel[1] + texel[2] * texel[2];
  if (length_sq <= 1e-20f) {
    texel[0] = 0.0f;
    texel[1] = 0.0f;
    texel[2] = 1.0f;
    return;
  }
  const f32 inv = 1.0f / std::sqrt(length_sq);
  texel[0] = texel[0] * inv;
  texel[1] = texel[1] * inv;
  texel[2] = texel[2] * inv;
}

inline u8 encode_normal_component(f32 v) noexcept { return unorm8(v * 0.5f + 0.5f); }

// A float level to 8-bit RGBA in the mode's encoding.
void quantize(const Vector<f32>& level, u32 width, u32 height, MipMode mode, Vector<u8>& out,
              jobs::JobSystem* pool) {
  out.resize(width * height * 4);
  const f32* src = level.data();
  u8* dst = out.data();
  for_rows(pool, height, [&](u32 begin, u32 end) {
    for (u32 y = begin; y < end; ++y) {
      for (u32 x = 0; x < width; ++x) {
        const usize at = (static_cast<usize>(y) * width + x) * 4;
        const f32* t = src + at;
        u8* o = dst + at;
        switch (mode) {
          case MipMode::srgb:
            o[0] = linear_to_srgb8(t[0]);
            o[1] = linear_to_srgb8(t[1]);
            o[2] = linear_to_srgb8(t[2]);
            break;
          case MipMode::linear:
            o[0] = unorm8(t[0]);
            o[1] = unorm8(t[1]);
            o[2] = unorm8(t[2]);
            break;
          case MipMode::normal:
            o[0] = encode_normal_component(t[0]);
            o[1] = encode_normal_component(t[1]);
            o[2] = encode_normal_component(t[2]);
            break;
        }
        o[3] = unorm8(t[3]);
      }
    }
  });
}

}  // namespace

f32 srgb8_to_linear(u8 code) noexcept { return k_srgb_decode[code]; }

u8 linear_to_srgb8(f32 linear) noexcept {
  // NaN compares false everywhere and so lands on code 0, as a clamp would put it.
  if (!(linear > 0.0f)) return 0;
  // The number of thresholds at or below the value: a binary search over 255 sorted entries.
  u32 lo = 0;
  u32 hi = 255;
  while (lo < hi) {
    const u32 mid = (lo + hi) / 2;
    if (k_srgb_threshold[mid] <= linear)
      lo = mid + 1;
    else
      hi = mid;
  }
  return static_cast<u8>(lo);
}

std::span<const f32> srgb_decode_table() noexcept { return std::span<const f32>(k_srgb_decode); }

std::span<const f32> srgb_threshold_table() noexcept {
  return std::span<const f32>(k_srgb_threshold);
}

f32 mip_kernel(f32 x) noexcept {
  // Mitchell and Netravali, "Reconstruction Filters in Computer Graphics" (1988), with their
  // recommended B = C = 1/3: the piecewise cubic
  //   |x| < 1:      (7|x|^3 - 12|x|^2 + 16/3) / 6
  //   1 <= |x| < 2: (-7/3|x|^3 + 12|x|^2 - 20|x| + 32/3) / 6
  // It is a compromise between blur and ringing that is close to the best a separable cubic does
  // for downsampling, and — the reason it is here rather than a windowed sinc — it is a polynomial,
  // which every compiler evaluates to the same float once contraction is off. Its small negative
  // lobes can take a filtered value a hair outside the source's range; the quantizer clamps.
  const f32 a = x < 0.0f ? -x : x;
  if (a < 1.0f) return (a * a * (7.0f * a - 12.0f) + 16.0f / 3.0f) * (1.0f / 6.0f);
  if (a < 2.0f) {
    return (a * (a * ((-7.0f / 3.0f) * a + 12.0f) - 20.0f) + 32.0f / 3.0f) * (1.0f / 6.0f);
  }
  return 0.0f;
}

void build_mip_chain(std::span<const u8> rgba, u32 width, u32 height, MipMode mode, bool mips,
                     Vector<MipLevel>& out, jobs::JobSystem* pool) {
  out.clear();
  if (width == 0 || height == 0 || rgba.size() < static_cast<usize>(width) * height * 4) return;
  const u32 count = mips ? texture_full_level_count(width, height) : 1u;
  out.reserve(count);

  ChannelTables tables;
  make_tables(mode, tables);

  // Level 0: the source's bytes, except that a normal map is put back on the unit sphere.
  MipLevel level0;
  level0.width = width;
  level0.height = height;
  level0.rgba.resize(width * height * 4);
  std::memcpy(level0.rgba.data(), rgba.data(), static_cast<usize>(width) * height * 4);
  if (mode == MipMode::normal) {
    u8* texels = level0.rgba.data();
    for_rows(pool, height, [&](u32 begin, u32 end) {
      for (u32 y = begin; y < end; ++y) {
        for (u32 x = 0; x < width; ++x) {
          u8* t = texels + (static_cast<usize>(y) * width + x) * 4;
          f32 v[3] = {tables.table[0][t[0]], tables.table[1][t[1]], tables.table[2][t[2]]};
          renormalize(v);
          t[0] = encode_normal_component(v[0]);
          t[1] = encode_normal_component(v[1]);
          t[2] = encode_normal_component(v[2]);
        }
      }
    });
  }
  out.push_back(std::move(level0));
  if (count == 1) return;

  // Every further level from the previous level's floats. Level 1 reads level 0's bytes through the
  // tables, so no float copy of the largest level is ever made.
  Vector<f32> previous;
  Vector<f32> next;
  Vector<f32> scratch;
  u32 pw = width;
  u32 ph = height;
  for (u32 level = 1; level < count; ++level) {
    const u32 w = texture_level_extent(width, level);
    const u32 h = texture_level_extent(height, level);
    if (level == 1) {
      const u8* bytes = out[0].rgba.data();
      auto fetch = [&](u32 x, u32 y, f32* texel) {
        const u8* t = bytes + (static_cast<usize>(y) * pw + x) * 4;
        texel[0] = tables.table[0][t[0]];
        texel[1] = tables.table[1][t[1]];
        texel[2] = tables.table[2][t[2]];
        texel[3] = tables.table[3][t[3]];
      };
      resample(fetch, pw, ph, w, h, scratch, next, pool);
    } else {
      const f32* floats = previous.data();
      auto fetch = [&](u32 x, u32 y, f32* texel) {
        const f32* t = floats + (static_cast<usize>(y) * pw + x) * 4;
        texel[0] = t[0];
        texel[1] = t[1];
        texel[2] = t[2];
        texel[3] = t[3];
      };
      resample(fetch, pw, ph, w, h, scratch, next, pool);
    }
    if (mode == MipMode::normal) {
      f32* texels = next.data();
      for (u32 i = 0; i < w * h; ++i)
        renormalize(texels + static_cast<usize>(i) * 4);
    }
    MipLevel built;
    built.width = w;
    built.height = h;
    quantize(next, w, h, mode, built.rgba, pool);
    out.push_back(std::move(built));
    std::swap(previous, next);
    pw = w;
    ph = h;
  }
}

}  // namespace engine::texture
