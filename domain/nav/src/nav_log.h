#pragma once

// The module's log category, declared once and defined in tile_build.cpp. Nav logs rarely and
// never from inside a build: a rebuild runs on an efficiency worker and a message per tile would
// turn a background job into contention on the sink (docs/subsystems/log.md).

#include <core/log/log.h>

ENGINE_LOG_CATEGORY_DECLARE(log_nav);
