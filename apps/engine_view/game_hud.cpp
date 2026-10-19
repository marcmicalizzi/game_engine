#include "game_hud.h"

#include <core/json/json.h>
#include <core/time/time.h>
#include <systems/renderer/scene_renderer.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <imgui.h>

namespace engine::view {

static_assert(sizeof(ImDrawVert) == sizeof(renderer::OverlayVertex),
              "the overlay's vertex is ImDrawVert byte for byte");
static_assert(sizeof(ImDrawIdx) == sizeof(u32), "cmake/EngineImgui.cmake makes ImDrawIdx 32 bits");

namespace {

// ImGui's default font is designed at this size; the style's paddings and spacings are in its
// units, so they are scaled by the ratio when the font is.
constexpr f32 k_imgui_default_px = 13.0f;

// A result field's value as the panel prints it: a whole number plainly, a fraction to one
// decimal, a string as it is, a bool as yes or no, anything else as its JSON.
void format_value(const JsonValue& v, char* out, usize size) {
  f64 number = 0.0;
  if (v.is_bool()) {
    std::snprintf(out, size, "%s", v.as_bool() ? "yes" : "no");
  } else if (v.is_string()) {
    std::snprintf(out, size, "%.*s", static_cast<int>(v.as_string().size()), v.as_string().data());
  } else if (v.get_f64(number)) {
    if (number == std::floor(number) && std::fabs(number) < 1.0e15) {
      std::snprintf(out, size, "%.0f", number);
    } else {
      std::snprintf(out, size, "%.1f", number);
    }
  } else {
    const std::string json = write_json(v, JsonWriteOptions{.pretty = false});
    std::snprintf(out, size, "%s", json.c_str());
  }
}

HudRect window_rect() {
  const ImVec2 pos = ImGui::GetWindowPos();
  const ImVec2 size = ImGui::GetWindowSize();
  return HudRect{pos.x, pos.y, pos.x + size.x, pos.y + size.y};
}

}  // namespace

f32 hud_font_px(u32 height, f32 display_scale) noexcept {
  const f32 scale = display_scale > 0.0f ? display_scale : 1.0f;
  const f32 px = std::round(k_hud_font_px * (static_cast<f32>(height) / 1080.0f) * scale);
  return px < k_hud_font_min_px ? k_hud_font_min_px
                                : (px > k_hud_font_max_px ? k_hud_font_max_px : px);
}

f32 hud_element_px(f32 size_px, u32 height, f32 display_scale) noexcept {
  const f32 scale = display_scale > 0.0f ? display_scale : 1.0f;
  const f32 px = std::round(size_px * (static_cast<f32>(height) / 1080.0f) * scale);
  return px < k_hud_font_min_px ? k_hud_font_min_px
                                : (px > k_hud_element_max_px ? k_hud_element_max_px : px);
}

// **The structured elements** (ABI 4), drawn on ImGui's background list — under the stack's panel
// and the result panel, which are windows — in the order they were first placed. Each is a box on
// one of nine points of the safe region: the text's own size at the element's font, grown by the
// panel's padding (with a panel) or the outline's width (without), hung from, centred on or stood
// on the point by its row and placed across by its alignment, at whole pixels so the glyphs land
// on the grid the atlas baked them for.
void GameHudOverlay::draw_elements(const GameHud& hud, f32 margin_x, f32 margin_y) {
  if (hud.element_count == 0) return;
  ImDrawList* draw = ImGui::GetBackgroundDrawList();
  ImFont* font = ImGui::GetFont();
  const f32 w = static_cast<f32>(width_);
  const f32 h = static_cast<f32>(height_);
  for (u32 i = 0; i < hud.element_count; ++i) {
    const GameHud::Element& e = hud.elements[i];
    Placed placed{e.id, HudRect{}, 0.0f};
    if (!e.visible || e.text[0] == '\0') {
      placed_[placed_count_++] = placed;
      continue;
    }
    const f32 px = hud_element_px(e.size_px, height_, display_scale_);
    const f32 outline =
        e.outline_px > 0.0f ? std::max(1.0f, std::round(e.outline_px * scale_)) : 0.0f;
    const bool panel = e.panel == hotload::HudPanel::Solid;
    const f32 margin = panel ? std::round(e.panel_padding_px * scale_) : outline;
    const ImVec2 text = font->CalcTextSizeA(px, FLT_MAX, 0.0f, e.text);
    const f32 box_w = text.x + 2.0f * margin;
    const f32 box_h = text.y + 2.0f * margin;
    const u32 a = static_cast<u32>(e.anchor);
    const u32 column = a % 3;  // left, centre, right
    const u32 row = a / 3;     // top, middle, bottom
    f32 x = column == 0 ? margin_x : column == 1 ? w * 0.5f : w - margin_x;
    f32 y = row == 0 ? margin_y : row == 1 ? h * 0.5f : h - margin_y;
    x += e.offset_px[0] * scale_;
    y += e.offset_px[1] * scale_;
    const f32 x0 = std::round(e.align == hotload::HudAlign::Left     ? x
                              : e.align == hotload::HudAlign::Centre ? x - box_w * 0.5f
                                                                     : x - box_w);
    const f32 y0 = std::round(row == 0 ? y : row == 1 ? y - box_h * 0.5f : y - box_h);
    if (panel) {
      draw->AddRectFilled(
          ImVec2(x0, y0), ImVec2(x0 + box_w, y0 + box_h),
          IM_COL32(e.panel_color[0], e.panel_color[1], e.panel_color[2], e.panel_color[3]),
          std::round(4.0f * scale_));
    }
    const ImVec2 at(x0 + margin, y0 + margin);
    if (outline > 0.0f) {
      // The outline is the text drawn eight times round its place in the outline's colour: what a
      // caption on a bright sky needs to be read, at no cost to the atlas (the same glyphs).
      const ImU32 oc =
          IM_COL32(e.outline_color[0], e.outline_color[1], e.outline_color[2], e.outline_color[3]);
      for (i32 dy = -1; dy <= 1; ++dy) {
        for (i32 dx = -1; dx <= 1; ++dx) {
          if (dx == 0 && dy == 0) continue;
          draw->AddText(
              font, px,
              ImVec2(at.x + static_cast<f32>(dx) * outline, at.y + static_cast<f32>(dy) * outline),
              oc, e.text);
        }
      }
    }
    draw->AddText(font, px, at, IM_COL32(e.color[0], e.color[1], e.color[2], e.color[3]), e.text);
    placed.rect = HudRect{x0, y0, x0 + box_w, y0 + box_h};
    placed.font_px = px;
    placed_[placed_count_++] = placed;
  }
}

HudRect GameHudOverlay::bounds_of(u32 id) const noexcept {
  for (u32 i = 0; i < placed_count_; ++i) {
    if (placed_[i].id == id) return placed_[i].rect;
  }
  return HudRect{};
}

namespace {

JsonValue rect_json(const HudRect& r) {
  JsonValue out = JsonValue::array();
  const bool empty = r.empty();
  for (const f32 v : {r.x0, r.y0, r.x1, r.y1})
    out.push_back(JsonValue(empty ? 0.0 : static_cast<f64>(v)));
  return out;
}

JsonValue colour_json(const u8 (&c)[4]) {
  JsonValue out = JsonValue::array();
  for (const u8 v : c)
    out.push_back(JsonValue(static_cast<u64>(v)));
  return out;
}

}  // namespace

JsonValue hud_elements_json(const GameHud& hud, const GameHudOverlay* overlay) {
  const auto bounds = [&](u32 id) {
    return overlay != nullptr ? overlay->bounds_of(id) : HudRect{};
  };
  JsonValue out = JsonValue::array();
  for (u32 m = 0; m < hud.meter_count; ++m) {
    const u32 id = hotload::k_hud_meter_id + m;
    JsonValue e = JsonValue::object();
    e.set("id", id);
    e.set("kind", "meter");
    e.set("anchor", "top_left");
    e.set("text", std::string_view(hud.meters[m].name));
    e.set("value", hud.meters[m].value);
    e.set("max", hud.meters[m].max);
    e.set("visible", true);
    e.set("tick", hud.tick);
    e.set("bounds_px", rect_json(bounds(id)));
    out.push_back(std::move(e));
  }
  for (u32 l = 0; l < GameHud::k_lines; ++l) {
    if (!hud.lines[l].set) continue;
    const u32 id = hotload::k_hud_line_id + l;
    JsonValue e = JsonValue::object();
    e.set("id", id);
    e.set("kind", "line");
    e.set("anchor", "top_left");
    e.set("text", std::string_view(hud.lines[l].text));
    e.set("visible", true);
    e.set("tick", hud.tick);
    e.set("bounds_px", rect_json(bounds(id)));
    out.push_back(std::move(e));
  }
  for (u32 i = 0; i < hud.element_count; ++i) {
    const GameHud::Element& el = hud.elements[i];
    JsonValue e = JsonValue::object();
    e.set("id", el.id);
    e.set("kind", "text");
    e.set("anchor", hud_anchor_name(el.anchor));
    e.set("align", hud_align_name(el.align));
    JsonValue offset = JsonValue::array();
    offset.push_back(JsonValue(static_cast<f64>(el.offset_px[0])));
    offset.push_back(JsonValue(static_cast<f64>(el.offset_px[1])));
    e.set("offset_px", std::move(offset));
    e.set("text", std::string_view(el.text));
    e.set("size_px", static_cast<f64>(el.size_px));
    e.set("color", colour_json(el.color));
    e.set("outline_px", static_cast<f64>(el.outline_px));
    e.set("outline_color", colour_json(el.outline_color));
    e.set("panel", el.panel == hotload::HudPanel::Solid ? "solid" : "none");
    if (el.panel == hotload::HudPanel::Solid) {
      e.set("panel_color", colour_json(el.panel_color));
      e.set("panel_padding_px", static_cast<f64>(el.panel_padding_px));
    }
    e.set("visible", el.visible);
    e.set("tick", el.tick);
    e.set("bounds_px", rect_json(bounds(el.id)));
    out.push_back(std::move(e));
  }
  return out;
}

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
  font_px_ = 0.0f;
  apply_font(k_imgui_default_px);
  return true;
}

// The style from scratch at `px`: ImGui's dark colours, this HUD's roundings, every size scaled
// from the default font's, and the font itself baked at `px` (ImGui 1.92's fonts are dynamic:
// `FontSizeBase` is the size glyphs are rasterized at, and the atlas gains that size when a frame
// first draws it, which `upload` then sends to the overlay — a resize, never a frame).
void GameHudOverlay::apply_font(f32 px) {
  if (px == font_px_) return;
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

void GameHudOverlay::destroy() noexcept {
  if (context_ == nullptr) return;
  ImGui::DestroyContext(context_);
  context_ = nullptr;
}

bool GameHudOverlay::layout(const GameHud& hud, const JsonValue* result, u32 width, u32 height,
                            f32 display_scale) {
  vertices_.clear();
  indices_.clear();
  commands_.clear();
  panel_ = HudRect{};
  result_panel_ = HudRect{};
  placed_count_ = 0;
  pending_textures_ = 0;
  width_ = width;
  height_ = height;
  const bool panel = result != nullptr && result->is_object();
  if (context_ == nullptr || (hud.empty() && !panel) || width == 0 || height == 0) return true;
  ++layouts_;
  ImGui::SetCurrentContext(context_);
  const f32 px = hud_font_px(height, display_scale);
  display_scale_ = display_scale > 0.0f ? display_scale : 1.0f;
  scale_ = static_cast<f32>(height) / 1080.0f * display_scale_;
  apply_font(px);
  ImGuiIO& io = ImGui::GetIO();
  io.DisplaySize = ImVec2(static_cast<f32>(width), static_cast<f32>(height));
  io.DeltaTime = 1.0f / 60.0f;  // nothing animates; ImGui only needs it positive
  ImGui::NewFrame();

  // The safe region: 5% in from each edge (a television's action-safe margin), at least 8 px.
  const f32 margin_x =
      static_cast<f32>(width) * 0.05f > 8.0f ? static_cast<f32>(width) * 0.05f : 8.0f;
  const f32 margin_y =
      static_cast<f32>(height) * 0.05f > 8.0f ? static_cast<f32>(height) * 0.05f : 8.0f;
  // The columns, in the font's units: a meter's name column and bar were 96 and 200 px at 13 px.
  const f32 name_column = std::round(px * 96.0f / k_imgui_default_px);
  const f32 meter_width = std::round(px * 200.0f / k_imgui_default_px);
  const ImGuiWindowFlags flags =
      ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
      ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
      ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove;
  // **The top-left stack**: `hud_text`'s lines and `hud_meter`'s meters, the reserved-id elements,
  // as the one panel they were before elements existed (the picture a module that calls only them
  // drew, to the pixel). Each row's rectangle is kept for the report.
  if (!hud.stack_empty()) {
    ImGui::SetNextWindowPos(ImVec2(margin_x, margin_y), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.45f);
    if (ImGui::Begin("game hud", nullptr, flags)) {
      char overlay_text[48];
      for (u32 m = 0; m < hud.meter_count; ++m) {
        const GameHud::Meter& meter = hud.meters[m];
        const f64 fraction = meter.max > 0.0 ? meter.value / meter.max : 0.0;
        std::snprintf(overlay_text, sizeof(overlay_text), "%.0f / %.0f", meter.value, meter.max);
        ImGui::TextUnformatted(meter.name);
        const ImVec2 row_min = ImGui::GetItemRectMin();
        ImGui::SameLine(name_column);
        ImGui::ProgressBar(static_cast<f32>(fraction < 0.0   ? 0.0
                                            : fraction > 1.0 ? 1.0
                                                             : fraction),
                           ImVec2(meter_width, 0.0f), overlay_text);
        const ImVec2 row_max = ImGui::GetItemRectMax();
        placed_[placed_count_++] = Placed{hotload::k_hud_meter_id + m,
                                          HudRect{row_min.x, row_min.y, row_max.x, row_max.y}, px};
      }
      for (u32 l = 0; l < GameHud::k_lines; ++l) {
        const GameHud::Line& line = hud.lines[l];
        if (!line.set) continue;
        ImGui::TextUnformatted(line.text);
        const ImVec2 a = ImGui::GetItemRectMin();
        const ImVec2 b = ImGui::GetItemRectMax();
        placed_[placed_count_++] =
            Placed{hotload::k_hud_line_id + l, HudRect{a.x, a.y, b.x, b.y}, px};
      }
      panel_ = window_rect();
    }
    ImGui::End();
  }
  draw_elements(hud, margin_x, margin_y);

  // **The result panel** (apps.md "--game", "Ending"; roadmap P44): a live window whose module
  // called `end_run` stays here until a key says otherwise. Centred, where a tester's eye goes;
  // the HUD above keeps the run's last lines and meters, since the module is not ticked again and
  // the runner keeps what it last said. The fields are the result object's, in its order, each a
  // key and a formatted value; the last line is the keys.
  if (panel) {
    ImGui::SetNextWindowPos(ImVec2(static_cast<f32>(width) * 0.5f, static_cast<f32>(height) * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowBgAlpha(0.75f);
    if (ImGui::Begin("game result", nullptr, flags)) {
      ImGui::TextUnformatted("The run is over");
      ImGui::Separator();
      const JsonValue::Object& fields = result->as_object();
      char value[96];
      for (u32 i = 0; i < fields.size(); ++i) {
        format_value(fields.value_at(i), value, sizeof(value));
        ImGui::TextUnformatted(fields.key_at(i).c_str());
        ImGui::SameLine(name_column);
        ImGui::TextUnformatted(value);
      }
      ImGui::Separator();
      ImGui::TextUnformatted("Enter to quit \xC2\xB7 R for a new run \xC2\xB7 Esc to quit");
      result_panel_ = window_rect();
    }
    ImGui::End();
  }
  ImGui::Render();

  ImDrawData* data = ImGui::GetDrawData();
  if (data == nullptr) return true;
  if (data->Textures != nullptr) {
    for (ImTextureData* tex : *data->Textures) {
      if (tex->Status == ImTextureStatus_WantCreate || tex->Status == ImTextureStatus_WantUpdates ||
          tex->Status == ImTextureStatus_WantDestroy) {
        ++pending_textures_;
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
      renderer::OverlayCommand c;
      c.clip_x0 = (cmd.ClipRect.x - data->DisplayPos.x) * data->FramebufferScale.x;
      c.clip_y0 = (cmd.ClipRect.y - data->DisplayPos.y) * data->FramebufferScale.y;
      c.clip_x1 = (cmd.ClipRect.z - data->DisplayPos.x) * data->FramebufferScale.x;
      c.clip_y1 = (cmd.ClipRect.w - data->DisplayPos.y) * data->FramebufferScale.y;
      c.texture = renderer::OverlayPass::k_no_texture;  // `upload` names it once the atlas is made
      c.first_index = index_base + cmd.IdxOffset;
      c.index_count = cmd.ElemCount;
      c.vertex_offset = vertex_base + cmd.VtxOffset;
      commands_.push_back(c);
    }
  }
  return true;
}

// Textures first, so every command names one the overlay has (engine-editor's EditorUi::render
// does the same; the atlas is made once and grown or re-baked a few times as glyphs or sizes are
// first used), then the commands' textures resolved from ImGui's ids.
bool GameHudOverlay::upload(renderer::SceneRenderer& renderer, std::string& error) {
  if (context_ == nullptr) return true;
  ImGui::SetCurrentContext(context_);
  ImDrawData* data = ImGui::GetDrawData();
  if (data == nullptr) return true;
  if (data->Textures != nullptr) {
    for (ImTextureData* tex : *data->Textures) {
      if (tex->Status == ImTextureStatus_WantCreate || tex->Status == ImTextureStatus_WantUpdates) {
        const i64 start = time::monotonic_ns();
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
        ++atlas_uploads_;
        atlas_ns_ += time::monotonic_ns() - start;
      } else if (tex->Status == ImTextureStatus_WantDestroy && tex->UnusedFrames > 0) {
        if (tex->TexID != ImTextureID_Invalid)
          renderer.remove_overlay_texture(static_cast<u32>(tex->TexID - 1));
        tex->SetTexID(ImTextureID_Invalid);
        tex->SetStatus(ImTextureStatus_Destroyed);
      }
    }
  }
  // The commands in the lists' order, as `layout` copied them: a command whose texture is still
  // not made (none, after the loop above) is left out.
  u32 c = 0;
  for (const ImDrawList* list : data->CmdLists) {
    for (const ImDrawCmd& cmd : list->CmdBuffer) {
      if (cmd.UserCallback != nullptr || cmd.ElemCount == 0) continue;
      if (c >= commands_.size()) break;
      const ImTextureID tex = cmd.GetTexID();
      commands_[c].texture = tex == ImTextureID_Invalid ? renderer::OverlayPass::k_no_texture
                                                        : static_cast<u32>(tex - 1);
      ++c;
    }
  }
  u32 kept = 0;
  for (u32 i = 0; i < commands_.size(); ++i) {
    if (commands_[i].texture == renderer::OverlayPass::k_no_texture) continue;
    commands_[kept++] = commands_[i];
  }
  commands_.resize(kept);
  return true;
}

bool GameHudOverlay::build(const GameHud& hud, const JsonValue* result, u32 width, u32 height,
                           f32 display_scale, renderer::SceneRenderer& renderer,
                           renderer::OverlayDrawData& out, std::string& error) {
  out = renderer::OverlayDrawData{};
  if (!layout(hud, result, width, height, display_scale)) return false;
  if (vertices_.empty()) return true;
  if (!upload(renderer, error)) return false;
  out.vertices = std::span<const renderer::OverlayVertex>(vertices_.data(), vertices_.size());
  out.indices = std::span<const u32>(indices_.data(), indices_.size());
  out.commands = std::span<const renderer::OverlayCommand>(commands_.data(), commands_.size());
  return true;
}

JsonValue GameHudOverlay::summary_json() const {
  JsonValue out = JsonValue::object();
  out.set("font_px", static_cast<f64>(font_px_));
  out.set("width", width_);
  out.set("height", height_);
  JsonValue panel = JsonValue::array();
  for (const f32 v : {panel_.x0, panel_.y0, panel_.x1, panel_.y1})
    panel.push_back(JsonValue(static_cast<f64>(v)));
  out.set("panel", std::move(panel));
  out.set("layouts", layouts_);
  out.set("atlas_uploads", atlas_uploads_);
  out.set("atlas_ms", atlas_ms());
  return out;
}

}  // namespace engine::view
