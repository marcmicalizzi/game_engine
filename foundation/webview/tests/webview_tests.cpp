// foundation/webview (docs/subsystems/webview.md) with no window: the runtime check answers with a
// reason when it says no, and a web view refuses to be made without a parent rather than making a
// window of its own on the desktop. The view itself, in a window beside the editor's viewport, is
// engine-editor's `--embedded` test (apps/engine_editor/tests/editor_tests.cpp).

#include <foundation/webview/webview.h>

#include <doctest/doctest.h>

#include <string>

using namespace engine;

TEST_CASE("webview: the runtime check gives a reason when it says no") {
  std::string why;
  const bool available = webview::runtime_available(&why);
  if (available) {
    CHECK(why.empty());
  } else {
    CHECK_FALSE(why.empty());
    MESSAGE("no web view runtime here: " << why);
  }
}

TEST_CASE("webview: no parent, no web view, and nothing opens") {
  webview::Webview view;
  std::string error;
  CHECK_FALSE(view.create(nullptr, {}, &error));
  CHECK_FALSE(error.empty());
  CHECK_FALSE(view.valid());
  CHECK(view.widget() == nullptr);
  CHECK_FALSE(view.navigate("http://127.0.0.1:1/"));
  view.set_rect(0, 0, 10, 10);  // harmless without a view
}
