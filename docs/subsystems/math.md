# math (core)

**Purpose.** Header-only linear algebra: `Vec2/3/4`, `Vec2i/3i`, `Quat`, `Mat3/4`, `Transform3` (position, rotation, scale), `Aabb3`, scalar helpers, and the view/projection constructors the renderer will use.

**Conventions** (fixed; everything else builds on them):
- Right-handed, y up, forward is -z (view space looks down -z).
- Column-major matrices, column vectors: `p' = M * p`, `M = T * R * S`.
- Quaternions are `{x, y, z, w}` with `w` the scalar; a vector is rotated as `q v q^-1`; `q` and `-q` compare equal under `approx_equal`.
- Angles are radians.
- Projections produce zero-to-one **reversed** depth (near = 1, far = 0), with an infinite-far option, for float precision across large view distances. The Vulkan y-flip is a viewport matter, not a projection one.
- Layouts are GPU-friendly and trivially copyable: `Vec3` is 12 bytes, `Mat4` 64, `Transform3` 40. These are also the IDL's `vec2 vec3 vec4 quat` primitives.

**Invariants (tested).** `M * inverse(M) ≈ I`; `quat_from_mat3(mat3_from_quat(q)) ≈ q`; `compose`/`inverse` on `Transform3` agree with the equivalent matrix; `normalize` of a zero vector is zero, not NaN; `inverse` of a singular matrix is the identity rather than NaN.

**Public API.** `core/math/math.h`. Free functions: `dot`, `cross`, `length`, `normalize`, `lerp`, `min`, `max`, `reflect`, `rotate`, `slerp`, `conjugate`, `inverse`, `transpose`, `determinant`, `translation`, `scaling`, `mat3/4_from_quat`, `quat_from_mat3`, `quat_from_axis_angle`, `quat_from_euler`, `mat4_from_transform`, `transform_point`, `transform_direction`, `compose`, `transform_aabb`, `look_at`, `perspective_reversed_z`, `orthographic_reversed_z`.

**Depends on.** `base`.

**Testing.** `tools/dev.ps1 test -Filter math`.

**Performance notes.** Scalar code, `constexpr` where the standard allows. SIMD versions arrive with ISPC kernels for the hot paths (culling, transforms, animation) rather than by widening these types; keeping `Vec3` at 12 bytes is deliberate for GPU buffer layouts.
