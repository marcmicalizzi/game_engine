// The proof behind backend/vulkan/vulkan.h's casts: every enumerator of the engine's vocabulary
// (rhi.h) carries the number of the Vulkan value it stands for, and every small description has
// Vulkan's layout. A mismatch is a compile error here rather than a wrong flag at run time. A
// second backend has a file like this one of its own, with a table where this one has a cast.
#include <domain/gfx/backend/vulkan/vulkan.h>
#include <domain/gfx/rhi.h>

#include <cstddef>

namespace engine::gfx {
namespace {

template <class E>
constexpr u64 bits(E e) noexcept {
  return static_cast<u64>(e);
}

static_assert(k_whole_size == VK_WHOLE_SIZE);

// ---- formats ----
static_assert(bits(Format::Undefined) == VK_FORMAT_UNDEFINED);
static_assert(bits(Format::R8Unorm) == VK_FORMAT_R8_UNORM);
static_assert(bits(Format::R8G8B8A8Unorm) == VK_FORMAT_R8G8B8A8_UNORM);
static_assert(bits(Format::R8G8B8A8Srgb) == VK_FORMAT_R8G8B8A8_SRGB);
static_assert(bits(Format::B8G8R8A8Unorm) == VK_FORMAT_B8G8R8A8_UNORM);
static_assert(bits(Format::B8G8R8A8Srgb) == VK_FORMAT_B8G8R8A8_SRGB);
static_assert(bits(Format::A2R10G10B10Unorm) == VK_FORMAT_A2R10G10B10_UNORM_PACK32);
static_assert(bits(Format::A2B10G10R10Unorm) == VK_FORMAT_A2B10G10R10_UNORM_PACK32);
static_assert(bits(Format::R16G16B16A16Sfloat) == VK_FORMAT_R16G16B16A16_SFLOAT);
static_assert(bits(Format::R32Uint) == VK_FORMAT_R32_UINT);
static_assert(bits(Format::R32Sfloat) == VK_FORMAT_R32_SFLOAT);
static_assert(bits(Format::R32G32Uint) == VK_FORMAT_R32G32_UINT);
static_assert(bits(Format::R32G32Sfloat) == VK_FORMAT_R32G32_SFLOAT);
static_assert(bits(Format::R32G32B32Sfloat) == VK_FORMAT_R32G32B32_SFLOAT);
static_assert(bits(Format::R32G32B32A32Uint) == VK_FORMAT_R32G32B32A32_UINT);
static_assert(bits(Format::R32G32B32A32Sfloat) == VK_FORMAT_R32G32B32A32_SFLOAT);
static_assert(bits(Format::D16Unorm) == VK_FORMAT_D16_UNORM);
static_assert(bits(Format::D32Sfloat) == VK_FORMAT_D32_SFLOAT);
static_assert(bits(Format::Bc1RgbUnorm) == VK_FORMAT_BC1_RGB_UNORM_BLOCK);
static_assert(bits(Format::Bc1RgbSrgb) == VK_FORMAT_BC1_RGB_SRGB_BLOCK);
static_assert(bits(Format::Bc3Unorm) == VK_FORMAT_BC3_UNORM_BLOCK);
static_assert(bits(Format::Bc3Srgb) == VK_FORMAT_BC3_SRGB_BLOCK);
static_assert(bits(Format::Bc4Unorm) == VK_FORMAT_BC4_UNORM_BLOCK);
static_assert(bits(Format::Bc5Unorm) == VK_FORMAT_BC5_UNORM_BLOCK);
static_assert(bits(Format::Bc7Unorm) == VK_FORMAT_BC7_UNORM_BLOCK);
static_assert(bits(Format::Bc7Srgb) == VK_FORMAT_BC7_SRGB_BLOCK);

// ---- surface colour spaces (VK_KHR_surface, VK_EXT_swapchain_colorspace, E39) ----
static_assert(bits(ColorSpace::SrgbNonlinear) == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR);
static_assert(bits(ColorSpace::DisplayP3Nonlinear) == VK_COLOR_SPACE_DISPLAY_P3_NONLINEAR_EXT);
static_assert(bits(ColorSpace::ExtendedSrgbLinear) == VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT);
static_assert(bits(ColorSpace::DisplayP3Linear) == VK_COLOR_SPACE_DISPLAY_P3_LINEAR_EXT);
static_assert(bits(ColorSpace::DciP3Nonlinear) == VK_COLOR_SPACE_DCI_P3_NONLINEAR_EXT);
static_assert(bits(ColorSpace::Bt709Linear) == VK_COLOR_SPACE_BT709_LINEAR_EXT);
static_assert(bits(ColorSpace::Bt709Nonlinear) == VK_COLOR_SPACE_BT709_NONLINEAR_EXT);
static_assert(bits(ColorSpace::Bt2020Linear) == VK_COLOR_SPACE_BT2020_LINEAR_EXT);
static_assert(bits(ColorSpace::Hdr10St2084) == VK_COLOR_SPACE_HDR10_ST2084_EXT);
static_assert(bits(ColorSpace::Hdr10Hlg) == VK_COLOR_SPACE_HDR10_HLG_EXT);
static_assert(bits(ColorSpace::AdobeRgbLinear) == VK_COLOR_SPACE_ADOBERGB_LINEAR_EXT);
static_assert(bits(ColorSpace::AdobeRgbNonlinear) == VK_COLOR_SPACE_ADOBERGB_NONLINEAR_EXT);
static_assert(bits(ColorSpace::PassThrough) == VK_COLOR_SPACE_PASS_THROUGH_EXT);
static_assert(bits(ColorSpace::ExtendedSrgbNonlinear) ==
              VK_COLOR_SPACE_EXTENDED_SRGB_NONLINEAR_EXT);
static_assert(bits(ColorSpace::DisplayNativeAmd) == VK_COLOR_SPACE_DISPLAY_NATIVE_AMD);

// ---- image layouts ----
static_assert(bits(ImageLayout::Undefined) == VK_IMAGE_LAYOUT_UNDEFINED);
static_assert(bits(ImageLayout::General) == VK_IMAGE_LAYOUT_GENERAL);
static_assert(bits(ImageLayout::ColorAttachment) == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
static_assert(bits(ImageLayout::DepthStencilAttachment) ==
              VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
static_assert(bits(ImageLayout::DepthStencilReadOnly) ==
              VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
static_assert(bits(ImageLayout::ShaderReadOnly) == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
static_assert(bits(ImageLayout::TransferSrc) == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
static_assert(bits(ImageLayout::TransferDst) == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
static_assert(bits(ImageLayout::Preinitialized) == VK_IMAGE_LAYOUT_PREINITIALIZED);
static_assert(bits(ImageLayout::Present) == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
static_assert(bits(ImageLayout::DepthAttachment) == VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);

// ---- buffer and image usage ----
static_assert(bits(BufferUsage::TransferSrc) == VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
static_assert(bits(BufferUsage::TransferDst) == VK_BUFFER_USAGE_TRANSFER_DST_BIT);
static_assert(bits(BufferUsage::Uniform) == VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
static_assert(bits(BufferUsage::Storage) == VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
static_assert(bits(BufferUsage::Index) == VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
static_assert(bits(BufferUsage::Vertex) == VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
static_assert(bits(BufferUsage::Indirect) == VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);
static_assert(bits(BufferUsage::ShaderDeviceAddress) == VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
static_assert(bits(BufferUsage::AccelerationBuildInput) ==
              VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR);
static_assert(bits(BufferUsage::AccelerationStorage) ==
              VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR);
static_assert(bits(ImageUsage::TransferSrc) == VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
static_assert(bits(ImageUsage::TransferDst) == VK_IMAGE_USAGE_TRANSFER_DST_BIT);
static_assert(bits(ImageUsage::Sampled) == VK_IMAGE_USAGE_SAMPLED_BIT);
static_assert(bits(ImageUsage::Storage) == VK_IMAGE_USAGE_STORAGE_BIT);
static_assert(bits(ImageUsage::ColorAttachment) == VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
static_assert(bits(ImageUsage::DepthStencilAttachment) ==
              VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);

// ---- synchronization2 stages and accesses ----
static_assert(bits(PipelineStage::None) == VK_PIPELINE_STAGE_2_NONE);
static_assert(bits(PipelineStage::TopOfPipe) == VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT);
static_assert(bits(PipelineStage::DrawIndirect) == VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT);
static_assert(bits(PipelineStage::VertexInput) == VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT);
static_assert(bits(PipelineStage::VertexShader) == VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT);
static_assert(bits(PipelineStage::FragmentShader) == VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);
static_assert(bits(PipelineStage::EarlyFragmentTests) ==
              VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT);
static_assert(bits(PipelineStage::LateFragmentTests) ==
              VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT);
static_assert(bits(PipelineStage::ColorAttachmentOutput) ==
              VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT);
static_assert(bits(PipelineStage::ComputeShader) == VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
static_assert(bits(PipelineStage::AllTransfer) == VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT);
static_assert(bits(PipelineStage::BottomOfPipe) == VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT);
static_assert(bits(PipelineStage::Host) == VK_PIPELINE_STAGE_2_HOST_BIT);
static_assert(bits(PipelineStage::AllGraphics) == VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT);
static_assert(bits(PipelineStage::AllCommands) == VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
static_assert(bits(PipelineStage::TaskShader) == VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT);
static_assert(bits(PipelineStage::MeshShader) == VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT);
static_assert(bits(PipelineStage::AccelerationBuild) ==
              VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR);
static_assert(bits(PipelineStage::Copy) == VK_PIPELINE_STAGE_2_COPY_BIT);
static_assert(bits(PipelineStage::Clear) == VK_PIPELINE_STAGE_2_CLEAR_BIT);
static_assert(bits(PipelineStage::IndexInput) == VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT);
static_assert(bits(MemoryAccess::None) == VK_ACCESS_2_NONE);
static_assert(bits(MemoryAccess::IndirectCommandRead) == VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
static_assert(bits(MemoryAccess::IndexRead) == VK_ACCESS_2_INDEX_READ_BIT);
static_assert(bits(MemoryAccess::ShaderRead) == VK_ACCESS_2_SHADER_READ_BIT);
static_assert(bits(MemoryAccess::ShaderWrite) == VK_ACCESS_2_SHADER_WRITE_BIT);
static_assert(bits(MemoryAccess::ColorAttachmentRead) == VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT);
static_assert(bits(MemoryAccess::ColorAttachmentWrite) == VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
static_assert(bits(MemoryAccess::DepthStencilAttachmentRead) ==
              VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT);
static_assert(bits(MemoryAccess::DepthStencilAttachmentWrite) ==
              VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
static_assert(bits(MemoryAccess::TransferRead) == VK_ACCESS_2_TRANSFER_READ_BIT);
static_assert(bits(MemoryAccess::TransferWrite) == VK_ACCESS_2_TRANSFER_WRITE_BIT);
static_assert(bits(MemoryAccess::HostRead) == VK_ACCESS_2_HOST_READ_BIT);
static_assert(bits(MemoryAccess::HostWrite) == VK_ACCESS_2_HOST_WRITE_BIT);
static_assert(bits(MemoryAccess::MemoryRead) == VK_ACCESS_2_MEMORY_READ_BIT);
static_assert(bits(MemoryAccess::MemoryWrite) == VK_ACCESS_2_MEMORY_WRITE_BIT);
static_assert(bits(MemoryAccess::AccelerationStructureRead) ==
              VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR);
static_assert(bits(MemoryAccess::AccelerationStructureWrite) ==
              VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR);
static_assert(bits(MemoryAccess::ShaderSampledRead) == VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
static_assert(bits(MemoryAccess::ShaderStorageRead) == VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
static_assert(bits(MemoryAccess::ShaderStorageWrite) == VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

// ---- shader stages and descriptor types ----
static_assert(bits(ShaderStage::Vertex) == VK_SHADER_STAGE_VERTEX_BIT);
static_assert(bits(ShaderStage::TessellationControl) == VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT);
static_assert(bits(ShaderStage::TessellationEvaluation) ==
              VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT);
static_assert(bits(ShaderStage::Geometry) == VK_SHADER_STAGE_GEOMETRY_BIT);
static_assert(bits(ShaderStage::Fragment) == VK_SHADER_STAGE_FRAGMENT_BIT);
static_assert(bits(ShaderStage::Compute) == VK_SHADER_STAGE_COMPUTE_BIT);
static_assert(bits(ShaderStage::Task) == VK_SHADER_STAGE_TASK_BIT_EXT);
static_assert(bits(ShaderStage::Mesh) == VK_SHADER_STAGE_MESH_BIT_EXT);
static_assert(bits(ShaderStage::RayGen) == VK_SHADER_STAGE_RAYGEN_BIT_KHR);
static_assert(bits(ShaderStage::AnyHit) == VK_SHADER_STAGE_ANY_HIT_BIT_KHR);
static_assert(bits(ShaderStage::ClosestHit) == VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR);
static_assert(bits(ShaderStage::Miss) == VK_SHADER_STAGE_MISS_BIT_KHR);
static_assert(bits(ShaderStage::Intersection) == VK_SHADER_STAGE_INTERSECTION_BIT_KHR);
static_assert(bits(ShaderStage::Callable) == VK_SHADER_STAGE_CALLABLE_BIT_KHR);
static_assert(bits(ShaderStage::AllGraphics) == VK_SHADER_STAGE_ALL_GRAPHICS);
static_assert(bits(ShaderStage::All) == VK_SHADER_STAGE_ALL);
static_assert(bits(DescriptorType::Sampler) == VK_DESCRIPTOR_TYPE_SAMPLER);
static_assert(bits(DescriptorType::CombinedImageSampler) ==
              VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
static_assert(bits(DescriptorType::SampledImage) == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
static_assert(bits(DescriptorType::StorageImage) == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
static_assert(bits(DescriptorType::UniformTexelBuffer) == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER);
static_assert(bits(DescriptorType::StorageTexelBuffer) == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER);
static_assert(bits(DescriptorType::UniformBuffer) == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
static_assert(bits(DescriptorType::StorageBuffer) == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
static_assert(bits(DescriptorType::AccelerationStructure) ==
              VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR);
static_assert(bits(DescriptorType::Unknown) == VK_DESCRIPTOR_TYPE_MAX_ENUM);

// ---- everything else ----
static_assert(bits(BindPoint::Graphics) == VK_PIPELINE_BIND_POINT_GRAPHICS);
static_assert(bits(BindPoint::Compute) == VK_PIPELINE_BIND_POINT_COMPUTE);
static_assert(bits(IndexType::Uint16) == VK_INDEX_TYPE_UINT16);
static_assert(bits(IndexType::Uint32) == VK_INDEX_TYPE_UINT32);
static_assert(bits(LoadOp::Load) == VK_ATTACHMENT_LOAD_OP_LOAD);
static_assert(bits(LoadOp::Clear) == VK_ATTACHMENT_LOAD_OP_CLEAR);
static_assert(bits(LoadOp::DontCare) == VK_ATTACHMENT_LOAD_OP_DONT_CARE);
static_assert(bits(CullMode::None) == VK_CULL_MODE_NONE);
static_assert(bits(CullMode::Front) == VK_CULL_MODE_FRONT_BIT);
static_assert(bits(CullMode::Back) == VK_CULL_MODE_BACK_BIT);
static_assert(bits(CullMode::FrontAndBack) == VK_CULL_MODE_FRONT_AND_BACK);
static_assert(bits(CompareOp::Never) == VK_COMPARE_OP_NEVER);
static_assert(bits(CompareOp::Less) == VK_COMPARE_OP_LESS);
static_assert(bits(CompareOp::Equal) == VK_COMPARE_OP_EQUAL);
static_assert(bits(CompareOp::LessOrEqual) == VK_COMPARE_OP_LESS_OR_EQUAL);
static_assert(bits(CompareOp::Greater) == VK_COMPARE_OP_GREATER);
static_assert(bits(CompareOp::NotEqual) == VK_COMPARE_OP_NOT_EQUAL);
static_assert(bits(CompareOp::GreaterOrEqual) == VK_COMPARE_OP_GREATER_OR_EQUAL);
static_assert(bits(CompareOp::Always) == VK_COMPARE_OP_ALWAYS);
static_assert(bits(Filter::Nearest) == VK_FILTER_NEAREST);
static_assert(bits(Filter::Linear) == VK_FILTER_LINEAR);
static_assert(bits(SamplerMipmapMode::Nearest) == VK_SAMPLER_MIPMAP_MODE_NEAREST);
static_assert(bits(SamplerMipmapMode::Linear) == VK_SAMPLER_MIPMAP_MODE_LINEAR);
static_assert(bits(SamplerAddressMode::Repeat) == VK_SAMPLER_ADDRESS_MODE_REPEAT);
static_assert(bits(SamplerAddressMode::MirroredRepeat) == VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT);
static_assert(bits(SamplerAddressMode::ClampToEdge) == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
static_assert(bits(SamplerAddressMode::ClampToBorder) == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER);
static_assert(bits(AccelerationBuildFlags::AllowUpdate) ==
              VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR);
static_assert(bits(AccelerationBuildFlags::AllowCompaction) ==
              VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR);
static_assert(bits(AccelerationBuildFlags::PreferFastTrace) ==
              VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR);
static_assert(bits(AccelerationBuildFlags::PreferFastBuild) ==
              VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR);
static_assert(bits(AccelerationBuildFlags::LowMemory) ==
              VK_BUILD_ACCELERATION_STRUCTURE_LOW_MEMORY_BIT_KHR);
static_assert(bits(GeometryInstanceFlags::TriangleFacingCullDisable) ==
              VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR);
static_assert(bits(GeometryInstanceFlags::TriangleFlipFacing) ==
              VK_GEOMETRY_INSTANCE_TRIANGLE_FLIP_FACING_BIT_KHR);
static_assert(bits(GeometryInstanceFlags::ForceOpaque) ==
              VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR);
static_assert(bits(GeometryInstanceFlags::ForceNoOpaque) ==
              VK_GEOMETRY_INSTANCE_FORCE_NO_OPAQUE_BIT_KHR);

// ---- layouts ----
// BufferCopy is handed to vkCmdCopyBuffer as it is (vk::native(const BufferCopy*)), so it has to
// be VkBufferCopy member for member; the indirect argument records are what the GPU reads.
static_assert(sizeof(BufferCopy) == sizeof(VkBufferCopy) &&
              alignof(BufferCopy) == alignof(VkBufferCopy));
static_assert(offsetof(BufferCopy, src_offset) == offsetof(VkBufferCopy, srcOffset) &&
              offsetof(BufferCopy, dst_offset) == offsetof(VkBufferCopy, dstOffset) &&
              offsetof(BufferCopy, size) == offsetof(VkBufferCopy, size));
static_assert(sizeof(ClearColor) == sizeof(VkClearColorValue));
static_assert(sizeof(DrawIndirectArgs) == sizeof(VkDrawIndirectCommand));
static_assert(sizeof(DrawIndexedIndirectArgs) == sizeof(VkDrawIndexedIndirectCommand));
static_assert(sizeof(DispatchIndirectArgs) == sizeof(VkDispatchIndirectCommand));
static_assert(offsetof(DrawIndexedIndirectArgs, vertex_offset) ==
              offsetof(VkDrawIndexedIndirectCommand, vertexOffset));
// A handle is a Vulkan handle's bits.
static_assert(sizeof(BufferHandle) == sizeof(VkBuffer) &&
              sizeof(CommandListHandle) == sizeof(VkCommandBuffer) &&
              sizeof(AllocationHandle) == sizeof(VmaAllocation));

}  // namespace
}  // namespace engine::gfx
