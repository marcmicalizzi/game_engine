#include "game_hud.h"

#include "loading.h"

#include <core/json/json.h>
#include <core/time/time.h>
#include <foundation/image/decode.h>
#include <systems/renderer/scene_renderer.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <imgui.h>
#include <imgui_internal.h>  // ImTextCharFromUtf8, for the letter-spaced text

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

// An image element's draw commands name `k_image_texture_base + n` for the runner's image n until
// `upload` swaps in the overlay texture made for it; ImGui's own textures are ids below 2^32.
constexpr ImTextureID k_image_texture_base = ImTextureID{1} << 40;

// `text` laid at `at` in `font` at `px`, with `spacing` pixels after each character but the last:
// ImGui's own `AddText` when there is none — the picture an ABI 4 element always drew — and a glyph
// at a time from the baked font's advances when there is.
void draw_text(ImDrawList* draw, ImFont* font, f32 px, ImVec2 at, ImU32 colour, const char* text,
               f32 spacing) {
  if (spacing == 0.0f) {
    draw->AddText(font, px, at, colour, text);
    return;
  }
  ImFontBaked* baked = font->GetFontBaked(px);
  const char* s = text;
  const char* end = text + std::strlen(text);
  f32 x = at.x;
  while (s < end) {
    unsigned int c = 0;
    const int n = ImTextCharFromUtf8(&c, s, end);
    s += n > 0 ? n : 1;
    if (c == 0) break;
    font->RenderChar(draw, px, ImVec2(x, at.y), colour, static_cast<ImWchar>(c));
    x += baked->GetCharAdvance(static_cast<ImWchar>(c)) + spacing;
  }
}

// The box `draw_text` fills: the font's own measure, or the advances plus the spacing between.
ImVec2 text_size(ImFont* font, f32 px, const char* text, f32 spacing) {
  ImVec2 size = font->CalcTextSizeA(px, FLT_MAX, 0.0f, text);
  if (spacing == 0.0f) return size;
  ImFontBaked* baked = font->GetFontBaked(px);
  const char* s = text;
  const char* end = text + std::strlen(text);
  f32 width = 0.0f;
  u32 count = 0;
  while (s < end) {
    unsigned int c = 0;
    const int n = ImTextCharFromUtf8(&c, s, end);
    s += n > 0 ? n : 1;
    if (c == 0) break;
    width += baked->GetCharAdvance(static_cast<ImWchar>(c));
    ++count;
  }
  size.x = width + (count > 1 ? spacing * static_cast<f32>(count - 1) : 0.0f);
  return size;
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
  const f32 w = static_cast<f32>(width_);
  const f32 h = static_cast<f32>(height_);
  // The point a box of `box_w` x `box_h` stands on for the element's anchor, offset and
  // alignment, at whole pixels.
  const auto place = [&](const GameHud::Element& e, f32 box_w, f32 box_h, f32& x0, f32& y0) {
    const u32 a = static_cast<u32>(e.anchor);
    const u32 column = a % 3;  // left, centre, right
    const u32 row = a / 3;     // top, middle, bottom
    f32 x = column == 0 ? margin_x : column == 1 ? w * 0.5f : w - margin_x;
    f32 y = row == 0 ? margin_y : row == 1 ? h * 0.5f : h - margin_y;
    x += e.offset_px[0] * scale_;
    y += e.offset_px[1] * scale_;
    x0 = std::round(e.align == hotload::HudAlign::Left     ? x
                    : e.align == hotload::HudAlign::Centre ? x - box_w * 0.5f
                                                           : x - box_w);
    y0 = std::round(row == 0 ? y : row == 1 ? y - box_h * 0.5f : y - box_h);
  };
  for (u32 i = 0; i < hud.element_count; ++i) {
    const GameHud::Element& e = hud.elements[i];
    Placed placed{e.id, HudRect{}, 0.0f};
    if (e.kind == GameHud::Kind::Image) {
      // **An image** (ABI 6): its box from the drawn size (or the file's own, or one side from the
      // other by the aspect), its nine-slice borders at the HUD's scale, shrunk together when the
      // box is smaller than its corners, and one textured quad per non-empty cell.
      const GameHud::File& file = hud.images[e.image];
      if (e.image < GameHud::k_images) image_used_[e.image] = true;
      if (!e.visible || file.width == 0 || file.height == 0) {
        placed_[placed_count_++] = placed;
        continue;
      }
      const f32 fw = static_cast<f32>(file.width);
      const f32 fh = static_cast<f32>(file.height);
      f32 iw = e.image_size_px[0];
      f32 ih = e.image_size_px[1];
      if (iw == 0.0f && ih == 0.0f) {
        iw = fw;
        ih = fh;
      } else if (iw == 0.0f) {
        iw = ih * fw / fh;
      } else if (ih == 0.0f) {
        ih = iw * fh / fw;
      }
      const f32 box_w = std::round(iw * scale_);
      const f32 box_h = std::round(ih * scale_);
      f32 x0 = 0.0f;
      f32 y0 = 0.0f;
      place(e, box_w, box_h, x0, y0);
      const ImTextureID texture = k_image_texture_base + static_cast<ImTextureID>(e.image);
      const ImU32 tint = IM_COL32(e.tint[0], e.tint[1], e.tint[2], e.tint[3]);
      f32 l = std::round(e.slice_px[0] * scale_);
      f32 t = std::round(e.slice_px[1] * scale_);
      f32 r = std::round(e.slice_px[2] * scale_);
      f32 b = std::round(e.slice_px[3] * scale_);
      if (l + r > box_w && l + r > 0.0f) {
        const f32 k = box_w / (l + r);
        l = std::floor(l * k);
        r = std::floor(r * k);
      }
      if (t + b > box_h && t + b > 0.0f) {
        const f32 k = box_h / (t + b);
        t = std::floor(t * k);
        b = std::floor(b * k);
      }
      const f32 xs[4] = {x0, x0 + l, x0 + box_w - r, x0 + box_w};
      const f32 ys[4] = {y0, y0 + t, y0 + box_h - b, y0 + box_h};
      const f32 us[4] = {0.0f, e.slice_px[0] / fw, 1.0f - e.slice_px[2] / fw, 1.0f};
      const f32 vs[4] = {0.0f, e.slice_px[1] / fh, 1.0f - e.slice_px[3] / fh, 1.0f};
      for (u32 row = 0; row < 3; ++row) {
        for (u32 col = 0; col < 3; ++col) {
          if (xs[col + 1] <= xs[col] || ys[row + 1] <= ys[row]) continue;
          draw->AddImage(ImTextureRef(texture), ImVec2(xs[col], ys[row]),
                         ImVec2(xs[col + 1], ys[row + 1]), ImVec2(us[col], vs[row]),
                         ImVec2(us[col + 1], vs[row + 1]), tint);
        }
      }
      placed.rect = HudRect{x0, y0, x0 + box_w, y0 + box_h};
      placed_[placed_count_++] = placed;
      continue;
    }
    if (!e.visible || e.text[0] == '\0') {
      placed_[placed_count_++] = placed;
      continue;
    }
    // The element's font: the HUD's own, or one the module loaded (ABI 6), the HUD's own again when
    // that one could not be added.
    ImFont* font = ImGui::GetFont();
    if (e.font > 0 && e.font <= GameHud::k_fonts && fonts_[e.font - 1] != nullptr)
      font = fonts_[e.font - 1];
    const f32 px = hud_element_px(e.size_px, height_, display_scale_);
    const f32 outline =
        e.outline_px > 0.0f ? std::max(1.0f, std::round(e.outline_px * scale_)) : 0.0f;
    const f32 spacing = e.letter_spacing_px * scale_;
    const bool panel = e.panel == hotload::HudPanel::Solid;
    const f32 margin = panel ? std::round(e.panel_padding_px * scale_) : outline;
    const ImVec2 text = text_size(font, px, e.text, spacing);
    const f32 box_w = text.x + 2.0f * margin;
    const f32 box_h = text.y + 2.0f * margin;
    f32 x0 = 0.0f;
    f32 y0 = 0.0f;
    place(e, box_w, box_h, x0, y0);
    if (panel) {
      draw->AddRectFilled(
          ImVec2(x0, y0), ImVec2(x0 + box_w, y0 + box_h),
          IM_COL32(e.panel_color[0], e.panel_color[1], e.panel_color[2], e.panel_color[3]),
          std::round(4.0f * scale_));
    }
    const ImVec2 at(x0 + margin, y0 + margin);
    // The drop shadow (ABI 6): the text once in the shadow's colour at its offset, under the
    // outline and the text, at whole pixels.
    if (e.shadow_offset_px[0] != 0.0f || e.shadow_offset_px[1] != 0.0f) {
      const ImVec2 shadow(at.x + std::round(e.shadow_offset_px[0] * scale_),
                          at.y + std::round(e.shadow_offset_px[1] * scale_));
      draw_text(
          draw, font, px, shadow,
          IM_COL32(e.shadow_color[0], e.shadow_color[1], e.shadow_color[2], e.shadow_color[3]),
          e.text, spacing);
    }
    if (outline > 0.0f) {
      // The outline is the text drawn eight times round its place in the outline's colour: what a
      // caption on a bright sky needs to be read, at no cost to the atlas (the same glyphs).
      const ImU32 oc =
          IM_COL32(e.outline_color[0], e.outline_color[1], e.outline_color[2], e.outline_color[3]);
      for (i32 dy = -1; dy <= 1; ++dy) {
        for (i32 dx = -1; dx <= 1; ++dx) {
          if (dx == 0 && dy == 0) continue;
          draw_text(
              draw, font, px,
              ImVec2(at.x + static_cast<f32>(dx) * outline, at.y + static_cast<f32>(dy) * outline),
              oc, e.text, spacing);
        }
      }
    }
    draw_text(draw, font, px, at, IM_COL32(e.color[0], e.color[1], e.color[2], e.color[3]), e.text,
              spacing);
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
  const auto pair = [](const f32(&v)[2]) {
    JsonValue values = JsonValue::array();
    values.push_back(JsonValue(static_cast<f64>(v[0])));
    values.push_back(JsonValue(static_cast<f64>(v[1])));
    return values;
  };
  for (u32 i = 0; i < hud.element_count; ++i) {
    const GameHud::Element& el = hud.elements[i];
    JsonValue e = JsonValue::object();
    e.set("id", el.id);
    if (el.kind == GameHud::Kind::Image) {
      // An image (ABI 6): its file as the module named it, the file's own size, how it is drawn.
      const GameHud::File& file = hud.images[el.image];
      e.set("kind", "image");
      e.set("anchor", hud_anchor_name(el.anchor));
      e.set("align", hud_align_name(el.align));
      e.set("offset_px", pair(el.offset_px));
      e.set("path", std::string_view(file.path));
      JsonValue own = JsonValue::array();
      own.push_back(JsonValue(static_cast<u64>(file.width)));
      own.push_back(JsonValue(static_cast<u64>(file.height)));
      e.set("image_px", std::move(own));
      e.set("size_px", pair(el.image_size_px));
      JsonValue slice = JsonValue::array();
      for (const f32 v : el.slice_px)
        slice.push_back(JsonValue(static_cast<f64>(v)));
      e.set("slice_px", std::move(slice));
      e.set("tint", colour_json(el.tint));
      e.set("visible", el.visible);
      e.set("tick", el.tick);
      e.set("bounds_px", rect_json(bounds(el.id)));
      out.push_back(std::move(e));
      continue;
    }
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
    // ABI 6's styled text: the font (0 the HUD's own) and its file, the spacing and the shadow.
    e.set("font", el.font);
    if (el.font > 0 && el.font <= hud.font_count)
      e.set("font_path", std::string_view(hud.fonts[el.font - 1].path));
    e.set("letter_spacing_px", static_cast<f64>(el.letter_spacing_px));
    e.set("shadow_offset_px", pair(el.shadow_offset_px));
    e.set("shadow_color", colour_json(el.shadow_color));
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
  for (u32 i = 0; i < GameHud::k_images; ++i) {
    image_texture_[i] = renderer::OverlayPass::k_no_texture;
    image_used_[i] = false;
  }
  for (ImFont*& f : fonts_)
    f = nullptr;
  fonts_loaded_ = 0;
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

// **The fonts a module named** (ABI 6), added to the atlas between frames, once each, in the order
// named. ImGui makes the first font added the default, so the HUD's own is added first when nothing
// else has been — the call its first frame would have made, at the same style — and stays what the
// stack and every element without a font draw with.
void GameHudOverlay::load_fonts(const GameHud& hud) {
  if (fonts_loaded_ >= hud.font_count) return;
  ImFontAtlas* atlas = ImGui::GetIO().Fonts;
  if (atlas->Fonts.Size == 0) atlas->AddFontDefault();
  for (; fonts_loaded_ < hud.font_count; ++fonts_loaded_) {
    const GameHud::File& file = hud.fonts[fonts_loaded_];
    fonts_[fonts_loaded_] = nullptr;
    if (file.status != hotload::Result::Ok || file.resolved[0] == '\0') continue;
    ImFontConfig config;
    config.FontDataOwnedByAtlas = true;
    fonts_[fonts_loaded_] = atlas->AddFontFromFileTTF(file.resolved, file.size_px, &config);
  }
}

u32 GameHudOverlay::image_textures() const noexcept {
  u32 n = 0;
  for (const u32 t : image_texture_) {
    if (t != renderer::OverlayPass::k_no_texture) ++n;
  }
  return n;
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
  hud_ = &hud;
  for (bool& used : image_used_)
    used = false;
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
  load_fonts(hud);
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
bool GameHudOverlay::upload(OverlayTextures& textures, std::string& error) {
  if (context_ == nullptr) return true;
  ImGui::SetCurrentContext(context_);
  ImDrawData* data = ImGui::GetDrawData();
  if (data == nullptr) return true;
  if (data->Textures != nullptr) {
    for (ImTextureData* tex : *data->Textures) {
      if (tex->Status == ImTextureStatus_WantCreate || tex->Status == ImTextureStatus_WantUpdates) {
        const i64 start = time::monotonic_ns();
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
        ++atlas_uploads_;
        atlas_ns_ += time::monotonic_ns() - start;
      } else if (tex->Status == ImTextureStatus_WantDestroy && tex->UnusedFrames > 0) {
        if (tex->TexID != ImTextureID_Invalid)
          textures.remove_texture(static_cast<u32>(tex->TexID - 1));
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
      if (tex >= k_image_texture_base) {
        // An image element's (ABI 6): its texture, made the first time a layout draws it.
        const u32 image = static_cast<u32>(tex - k_image_texture_base);
        if (!image_texture(textures, image, error)) return false;
        commands_[c].texture = image_texture_[image];
      } else {
        commands_[c].texture = tex == ImTextureID_Invalid ? renderer::OverlayPass::k_no_texture
                                                          : static_cast<u32>(tex - 1);
      }
      ++c;
    }
  }
  release_images(textures);
  u32 kept = 0;
  for (u32 i = 0; i < commands_.size(); ++i) {
    if (commands_[i].texture == renderer::OverlayPass::k_no_texture) continue;
    commands_[kept++] = commands_[i];
  }
  commands_.resize(kept);
  return true;
}

// An image's overlay texture (ABI 6): decoded from the file the runner found, uploaded once, kept
// while an element names it. The upload is a blocking submission in the HUD's build, before the
// frame is recorded, the first time a frame draws the image — never in a steady frame.
bool GameHudOverlay::image_texture(OverlayTextures& textures, u32 image,
                                   std::string& error) {
  if (image >= GameHud::k_images || hud_ == nullptr || image >= hud_->image_count) {
    error = "an image element names no image the runner holds";
    return false;
  }
  if (image_texture_[image] != renderer::OverlayPass::k_no_texture) return true;
  const GameHud::File& file = hud_->images[image];
  image::Image decoded;
  if (image::read_image(file.resolved, decoded, 4, &error) != io::Status::Ok) {
    error = "the HUD image " + std::string(file.path) + " cannot be read: " + error;
    return false;
  }
  const u32 id =
      textures.add_texture(decoded.width, decoded.height, decoded.pixels.data(), &error);
  if (id == renderer::OverlayPass::k_no_texture) return false;
  image_texture_[image] = id;
  ++image_uploads_;
  return true;
}

// The textures of images no element of the last layout named: removed with the element, which
// waits for the device (a removal, never a frame's cost).
void GameHudOverlay::release_images(OverlayTextures& textures) {
  for (u32 i = 0; i < GameHud::k_images; ++i) {
    if (image_used_[i] || image_texture_[i] == renderer::OverlayPass::k_no_texture) continue;
    textures.remove_texture(image_texture_[i]);
    image_texture_[i] = renderer::OverlayPass::k_no_texture;
  }
}

bool GameHudOverlay::build(const GameHud& hud, const JsonValue* result, u32 width, u32 height,
                           f32 display_scale, OverlayTextures& textures,
                           renderer::OverlayDrawData& out, std::string& error) {
  out = renderer::OverlayDrawData{};
  if (!layout(hud, result, width, height, display_scale)) return false;
  if (vertices_.empty()) {
    release_images(textures);
    return true;
  }
  if (!upload(textures, error)) return false;
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
  out.set("images", image_textures());
  out.set("image_uploads", image_uploads_);
  out.set("fonts", fonts_loaded_);
  return out;
}

}  // namespace engine::view
