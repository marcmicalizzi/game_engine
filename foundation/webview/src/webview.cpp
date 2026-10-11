#include <core/log/log.h>
#include <foundation/webview/webview.h>

// The library's C API only: its implementation is compiled once, as third-party code, into
// engine_webview_core (cmake/EngineWebview.cmake), which catches its own exceptions at this API.
#define WEBVIEW_HEADER
#include <webview/webview.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace engine::webview {

ENGINE_LOG_CATEGORY_DEFINE(log_webview, "webview");

namespace {

#if defined(_WIN32)
// Microsoft's documented detection of the Evergreen WebView2 runtime: its client key's `pv` value,
// machine-wide (the 32-bit view on 64-bit Windows) or for the user, present and not 0.0.0.0.
bool runtime_version(std::string& out) {
  static constexpr const wchar_t* k_client =
      L"\\Microsoft\\EdgeUpdate\\Clients\\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}";
  struct Place {
    HKEY root;
    const wchar_t* prefix;
  };
  const Place places[] = {{HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node"},
                          {HKEY_LOCAL_MACHINE, L"SOFTWARE"},
                          {HKEY_CURRENT_USER, L"Software"}};
  for (const Place& place : places) {
    std::wstring key = place.prefix;
    key += k_client;
    wchar_t value[64] = {};
    DWORD bytes = sizeof(value);
    if (RegGetValueW(place.root, key.c_str(), L"pv", RRF_RT_REG_SZ, nullptr, value, &bytes) !=
        ERROR_SUCCESS)
      continue;
    std::string version;
    for (const wchar_t* c = value; *c != 0; ++c)
      version.push_back(static_cast<char>(*c & 0x7f));
    if (version.empty() || version == "0.0.0.0") continue;
    out = version;
    return true;
  }
  return false;
}
#endif

}  // namespace

bool runtime_available(std::string* why) {
#if defined(_WIN32)
  std::string version;
  if (runtime_version(version)) return true;
  if (why != nullptr)
    *why = "the Edge WebView2 runtime is not installed (no registered version under EdgeUpdate)";
  return false;
#else
  if (why != nullptr) *why = "this build has no web view backend on this platform";
  return false;
#endif
}

bool Webview::create(void* parent, const WebviewOptions& options, std::string* error) {
  destroy();
  std::string why;
  if (!runtime_available(&why)) {
    if (error != nullptr) *error = why;
    return false;
  }
  if (parent == nullptr) {
    if (error != nullptr) *error = "a web view needs a parent window";
    return false;
  }
  // The library takes the parent's HWND itself (or a pointer to one) and makes its own child
  // widget inside it, hidden until it is given a size.
  handle_ = webview_create(options.devtools ? 1 : 0, parent);
  if (handle_ == nullptr) {
    if (error != nullptr)
      *error = "the WebView2 runtime refused to make a web view (webview_create failed)";
    return false;
  }
#if defined(_WIN32)
  // The widget is a sibling of whatever else the parent holds (the editor's viewport): clip
  // against them, so where a sibling is above it the sibling's pixels show.
  if (HWND w = static_cast<HWND>(widget()); w != nullptr) {
    SetWindowLongPtrW(w, GWL_STYLE, GetWindowLongPtrW(w, GWL_STYLE) | WS_CLIPSIBLINGS);
  }
#endif
  ENGINE_LOG_INFO(log_webview, "web view created");
  return true;
}

void Webview::destroy() noexcept {
  if (handle_ == nullptr) return;
  webview_destroy(static_cast<webview_t>(handle_));
  handle_ = nullptr;
}

bool Webview::navigate(const std::string& url) noexcept {
  if (handle_ == nullptr) return false;
  return webview_navigate(static_cast<webview_t>(handle_), url.c_str()) == WEBVIEW_ERROR_OK;
}

void* Webview::widget() const noexcept {
  if (handle_ == nullptr) return nullptr;
  return webview_get_native_handle(static_cast<webview_t>(handle_),
                                   WEBVIEW_NATIVE_HANDLE_KIND_UI_WIDGET);
}

void Webview::set_rect(i32 x, i32 y, u32 width, u32 height) noexcept {
#if defined(_WIN32)
  HWND w = static_cast<HWND>(widget());
  if (w == nullptr) return;
  // The widget resizes the runtime's controller to its client area on WM_SIZE.
  MoveWindow(w, x, y, static_cast<int>(width), static_cast<int>(height), TRUE);
  if (!IsWindowVisible(w)) ShowWindow(w, SW_SHOWNA);
#else
  (void)x;
  (void)y;
  (void)width;
  (void)height;
#endif
}

}  // namespace engine::webview
