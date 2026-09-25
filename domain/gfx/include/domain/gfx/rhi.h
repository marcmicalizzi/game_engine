#pragma once

// The RHI's own vocabulary (docs/subsystems/gfx.md, "The RHI surface and the backend surface"):
// the handles, formats, usages, layouts, stages and small descriptions a public header of
// `domain/gfx` or `systems/renderer` may name. Nothing here names a Vulkan type, and nothing
// here includes a Vulkan header; `tools/lint.ps1` (`vulkan-in-public-header`) keeps it so for
// every header under an `include/` directory outside the backend set.
//
// **Why it exists.** Until 2026-09-25 the public headers said `VkBuffer`, `VkFormat` and
// `VkCommandBuffer`, so every module that drew anything saw Vulkan, and every week the leak went
// further into the renderer. A second backend — D3D12 behind the trigger of plan 08 §8.3, or a
// console's API — cannot live beside that, and retiring it gets more expensive the more code
// names it (plan 10 §10.6). The backend itself lives in `src/` and in the backend header set,
// `domain/gfx/backend/vulkan/`, which only the modules that own a device, a swapchain or a
// surface may include.
//
// **Handles** are 64 bits, trivially copyable, and zero is null: the same size as the Vulkan
// handle they carry today, so no struct that holds one changed size (the size table pins it).
// A handle is strongly typed — a buffer cannot be passed where an image view is wanted — and
// opaque: only the backend turns one into its own object.
//
// **Enumerators carry Vulkan's numbers.** That is a convenience for the first backend, not part
// of the contract: it makes the Vulkan backend's mapping a cast that `static_assert`s prove
// (backend/vulkan/vulkan.h), so moving the renderer onto this vocabulary could not change one
// flag it passes. Nothing above the RHI may rely on the numbers — do arithmetic on the named
// enumerators only — and a second backend maps them through a table of its own.

#include <core/base/types.h>

#include <type_traits>

namespace engine::gfx {

// ---- handles ------------------------------------------------------------------------------------

template <class Tag>
class Handle {
 public:
  constexpr Handle() noexcept = default;
  // For the backend: the bits of its own object. Nothing above the RHI makes a handle from bits.
  constexpr explicit Handle(u64 bits) noexcept : bits_(bits) {}

  constexpr u64 bits() const noexcept { return bits_; }
  constexpr bool valid() const noexcept { return bits_ != 0; }
  constexpr explicit operator bool() const noexcept { return bits_ != 0; }
  friend constexpr bool operator==(const Handle& a, const Handle& b) noexcept {
    return a.bits_ == b.bits_;
  }
  friend constexpr bool operator!=(const Handle& a, const Handle& b) noexcept {
    return a.bits_ != b.bits_;
  }

 private:
  u64 bits_ = 0;
};

// Resources and what views them.
using BufferHandle = Handle<struct BufferTag>;
using ImageHandle = Handle<struct ImageTag>;
using ImageViewHandle = Handle<struct ImageViewTag>;
using SamplerHandle = Handle<struct SamplerTag>;
using AccelerationStructureHandle = Handle<struct AccelerationStructureTag>;
// The allocator's record of a resource's memory (VMA's allocation today).
using AllocationHandle = Handle<struct AllocationTag>;
// Shaders and pipelines.
using ShaderModuleHandle = Handle<struct ShaderModuleTag>;
using PipelineHandle = Handle<struct PipelineTag>;
using PipelineLayoutHandle = Handle<struct PipelineLayoutTag>;
using DescriptorSetLayoutHandle = Handle<struct DescriptorSetLayoutTag>;
using DescriptorSetHandle = Handle<struct DescriptorSetTag>;
using DescriptorPoolHandle = Handle<struct DescriptorPoolTag>;
// Submission: a command list being recorded, the pool it came from, a semaphore, a query pool.
using CommandListHandle = Handle<struct CommandListTag>;
using CommandPoolHandle = Handle<struct CommandPoolTag>;
using SemaphoreHandle = Handle<struct SemaphoreTag>;
using QueryPoolHandle = Handle<struct QueryPoolTag>;

// A buffer's address on the device (buffer device address): what every scene buffer is read
// through, and what the push blocks and parameter blocks carry.
using DeviceAddress = u64;

// "To the end of the buffer" for a size or a range.
inline constexpr u64 k_whole_size = ~u64{0};

// ---- flags --------------------------------------------------------------------------------------

// `|`, `&`, `|=`, `&=` and `~` for an enum class that is a set of bits, and `any()` to test one.
#define ENGINE_GFX_FLAGS(E)                                            \
  constexpr E operator|(E a, E b) noexcept {                           \
    return static_cast<E>(static_cast<std::underlying_type_t<E>>(a) |  \
                          static_cast<std::underlying_type_t<E>>(b));  \
  }                                                                    \
  constexpr E operator&(E a, E b) noexcept {                           \
    return static_cast<E>(static_cast<std::underlying_type_t<E>>(a) &  \
                          static_cast<std::underlying_type_t<E>>(b));  \
  }                                                                    \
  constexpr E operator~(E a) noexcept {                                \
    return static_cast<E>(~static_cast<std::underlying_type_t<E>>(a)); \
  }                                                                    \
  constexpr E& operator|=(E& a, E b) noexcept { return a = a | b; }    \
  constexpr E& operator&=(E& a, E b) noexcept { return a = a & b; }    \
  constexpr bool any(E a) noexcept { return static_cast<std::underlying_type_t<E>>(a) != 0; }

// How a buffer may be used. Every scene buffer is read through a device address, so
// `ShaderDeviceAddress` is on almost all of them. (Not `DeviceAddress`: that is the type of the
// address itself, above, and GCC's -Wshadow rightly refuses an enumerator hiding it.)
enum class BufferUsage : u32 {
  None = 0,
  TransferSrc = 0x1,
  TransferDst = 0x2,
  Uniform = 0x10,
  Storage = 0x20,
  Index = 0x40,
  Vertex = 0x80,
  Indirect = 0x100,
  ShaderDeviceAddress = 0x20000,
  // Read by an acceleration structure build: geometry, instance records, cluster records.
  AccelerationBuildInput = 0x80000,
  // Holds acceleration structures.
  AccelerationStorage = 0x100000,
};
ENGINE_GFX_FLAGS(BufferUsage)

enum class ImageUsage : u32 {
  None = 0,
  TransferSrc = 0x1,
  TransferDst = 0x2,
  Sampled = 0x4,
  Storage = 0x8,
  ColorAttachment = 0x10,
  DepthStencilAttachment = 0x20,
};
ENGINE_GFX_FLAGS(ImageUsage)

// Pipeline stages, for the render graph's imports and the few barriers recorded outside it
// (synchronization2's stage bits).
enum class PipelineStage : u64 {
  None = 0,
  TopOfPipe = 0x1,
  DrawIndirect = 0x2,
  VertexInput = 0x4,
  VertexShader = 0x8,
  FragmentShader = 0x80,
  EarlyFragmentTests = 0x100,
  LateFragmentTests = 0x200,
  ColorAttachmentOutput = 0x400,
  ComputeShader = 0x800,
  AllTransfer = 0x1000,
  BottomOfPipe = 0x2000,
  Host = 0x4000,
  AllGraphics = 0x8000,
  AllCommands = 0x10000,
  TaskShader = 0x80000,
  MeshShader = 0x100000,
  AccelerationBuild = 0x2000000,
  Copy = 0x100000000,
  Clear = 0x800000000,
  IndexInput = 0x1000000000,
};
ENGINE_GFX_FLAGS(PipelineStage)

// Memory accesses, the companion of PipelineStage (synchronization2's access bits). Not the
// render graph's `Access`, which is how a *pass* uses a resource and maps to one of these.
enum class MemoryAccess : u64 {
  None = 0,
  IndirectCommandRead = 0x1,
  IndexRead = 0x2,
  ShaderRead = 0x20,
  ShaderWrite = 0x40,
  ColorAttachmentRead = 0x80,
  ColorAttachmentWrite = 0x100,
  DepthStencilAttachmentRead = 0x200,
  DepthStencilAttachmentWrite = 0x400,
  TransferRead = 0x800,
  TransferWrite = 0x1000,
  HostRead = 0x2000,
  HostWrite = 0x4000,
  MemoryRead = 0x8000,
  MemoryWrite = 0x10000,
  AccelerationStructureRead = 0x200000,
  AccelerationStructureWrite = 0x400000,
  ShaderSampledRead = 0x100000000,
  ShaderStorageRead = 0x200000000,
  ShaderStorageWrite = 0x400000000,
};
ENGINE_GFX_FLAGS(MemoryAccess)

// Shader stages: a push-constant range's visibility and a reflected entry point's stage.
enum class ShaderStage : u32 {
  None = 0,
  Vertex = 0x1,
  TessellationControl = 0x2,
  TessellationEvaluation = 0x4,
  Geometry = 0x8,
  Fragment = 0x10,
  Compute = 0x20,
  Task = 0x40,
  Mesh = 0x80,
  RayGen = 0x100,
  AnyHit = 0x200,
  ClosestHit = 0x400,
  Miss = 0x800,
  Intersection = 0x1000,
  Callable = 0x2000,
  AllGraphics = 0x1F,
  All = 0x7FFFFFFF,
};
ENGINE_GFX_FLAGS(ShaderStage)

// How an acceleration structure is built.
enum class AccelerationBuildFlags : u32 {
  None = 0,
  AllowUpdate = 0x1,
  AllowCompaction = 0x2,
  PreferFastTrace = 0x4,
  PreferFastBuild = 0x8,
  LowMemory = 0x10,
};
ENGINE_GFX_FLAGS(AccelerationBuildFlags)

// Per-instance flags of a top-level structure's instance record.
enum class GeometryInstanceFlags : u32 {
  None = 0,
  TriangleFacingCullDisable = 0x1,
  TriangleFlipFacing = 0x2,
  ForceOpaque = 0x4,
  ForceNoOpaque = 0x8,
};
ENGINE_GFX_FLAGS(GeometryInstanceFlags)

// ---- enumerations -------------------------------------------------------------------------------

// The formats the engine creates, samples, captures or presents. A backend may hand back one
// this list does not name (a swapchain's), which passes through untouched.
enum class Format : u32 {
  Undefined = 0,
  R8Unorm = 9,
  R8G8B8A8Unorm = 37,
  R8G8B8A8Srgb = 43,
  B8G8R8A8Unorm = 44,
  B8G8R8A8Srgb = 50,
  R16G16B16A16Sfloat = 97,
  R32Uint = 98,
  R32Sfloat = 100,
  R32G32Uint = 101,
  R32G32Sfloat = 103,
  R32G32B32Sfloat = 106,
  R32G32B32A32Uint = 107,
  R32G32B32A32Sfloat = 109,
  D16Unorm = 124,
  D32Sfloat = 126,
  Bc1RgbUnorm = 131,
  Bc1RgbSrgb = 132,
  Bc3Unorm = 137,
  Bc3Srgb = 138,
  Bc4Unorm = 139,
  Bc5Unorm = 141,
  Bc7Unorm = 145,
  Bc7Srgb = 146,
};

// The layout an image is in, which is what the render graph's `Access` vocabulary maps to and
// what a caller tells the graph an imported image arrives in or has to leave in.
enum class ImageLayout : u32 {
  Undefined = 0,
  General = 1,
  ColorAttachment = 2,
  DepthStencilAttachment = 3,
  DepthStencilReadOnly = 4,
  ShaderReadOnly = 5,
  TransferSrc = 6,
  TransferDst = 7,
  Preinitialized = 8,
  Present = 1000001002,
  DepthAttachment = 1000241000,
};

enum class BindPoint : u32 { Graphics = 0, Compute = 1 };
enum class IndexType : u32 { Uint16 = 0, Uint32 = 1 };
enum class LoadOp : u32 { Load = 0, Clear = 1, DontCare = 2 };
enum class CullMode : u32 { None = 0, Front = 1, Back = 2, FrontAndBack = 3 };
enum class CompareOp : u32 {
  Never = 0,
  Less = 1,
  Equal = 2,
  LessOrEqual = 3,
  Greater = 4,
  NotEqual = 5,
  GreaterOrEqual = 6,
  Always = 7,
};
enum class Filter : u32 { Nearest = 0, Linear = 1 };
enum class SamplerMipmapMode : u32 { Nearest = 0, Linear = 1 };
enum class SamplerAddressMode : u32 {
  Repeat = 0,
  MirroredRepeat = 1,
  ClampToEdge = 2,
  ClampToBorder = 3,
};

// What a reflected shader binding is (shader_library.h).
enum class DescriptorType : u32 {
  Sampler = 0,
  CombinedImageSampler = 1,
  SampledImage = 2,
  StorageImage = 3,
  UniformTexelBuffer = 4,
  StorageTexelBuffer = 5,
  UniformBuffer = 6,
  StorageBuffer = 7,
  AccelerationStructure = 1000150000,
  Unknown = 0x7FFFFFFF,
};

// ---- small descriptions -------------------------------------------------------------------------

struct Extent2D {
  u32 width = 0;
  u32 height = 0;
};

struct Offset2D {
  i32 x = 0;
  i32 y = 0;
};

struct Rect2D {
  Offset2D offset;
  Extent2D extent;
};

struct Viewport {
  f32 x = 0.0f;
  f32 y = 0.0f;
  f32 width = 0.0f;
  f32 height = 0.0f;
  f32 min_depth = 0.0f;
  f32 max_depth = 1.0f;
};

// One region of a buffer-to-buffer copy.
struct BufferCopy {
  u64 src_offset = 0;
  u64 dst_offset = 0;
  u64 size = 0;
};

// A colour attachment's clear value, read as whichever the attachment's format is.
union ClearColor {
  f32 float32[4];
  i32 int32[4];
  u32 uint32[4];
};

// The argument records the indirect commands read, as the GPU lays them out.
struct DrawIndirectArgs {
  u32 vertex_count = 0;
  u32 instance_count = 0;
  u32 first_vertex = 0;
  u32 first_instance = 0;
};
struct DrawIndexedIndirectArgs {
  u32 index_count = 0;
  u32 instance_count = 0;
  u32 first_index = 0;
  i32 vertex_offset = 0;
  u32 first_instance = 0;
};
struct DispatchIndirectArgs {
  u32 x = 0;
  u32 y = 0;
  u32 z = 0;
};

static_assert(sizeof(BufferHandle) == 8 && std::is_trivially_copyable_v<BufferHandle>);
static_assert(sizeof(BufferCopy) == 24 && sizeof(ClearColor) == 16);
static_assert(sizeof(DrawIndirectArgs) == 16 && sizeof(DrawIndexedIndirectArgs) == 20 &&
              sizeof(DispatchIndirectArgs) == 12);

}  // namespace engine::gfx
