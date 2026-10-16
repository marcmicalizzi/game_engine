#pragma once

// **A game's HUD, provisional** (docs/subsystems/apps.md, "The HUD, provisional"; roadmap P41): the
// lines and meters a `--game` module said on its last tick (game_runner.h `GameHud`), laid out by
// Dear ImGui as one fixed panel at the top left of the picture's safe region and handed to the
// renderer's overlay pass (`FrameDesc::overlay`, ADR-0054) — the same path engine-editor's tools UI
// takes to the screen. Text lines and horizontal meters, no input, nothing interactive.
//
// **This is a proof-of-concept HUD, not the game UI framework.** ADR-0054 decided the *tools* UI is
// ImGui through the overlay and said in so many words that a tool's overlay is not a HUD; plan 04
// §4.7 and 10 §10.6 R45 defer the game UI framework. What a game needs from a HUD — layout that
// scales, fonts, images, localisation, HDR composition at paper white — is that framework's, and
// none of it is here. This draws what a fixture needs to be played and judged, through a path that
// already exists, and is meant to be replaced, not grown.
//
// **It costs nothing without a module**: nothing creates it, no ImGui context exists, and no frame
// names an overlay. With one, it is one ImGui frame a drawn frame (a panel of a few lines), its
// lists copied into vectors that keep their capacity.

#include "game_runner.h"

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <systems/renderer/overlay.h>

#include <string>

struct ImGuiContext;

namespace engine::renderer {
class SceneRenderer;
}

namespace engine::view {

class GameHudOverlay {
 public:
  GameHudOverlay() = default;
  ~GameHudOverlay();
  GameHudOverlay(const GameHudOverlay&) = delete;
  GameHudOverlay& operator=(const GameHudOverlay&) = delete;

  bool create();
  void destroy() noexcept;
  bool valid() const noexcept { return context_ != nullptr; }

  // This frame's draw lists for `hud` over a `width` x `height` target, into `out` (valid until the
  // next call). Makes and retires the font atlas's overlay texture as ImGui asks. An empty HUD
  // draws nothing (`out` names no commands). False with `error` when a texture could not be made.
  bool build(const GameHud& hud, u32 width, u32 height, renderer::SceneRenderer& renderer,
             renderer::OverlayDrawData& out, std::string& error);
  // Commands drawn by the last `build`.
  u32 commands() const noexcept { return commands_.size(); }

 private:
  ImGuiContext* context_ = nullptr;
  Vector<renderer::OverlayVertex> vertices_;
  Vector<u32> indices_;
  Vector<renderer::OverlayCommand> commands_;
};

}  // namespace engine::view
