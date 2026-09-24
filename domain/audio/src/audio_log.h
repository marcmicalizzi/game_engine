#pragma once

// The capability's one log category, defined in audio.cpp. Clip decodes that fail, a device that
// would not open, a bus tree that was malformed — all under `audio`. Never from the audio thread:
// `Mixer::render` logs nothing, and the tests hold it to that with a counting sink.

#include <core/log/log.h>

namespace engine::audio {
ENGINE_LOG_CATEGORY_DECLARE(log_audio);
}  // namespace engine::audio
