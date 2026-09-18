#pragma once

// The module's one log category, defined in settings.cpp. Everything the renderer says about a
// picture — a flag it had to override, a mesh it loaded, a cache entry it wrote — goes under
// `renderer`, so one `--log renderer=debug` follows a frame from the scene load to the summary
// whichever app is hosting it.

#include <core/log/log.h>

namespace engine::renderer {
ENGINE_LOG_CATEGORY_DECLARE(log_renderer);
}  // namespace engine::renderer
