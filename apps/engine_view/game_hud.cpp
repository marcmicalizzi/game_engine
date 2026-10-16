#include "game_hud.h"

#include <systems/renderer/scene_renderer.h>

#include <cstdio>
#include <cstring>
#include <imgui.h>

namespace engine::view {

static_assert(sizeof(ImDrawVert) == sizeof(renderer::OverlayVertex),
              "the overlay's vertex is ImDrawVert byte for byte");
static_assert(sizeof(ImDrawIdx) == sizeof(u32), "cmake/EngineImgui.cmake makes ImDrawIdx 32 bits");

GameHudOverlay::~GameHudOverlay() { destroy(); }

bool GameHudOverlay::create() {
  destroy();
  IMGUI_CHECKVERSION();
  context_ = ImGui::CreateContext();
  ImGui::SetCurrentContext(context_);
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;  // nothing is written beside the run
  io.LogFilename = nullptr;
  io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
  io.BackendRendererName = "engine overlay";
  io.BackendPlatformName = "engine-view hud";
  ImGui::StyleColorsDark();
  ImGuiStyle& style = ImGui::GetStyle();
  style.WindowRounding = 4.0f;
  style.WindowBorderSize = 0.0f;
  style.FrameRounding = 2.0f;
  return true;
}

void GameHudOverlay::destroy() noexcept {
  if (context_ == nullptr) return;
  ImGui::DestroyContext(context_);
  context_ = nullptr;
}

bool GameHudOverlay::build(const GameHud& hud, u32 width, u32 height,
                           renderer::SceneRenderer& renderer, renderer::OverlayDrawData& out,
                           std::string& error) {
  out = renderer::OverlayDrawData{};
  vertices_.clear();
  indices_.clear();
  commands_.clear();
  if (context_ == nullptr || hud.empty() || width == 0 || height == 0) return true;
  ImGui::SetCurrentContext(context_);
  ImGuiIO& io = ImGui::GetIO();
  io.DisplaySize = ImVec2(static_cast<f32>(width), static_cast<f32>(height));
  io.DeltaTime = 1.0f / 60.0f;  // nothing animates; ImGui only needs it positive
  ImGui::NewFrame();

  // The safe region: 5% in from each edge (a television's action-safe margin), at least 8 px.
  const f32 margin_x =
      static_cast<f32>(width) * 0.05f > 8.0f ? static_cast<f32>(width) * 0.05f : 8.0f;
  const f32 margin_y =
      static_cast<f32>(height) * 0.05f > 8.0f ? static_cast<f32>(height) * 0.05f : 8.0f;
  ImGui::SetNextWindowPos(ImVec2(margin_x, margin_y), ImGuiCond_Always);
  ImGui::SetNextWindowBgAlpha(0.45f);
  const ImGuiWindowFlags flags =
      ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
      ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
      ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove;
  if (ImGui::Begin("game hud", nullptr, flags)) {
    char overlay_text[48];
    for (u32 m = 0; m < hud.meter_count; ++m) {
      const GameHud::Meter& meter = hud.meters[m];
      const f64 fraction = meter.max > 0.0 ? meter.value / meter.max : 0.0;
      std::snprintf(overlay_text, sizeof(overlay_text), "%.0f / %.0f", meter.value, meter.max);
      ImGui::TextUnformatted(meter.name);
      ImGui::SameLine(96.0f);
      ImGui::ProgressBar(static_cast<f32>(fraction < 0.0   ? 0.0
                                          : fraction > 1.0 ? 1.0
                                                           : fraction),
                         ImVec2(200.0f, 0.0f), overlay_text);
    }
    for (const GameHud::Line& line : hud.lines) {
      if (line.set) ImGui::TextUnformatted(line.text);
    }
  }
  ImGui::End();
  ImGui::Render();

  ImDrawData* data = ImGui::GetDrawData();
  if (data == nullptr) return true;
  // Textures first, so every command names one the overlay has (engine-editor's EditorUi::render
  // does the same; the atlas is made once and grown a few times as glyphs are first used).
  if (data->Textures != nullptr) {
    for (ImTextureData* tex : *data->Textures) {
      if (tex->Status == ImTextureStatus_WantCreate || tex->Status == ImTextureStatus_WantUpdates) {
        if (tex->TexID != ImTextureID_Invalid)
          renderer.remove_overlay_texture(static_cast<u32>(tex->TexID - 1));
        if (tex->Format != ImTextureFormat_RGBA32) {
          error = "ImGui asked for a texture format the overlay does not take";
          return false;
        }
        const u32 id = renderer.overlay().add_texture(
            static_cast<u32>(tex->Width), static_cast<u32>(tex->Height), tex->GetPixels(), &error);
        if (id == renderer::OverlayPass::k_no_texture) return false;
        tex->SetTexID(static_cast<ImTextureID>(id) + 1);
        tex->SetStatus(ImTextureStatus_OK);
      } else if (tex->Status == ImTextureStatus_WantDestroy && tex->UnusedFrames > 0) {
        if (tex->TexID != ImTextureID_Invalid)
          renderer.remove_overlay_texture(static_cast<u32>(tex->TexID - 1));
        tex->SetTexID(ImTextureID_Invalid);
        tex->SetStatus(ImTextureStatus_Destroyed);
      }
    }
  }
  for (const ImDrawList* list : data->CmdLists) {
    const u32 vertex_base = static_cast<u32>(vertices_.size());
    const u32 index_base = static_cast<u32>(indices_.size());
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

}  // namespace engine::view
