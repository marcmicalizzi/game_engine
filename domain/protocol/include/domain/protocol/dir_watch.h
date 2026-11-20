#pragma once

// A directory-change notice for one file of one directory (docs/subsystems/protocol.md, "The
// ledger's events"; roadmap A55): the operating system's own — `ReadDirectoryChangesW` on Windows,
// inotify on Linux — on a thread of its own that sleeps until the system has something to say, so
// nothing wakes while nothing happens. What it reports is a hint to look, never the decision: a
// notice may name a write that changed nothing, notices for a burst of writes arrive together or
// apart, and the system may drop them (and says so: an overflowed buffer, `IN_Q_OVERFLOW`), so
// whoever is told reads the file and decides from what it holds.
//
// Here rather than in foundation/io because its users are this module's: the ledger's watch
// (session.h) and, when it is built, the review subject's asset roots (roadmap A56). A watcher of
// anything else moves it down.
//
// Where the system gives no notice — the directory cannot be opened for one, the platform has
// none — the watch looks once a second instead, logs that it does, and says so in `polling()`.

#include <core/base/macros.h>
#include <core/base/types.h>

#include <memory>
#include <string>

namespace engine::protocol {

class DirWatch {
 public:
  // Called on the watch's own thread after every notice that names the file, every notice the
  // system says it dropped, and (polling) once a second. It must not wait on anything a thread
  // that stops the watch may hold: `stop` joins the thread.
  using Notify = void (*)(void* context);

  DirWatch() noexcept;
  ~DirWatch();
  ENGINE_NON_COPYABLE(DirWatch);

  // Starts watching `native_dir` (not its subdirectories) for a change to the file named `name`
  // in it (created, written, renamed over, removed). A second start stops the first. Returns only
  // once the system's watch is registered, or the watch has fallen back to looking (`polling`), so
  // a caller that reads the file after it misses no later write.
  void start(std::string native_dir, std::string name, Notify notify, void* context);
  // Tests only: the next start's thread waits `ms` before it registers with the system, which
  // widens the gap a start that returned before its registration left open.
  static void delay_registration_for_test(u32 ms) noexcept;
  // Stops and joins the thread; nothing is called after it returns. Idempotent.
  void stop() noexcept;
  bool running() const noexcept;
  // The system gave no notice for the directory, so the watch looks once a second.
  bool polling() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace engine::protocol
