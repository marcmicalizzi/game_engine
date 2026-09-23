// Size table for domain/geometry (ADR-0019). ClusterDesc is read by the GPU through a device
// address, so its layout is part of the shader contract as well as the footprint budget.
#include <core/base/size_table.h>
#include <domain/geometry/cluster.h>
#include <domain/geometry/cluster_file.h>
#include <domain/geometry/cluster_lod.h>
#include <domain/geometry/cluster_pages.h>
#include <domain/geometry/surface_binding.h>

using namespace engine;

ENGINE_EXPECT_SIZE(48, 4, geometry::ClusterDesc);

ENGINE_EXPECT_SIZE(48, 4, geometry::ClusterLodDesc);

// A page descriptor travels to the GPU with the cluster it names, so its layout is the streaming
// contract as well as the footprint: twelve words, no padding, a multiple of sixteen bytes.
ENGINE_EXPECT_SIZE(48, 4, geometry::ClusterPageDesc);

// One page request, which the cull pass will write to a feedback buffer once §4.9's GPU half
// exists: three words, so a lane emits one and the CPU reads it back without a conversion.
ENGINE_EXPECT_SIZE(12, 4, geometry::PageRequest);

ENGINE_EXPECT_SIZE(8, 4, geometry::VertexAttributes);

// The skin binding is the same eight bytes a vertex as the attributes, and it is read by the
// deform pass through a device address, so its layout is the shader contract too: four u8 joint
// indices then four u8 weights, no padding, alignment 1 so the stream is a plain byte array.
ENGINE_EXPECT_SIZE(8, 1, geometry::SkinBinding);

// A morph channel record: four floats the deform chain and the renderer read through a device
// address — the two quantization scales, the displacement bound an instance's `bounds_padding`
// is summed from, and the source's default weight. Sixteen bytes is a whole float4 load, and
// there is deliberately no name in it: names are a host-side array, because a shader has no use
// for one and putting it here would make the record a variable-length thing.
ENGINE_EXPECT_SIZE(16, 4, geometry::MorphChannel);

// One channel's run of deltas inside one cluster, read by the deform chain: three words, no
// padding. The count is stored rather than derived from the next slice's `first_delta` because a
// slice is also the unit the page layout moves, and an explicit count is one fewer invariant for
// a permutation to break.
ENGINE_EXPECT_SIZE(12, 4, geometry::MorphSlice);

// The .clusters container: these three are the file itself, so their size is the format.
ENGINE_EXPECT_SIZE(32, 8, geometry::ClusterFileHeader);

ENGINE_EXPECT_SIZE(24, 8, geometry::ClusterFileSection);

ENGINE_EXPECT_SIZE(32, 4, geometry::ClusterFileScalars);

ENGINE_EXPECT_SIZE(64, 4, geometry::ClusterFileMaterial);

// The image record is the format too: three words of range and identity plus the one-based media
// type, with no padding to leave a later field room in.
ENGINE_EXPECT_SIZE(24, 8, geometry::ClusterFileImage);

// The surface binding record (surface_binding.h; plan 07 §7.11's proposed packing): a u16 refined
// triangle, two u16 barycentrics, a half-float normal offset and a u16 blend weight, five u16 and
// no padding. It is stored once per bound render vertex and read once per frame by the transfer,
// so its ten bytes are the per-vertex cost of a region; the plan budgets 10, 12 with a region id.
ENGINE_EXPECT_SIZE(10, 2, geometry::SurfaceBinding);
