#pragma once

// The capability's one log category, defined in animation.cpp. Everything this capability says —
// a library load, a skeleton it could not attach, a pool that grew — goes under `animation`, so
// `--log animation=debug` follows a character from the glTF file to the bone matrices.

#include <core/log/log.h>

namespace engine::animation {
ENGINE_LOG_CATEGORY_DECLARE(log_animation);
}  // namespace engine::animation
