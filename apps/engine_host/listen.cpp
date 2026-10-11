#include "listen.h"

namespace engine::host {

bool HostCaptures::capture(std::string_view id, std::string_view part,
                           net::HttpResponse& response) {
  // Called under the dispatch lock (protocol::Listener), so the renderer's kept captures are read
  // as a method would read them.
  const RenderHost::KeptCapture* kept = state_->render.kept_capture(id);
  if (kept == nullptr) {
    response = net::http_error(404, "no kept capture by that id (the host keeps the last eight)");
    return false;
  }
  const Vector<u8>* bytes = nullptr;
  if (part.empty()) {
    bytes = &kept->png;
    response.content_type = "image/png";
  } else if (part == "ids") {
    bytes = &kept->ids;
    response.content_type = "application/octet-stream";
  } else {
    response = net::http_error(404);
    return false;
  }
  if (bytes->empty()) {
    response = net::http_error(404, "that capture has no such channel");
    return false;
  }
  response.status = 200;
  response.body.assign(reinterpret_cast<const char*>(bytes->data()), bytes->size());
  response.headers.push_back(
      {"X-Engine-Capture-Size", std::to_string(kept->width) + "x" + std::to_string(kept->height)});
  return true;
}

}  // namespace engine::host
