#pragma once

// The platform's web view, embedded as a child of a native window (docs/subsystems/webview.md;
// roadmap A37). On Windows it is the Edge WebView2 runtime that Windows ships, reached through
// webview/webview's C API and its built-in loader, so no Microsoft binary is linked or shipped
// (cmake/EngineWebview.cmake). It is how engine-editor shows the web shell (apps/engine_shell)
// beside its Vulkan viewport: the page and the viewport tile and never overlap, and the page talks
// to the editor only through the protocol over the editor's own loopback listener — this module
// installs no bridge between the page and the process, so the same page runs in a plain browser.
//
// Everything here is main-thread only, and the thread must pump its window messages (the editor's
// SDL event loop does): WebView2 delivers its callbacks as messages.
//
//     webview::Webview view;
//     if (!view.create(window.os_handle(), {}, &error)) { /* no runtime: run without it */ }
//     view.set_rect(0, 0, width, height);
//     view.navigate("http://127.0.0.1:52341/?token=...");

#include <core/base/macros.h>
#include <core/base/types.h>

#include <string>

namespace engine::webview {

// Whether this machine can show a web view: the build has a backend and the platform's runtime is
// installed (on Windows, the WebView2 runtime's version is registered machine-wide or for the
// user, which is how Microsoft says to detect it). False with the reason otherwise — on a hosted
// CI runner, a Windows Server image without the runtime, or a Windows N edition.
bool runtime_available(std::string* why = nullptr);

struct WebviewOptions {
  // DevTools on a right click (F12): for a person working on the shell, never in a test.
  bool devtools = false;
};

class Webview {
 public:
  Webview() noexcept = default;
  ~Webview() { destroy(); }
  ENGINE_NON_COPYABLE(Webview);

  // Embeds a web view in `parent` (the platform's window handle: an HWND on Windows), hidden and
  // empty until `set_rect` gives it a place. Blocks, pumping the thread's messages, until the
  // runtime has made it (a few hundred milliseconds the first time). False with the reason when
  // the runtime is absent or refuses.
  bool create(void* parent, const WebviewOptions& options, std::string* error = nullptr);
  void destroy() noexcept;
  bool valid() const noexcept { return handle_ != nullptr; }

  bool navigate(const std::string& url) noexcept;
  // Where the view is in its parent, in the parent's coordinates; shows it. A click in the page
  // gives it the keyboard, as in any browser.
  void set_rect(i32 x, i32 y, u32 width, u32 height) noexcept;
  // The platform's handle of the view's own window (an HWND on Windows): a sibling of whatever else
  // the parent holds, for a host that orders its children.
  void* widget() const noexcept;

 private:
  void* handle_ = nullptr;  // webview_t
};

}  // namespace engine::webview
