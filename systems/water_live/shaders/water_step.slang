// The shallow-water step on the card (roadmap W175; docs/subsystems/water_live.md, "The step").
//
// The same scheme as domain/water_state's reference, in single precision: the hydrostatic
// reconstruction of Audusse, Bouchut, Bristeau and Sainte-Marie (2016), (3.1)-(3.5), over the
// central-upwind flux of Bollermann, Chen, Kurganov and Noelle (2014), (2.8) with (2.13)-(2.14);
// the bed at cell centres and a face's bed the larger of its two; a velocity of zero below the dry
// depth; Manning's friction taken implicitly; sources and sinks; closed and open edges; and the
// water's ledger. Every expression is written in the reference's own order, operand for operand.
//
// **Each cell gathers.** `update_main` computes the fluxes through a cell's four faces from its
// neighbours and writes its new state into the other copy: no scatter, no atomic, a fixed order. A
// face is computed twice, once by each of its cells, from the same arguments in the same order.
//
// Three entries, run as passes of a render graph the host builds (src/water_live.cpp):
//   prime_main   the speeds of a state just written from the host, one record per workgroup;
//   dt_main      one workgroup: the step's length from the records, and the ledger's sums;
//   update_main  one cell a thread: the step, and its workgroup's record for the next dt_main.
//
// **The arithmetic.** No multiply and add may be fused: the module is compiled with
// `-fp-mode precise` (CMakeLists.txt), which decorates every float operation NoContraction, and
// every line is also `precise` for a reader (Slang 2026.17's `precise` alone emits no decoration
// when it writes SPIR-V directly; tests/water_live_tests.cpp counts them). Addition, subtraction
// and multiplication are then correctly rounded on every conformant device. Division and the square root are not (Vulkan allows 2.5 ulp and more), so
// with `k_fixed_ops` (specialization constant 0) they, and the cube root the friction needs, are
// built from those three alone: an estimate from the operand's bits, which integer arithmetic makes
// exactly, and a fixed number of Newton iterations. docs/experiments/water-step-on-the-card.md has
// what each form costs and what it gives.

[vk::constant_id(0)] const bool k_fixed_ops = false;

struct WaterStepParams {
  uint64_t state_in;   // three planes of nx * ny floats: h, hu, hv
  uint64_t state_out;  // the same, written by update_main
  uint64_t bed;        // nx * ny floats, metres from the grid's own datum
  uint64_t manning;    // nx * ny floats, or unused (flags)
  uint64_t source;     // nx * ny floats, m/s, or unused (flags)
  uint64_t partials;   // one WaterPartial per workgroup of update_main / prime_main
  uint64_t ledger;     // one WaterLedger
  uint nx;
  uint ny;
  uint groups_x;
  uint group_count;
  uint flags;  // k_flag_* below
  float cell_m;
  float gravity;
  float cfl;
  float dry_depth_m;
  float max_dt;
  float source_scale;  // this step applies source * dt * source_scale (a coarser clock: N, or 0)
  uint pad;
  uint64_t faces_x;  // with k_flag_stored_faces: (nx + 1) * ny faces of four floats, face f west of cell f
  uint64_t faces_y;  // nx * (ny + 1) faces, row r south of cell row r
};
[[vk::push_constant]] ConstantBuffer<WaterStepParams> g_step;

static const uint k_flag_west_open = 1u;
static const uint k_flag_east_open = 2u;
static const uint k_flag_south_open = 4u;
static const uint k_flag_north_open = 8u;
static const uint k_flag_manning = 16u;
static const uint k_flag_source = 32u;
// The source's remainder kept per cell (state plane 3): what rounding did not add to the depth is
// carried to the next step, so an exchange under the depth's last bit is not lost
// (docs/subsystems/water_live.md, "An exchange under the last bit").
static const uint k_flag_compensate = 64u;
// The faces' fluxes computed once each by faces_x_main and faces_y_main and read by update_main,
// in place of each cell computing its own four (the two forms must give the same bytes).
static const uint k_flag_stored_faces = 128u;

// A workgroup's record: the largest |u| + c and |v| + c of the state it wrote, and the volumes its
// cells added to the ledger in that step (m^3). Mirrored by water_live::WaterPartial.
struct WaterPartial {
  float speed_x;
  float speed_y;
  float source_m3;
  float open_m3;
  float roundoff_m3;
  float pad0;
  float pad1;
  float pad2;
};

// The ledger, each sum a pair (hi + lo) kept by Knuth's TwoSum, so a run of 10^5 steps adds in
// about twice single precision. Mirrored by water_live::WaterLedger.
struct WaterLedger {
  float time_hi;
  float time_lo;
  float source_hi;
  float source_lo;
  float open_hi;
  float open_lo;
  float roundoff_hi;
  float roundoff_lo;
  float remaining_s;  // an advance's time left; +inf for plain steps
  float dt;           // the length dt_main chose, which update_main reads
  uint steps;         // steps of non-zero length
  uint pad;
};

static const uint k_group_side = 16u;
static const uint k_group_cells = 256u;

// ---- the arithmetic ---------------------------------------------------------------------------

float positive_infinity() { return asfloat(0x7f800000u); }

// 1 / x for x > 0: the bits' estimate (relative error under 1/8), then four Newton steps
// r <- r (2 - x r), each squaring the error: 1/8, 1/64, 2.4e-4, 6e-8, then rounding's.
// Outside the normal range the estimate is not one: an x of 2^126 or more has a reciprocal below
// the smallest normal, taken as zero (what a device that flushes denormals gives), and a zero or a
// denormal x the largest finite float, so a zero numerator gives zero and never 0 x inf.
float rcp_fixed(float x) {
  const uint bits = asuint(x);
  if (bits >= 0x7e800000u) return 0.0;
  if (bits < 0x00800000u) return asfloat(0x7f7fffffu);
  precise float r = asfloat(0x7ef311c3u - bits);
  [ForceUnroll]
  for (int i = 0; i < 4; ++i) {
    precise float t = x * r;
    precise float e = 2.0 - t;
    r = r * e;
  }
  return r;
}

// 1 / sqrt(x) for a normal x > 0 (sqrt_op answers zero and the denormals itself): the bits'
// estimate (relative error under 3.5e-3), then three Newton steps y <- y (1.5 - 0.5 x y y).
float rsqrt_fixed(float x) {
  precise float y = asfloat(0x5f375a86u - (asuint(x) >> 1));
  precise float half_x = 0.5 * x;
  [ForceUnroll]
  for (int i = 0; i < 3; ++i) {
    precise float yy = y * y;
    precise float t = half_x * yy;
    precise float e = 1.5 - t;
    y = y * e;
  }
  return y;
}

// x^(1/3) for x > 0: the bits divided by three (exact in integers) plus a bias, then four Newton
// steps x <- (2 x + h / x^2) / 3.
float cbrt_fixed(float h) {
  precise float x = asfloat(asuint(h) / 3u + 709921077u);
  [ForceUnroll]
  for (int i = 0; i < 4; ++i) {
    precise float xx = x * x;
    precise float q = h * rcp_fixed(xx);
    precise float twice = x + x;
    precise float s = twice + q;
    x = s * (1.0 / 3.0);
  }
  return x;
}

float div_op(float a, float b) {
  if (k_fixed_ops) {
    if (a == 0.0) return a;  // 0 / b, its sign kept, whatever b's reciprocal came to
    precise float q = a * rcp_fixed(b);
    return q;
  }
  precise float q = a / b;
  return q;
}

float sqrt_op(float x) {
  if (k_fixed_ops) {
    // A zero or a denormal: zero (what a device that flushes denormals gives). The estimate's
    // iterations would square a number near 2^63 and overflow to infinity, and 0 x inf is NaN.
    if (asuint(x) < 0x00800000u) return 0.0;
    precise float s = x * rsqrt_fixed(x);
    return s;
  }
  precise float s = sqrt(x);
  return s;
}

float cbrt_op(float h) {
  if (k_fixed_ops) return cbrt_fixed(h);
  precise float l = log2(h);
  precise float t = l * (1.0 / 3.0);
  precise float c = exp2(t);
  return c;
}

float pressure(float g, float h) {
  precise float a = 0.5 * g;
  precise float b = a * h;
  precise float p = b * h;
  return p;
}

// ---- the faces --------------------------------------------------------------------------------

struct Flux {
  float h;
  float n;
  float t;
};

// The flux of the system without a bed between two states (Bollermann et al. 2014, (2.8) with
// (2.13)-(2.14)), the reference's `face_flux` line for line. Two identical states give that
// state's exact flux, which keeps a lake at rest at rest to the bit.
Flux face_flux(float g, float hl, float ul, float vl, float hr, float ur, float vr) {
  Flux f;
  if (hl == hr && ul == ur && vl == vr) {
    precise float q = hl * ul;
    precise float qu = q * ul;
    precise float n = qu + pressure(g, hl);
    precise float t = q * vl;
    f.h = q;
    f.n = n;
    f.t = t;
    return f;
  }
  precise float gl = g * hl;
  precise float gr = g * hr;
  precise float cl = sqrt_op(gl);
  precise float cr = sqrt_op(gr);
  precise float ap = max(max(ur + cr, ul + cl), 0.0);
  precise float am = min(min(ur - cr, ul - cl), 0.0);
  precise float span = ap - am;
  if (!(span > 0.0)) {
    f.h = 0.0;
    f.n = 0.0;
    f.t = 0.0;
    return f;
  }
  precise float ql = hl * ul;
  precise float qr = hr * ur;
  precise float apam = ap * am;
  precise float h1 = ap * ql;
  precise float h2 = am * qr;
  precise float h3 = hr - hl;
  precise float h4 = apam * h3;
  precise float h5 = h1 - h2;
  precise float h6 = h5 + h4;
  precise float n1 = ql * ul;
  precise float n2 = n1 + pressure(g, hl);
  precise float n3 = ap * n2;
  precise float n4 = qr * ur;
  precise float n5 = n4 + pressure(g, hr);
  precise float n6 = am * n5;
  precise float n7 = qr - ql;
  precise float n8 = apam * n7;
  precise float n9 = n3 - n6;
  precise float n10 = n9 + n8;
  precise float t1 = ql * vl;
  precise float t2 = ap * t1;
  precise float t3 = qr * vr;
  precise float t4 = am * t3;
  precise float t5 = hr * vr;
  precise float t6 = hl * vl;
  precise float t7 = t5 - t6;
  precise float t8 = apam * t7;
  precise float t9 = t2 - t4;
  precise float t10 = t9 + t8;
  f.h = div_op(h6, span);
  f.n = div_op(n10, span);
  f.t = div_op(t10, span);
  return f;
}

// One face between a left (or south) cell and a right (or north) cell, the reference's
// ENGINE_WATER_FACE: the face's bed the larger of the two, each side's depth reconstructed against
// it, the flux between them, and the normal momentum's flux less each side's own pressure.
struct Face {
  float h;
  float nl;  // normal momentum's flux less the left side's pressure
  float nr;  // less the right side's
  float t;
};

Face face(float g, float hl0, float zl, float ln, float lt, float hr0, float zr, float rn,
          float rt) {
  precise float zf = max(zl, zr);
  precise float sl = hl0 + zl;
  precise float el = sl - zf;
  precise float sr = hr0 + zr;
  precise float er = sr - zf;
  precise float hl = el > 0.0 ? el : 0.0;
  precise float hr = er > 0.0 ? er : 0.0;
  const Flux f = face_flux(g, hl, ln, lt, hr, rn, rt);
  Face out;
  precise float nl = f.n - pressure(g, hl);
  precise float nr = f.n - pressure(g, hr);
  out.h = f.h;
  out.nl = nl;
  out.nr = nr;
  out.t = f.t;
  return out;
}

// ---- the state --------------------------------------------------------------------------------

struct Cell {
  float h;
  float u;
  float v;
  float z;
};

Cell load_cell(uint i) {
  const uint cells = g_step.nx * g_step.ny;
  float* state = (float*)g_step.state_in;
  float* bed = (float*)g_step.bed;
  Cell c;
  c.h = state[i];
  const bool wet = c.h >= g_step.dry_depth_m;
  c.u = wet ? div_op(state[cells + i], c.h) : 0.0;
  c.v = wet ? div_op(state[2u * cells + i], c.h) : 0.0;
  c.z = bed[i];
  return c;
}

// |u| + c and |v| + c of a cell's state, as the reference's `velocity_rows` takes them.
float2 cell_speeds(float h, float hu, float hv) {
  const bool wet = h >= g_step.dry_depth_m;
  precise float u = wet ? div_op(hu, h) : 0.0;
  precise float v = wet ? div_op(hv, h) : 0.0;
  precise float gh = g_step.gravity * h;
  precise float c = sqrt_op(gh);
  precise float au = abs(u) + c;
  precise float av = abs(v) + c;
  return float2(au, av);
}

// ---- a workgroup's record ---------------------------------------------------------------------

groupshared float s_speed_x[k_group_cells];
groupshared float s_speed_y[k_group_cells];
groupshared float s_source[k_group_cells];
groupshared float s_open[k_group_cells];
groupshared float s_roundoff[k_group_cells];

// A fixed tree over the workgroup's 256 slots: the same pairs added in the same order every time.
// A maximum is exact in any order; a sum is the same bytes because the order is fixed.
void write_partial(uint local, uint group, float2 speeds, float source, float open, float roundoff) {
  s_speed_x[local] = speeds.x;
  s_speed_y[local] = speeds.y;
  s_source[local] = source;
  s_open[local] = open;
  s_roundoff[local] = roundoff;
  GroupMemoryBarrierWithGroupSync();
  [ForceUnroll]
  for (uint stride = k_group_cells / 2u; stride > 0u; stride >>= 1u) {
    if (local < stride) {
      s_speed_x[local] = max(s_speed_x[local], s_speed_x[local + stride]);
      s_speed_y[local] = max(s_speed_y[local], s_speed_y[local + stride]);
      precise float a = s_source[local] + s_source[local + stride];
      precise float b = s_open[local] + s_open[local + stride];
      precise float c = s_roundoff[local] + s_roundoff[local + stride];
      s_source[local] = a;
      s_open[local] = b;
      s_roundoff[local] = c;
    }
    GroupMemoryBarrierWithGroupSync();
  }
  if (local == 0u) {
    WaterPartial p;
    p.speed_x = s_speed_x[0];
    p.speed_y = s_speed_y[0];
    p.source_m3 = s_source[0];
    p.open_m3 = s_open[0];
    p.roundoff_m3 = s_roundoff[0];
    p.pad0 = 0.0;
    p.pad1 = 0.0;
    p.pad2 = 0.0;
    ((WaterPartial*)g_step.partials)[group] = p;
  }
}

[shader("compute")]
[numthreads(16, 16, 1)]
void prime_main(uint3 id : SV_DispatchThreadID, uint3 gid : SV_GroupID, uint3 lid : SV_GroupThreadID) {
  const uint local = lid.y * k_group_side + lid.x;
  const uint group = gid.y * g_step.groups_x + gid.x;
  float2 speeds = float2(0.0, 0.0);
  if (id.x < g_step.nx && id.y < g_step.ny) {
    const uint cells = g_step.nx * g_step.ny;
    const uint i = id.y * g_step.nx + id.x;
    float* state = (float*)g_step.state_in;
    speeds = cell_speeds(state[i], state[cells + i], state[2u * cells + i]);
  }
  write_partial(local, group, speeds, 0.0, 0.0, 0.0);
}

// ---- the step's length and the ledger ---------------------------------------------------------

// Adds x to the pair (hi, lo): Knuth's TwoSum, then the pair renormalized (Dekker's Fast2Sum).
void pair_add(inout float hi, inout float lo, float x) {
  precise float s = hi + x;
  precise float bb = s - hi;
  precise float sa = s - bb;
  precise float da = hi - sa;
  precise float db = x - bb;
  precise float e = da + db;
  precise float l = lo + e;
  precise float h2 = s + l;
  precise float back = h2 - s;
  precise float l2 = l - back;
  hi = h2;
  lo = l2;
}

groupshared float s_a[k_group_cells];
groupshared float s_b[k_group_cells];
groupshared float s_src[k_group_cells];
groupshared float s_opn[k_group_cells];
groupshared float s_rnd[k_group_cells];

[shader("compute")]
[numthreads(256, 1, 1)]
void dt_main(uint3 lid : SV_GroupThreadID) {
  const uint t = lid.x;
  WaterPartial* partials = (WaterPartial*)g_step.partials;
  float a = 0.0;
  float b = 0.0;
  float src = 0.0;
  float opn = 0.0;
  float rnd = 0.0;
  for (uint g = t; g < g_step.group_count; g += k_group_cells) {
    const WaterPartial p = partials[g];
    a = max(a, p.speed_x);
    b = max(b, p.speed_y);
    precise float s1 = src + p.source_m3;
    precise float s2 = opn + p.open_m3;
    precise float s3 = rnd + p.roundoff_m3;
    src = s1;
    opn = s2;
    rnd = s3;
    // Counted once: a flush followed by a step must not add the same volumes twice.
    partials[g].source_m3 = 0.0;
    partials[g].open_m3 = 0.0;
    partials[g].roundoff_m3 = 0.0;
  }
  s_a[t] = a;
  s_b[t] = b;
  s_src[t] = src;
  s_opn[t] = opn;
  s_rnd[t] = rnd;
  GroupMemoryBarrierWithGroupSync();
  [ForceUnroll]
  for (uint stride = k_group_cells / 2u; stride > 0u; stride >>= 1u) {
    if (t < stride) {
      s_a[t] = max(s_a[t], s_a[t + stride]);
      s_b[t] = max(s_b[t], s_b[t + stride]);
      precise float x = s_src[t] + s_src[t + stride];
      precise float y = s_opn[t] + s_opn[t + stride];
      precise float z = s_rnd[t] + s_rnd[t + stride];
      s_src[t] = x;
      s_opn[t] = y;
      s_rnd[t] = z;
    }
    GroupMemoryBarrierWithGroupSync();
  }
  if (t != 0u) return;
  WaterLedger* ledger = (WaterLedger*)g_step.ledger;
  WaterLedger l = ledger[0];
  pair_add(l.source_hi, l.source_lo, s_src[0]);
  pair_add(l.open_hi, l.open_lo, s_opn[0]);
  pair_add(l.roundoff_hi, l.roundoff_lo, s_rnd[0]);
  // max_dt == 0 is a flush: the sums are folded in and no step is chosen.
  if (g_step.max_dt > 0.0) {
    precise float speed = s_a[0] + s_b[0];
    precise float reach = g_step.cfl * g_step.cell_m;
    const float stable = speed > 0.0 ? div_op(reach, speed) : positive_infinity();
    float dt = min(stable, g_step.max_dt);
    dt = min(dt, l.remaining_s);
    precise float left = l.remaining_s - dt;
    l.remaining_s = left;
    l.dt = dt;
    pair_add(l.time_hi, l.time_lo, dt);
    l.steps = l.steps + (dt > 0.0 ? 1u : 0u);
  } else {
    l.dt = 0.0;
  }
  ledger[0] = l;
}

// ---- the faces, computed once each (k_flag_stored_faces) --------------------------------------

Face load_face(uint64_t address, uint f) {
  const float4 v = ((float4*)address)[f];
  Face out;
  out.h = v.x;
  out.nl = v.y;
  out.nr = v.z;
  out.t = v.w;
  return out;
}

void store_face(uint64_t address, uint f, Face face) {
  ((float4*)address)[f] = float4(face.h, face.nl, face.nr, face.t);
}

// x faces: thread (f, y) for f in [0, nx], face f west of cell f; the reference's x_face_rows.
[shader("compute")]
[numthreads(16, 16, 1)]
void faces_x_main(uint3 id : SV_DispatchThreadID) {
  const uint nx = g_step.nx;
  if (id.x > nx || id.y >= g_step.ny) return;
  if (!(((WaterLedger*)g_step.ledger)[0].dt > 0.0)) return;
  const float g = g_step.gravity;
  const uint row = id.y * nx;
  Face f;
  if (id.x == 0u) {
    const Cell c = load_cell(row);
    const float sign = (g_step.flags & k_flag_west_open) != 0u ? 1.0 : -1.0;
    precise float gn = sign * c.u;
    f = face(g, c.h, c.z, gn, c.v, c.h, c.z, c.u, c.v);
  } else if (id.x == nx) {
    const Cell c = load_cell(row + nx - 1u);
    const float sign = (g_step.flags & k_flag_east_open) != 0u ? 1.0 : -1.0;
    precise float gn = sign * c.u;
    f = face(g, c.h, c.z, c.u, c.v, c.h, c.z, gn, c.v);
  } else {
    const Cell l = load_cell(row + id.x - 1u);
    const Cell r = load_cell(row + id.x);
    f = face(g, l.h, l.z, l.u, l.v, r.h, r.z, r.u, r.v);
  }
  store_face(g_step.faces_x, id.y * (nx + 1u) + id.x, f);
}

// y faces: thread (x, r) for r in [0, ny], row r south of cell row r; the reference's y_face_rows.
[shader("compute")]
[numthreads(16, 16, 1)]
void faces_y_main(uint3 id : SV_DispatchThreadID) {
  const uint nx = g_step.nx;
  const uint ny = g_step.ny;
  if (id.x >= nx || id.y > ny) return;
  if (!(((WaterLedger*)g_step.ledger)[0].dt > 0.0)) return;
  const float g = g_step.gravity;
  Face f;
  if (id.y == 0u) {
    const Cell c = load_cell(id.x);
    const float sign = (g_step.flags & k_flag_south_open) != 0u ? 1.0 : -1.0;
    precise float gn = sign * c.v;
    f = face(g, c.h, c.z, gn, c.u, c.h, c.z, c.v, c.u);
  } else if (id.y == ny) {
    const Cell c = load_cell((ny - 1u) * nx + id.x);
    const float sign = (g_step.flags & k_flag_north_open) != 0u ? 1.0 : -1.0;
    precise float gn = sign * c.v;
    f = face(g, c.h, c.z, c.v, c.u, c.h, c.z, gn, c.u);
  } else {
    const Cell s = load_cell((id.y - 1u) * nx + id.x);
    const Cell n = load_cell(id.y * nx + id.x);
    f = face(g, s.h, s.z, s.v, s.u, n.h, n.z, n.v, n.u);
  }
  store_face(g_step.faces_y, id.y * nx + id.x, f);
}

// ---- the step ---------------------------------------------------------------------------------

[shader("compute")]
[numthreads(16, 16, 1)]
void update_main(uint3 id : SV_DispatchThreadID, uint3 gid : SV_GroupID, uint3 lid : SV_GroupThreadID) {
  const uint local = lid.y * k_group_side + lid.x;
  const uint group = gid.y * g_step.groups_x + gid.x;
  const uint nx = g_step.nx;
  const uint ny = g_step.ny;
  float2 speeds = float2(0.0, 0.0);
  float cell_source = 0.0;
  float cell_open = 0.0;
  float cell_roundoff = 0.0;
  if (id.x < nx && id.y < ny) {
    const uint x = id.x;
    const uint y = id.y;
    const uint cells = nx * ny;
    const uint i = y * nx + x;
    const float g = g_step.gravity;
    const float dt = ((WaterLedger*)g_step.ledger)[0].dt;
    const uint flags = g_step.flags;
    float* state = (float*)g_step.state_in;
    float* out = (float*)g_step.state_out;
    if (!(dt > 0.0)) {
      // A step of no length (an advance's time has run out): the state as it is, to the bit.
      const float h0 = state[i];
      const float hu0 = state[cells + i];
      const float hv0 = state[2u * cells + i];
      out[i] = h0;
      out[cells + i] = hu0;
      out[2u * cells + i] = hv0;
      out[3u * cells + i] = state[3u * cells + i];
      speeds = cell_speeds(h0, hu0, hv0);
    } else {
    const Cell c = load_cell(i);
    // The four faces, each the reference's: an edge face is the cell against its own ghost (a wall
    // mirrors the normal velocity, an open edge copies it).
    Face fw, fe, fs, fn;
    if ((flags & k_flag_stored_faces) != 0u) {
      fw = load_face(g_step.faces_x, y * (nx + 1u) + x);
      fe = load_face(g_step.faces_x, y * (nx + 1u) + x + 1u);
      fs = load_face(g_step.faces_y, y * nx + x);
      fn = load_face(g_step.faces_y, (y + 1u) * nx + x);
    } else {
    if (x == 0u) {
      const float sign = (flags & k_flag_west_open) != 0u ? 1.0 : -1.0;
      precise float gn = sign * c.u;
      fw = face(g, c.h, c.z, gn, c.v, c.h, c.z, c.u, c.v);
    } else {
      const Cell w = load_cell(i - 1u);
      fw = face(g, w.h, w.z, w.u, w.v, c.h, c.z, c.u, c.v);
    }
    if (x + 1u == nx) {
      const float sign = (flags & k_flag_east_open) != 0u ? 1.0 : -1.0;
      precise float gn = sign * c.u;
      fe = face(g, c.h, c.z, c.u, c.v, c.h, c.z, gn, c.v);
    } else {
      const Cell e = load_cell(i + 1u);
      fe = face(g, c.h, c.z, c.u, c.v, e.h, e.z, e.u, e.v);
    }
    if (y == 0u) {
      const float sign = (flags & k_flag_south_open) != 0u ? 1.0 : -1.0;
      precise float gn = sign * c.v;
      fs = face(g, c.h, c.z, gn, c.u, c.h, c.z, c.v, c.u);
    } else {
      const Cell s = load_cell(i - nx);
      fs = face(g, s.h, s.z, s.v, s.u, c.h, c.z, c.v, c.u);
    }
    if (y + 1u == ny) {
      const float sign = (flags & k_flag_north_open) != 0u ? 1.0 : -1.0;
      precise float gn = sign * c.v;
      fn = face(g, c.h, c.z, c.v, c.u, c.h, c.z, gn, c.u);
    } else {
      const Cell n = load_cell(i + nx);
      fn = face(g, c.h, c.z, c.v, c.u, n.h, n.z, n.v, n.u);
    }
    }
    const float area = g_step.cell_m * g_step.cell_m;
    precise float lam = div_op(dt, g_step.cell_m);
    precise float edge_volume = dt * g_step.cell_m;
    if (x == 0u && (flags & k_flag_west_open) != 0u) {
      precise float o = fw.h * edge_volume;
      precise float r = cell_open - o;
      cell_open = r;
    }
    if (x + 1u == nx && (flags & k_flag_east_open) != 0u) {
      precise float o = fe.h * edge_volume;
      precise float r = cell_open + o;
      cell_open = r;
    }
    if (y == 0u && (flags & k_flag_south_open) != 0u) {
      precise float o = fs.h * edge_volume;
      precise float r = cell_open - o;
      cell_open = r;
    }
    if (y + 1u == ny && (flags & k_flag_north_open) != 0u) {
      precise float o = fn.h * edge_volume;
      precise float r = cell_open + o;
      cell_open = r;
    }
    // U' = U - dt/dx ((F_e - F_w) + (F_n - F_s)), the reference's update_rows.
    precise float dh1 = fe.h - fw.h;
    precise float dh2 = fn.h - fs.h;
    precise float dh = dh1 + dh2;
    precise float lh = lam * dh;
    precise float h = c.h - lh;
    precise float du1 = fe.nl - fw.nr;
    precise float du2 = fn.t - fs.t;
    precise float du = du1 + du2;
    precise float lu = lam * du;
    precise float hu = state[cells + i] - lu;
    precise float dv1 = fe.t - fw.t;
    precise float dv2 = fn.nl - fs.nr;
    precise float dv = dv1 + dv2;
    precise float lv = lam * dv;
    precise float hv = state[2u * cells + i] - lv;
    // What rounding leaves below zero is clamped and counted (water_state.md, "The step's length").
    if (h < 0.0) {
      precise float lost = h * area;
      precise float r = cell_roundoff - lost;
      cell_roundoff = r;
      h = 0.0;
    }
    float residual = 0.0;
    if ((flags & k_flag_compensate) != 0u) residual = state[3u * cells + i];
    if ((flags & k_flag_source) != 0u && (flags & k_flag_compensate) != 0u) {
      // The same source with its remainder: the target is what this step owes plus what earlier
      // steps' rounding left out; what reaches the depth is h' - h, exact by Sterbenz's lemma while
      // the step is small against the depth, and the rest is kept for the next step.
      precise float owed = ((float*)g_step.source)[i] * dt;
      precise float scaled = owed * g_step.source_scale;
      precise float target = scaled + residual;
      precise float h2 = h;
      if (target >= 0.0) {
        precise float t2 = h + target;
        h2 = t2;
      } else {
        precise float want = -target;
        precise float taken = want < h ? want : h;
        precise float t2 = h - taken;
        h2 = t2;
      }
      precise float applied = h2 - h;
      precise float left = target - applied;
      // A dry cell owes nothing more: a sink with no water to take keeps no debt.
      residual = (h2 == 0.0 && target < 0.0) ? 0.0 : left;
      precise float v = applied * area;
      precise float r = cell_source + v;
      cell_source = r;
      h = h2;
    } else if ((flags & k_flag_source) != 0u) {
      precise float owed = ((float*)g_step.source)[i] * dt;
      precise float added = owed * g_step.source_scale;
      if (added >= 0.0) {
        precise float h2 = h + added;
        h = h2;
        precise float v = added * area;
        precise float r = cell_source + v;
        cell_source = r;
      } else {
        precise float want = -added;
        precise float taken = want < h ? want : h;
        precise float h2 = h - taken;
        h = h2;
        precise float v = taken * area;
        precise float r = cell_source - v;
        cell_source = r;
      }
    }
    if (h >= g_step.dry_depth_m) {
      if ((flags & k_flag_manning) != 0u) {
        // (hu)' = (hu)* / (1 + dt g n^2 |u*| / h^(4/3)), the reference's form and order.
        const float nn = ((float*)g_step.manning)[i];
        precise float u = div_op(hu, h);
        precise float v = div_op(hv, h);
        precise float uu = u * u;
        precise float vv = v * v;
        precise float s2 = uu + vv;
        precise float sp = sqrt_op(s2);
        precise float cb = cbrt_op(h);
        precise float k1 = g * nn;
        precise float k2 = k1 * nn;
        precise float k3 = k2 * sp;
        precise float k4 = h * cb;
        precise float k = div_op(k3, k4);
        precise float dk = dt * k;
        precise float denom = 1.0 + dk;
        precise float hu2 = div_op(hu, denom);
        precise float hv2 = div_op(hv, denom);
        hu = hu2;
        hv = hv2;
      }
    } else {
      hu = 0.0;
      hv = 0.0;
    }
    out[i] = h;
    out[cells + i] = hu;
    out[2u * cells + i] = hv;
    if ((flags & k_flag_compensate) != 0u) out[3u * cells + i] = residual;
    speeds = cell_speeds(h, hu, hv);
    }
  }
  write_partial(local, group, speeds, cell_source, cell_open, cell_roundoff);
}
