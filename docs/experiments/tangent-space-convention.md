# One tangent-space convention (R202, G2)

**Settled 2026-11-20: every frame the engine builds is glTF's.** A normal map's +X is right and +Y
is **up the image**; an authored frame's bitangent is `cross(normal, tangent.xyz) * tangent.w`; a
derived frame's bitangent is **-dP/dv** for an unmirrored island. Every map family the engine draws
already said +Y up, so nothing about a map or a material changed: only the frames did, and the bakes
that write a normal in one.

## The standard

From the glTF 2.0 specification (registry.khronos.org, `glTF-2.0.html`), read for this page:

- §3.8.3, "Images": "The origin of the texture coordinates (0, 0) corresponds to the upper left
  corner of a texture image." So v runs **down** the image.
- §3.7.2.1, meshes: `TANGENT` is "XYZW vertex tangents where the XYZ portion is normalized, and the
  W component is a sign value (-1 or +1) indicating handedness of the tangent basis"; the bitangent
  MUST be `cross(normal.xyz, tangent.xyz) * tangent.w`; without tangents a client SHOULD compute
  them "using default MikkTSpace algorithms".
- §5.19.5, `material.normalTexture`: the normal vectors use the convention +X right and +Y up, +Z
  towards the viewer. **Still unverified**: the fetch of the specification's page stopped before
  it, and the schema (`specification/2.0/schema/material.schema.json` in the KhronosGroup/glTF
  repository, read 2026-11-20) describes `normalTexture` only as "The tangent-space normal
  texture binding object.", which states no convention. What the engine relies on is held instead
  by what every family's maps were measured to be (below) and by the Khronos samples' own tangents.

Together: +Y up the image is towards **falling** v, so for an island laid upright (unmirrored) the
bitangent is -dP/dv. MikkTSpace's bitangent follows +v of the coordinates it is handed; glTF
exporters run it on v-up coordinates and write v flipped, which is why the first leg of G2 found
the files' bitangent opposite the old derived frame on nearly every corner of five Khronos samples
([assets](../subsystems/assets.md#what-g2-reads)).

## What was the other way round, and since when

| Where | Before R202 | Since |
|---|---|---|
| `material.slang`, `tangent_frame`'s derived half (resolve, ray path, reference path tracer) | bitangent +dP/dv: down the image | 4ded1982c, 2026-09-17 (moved to `material.slang` in 57ae3c035) |
| `terrain_material.slang`, the ground's fixed frames (the mapped layer and the three projections) | b = +z on the ground, +y on a side: +dP/dv | 0d4a16c65, 2026-10-23 |
| `brdf_reference.h`, `tangent_frame` (the CPU mirror) | +dP/dv | with the shader |
| `domain/atlas/src/rebake.cpp`, the repack's normal-map conversion | +dP/dv | the atlas step |
| `domain/flora/src/impostor.cpp`, `triangle_frame` and the card atlas's encoding (impostors, sward cards, wetland cards) | +dP/dv; the card's y written along the view's up, which the card's v follows | the impostor chain |
| `domain/rocks/src/surface_bake.cpp`, a rock's opt-in surface bake | green -dh/dv: +Y down | 44c937b61, 2026-10-28 |
| `domain/deadwood/src/surface.cpp`, deadwood's surface bake | green -dh/dv: +Y down | 3b4abda44, 2026-10-29 |

Every one of them is turned over by R202. Searched, and building no tangent frame for a map: the
sky, water (`water.slang` leans its normal by its own slopes), ground detail (procedural), the GI
probes' and the path tracer's sampling bases (`onb_from_normal`), `deform.slang` (normals only),
the rock solver's and the flora solvers' geometric frames (`solver.cpp`, `conifer.cpp`,
`companion.cpp`), the atlas's proxy charts (a UV parameterization), and the glTF importer (it keeps
the file's tangents and flips w under a mirroring node).

## Every family of normal map

| Family | Which way green points | Where it was read |
|---|---|---|
| Khronos samples, any glTF | +Y up | the standard |
| `engine-content matgen` | +Y up: `ny = (below - above) / 2 texel`, "+Y is up the image"; its roles are written `normal_convention: "opengl"` | `domain/matgen/src/matgen.cpp`, `apps/engine_content/matgen_commands.cpp` |
| The material pack | +Y up: `materials.cpp` refuses a role whose `normal_convention` is not `"opengl"`, and `content/materials/README.md` says OpenGL (+Y) | read; the pack was not on the machine this was measured on |
| The sward's ground bake | +Y up ("OpenGL +Y: the image's up is the tangent's y, opposite the rows", `sward.cpp`), blended with cards baked +Y down until R202 | read |
| Rock and deadwood surface bakes | +Y down until R202, +Y up since: a height of slope 0.5 rising along +v bakes a green of 185, along +u a red of 70 (`rocks: a known slope bakes a green and a red on glTF's side of 128`, `deadwood: a known slope ...`, each baking a height named in place of its field; with the green turned back, both read 70 and fail) | above |
| Terrain and ground layers | the sets above; drawn through a frame that read them +Y down until R202 | `terrain_material.slang` |

No `.tex` build, sampler or decoder flips a channel (`texture_build.cpp` stores the bytes).

## The known relief

`material resolve: a bump lights the flank its normal map's convention says`
(`domain/gfx/tests/tangent_convention_tests.cpp`): a Gaussian bump (sigma 0.12 of the tile, a
steepest slope of 0.8) whose map is matgen's formula (+Y up) or its green negated (+Y down), on a
mirrored and an unmirrored quad, under a sun low over +z in the y-z plane, so only the bitangent's
sign shows. The mean code of the bump's sunward flank against the other, RTX 5090, mesh path; every
pixel inside one texel is also held to the double-precision reference (`brdf_reference.h`):

| Frame, map | Before R202 | After |
|---|---|---|
| derived, +Y up | **the wrong flank lit**, 78 against 139, both islands; 72 codes from the new reference | the right flank, 139 against 79, both islands; 1 code from the reference |
| exported tangents (-dP/dv), +Y up | the right flank, 1 code | the right flank, 1 code |
| derived, +Y down | the right flank | the wrong flank, as glTF says of such a map |
| exported, +Y down | the wrong flank | the wrong flank |

**A mirroring instance** (an instance whose linear part has a negative determinant, such as scale
(-1, 1, 1)) drew every normal map's relief turned over on both axes until the second mend of R202,
2026-11-20: the shading normal was the cofactor normal, which under a mirror points into the
surface, and the resolve's turn towards the geometric normal then negated both slopes. It was
otherwise drawn rightly on every rasterizer and the ray path (measured on the translation suite's
cubes mirrored in x: the same colour and depth as unmirrored, on all seven paths;
[gfx](../subsystems/gfx.md), "A mirroring instance"). The same case draws its two quads a second
time placed by scale (-1, 1, 1): before, the wrong flank on both quads and through both frames (78
against 139, 72 codes from the reference); since `transform_normal` returns the outward normal and
the author's bitangent takes the instance's handedness, the right one, 139 against 79, 1 code from
the reference. So what is said above of the frames holds for a mirroring instance too.

On the ground: `renderer: one normal map lights the same flank on a mesh and on the ground`
(`systems/renderer/tests/terrain_materials_tests.cpp`) lays one constant map, leaning 45 degrees up
the image, on the ground's layer and on a mesh quad lying on it with the ground's own mapping, under
a low sun from -z, and holds both brighter than with the map's green turned over (142 against 100
mean code on each). That is the floor's projection, over (x, z), on its +y face. The side
projections are held by `renderer: one normal map lights the ground's four walls through their own
projections`: a test ground with a square mesa 2 m high whose four walls (76 degrees, facing +x,
-x, +z and -z) take the projections over (z, y) and (x, y), each with its bitangent -y, so the same
map as made leans every wall's normal down and under a sun overhead every wall is darker than with
the green turned over: 68.5 to 68.9 against 98.0 to 99.6 mean code with the layer's plain sampling,
68.7 to 70.9 against 91.1 to 94.1 with its stochastic sampling under a turn of up to 60 degrees
(RTX 5090, 460 to 969 pixels a wall); before R202 the sides' bitangent was +y and every wall would
have read the other way. **Not held by a case**: the mapped layer (`terrain_material.slang`, read at
`map_uv`, the point's (x, z)), whose frame by reading is the floor's: tangent +x, bitangent -z.

## The author's frame against the derived one

With one convention the two frames differ only by the facet: the author's tangent turns across a
triangle where the derived one is the chord's. Measured, RTX 5090:

- `renderer: an asset draws one relief whether its tangents reach the scene or not`: a curved patch
  (a quarter cylinder, analytic tangents) loaded whole — the stream uploaded — and streamed, which
  uploads none: worst 1 code of 255, mean 0.001, on the vertex and the mesh path (before R202: worst
  123, every one of 5,996 pixels more than 8 apart). The case holds 2.
- On a Khronos sample with authored tangents, SciFiHelmet (`engine-view --mesh` at 800 × 800, 120
  frames, no shadows, the derived-data cache off), loaded whole against `--stream`: PSNR 45.2 dB,
  SSIM 0.998, FLIP mean 0.0035, 95th percentile 0.012. The worst pixels (FLIP up to 0.98, a handful)
  are where the two runs' cuts or their silhouettes differ and where the file's own tangents disagree
  with the derived frame: the first leg measured its bitangent opposite the old derived frame on 77 %
  of its corners, so on the other 23 % the file states the handedness R202 replaced, and its own
  frame and the derived one now differ there.
- **Split by the sign of the triangle's UV area** (the same measurement, `gltf G2: the Khronos
  samples' tangents against the per-triangle frame` in `domain/assets/tests/import_extras_tests.cpp`,
  2026-11-20; the UVs as the file has them, v down): of SciFiHelmet's 70,074 corners, the 16,012
  whose stated bitangent is along +dP/dv (the 23 %) are **15,929 on triangles of positive UV area
  and 83 on negative**; the 54,062 whose bitangent is along -dP/dv are 31 and 54,031. The stated
  tangent is within 10 degrees of dP/du on every corner. So the file's bitangent follows the sign of
  the UV area almost exactly as `cross(N, T)` with one handedness would: its `w` does not turn over
  on the islands whose UVs run the other way (most likely mirrored islands an exporter did not
  flip), where MikkTSpace run on the same UVs would.
- What is established: the engine reads a file's own tangents as glTF 2.0 requires (the bitangent
  `cross(N, T) * w`, 3.7.2.1), as a reference viewer does; the derived frame follows the UVs on
  every island; and where a file's `w` contradicts its own UVs, as on those 23 % of SciFiHelmet's
  corners, the two routes (the whole load with the stream, and `--stream` or a tile, which derive)
  draw that island's relief along v the opposite way. What is not: which of the two the artist's
  bake meant, which is the file's matter and was not measured here; a normal map baked against
  MikkTSpace frames would be drawn right by the derived route there and turned over by the file's.

## What it moved

The pictures of every mesh whose normal map takes the derived frame, every ground layer with a
normal map, every rock or dead tree with a surface bake, and every impostor and sward card: their
relief along v turns over, to the side the height says. A scene with no normal map draws the same
bytes (`engine-view`'s heightfield, captured before and after: identical). The versions and pins
that moved are in the roadmap's R202 row and its commits.

## Not done

A deformed instance's own tangent (R203): a skinned or morphed instance draws the derived frame from
its deformed positions, which follows the skin and agrees with the author's to the facet, but no
deformed tangent is carried. The imported stream in the mesh room and the paged path (R204): a tile's
mesh and a streamed scene draw the derived frame, which agrees with the author's to the facet.
