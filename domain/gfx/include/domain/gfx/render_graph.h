#pragma once

// Render graph v0 (docs/plan/04-renderer.md §4.2): passes declare the resources they read and
// write; the graph orders them, allocates transient resources, derives every synchronization2
// barrier and image layout transition, and records into a command buffer. Nothing in a pass
// body issues a barrier by hand.
//
// clang-format off
//     RenderGraph graph(device);
//     const RgBuffer temp = graph.create_buffer("temp", {bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT});
//     const RgBuffer out = graph.import_buffer("out", out_resource);
//     graph.add_pass("fill", PassKind::Compute,
//         [&](PassBuilder& b) { b.write(temp, Access::ComputeWrite); },
//         [&](VkCommandBuffer cb, RenderGraph& g) { ...dispatch over g.buffer(temp)... });
//     graph.add_pass("scale", PassKind::Compute,
//         [&](PassBuilder& b) { b.read(temp, Access::ComputeRead); b.write(out, Access::ComputeWrite); },
//         [&](VkCommandBuffer cb, RenderGraph& g) { ... });
//     graph.compile(&error);
//     graph.execute(commands);
// clang-format on
//
// v0 executes passes in declaration order and validates that every read has a producer; it
// allocates one physical resource per transient (no aliasing yet) and keeps them until reset().
// Pass bodies are stored in an arena, so they must be trivially destructible: capture by
// reference or pointer, which matches per-frame lifetimes. Later: aliasing, async compute
// scheduling, raster passes through dynamic rendering, and a persistent graph cache.

#include <core/base/macros.h>
#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/memory/arena.h>
#include <domain/gfx/vulkan.h>

#include <string>
#include <type_traits>
#include <utility>

namespace engine::gfx {

struct RgBuffer {
  u32 index = ~u32{0};
  bool valid() const noexcept { return index != ~u32{0}; }
};
struct RgImage {
  u32 index = ~u32{0};
  bool valid() const noexcept { return index != ~u32{0}; }
};

struct RgBufferDesc {
  u64 size = 0;
  VkBufferUsageFlags usage = 0;
  bool host_visible = false;  // persistently mapped; for readback and upload
};

struct RgImageDesc {
  u32 width = 0;
  u32 height = 0;
  VkFormat format = VK_FORMAT_UNDEFINED;
  VkImageUsageFlags usage = 0;
};

enum class PassKind : u8 { Compute, Transfer, Raster };

// How a pass touches a resource. Each maps to a stage, an access mask, and (for images) the
// layout the pass needs; the graph derives barriers from the transitions between them.
enum class Access : u8 {
  ComputeRead,       // storage read in a compute shader (images: GENERAL)
  ComputeWrite,      // storage write in a compute shader (images: GENERAL)
  ComputeReadWrite,  // both
  SampledRead,       // sampled/texture read in any shader (images: SHADER_READ_ONLY_OPTIMAL)
  TransferRead,      // copy source (images: TRANSFER_SRC_OPTIMAL)
  TransferWrite,     // copy or clear destination (images: TRANSFER_DST_OPTIMAL)
  ColorAttachment,   // written through dynamic rendering (images: COLOR_ATTACHMENT_OPTIMAL)
  DepthAttachment,   // depth/stencil attachment (images: DEPTH_ATTACHMENT_OPTIMAL)
};
bool access_writes(Access access) noexcept;

class RenderGraph;

class PassBuilder {
 public:
  void read(RgBuffer buffer, Access access);
  void write(RgBuffer buffer, Access access);
  void read(RgImage image, Access access);
  void write(RgImage image, Access access);
  // Raster passes only: the graph begins and ends dynamic rendering around the body with these
  // attachments, a full-extent viewport and scissor, and the declared load operation.
  void color_attachment(RgImage image, VkAttachmentLoadOp load = VK_ATTACHMENT_LOAD_OP_LOAD,
                        VkClearColorValue clear = {});
  void depth_attachment(RgImage image, VkAttachmentLoadOp load = VK_ATTACHMENT_LOAD_OP_LOAD,
                        float clear_depth = 0.0f);

 private:
  friend class RenderGraph;
  PassBuilder(RenderGraph& graph, u32 pass) noexcept : graph_(&graph), pass_(pass) {}
  RenderGraph* graph_;
  u32 pass_;
};

class RenderGraph {
 public:
  using ExecuteFn = void (*)(VkCommandBuffer commands, RenderGraph& graph, void* context);

  explicit RenderGraph(const Device& device);
  ~RenderGraph();
  ENGINE_NON_COPYABLE(RenderGraph);

  // Releases every transient resource and every pass, ready for a new frame. The GPU must have
  // finished the previous execute() (a FrameContext slot recycle guarantees that).
  void reset() noexcept;

  // --- resources ---
  RgBuffer create_buffer(const char* name, const RgBufferDesc& desc);
  RgImage create_image(const char* name, const RgImageDesc& desc);
  // Imported resources keep their identity across graphs; the graph is told how the resource
  // was last used so its first barrier is correct, and reports the final state afterwards.
  RgBuffer import_buffer(const char* name, const BufferResource& buffer,
                         VkPipelineStageFlags2 last_stage = VK_PIPELINE_STAGE_2_NONE,
                         VkAccessFlags2 last_access = VK_ACCESS_2_NONE);
  RgImage import_image(const char* name, const ImageResource& image,
                       VkImageLayout current_layout = VK_IMAGE_LAYOUT_UNDEFINED,
                       VkPipelineStageFlags2 last_stage = VK_PIPELINE_STAGE_2_NONE,
                       VkAccessFlags2 last_access = VK_ACCESS_2_NONE);
  // Layout an image must be in after execute(), such as VK_IMAGE_LAYOUT_PRESENT_SRC_KHR for a
  // swapchain image; the graph appends the transition after the last pass when needed.
  void set_final_layout(RgImage image, VkImageLayout layout);

  // --- passes ---
  // `setup` runs now with a PassBuilder; `execute` runs during execute(). Both must be trivially
  // destructible (capture by reference or pointer).
  template <class Setup, class Execute>
  void add_pass(const char* name, PassKind kind, Setup&& setup, Execute&& execute) {
    using E = std::decay_t<Execute>;
    static_assert(std::is_trivially_destructible_v<E>,
                  "render graph pass bodies must capture by reference or pointer");
    E* stored = arena_.create<E>(std::forward<Execute>(execute));
    const u32 pass = add_pass_raw(
        name, kind,
        [](VkCommandBuffer commands, RenderGraph& graph, void* context) {
          (*static_cast<E*>(context))(commands, graph);
        },
        stored);
    PassBuilder builder(*this, pass);
    setup(builder);
  }
  u32 add_pass_raw(const char* name, PassKind kind, ExecuteFn body, void* context);

  // --- compile and execute ---
  // Validates (every read has a prior writer or is imported; declared resources exist),
  // allocates transients, and computes barriers. False with `error` on a problem.
  bool compile(std::string* error = nullptr);
  bool compiled() const noexcept { return compiled_; }
  // Records barriers and pass bodies into `commands`, in order.
  void execute(VkCommandBuffer commands);

  // --- lookups for pass bodies ---
  const BufferResource& buffer(RgBuffer handle) const noexcept;
  const ImageResource& image(RgImage handle) const noexcept;
  // The layout an image is in during the currently executing pass (after that pass's barriers).
  VkImageLayout image_layout(RgImage handle) const noexcept;
  // A whole-image view, created at compile for images used as attachments (VK_NULL_HANDLE
  // otherwise); owned by the graph for transients and for imported images alike.
  VkImageView image_view(RgImage handle) const noexcept;
  // Extent of the current raster pass's attachments.
  VkExtent2D render_area() const noexcept { return render_area_; }
  // After execute(): the layout an imported image was left in.
  VkImageLayout final_layout(RgImage handle) const noexcept;

  struct Stats {
    u32 passes = 0;
    u32 buffers = 0;
    u32 images = 0;
    u32 transient_buffers = 0;
    u32 transient_images = 0;
    u32 buffer_barriers = 0;
    u32 image_barriers = 0;
    u32 layout_transitions = 0;
    u32 raster_passes = 0;
  };
  const Stats& stats() const noexcept { return stats_; }

 private:
  friend class PassBuilder;

  struct State {
    VkPipelineStageFlags2 stage = VK_PIPELINE_STAGE_2_NONE;
    VkAccessFlags2 access = VK_ACCESS_2_NONE;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    bool written = false;  // the last access wrote
  };
  struct BufferNode {
    const char* name;
    RgBufferDesc desc;
    BufferResource resource;  // allocated at compile for transients
    bool imported = false;
    State state;  // initial state (imports) then evolving state during compile
    bool has_producer = false;
  };
  struct ImageNode {
    const char* name;
    RgImageDesc desc;
    ImageResource resource;
    VkImageView view = VK_NULL_HANDLE;  // created at compile when used as an attachment
    bool imported = false;
    State state;
    VkImageLayout final_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageLayout requested_final = VK_IMAGE_LAYOUT_UNDEFINED;
    bool has_producer = false;
  };
  struct Use {
    bool is_image;
    u32 index;
    Access access;
    bool write;
  };
  struct Attachment {
    u32 image;
    VkAttachmentLoadOp load;
    VkClearValue clear;
    bool depth;
  };
  struct Pass {
    const char* name;
    PassKind kind;
    ExecuteFn execute;
    void* context;
    u32 first_use = 0;
    u32 use_count = 0;
    u32 first_attachment = 0;
    u32 attachment_count = 0;
    u32 first_buffer_barrier = 0;
    u32 buffer_barrier_count = 0;
    u32 first_image_barrier = 0;
    u32 image_barrier_count = 0;
    u32 first_layout = 0;  // into image_layouts_: layout per image use, in use order
  };

  void add_use(u32 pass, bool is_image, u32 index, Access access, bool write);
  void add_attachment(u32 pass, u32 image, VkAttachmentLoadOp load, VkClearValue clear, bool depth);
  bool allocate_transients(std::string* error);
  bool create_attachment_views(std::string* error);
  void begin_rendering(VkCommandBuffer commands, const Pass& pass);
  void compute_barriers();

  const Device* device_;
  mem::Arena arena_;
  Vector<BufferNode> buffers_;
  Vector<ImageNode> images_;
  Vector<Use> uses_;
  Vector<Attachment> attachments_;
  Vector<Pass> passes_;
  Vector<VkMemoryBarrier2> buffer_barriers_;
  Vector<VkImageMemoryBarrier2> image_barriers_;
  Vector<VkImageMemoryBarrier2> final_barriers_;  // after the last pass (set_final_layout)
  Vector<VkImageLayout> pass_image_layouts_;      // per (pass, image) layout during that pass
  u32 current_pass_ = ~u32{0};
  VkExtent2D render_area_{};
  bool compiled_ = false;
  Stats stats_;
};

}  // namespace engine::gfx
