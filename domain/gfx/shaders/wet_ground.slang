// **Wet ground is a material** (roadmap S99; docs/subsystems/terrain.md, "Wet ground"; the
// grounding, docs/experiments/shore.md, "Wet ground"). No bindings and no entry points, like
// brdf.slang: terrain_material.slang's `terrain_wet` calls it where the ground's material is sampled,
// the resolve's water stage calls it for a ground with no layers and for the bottom a swash bares,
// and domain/gfx/tests/wet_reference.h mirrors it in double (held to 1 of 255 by wet_ground_tests).
//
// A ground at saturation `s` in [0, 1] (the share of its pores holding water) with a free film of
// weight `film` in [0, 1] (water standing on the grains, S94's film term) is, before lighting:
//
//   - **darker** by Lekner and Dorf (1988): water in the pores turns light that a dry grain would
//     have sent out back into the ground by total internal reflection at the water's surface, so a
//     wet albedo a' = (1 - r_e)(1 - r_i) a / (1 - r_i a), with water's diffuse external reflectance
//     r_e = 0.066 and internal r_i = 1 - (1 - r_e) / n^2 = 0.4744 at n = 1.333 (the factor below,
//     never above 1). The darkening's weight is s times the substrate's porosity over sand's
//     (`k_wet_porosity_full`, 0.3, the low end of a sand's 0.3-0.5), at most 1: a rock of porosity
//     0.05 takes a sixth of it and mostly films, as Jensen, Legakis and Dorsey (1999) separate the
//     two (water in the material against water on it);
//   - **smoother**: the damp substrate's roughness falls to `k_wet_damp_roughness` of its own at
//     s = 1 (water menisci round the grains; Lagarde 2013's porous rule, gentler, because the
//     owner's beach frames show the damp band matte), and a **film** takes it to the film's
//     roughness `k_wet_film_roughness`: the film's weight is the larger of the handed-in `film` and
//     the free water standing once the pores are full (s above `k_wet_film_from`, quadratic), so the
//     boundary between film and matte is as sharp as the saturation's and the damp band never
//     shines;
//   - the film's **Fresnel** is water's (F0 0.0201, `k_wet_film_f0`) for the reflection of the sky
//     the resolve adds along the mirrored view (water.md, "The swash"); the BSDF's direct lobe
//     keeps its one dielectric F0 0.04 (brdf.slang has one), said on the page.
//
// s = 0 returns its inputs untouched, so dry ground is the bytes it was.
#ifndef ENGINE_WET_GROUND_SLANG
#define ENGINE_WET_GROUND_SLANG

static const float k_wet_through = 0.490946;   // (1 - r_e)(1 - r_i)
static const float k_wet_internal = 0.474362;  // r_i
static const float k_wet_porosity_full = 0.3;
static const float k_wet_damp_roughness = 0.7;
static const float k_wet_film_roughness = 0.05;
static const float k_wet_film_from = 0.85;
static const float k_wet_film_f0 = 0.020059312;  // ((1.333 - 1) / (1.333 + 1))^2, water.slang's
// ResolveParams::shadow_flags' measurement switch `renderer.water.wet_film` false sets: the resolve
// then hands the material a negative film, which `wet_ground_film` reads as none.
static const uint k_shadow_wet_film_off = 65536u;  // gfx::k_shadow_wet_film_off, 1 << 16
// And `renderer.water.banks` off: the resolve walks no channel bank.
static const uint k_shadow_wet_banks_off = 131072u;  // gfx::k_shadow_wet_banks_off, 1 << 17

// Lekner and Dorf's factor on a dry albedo, per channel, at most 1.
float3 wet_ground_darkening(float3 albedo) {
  return min(k_wet_through / (1.0 - k_wet_internal * clamp(albedo, 0.0, 1.0)), float3(1.0));
}

// The film's weight: the handed-in film, or the free water standing once the pores are full. A
// negative `film` is no film at all (the resolve's `k_shadow_wet_film_off`, a measurement switch).
float wet_ground_film(float s, float film) {
  if (film < 0.0) return 0.0;
  const float standing = saturate((s - k_wet_film_from) / (1.0 - k_wet_film_from));
  return saturate(max(film, standing * standing));
}

// The roughness at saturation s under a film of weight `film`: monotone non-increasing in both,
// never below the smaller of its own and the film's, never above its own.
float wet_ground_roughness(float roughness, float s, float film) {
  const float sat = saturate(s);
  const float damp = roughness * lerp(1.0, k_wet_damp_roughness, sat);
  return lerp(damp, min(damp, k_wet_film_roughness), wet_ground_film(sat, film));
}

// The albedo at saturation s over a substrate of `porosity`.
float3 wet_ground_albedo(float3 albedo, float s, float porosity) {
  const float weight = saturate(s) * saturate(porosity / k_wet_porosity_full);
  return albedo * lerp(float3(1.0), wet_ground_darkening(albedo), weight);
}

// The capillary fringe above a still waterline (shore.md, "Wet ground"): the saturation `h` metres
// above the water over a fringe of `fringe_m` — 1 at and below the waterline, falling over the
// fringe by a smoothstep (a stand-in for van Genuchten's retention curve, whose shape it shares:
// flat at the water, steepest midway, flat at the top), 0 above it.
float wet_ground_fringe(float h, float fringe_m) {
  if (!(fringe_m > 0.0)) return h < 0.0 ? 1.0 : 0.0;
  if (h <= 0.0) return 1.0;
  return 1.0 - smoothstep(0.0, fringe_m, h);
}

#endif  // ENGINE_WET_GROUND_SLANG
