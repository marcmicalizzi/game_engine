#include <core/base/macros.h>
#include <core/platform/random.h>

#if ENGINE_PLATFORM_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// windows.h first: bcrypt.h uses its types.
#include <bcrypt.h>
#else
#include <cerrno>
#include <sys/random.h>
#endif

namespace engine::platform {

bool secure_random(std::span<u8> out) noexcept {
#if ENGINE_PLATFORM_WINDOWS
  // BCryptGenRandom takes a ULONG count; a secret is a few dozen bytes, but a caller asking for
  // more than four gigabytes gets it in pieces rather than a truncated count.
  usize done = 0;
  while (done < out.size()) {
    const usize left = out.size() - done;
    const ULONG count = left > 0x40000000u ? 0x40000000u : static_cast<ULONG>(left);
    if (BCryptGenRandom(nullptr, out.data() + done, count, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
      return false;
    }
    done += count;
  }
  return true;
#else
  usize done = 0;
  while (done < out.size()) {
    const ssize_t got = getrandom(out.data() + done, out.size() - done, 0);
    if (got < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    done += static_cast<usize>(got);
  }
  return true;
#endif
}

}  // namespace engine::platform
