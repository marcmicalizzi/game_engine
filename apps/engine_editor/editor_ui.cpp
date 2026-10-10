#include "editor_ui.h"

#include <cstring>
#include <imgui.h>
#include <imgui_internal.h>
#include <string>

namespace engine::editor {

static_assert(sizeof(ImDrawVert) == sizeof(renderer::OverlayVertex),
              "the overlay's vertex is ImDrawVert byte for byte");
static_assert(sizeof(ImDrawIdx) == sizeof(u32), "cmake/EngineImgui.cmake makes ImDrawIdx 32 bits");

namespace {

constexpr u32 k_journal_rows = 64;

ImGuiKey key_of(window::Key key) noexcept {
  using window::Key;
  switch (key) {
    case Key::Escape: return ImGuiKey_Escape;
    case Key::Space: return ImGuiKey_Space;
    case Key::Enter: return ImGuiKey_Enter;
    case Key::Tab: return ImGuiKey_Tab;
    case Key::Backspace: return ImGuiKey_Backspace;
    case Key::Left: return ImGuiKey_LeftArrow;
    case Key::Right: return ImGuiKey_RightArrow;
    case Key::Up: return ImGuiKey_UpArrow;
    case Key::Down: return ImGuiKey_DownArrow;
    case Key::Shift: return ImGuiKey_LeftShift;
    case Key::Control: return ImGuiKey_LeftCtrl;
    case Key::Alt: return ImGuiKey_LeftAlt;
    default: break;
  }
  const u16 k = static_cast<u16>(key);
  if (k >= static_cast<u16>(Key::A) && k <= static_cast<u16>(Key::Z))
    return static_cast<ImGuiKey>(ImGuiKey_A + (k - static_cast<u16>(Key::A)));
  if (k >= static_cast<u16>(Key::Digit0) && k <= static_cast<u16>(Key::Digit9))
    return static_cast<ImGuiKey>(ImGuiKey_0 + (k - static_cast<u16>(Key::Digit0)));
  if (k >= static_cast<u16>(Key::F1) && k <= static_cast<u16>(Key::F12))
    return static_cast<ImGuiKey>(ImGuiKey_F1 + (k - static_cast<u16>(Key::F1)));
  return ImGuiKey_None;
}

// The window's buttons (1 left, 2 middle, 3 right) as ImGui's (0 left, 1 right, 2 middle).
int button_of(u8 button) noexcept {
  switch (button) {
    case 1: return 0;
    case 3: return 1;
    case 2: return 2;
    default: return button >= 4 ? button - 1 : -1;
  }
}

ImU32 rgba(u8 r, u8 g, u8 b, u8 a) { return IM_COL32(r, g, b, a); }

}  // namespace

bool EditorUi::create() {
  destroy();
  IMGUI_CHECKVERSION();
  context_ = ImGui::CreateContext();
  ImGui::SetCurrentContext(context_);
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;  // the layout is built each start; nothing is written beside the run
  io.LogFilename = nullptr;
  io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
  // Textures are the renderer's overlay textures, made and retired as ImGui asks (`render`).
  io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
  io.BackendRendererName = "engine overlay";
  io.BackendPlatformName = "engine window";
  ImGui::StyleColorsDark();
  laid_out_ = false;
  return true;
}

void EditorUi::destroy() noexcept {
  if (context_ == nullptr) return;
  ImGui::DestroyContext(context_);
  context_ = nullptr;
}

void EditorUi::feed(const window::Event& e) {
  if (context_ == nullptr) return;
  ImGui::SetCurrentContext(context_);
  ImGuiIO& io = ImGui::GetIO();
  switch (e.kind) {
    case window::EventKind::MouseMove: io.AddMousePosEvent(e.x, e.y); break;
    case window::EventKind::MouseButtonDown:
    case window::EventKind::MouseButtonUp: {
      const int b = button_of(e.button);
      io.AddMousePosEvent(e.x, e.y);
      if (b >= 0 && b < ImGuiMouseButton_COUNT)
        io.AddMouseButtonEvent(b, e.kind == window::EventKind::MouseButtonDown);
      break;
    }
    case window::EventKind::MouseWheel: io.AddMouseWheelEvent(e.dx, e.dy); break;
    case window::EventKind::KeyDown:
    case window::EventKind::KeyUp: {
      const bool down = e.kind == window::EventKind::KeyDown;
      if (e.key == window::Key::Shift) io.AddKeyEvent(ImGuiMod_Shift, down);
      if (e.key == window::Key::Control) io.AddKeyEvent(ImGuiMod_Ctrl, down);
      if (e.key == window::Key::Alt) io.AddKeyEvent(ImGuiMod_Alt, down);
      const ImGuiKey key = key_of(e.key);
      if (key != ImGuiKey_None) io.AddKeyEvent(key, down);
      break;
    }
    case window::EventKind::FocusGained: io.AddFocusEvent(true); break;
    case window::EventKind::FocusLost: io.AddFocusEvent(false); break;
    default: break;
  }
}

bool EditorUi::wants_mouse() const noexcept {
  return context_ != nullptr && ImGui::GetIO().WantCaptureMouse;
}

bool EditorUi::wants_keyboard() const noexcept {
  return context_ != nullptr && ImGui::GetIO().WantCaptureKeyboard;
}

void EditorUi::refresh(const EditorDocument& document) {
  const doc::Document& d = document.document();
  const u32 journal_size = static_cast<u32>(d.journal().size());
  if (d.revision() == revision_ && document.selection() == shown_ && journal_size == journal_size_)
    return;
  revision_ = d.revision();
  shown_ = document.selection();
  journal_size_ = journal_size;
  document.outliner(outliner_);
  if (!document.properties(shown_, properties_)) properties_.clear();
  document.journal(journal_, k_journal_rows);
}

void EditorUi::frame(f32 dt, u32 width, u32 height, const EditorDocument& document,
                     const Viewport& viewport, const renderer::Camera& camera, Actions& out) {
  out = Actions{};
  ImGui::SetCurrentContext(context_);
  ImGuiIO& io = ImGui::GetIO();
  io.DisplaySize = ImVec2(static_cast<f32>(width), static_cast<f32>(height));
  io.DeltaTime = dt > 0.0f ? dt : 1.0f / 60.0f;
  refresh(document);
  ImGui::NewFrame();

  // The panels dock round a central node that lets the pointer through to the viewport.
  const ImGuiID dock =
      ImGui::DockSpaceOverViewport(0, nullptr, ImGuiDockNodeFlags_PassthruCentralNode);
  if (!laid_out_) {
    laid_out_ = true;
    ImGui::DockBuilderRemoveNode(dock);
    ImGui::DockBuilderAddNode(
        dock, static_cast<ImGuiDockNodeFlags>(ImGuiDockNodeFlags_DockSpace) |
                  static_cast<ImGuiDockNodeFlags>(ImGuiDockNodeFlags_PassthruCentralNode));
    ImGui::DockBuilderSetNodeSize(dock, io.DisplaySize);
    ImGuiID center = dock;
    const ImGuiID left =
        ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.22f, nullptr, &center);
    const ImGuiID right =
        ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.30f, nullptr, &center);
    const ImGuiID bottom =
        ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.24f, nullptr, &center);
    ImGui::DockBuilderDockWindow("Outliner", left);
    ImGui::DockBuilderDockWindow("Properties", right);
    ImGui::DockBuilderDockWindow("Journal", bottom);
    ImGui::DockBuilderFinish(dock);
  }

  if (ImGui::Begin("Outliner")) {
    for (const OutlinerRow& row : outliner_) {
      const std::string label = (row.name.empty() ? std::string("(unnamed)") : row.name) + "  " +
                                row.type + "##" + hex(row.id);
      ImGui::Indent(static_cast<f32>(row.depth) * 12.0f + 0.001f);
      if (ImGui::Selectable(label.c_str(), row.id == document.selection())) {
        out.select = true;
        out.selected = row.id;
      }
      ImGui::Unindent(static_cast<f32>(row.depth) * 12.0f + 0.001f);
    }
  }
  ImGui::End();

  if (ImGui::Begin("Properties")) {
    if (document.selection().is_null() || properties_.empty()) {
      ImGui::TextDisabled("Nothing selected: click an object or a row of the outliner.");
    } else {
      const std::string type(document.document().type_of(document.selection()));
      ImGui::TextUnformatted(type.c_str());
      ImGui::TextDisabled("%s", hex(document.selection()).c_str());
      if (ImGui::BeginTable(
              "properties", 3,
              ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("field");
        ImGui::TableSetupColumn("kind");
        ImGui::TableSetupColumn("value");
        ImGui::TableHeadersRow();
        for (const PropertyRow& row : properties_) {
          ImGui::TableNextRow();
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(row.name.c_str());
          ImGui::TableNextColumn();
          ImGui::TextDisabled("%s", row.kind.c_str());
          ImGui::TableNextColumn();
          if (row.set) {
            ImGui::TextWrapped("%s", row.value.c_str());
          } else {
            ImGui::TextDisabled("%s (default)", row.value.c_str());
          }
        }
        ImGui::EndTable();
      }
    }
  }
  ImGui::End();

  if (ImGui::Begin("Journal")) {
    for (const JournalRow& row : journal_) {
      const std::string line = "#" + std::to_string(row.index) + "  " + row.actor + " (" +
                               row.role + ", " + row.task + "): " + row.rationale + "  [" +
                               std::to_string(row.commands) + " commands]";
      if (row.undone) {
        ImGui::TextDisabled("%s (undone)", line.c_str());
      } else {
        ImGui::TextUnformatted(line.c_str());
      }
    }
    if (journal_.empty()) ImGui::TextDisabled("No transactions yet.");
  }
  ImGui::End();

  // The selection's box, over the picture and under the panels.
  const u32 instance = viewport.scene().instance_of(document.selection());
  WorldPos corners[8];
  if (instance != ~0u && viewport.instance_box(instance, corners) && viewport.valid()) {
    renderer::Camera view_camera = camera;
    const renderer::ViewSet& views =
        const_cast<Viewport&>(viewport).renderer().update_views(view_camera);
    if (views.size() > 0) {
      const Mat4& view_proj = views[0].view_proj;
      ImVec2 screen[8];
      bool front[8];
      for (u32 c = 0; c < 8; ++c) {
        const Vec3 p = relative(corners[c], camera.position);
        const Vec4 clip = view_proj * Vec4(p, 1.0f);
        front[c] = clip.w > 1e-4f;
        const f32 w = front[c] ? clip.w : 1.0f;
        screen[c] = ImVec2((clip.x / w * 0.5f + 0.5f) * static_cast<f32>(width),
                           (0.5f - clip.y / w * 0.5f) * static_cast<f32>(height));
      }
      static constexpr u8 k_edges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3},
                                            {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
      ImDrawList* draw = ImGui::GetBackgroundDrawList();
      for (const auto& e : k_edges) {
        if (!front[e[0]] || !front[e[1]]) continue;
        draw->AddLine(screen[e[0]], screen[e[1]], rgba(255, 196, 64, 255), 2.0f);
      }
    }
  }
  ImGui::Render();
}

bool EditorUi::render(renderer::SceneRenderer& renderer, renderer::OverlayDrawData& out,
                      std::string& error) {
  out = renderer::OverlayDrawData{};
  ImGui::SetCurrentContext(context_);
  ImDrawData* data = ImGui::GetDrawData();
  if (data == nullptr) return true;
  // Textures first, so every command below names one the overlay has. An update makes the
  // texture again whole: ImGui grows its atlas a few times as glyphs are first used, and a blocking
  // re-upload of a 512 x 128 atlas is a click's cost, not a frame's.
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
  vertices_.clear();
  indices_.clear();
  commands_.clear();
  for (const ImDrawList* list : data->CmdLists) {
    const u32 vertex_base = static_cast<u32>(vertices_.size());
    const u32 index_base = static_cast<u32>(indices_.size());
    const u32 vertex_count = static_cast<u32>(list->VtxBuffer.Size);
    const u32 index_count = static_cast<u32>(list->IdxBuffer.Size);
    vertices_.resize(vertex_base + vertex_count);
    indices_.resize(index_base + index_count);
    if (vertex_count > 0)
      std::memcpy(vertices_.data() + vertex_base, list->VtxBuffer.Data,
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

void EditorUi::forget_textures() noexcept {
  if (context_ == nullptr) return;
  ImGui::SetCurrentContext(context_);
  for (ImTextureData* tex : ImGui::GetPlatformIO().Textures) {
    tex->SetTexID(ImTextureID_Invalid);
    tex->SetStatus(ImTextureStatus_Destroyed);  // with pixels, that is "make it again"
  }
}

}  // namespace engine::editor
