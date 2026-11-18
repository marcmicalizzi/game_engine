#pragma once

// A read-modify-write of a side file that two processes cannot interleave (docs/subsystems/
// protocol.md, "The task ledger and review items"). Internal to the module.
//
// `leases.json` and `transactions.json` are read when a session opens and written whole from the
// host's memory, so two hosts on one document lose each other's writes (found by a review of the
// transactions work, roadmap A11). The ledger is never held in memory: every call takes
// `<file>.lock` exclusively, reads the file, changes it and writes it back, and only then lets go.
// The lock is the operating system's on an open handle (`LockFileEx` on Windows, `flock`
// elsewhere), so a host that dies holding it leaves nothing behind for anyone to break: the system
// lets go with the handle.
//
// To be joined with the helper roadmap A11's mend of the same fault writes for
// `transactions.json` and `leases.json`, once both are in: one function for every side file.

#include <core/base/types.h>

#include <string>

namespace engine::protocol::detail {

enum class ExclusiveRun : u8 {
  Ran,     // `body` ran with the lock held; its answer is in `body_result`
  Busy,    // another holder kept the lock past `wait_ms`; `body` did not run
  Failed,  // the lock file could not be opened or locked; `message` says why
};

// Runs `body(context)` while this process holds `lock_path` exclusively against every other
// process and every other handle of this one (the file is made when missing and never removed:
// removing it would let a waiter lock a file a newcomer no longer sees). Waits up to `wait_ms`,
// trying every two milliseconds.
ExclusiveRun run_exclusive(const std::string& lock_path, i64 wait_ms, bool (*body)(void*),
                           void* context, bool& body_result, std::string& message);

// The same for any callable `bool()`.
template <class F>
ExclusiveRun run_exclusive(const std::string& lock_path, i64 wait_ms, F& body, bool& body_result,
                           std::string& message) {
  return run_exclusive(
      lock_path, wait_ms, [](void* c) { return (*static_cast<F*>(c))(); }, &body, body_result,
      message);
}

}  // namespace engine::protocol::detail
