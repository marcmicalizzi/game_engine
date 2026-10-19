#pragma once

// **A game's HUD, provisional** (docs/subsystems/apps.md, "The HUD, provisional"; roadmap P41,
// P44): the lines and meters a `--game` module said on its last tick (game_runner.h `GameHud`),
// laid out by Dear ImGui as one fixed panel at the top left of the picture's safe region and handed
// to the renderer's overlay pass (`FrameDesc::overlay`, ADR-0054) — the same path engine-editor's
// tools UI takes to the screen. Text lines and horizontal meters, no input, nothing interactive;
// and, in a live window whose module ended the run, the result panel in the picture's centre.
//
// **This is a proof-of-concept HUD, not the game UI framework.** ADR-0054 decided the *tools* UI is
// ImGui through the overlay and said in so many words that a tool's overlay is not a HUD; plan 04
// §4.7 and 10 §10.6 R45 defer the game UI framework. What a game needs from a HUD — layout that
// scales, fonts, images, localisation, HDR composition at paper white — is that framework's, and
// none of it is here. This draws what a fixture needs to be played and judged, through a path that
// already exists, and is meant to be replaced, not grown.
//
// **It scales with the picture** (P44). The font is rasterized at `hud_font_px(height, scale)` —
// `k_hud_font_px` on a 1080-pixel-high picture at 100 %, other heights and display scales in
// proportion, clamped to [10, 64] — and the meters' widths, the name column and the panel's padding
// follow it, so a 450-pixel test capture and a 2160-pixel 4K window are both legible. ImGui 1.92's
// fonts are dynamic: a size is baked into the atlas when it is first drawn at and the atlas
// texture is re-uploaded then (a resize), never per frame; a frame whose size did not change costs
// what it did.
//
// **It costs nothing without a module**: nothing creates it, no ImGui context exists, and no frame
// names an overlay. With one, it is one ImGui frame a drawn frame (a panel of a few lines), its
// lists copied into vectors that keep their capacity.

#include "game_runner.h"

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/json/json_value.h>
#include <systems/renderer/overlay.h>

#include <string>

struct ImGuiContext;

namespace engine::renderer {
class SceneRenderer;
}

namespace engine::view {

// **The HUD's font on a 1080-pixel-high picture at 100 %**, in pixels. Why 18: the panel is laid
// inside a television's action-safe region (5 % in from each edge, 90 % of the picture), and
// broadcast practice puts the smallest readable caption at about 1/40 of the picture height — 27 px
// at 1080p — for a viewer three picture heights away; a desk is about half that distance, so 18 px
// (1/60 of the height) subtends what a caption does from the sofa, with margin, while sixteen lines
// at ImGui's leading take about 375 px, a third of the safe region's 972, and leave the picture to
// the picture. The clamp's floor is where ProggyClean stops being readable; its ceiling keeps a
// tiny window on a large display from drawing one line across it.
inline constexpr f32 k_hud_font_px = 18.0f;
inline constexpr f32 k_hud_font_min_px = 10.0f;
inline constexpr f32 k_hud_font_max_px = 64.0f;
// The font for a `height`-pixel picture on a display at `display_scale` (1 at 100 %, 1.5 at 150 %;
// an offscreen frame's is 1), whole pixels, clamped.
f32 hud_font_px(u32 height, f32 display_scale) noexcept;
// **An element's font** (`hotload::HudElement::size_px`, ABI 4): its size in pixels of a
// 1080-pixel-high picture at 100 %, times the HUD's scale, whole pixels, never under
// `k_hud_font_min_px` (the host's accessibility floor, the stack's) nor over
// `k_hud_element_max_px` (a headline's 64 px at 1080p, on a 4K picture; every size drawn is baked
// into the font atlas once, so the ceiling bounds the atlas too).
inline constexpr f32 k_hud_element_max_px = 128.0f;
f32 hud_element_px(f32 size_px, u32 height, f32 display_scale) noexcept;

// **The report's and the summary's `hud_elements`** (apps.md "--report"): every element of `hud`
// — the stack's lines and meters under their reserved ids, then the module's elements — with its
// last state and where `overlay`'s last layout drew it (`bounds_px`, [x0, y0, x1, y1], empty
// [0, 0, 0, 0] when it drew nothing there or there is no overlay).
class GameHudOverlay;
JsonValue hud_elements_json(const GameHud& hud, const GameHudOverlay* overlay);

// A rectangle of the picture, in pixels (x1, y1 exclusive); empty when nothing was drawn.
struct HudRect {
  f32 x0 = 0.0f;
  f32 y0 = 0.0f;
  f32 x1 = 0.0f;
  f32 y1 = 0.0f;
  bool empty() const noexcept { return x1 <= x0 || y1 <= y0; }
};

class GameHudOverlay {
 public:
  GameHudOverlay() = default;
  ~GameHudOverlay();
  GameHudOverlay(const GameHudOverlay&) = delete;
  GameHudOverlay& operator=(const GameHudOverlay&) = delete;

  bool create();
  void destroy() noexcept;
  bool valid() const noexcept { return context_ != nullptr; }

  // This frame's draw lists for `hud` over a `width` x `height` target on a display at
  // `display_scale`, into `out` (valid until the next call). Makes and retires the font atlas's
  // overlay texture as ImGui asks. An empty HUD draws nothing (`out` names no commands). With
  // `result` — `end_run`'s object, in a live window that stays on the run's end (apps.md "--game",
  // "Ending") — a **result panel** is drawn in the picture's centre beside the HUD: the result's
  // fields formatted, and the keys that quit or start a new run. False with `error` when a texture
  // could not be made.
  bool build(const GameHud& hud, const JsonValue* result, u32 width, u32 height, f32 display_scale,
             renderer::SceneRenderer& renderer, renderer::OverlayDrawData& out, std::string& error);
  // The two halves of `build`, for a test with no device: the ImGui frame — the font size applied,
  // the panels laid out, the lists copied — and then the atlas's textures made and retired through
  // the renderer. `layout` leaves texture requests pending (`pending_textures`); its commands name
  // only textures already made.
  bool layout(const GameHud& hud, const JsonValue* result, u32 width, u32 height,
              f32 display_scale);
  bool upload(renderer::SceneRenderer& renderer, std::string& error);

  // Commands drawn by the last `build`.
  u32 commands() const noexcept { return commands_.size(); }
  // What the last layout drew: the font it used, where the HUD panel and the result panel are.
  f32 font_px() const noexcept { return font_px_; }
  const HudRect& panel() const noexcept { return panel_; }
  const HudRect& result_panel() const noexcept { return result_panel_; }
  // The atlas's cost: texture creations or updates ImGui asked for (one at the first frame, one
  // more each time the font's size changed), what the last layout left pending, and the wall time
  // the uploads took in all.
  u32 atlas_uploads() const noexcept { return atlas_uploads_; }
  u32 pending_textures() const noexcept { return pending_textures_; }
  f64 atlas_ms() const noexcept { return static_cast<f64>(atlas_ns_) / 1.0e6; }
  u64 layouts() const noexcept { return layouts_; }
  // The summary's `interactive.game.overlay` block.
  JsonValue summary_json() const;
  // Where the last layout drew the element `id` (an element's own, or a reserved id of the stack's
  // lines and meters); empty when it drew nothing there.
  HudRect bounds_of(u32 id) const noexcept;
  // The HUD scale the last layout used: the picture's height over 1080 times the display's scale.
  f32 hud_scale() const noexcept { return scale_; }

 private:
  void apply_font(f32 px);
  void draw_elements(const GameHud& hud, f32 margin_x, f32 margin_y);

  // The rectangles of the last layout, by id: the stack's lines and meters, then the elements.
  struct Placed {
    u32 id = 0;
    HudRect rect;
    f32 font_px = 0.0f;
  };
  static constexpr u32 k_placed = GameHud::k_lines + GameHud::k_meters + GameHud::k_elements;
  Placed placed_[k_placed];
  u32 placed_count_ = 0;
  f32 scale_ = 1.0f;
  f32 display_scale_ = 1.0f;

  ImGuiContext* context_ = nullptr;
  Vector<renderer::OverlayVertex> vertices_;
  Vector<u32> indices_;
  Vector<renderer::OverlayCommand> commands_;
  f32 font_px_ = 0.0f;
  u32 width_ = 0;
  u32 height_ = 0;
  HudRect panel_;
  HudRect result_panel_;
  u32 atlas_uploads_ = 0;
  u32 pending_textures_ = 0;
  i64 atlas_ns_ = 0;
  u64 layouts_ = 0;
};

}  // namespace engine::view
