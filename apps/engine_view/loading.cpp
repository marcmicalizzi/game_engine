#include "loading.h"

#include <core/json/json.h>
#include <domain/gfx/backend/vulkan/swapchain.h>
#include <domain/gfx/device.h>
#include <foundation/io/vfs.h>
#include <systems/renderer/scene_renderer.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <imgui.h>

namespace engine::view {

// ---- textures
// ------------------------------------------------------------------------------------

u32 RendererTextures::add_texture(u32 width, u32 height, const void* rgba, std::string* error) {
  return renderer_.overlay().add_texture(width, height, rgba, error);
}

void RendererTextures::remove_texture(u32 texture) { renderer_.remove_overlay_texture(texture); }

// ---- the default overlay
// -------------------------------------------------------------------------

namespace {
constexpr f32 k_imgui_default_px = 13.0f;  // ImGui's own font's size, which its style is sized for

// The HUD's rule (game_hud.h `hud_font_px`): 18 px on a 1080-pixel-high picture at 100 %, scaled
// with the height and the display, within 10..64.
f32 loading_font_px(u32 height, f32 display_scale) noexcept {
  const f32 scale = display_scale > 0.0f ? display_scale : 1.0f;
  const f32 px = std::round(18.0f * static_cast<f32>(height) / 1080.0f * scale);
  return std::clamp(px, 10.0f, 64.0f);
}
}  // namespace

LoadingOverlay::~LoadingOverlay() { destroy(); }

bool LoadingOverlay::create() {
  destroy();
  IMGUI_CHECKVERSION();
  ImGuiContext* previous = ImGui::GetCurrentContext();
  context_ = ImGui::CreateContext();
  ImGui::SetCurrentContext(context_);
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.LogFilename = nullptr;
  io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
  io.BackendRendererName = "engine overlay";
  io.BackendPlatformName = "engine-view loading";
  font_px_ = 0.0f;
  ImGui::SetCurrentContext(previous);
  return true;
}

void LoadingOverlay::forget_textures() noexcept {
  if (context_ == nullptr) return;
  ImGuiContext* previous = ImGui::GetCurrentContext();
  ImGui::SetCurrentContext(context_);
  for (ImTextureData* tex : ImGui::GetPlatformIO().Textures) {
    if (tex->Status == ImTextureStatus_Destroyed) continue;
    tex->SetTexID(ImTextureID_Invalid);
    tex->SetStatus(ImTextureStatus_WantCreate);
  }
  ImGui::SetCurrentContext(previous);
}

void LoadingOverlay::destroy() noexcept {
  if (context_ == nullptr) return;
  ImGuiContext* previous = ImGui::GetCurrentContext();
  ImGui::DestroyContext(context_);
  context_ = nullptr;
  if (previous != context_) ImGui::SetCurrentContext(previous);
}

bool LoadingOverlay::build(const profiling::LoadProgress& progress, u32 width, u32 height,
                           f32 display_scale, OverlayTextures& textures,
                           renderer::OverlayDrawData& out, std::string& error) {
  out = renderer::OverlayDrawData{};
  if (context_ == nullptr || width == 0 || height == 0) return true;
  ImGuiContext* previous = ImGui::GetCurrentContext();
  ImGui::SetCurrentContext(context_);
  struct Restore {
    ImGuiContext* context;
    ~Restore() { ImGui::SetCurrentContext(context); }
  } restore{previous};

  const f32 px = loading_font_px(height, display_scale);
  if (px != font_px_) {
    font_px_ = px;
    ImGuiStyle& style = ImGui::GetStyle();
    style = ImGuiStyle();
    ImGui::StyleColorsDark();
    style.WindowRounding = 4.0f;
    style.WindowBorderSize = 0.0f;
    style.FrameRounding = 2.0f;
    style.ScaleAllSizes(px / k_imgui_default_px);
    style.FontSizeBase = px;
  }
  ImGuiIO& io = ImGui::GetIO();
  io.DisplaySize = ImVec2(static_cast<f32>(width), static_cast<f32>(height));
  io.DeltaTime = 1.0f / 30.0f;
  ImGui::NewFrame();

  // The line: the stage, what it is doing inside, and the fraction; the bar under it.
  const f32 percent = std::clamp(progress.fraction, 0.0f, 1.0f) * 100.0f;
  if (progress.detail[0] != '\0') {
    std::snprintf(line_, sizeof(line_), "%s \xC2\xB7 %s",
                  progress.stage[0] != '\0' ? progress.stage : "loading", progress.detail);
  } else {
    std::snprintf(line_, sizeof(line_), "%s",
                  progress.stage[0] != '\0' ? progress.stage : "loading");
  }
  const f32 bar_width = std::round(static_cast<f32>(width) * 0.4f);
  const ImGuiWindowFlags flags =
      ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
      ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
      ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove;
  ImGui::SetNextWindowPos(
      ImVec2(static_cast<f32>(width) * 0.5f, std::round(static_cast<f32>(height) * 0.8f)),
      ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowBgAlpha(0.55f);
  if (ImGui::Begin("loading", nullptr, flags)) {
    ImGui::PushTextWrapPos(bar_width);
    ImGui::TextUnformatted(line_);
    ImGui::PopTextWrapPos();
    char overlay_text[16];
    std::snprintf(overlay_text, sizeof(overlay_text), "%.0f%%", static_cast<f64>(percent));
    ImGui::ProgressBar(percent / 100.0f, ImVec2(bar_width, 0.0f), overlay_text);
  }
  ImGui::End();
  ImGui::Render();
  ++builds_;

  ImDrawData* data = ImGui::GetDrawData();
  vertices_.clear();
  indices_.clear();
  commands_.clear();
  if (data == nullptr) return true;
  // Textures first (the atlas, made once and re-baked when a size is first drawn).
  if (data->Textures != nullptr) {
    for (ImTextureData* tex : *data->Textures) {
      if (tex->Status == ImTextureStatus_WantCreate || tex->Status == ImTextureStatus_WantUpdates) {
        if (tex->TexID != ImTextureID_Invalid)
          textures.remove_texture(static_cast<u32>(tex->TexID - 1));
        if (tex->Format != ImTextureFormat_RGBA32) {
          error = "ImGui asked for a texture format the overlay does not take";
          return false;
        }
        const u32 id = textures.add_texture(
            static_cast<u32>(tex->Width), static_cast<u32>(tex->Height), tex->GetPixels(), &error);
        if (id == renderer::OverlayPass::k_no_texture) return false;
        tex->SetTexID(static_cast<ImTextureID>(id) + 1);
        tex->SetStatus(ImTextureStatus_OK);
      } else if (tex->Status == ImTextureStatus_WantDestroy && tex->UnusedFrames > 0) {
        if (tex->TexID != ImTextureID_Invalid)
          textures.remove_texture(static_cast<u32>(tex->TexID - 1));
        tex->SetTexID(ImTextureID_Invalid);
        tex->SetStatus(ImTextureStatus_Destroyed);
      }
    }
  }
  for (const ImDrawList* list : data->CmdLists) {
    const u32 vertex_base = vertices_.size();
    const u32 index_base = indices_.size();
    const u32 vertex_count = static_cast<u32>(list->VtxBuffer.Size);
    const u32 index_count = static_cast<u32>(list->IdxBuffer.Size);
    vertices_.resize(vertex_base + vertex_count);
    indices_.resize(index_base + index_count);
    if (vertex_count > 0)
      std::memcpy(static_cast<void*>(vertices_.data() + vertex_base), list->VtxBuffer.Data,
                  vertex_count * sizeof(ImDrawVert));
    if (index_count > 0)
      std::memcpy(indices_.data() + index_base, list->IdxBuffer.Data,
                  index_count * sizeof(ImDrawIdx));
    for (const ImDrawCmd& cmd : list->CmdBuffer) {
      if (cmd.UserCallback != nullptr || cmd.ElemCount == 0) continue;
      const ImTextureID tex = cmd.GetTexID();
      if (tex == ImTextureID_Invalid) continue;
      renderer::OverlayCommand c;
      c.clip_x0 = (cmd.ClipRect.x - data->DisplayPos.x) * data->FramebufferScale.x;
      c.clip_y0 = (cmd.ClipRect.y - data->DisplayPos.y) * data->FramebufferScale.y;
      c.clip_x1 = (cmd.ClipRect.z - data->DisplayPos.x) * data->FramebufferScale.x;
      c.clip_y1 = (cmd.ClipRect.w - data->DisplayPos.y) * data->FramebufferScale.y;
      c.texture = static_cast<u32>(tex - 1);
      c.first_index = index_base + cmd.IdxOffset;
      c.index_count = cmd.ElemCount;
      c.vertex_offset = vertex_base + cmd.VtxOffset;
      commands_.push_back(c);
    }
  }
  out.vertices = std::span<const renderer::OverlayVertex>(vertices_.data(), vertices_.size());
  out.indices = std::span<const u32>(indices_.data(), indices_.size());
  out.commands = std::span<const renderer::OverlayCommand>(commands_.data(), commands_.size());
  return true;
}

// ---- the presenter
// -------------------------------------------------------------------------------

LoadingPresenter::~LoadingPresenter() { destroy(); }

bool LoadingPresenter::create(const gfx::Device& device, gfx::Swapchain& swapchain,
                              std::string* error) {
  destroy();
  device_ = &device;
  swapchain_ = &swapchain;
  // A handful of slots: the font atlas, a game's HUD images, one sampler.
  gfx::BindlessConfig config;
  config.sampled_images = 64;
  config.storage_images = 4;
  config.samplers = 4;
  config.acceleration_structures = 0;
  const auto give_up = [&](const char* what) {
    if (error != nullptr) *error = std::string(what) + (error->empty() ? "" : ": " + *error);
    destroy();
    return false;
  };
  if (!bindless_.create(device, config, error)) return give_up("the loading frame's bindless set");
  if (!shaders_.create(&device, error)) return give_up("the loading frame's shaders");
  renderer::add_overlay_shader(shaders_);
  if (!overlay_.create(device, bindless_, shaders_, swapchain.format(), 2, error))
    return give_up("the loading frame's overlay");
  if (!frames_.create(device, 2, error)) return give_up("the loading frame's frames");
  graph_ = std::make_unique<gfx::RenderGraph>(device);
  const gfx::ColorSpace space = swapchain.surface_color_space();
  encoding_ = space == gfx::ColorSpace::Hdr10St2084          ? renderer::OverlayEncoding::Pq
              : space == gfx::ColorSpace::ExtendedSrgbLinear ? renderer::OverlayEncoding::ScRgb
                                                             : renderer::OverlayEncoding::Encoded;
  return true;
}

void LoadingPresenter::destroy() noexcept {
  if (device_ == nullptr) return;
  if (frames_.valid()) frames_.wait_idle();
  graph_.reset();
  overlay_.destroy();
  frames_.destroy();
  shaders_.destroy();
  bindless_.destroy();
  device_ = nullptr;
  swapchain_ = nullptr;
}

u32 LoadingPresenter::width() const noexcept {
  return swapchain_ != nullptr ? swapchain_->extent().width : 0;
}
u32 LoadingPresenter::height() const noexcept {
  return swapchain_ != nullptr ? swapchain_->extent().height : 0;
}

u32 LoadingPresenter::add_texture(u32 width, u32 height, const void* rgba, std::string* error) {
  return overlay_.add_texture(width, height, rgba, error);
}

void LoadingPresenter::remove_texture(u32 texture) {
  if (frames_.valid()) frames_.wait_idle();
  overlay_.remove_texture(texture);
}

bool LoadingPresenter::present(const renderer::OverlayDrawData* overlay, u32 width, u32 height,
                               std::string* error) {
  if (device_ == nullptr) return false;
  gfx::CommandList commands = frames_.begin_frame();
  u32 image_index = 0;
  const gfx::PresentStatus acquired = swapchain_->acquire(frames_.acquire_semaphore(), image_index);
  if (acquired != gfx::PresentStatus::Ok) {
    frames_.end_frame();
    if (acquired == gfx::PresentStatus::Error) {
      if (error != nullptr) *error = "the loading frame could not acquire a swapchain image";
      return false;
    }
    return width == 0 || height == 0 || swapchain_->resize(width, height, error);
  }
  const u32 slot = frames_.slot();
  const bool drawn = overlay != nullptr && !overlay->commands.empty();
  if (drawn && !overlay_.stage(slot, *overlay, frames_, error)) {
    frames_.end_frame();
    return false;
  }
  gfx::RenderGraph& graph = *graph_;
  graph.reset();
  const gfx::RgImage color = graph.import_image("swapchain", swapchain_->image(image_index));
  gfx::ClearColor clear{};
  // A dark slate: no picture yet, and nothing a viewer reads as a fault.
  clear.float32[0] = encoding_ == renderer::OverlayEncoding::Encoded ? 0.055f : 0.004f;
  clear.float32[1] = encoding_ == renderer::OverlayEncoding::Encoded ? 0.06f : 0.005f;
  clear.float32[2] = encoding_ == renderer::OverlayEncoding::Encoded ? 0.07f : 0.006f;
  clear.float32[3] = 1.0f;
  const u32 w = swapchain_->extent().width;
  const u32 h = swapchain_->extent().height;
  const renderer::OverlayEncoding encoding = encoding_;
  graph.add_pass(
      "loading", gfx::PassKind::Raster,
      [&](gfx::PassBuilder& b) { b.color_attachment(color, gfx::LoadOp::Clear, clear); },
      [&, slot, w, h, encoding, drawn](gfx::CommandList cb, gfx::RenderGraph&) {
        if (!drawn) return;
        bindless_.bind(cb, gfx::BindPoint::Graphics);
        overlay_.record(cb, slot, *overlay, w, h, encoding,
                        encoding == renderer::OverlayEncoding::Encoded ? 0.0f : 203.0f);
      });
  graph.set_final_layout(color, gfx::ImageLayout::Present);
  if (!graph.compile(error)) {
    frames_.end_frame();
    return false;
  }
  graph.execute(commands);
  gfx::FrameContext::PresentSync sync;
  sync.wait = frames_.acquire_semaphore();
  sync.signal = swapchain_->render_finished(image_index);
  frames_.end_frame(sync);
  const gfx::PresentStatus shown =
      swapchain_->present(image_index, swapchain_->render_finished(image_index));
  if (shown == gfx::PresentStatus::Error) {
    if (error != nullptr) *error = "the loading frame could not be presented";
    return false;
  }
  ++presented_;
  if (shown == gfx::PresentStatus::OutOfDate && width != 0 && height != 0)
    return swapchain_->resize(width, height, error);
  return true;
}

// ---- the plan
// ------------------------------------------------------------------------------------

std::string progress_plan_path(const std::string& ddc, u64 key) {
  if (ddc.empty()) return {};
  char name[32];
  std::snprintf(name, sizeof(name), "%016llx.json", static_cast<unsigned long long>(key));
  return ddc + "/startup/" + name;
}

profiling::ProgressPlan load_progress_plan(const std::string& path,
                                           std::span<const char* const> defaults,
                                           std::span<const char* const> milestones,
                                           std::string* why) {
  profiling::ProgressPlan plan;
  const auto fallback = [&](const char* reason) {
    plan = profiling::ProgressPlan{};
    for (const char* stage : defaults)
      (void)plan.add_stage(stage, 1.0);
    for (const char* m : milestones)
      (void)plan.add_milestone(m);
    if (why != nullptr) *why = reason;
    return plan;
  };
  if (path.empty()) return fallback("no derived-data root");
  std::string text;
  if (io::read_file(path, text) != io::Status::Ok) return fallback("no previous run");
  JsonValue root;
  if (!parse_json(text, root).ok || !root.is_object()) return fallback("unreadable");
  const JsonValue* stages = root.find("stages");
  if (stages == nullptr || !stages->is_array()) return fallback("no stages");
  for (usize i = 0; i < stages->size(); ++i) {
    const JsonValue& s = (*stages)[i];
    const JsonValue* name = s.find("name");
    const JsonValue* ms = s.find("ms");
    std::string_view n;
    f64 w = 0.0;
    if (name == nullptr || ms == nullptr || !name->get_string(n) || !ms->get_f64(w))
      return fallback("a stage without its name or time");
    (void)plan.add_stage(n, w);
  }
  if (plan.stage_count == 0) return fallback("no stages");
  plan.from_previous = true;
  for (const char* m : milestones)
    (void)plan.add_milestone(m);
  if (why != nullptr) *why = "the previous run";
  return plan;
}

bool save_progress_plan(const std::string& path, const profiling::ProgressPlan& measured,
                        std::string* error) {
  if (path.empty() || measured.stage_count == 0) return false;
  JsonValue root = JsonValue::object();
  root.set("format", "engine.view.load_plan");
  root.set("version", 1);
  JsonValue stages = JsonValue::array();
  for (u32 i = 0; i < measured.stage_count; ++i) {
    JsonValue s = JsonValue::object();
    s.set("name", std::string(measured.stages[i]));
    s.set("ms", measured.weights[i]);
    stages.push_back(std::move(s));
  }
  root.set("stages", std::move(stages));
  const usize slash = path.find_last_of("/\\");
  if (slash != std::string::npos) (void)io::make_directories(path.substr(0, slash));
  if (io::write_file_atomic(path, write_json(root)) != io::Status::Ok) {
    if (error != nullptr) *error = "cannot write " + path;
    return false;
  }
  return true;
}

}  // namespace engine::view
