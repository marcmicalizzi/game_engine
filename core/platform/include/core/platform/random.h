#pragma once

// The operating system's cryptographically secure random source: `BCryptGenRandom` with the
// system-preferred generator on Windows, `getrandom(2)` on Linux. For secrets — engine-host's
// per-session token for its WebSocket listener (docs/subsystems/net.md) is the first — and never
// for anything a simulation reads: a seeded generator is what makes a run reproducible, and this
// one is the opposite on purpose. It is a system call, so nothing on a frame path may call it.

#include <core/base/types.h>

#include <span>

namespace engine::platform {

// Fills every byte of `out`. False when the system will not answer, which on a working machine
// does not happen; a caller that needs a secret refuses to go on rather than using a weaker one.
bool secure_random(std::span<u8> out) noexcept;

}  // namespace engine::platform
