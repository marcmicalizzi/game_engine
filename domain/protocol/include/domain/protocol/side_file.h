#pragma once

// A file beside a document that several host processes read, change and write back
// (docs/subsystems/protocol.md, "Two hosts, one side file"): `leases.json`, `transactions.json`,
// and any other table a document keeps outside its layers. engine-cli starts a host for every
// call and an agent makes parallel calls as a matter of course, so two processes read one such
// file, change it in memory and write it whole — and without a lock the second writer silently
// drops what the first wrote (batch 68's review, 2026-11-18).
//
// Three pieces, plain enough for any store to use:
//
// - `SideFileLock`: an exclusive hold on `<file>.lock`, between processes and between two handles
//   of one process, taken through the operating system's own file lock (`LockFileEx`, `flock`), so
//   a host that dies lets go with its handles and nothing is ever left to judge stale or break.
//   The lock file is made once and never removed: removing it would let a third process lock a
//   new file while a second still held the old one.
// - `update_side_file`: the read-modify-write under that lock — read the file afresh (its type's
//   default when it is absent), let the caller change the value, write it back when the caller
//   says so (atomically, and not at all when the bytes are the ones there), release.
// - `fresh_side_file_id`: an id no counter in the file has to hand out — 53 random bits, so a
//   JSON number carries it exactly in every client — drawn again while the caller says it is
//   taken.
//
// A side file names no version of its type, so `read_side_file` reads the build's version first
// and then each older one through the registered migrations, newest first.

#include <core/base/macros.h>
#include <core/base/types.h>
#include <core/json/json.h>
#include <core/schema/json_reflect.h>
#include <domain/protocol/rpc.h>
#include <foundation/io/vfs.h>

#include <string>
#include <string_view>
#include <utility>

namespace engine::protocol {

// How long a caller waits for another holder before the call is refused (1005). A holder keeps
// the lock for one read-modify-write — a commit's includes the document's save — so a wait this
// long is a holder that has stopped, not one that is slow.
inline constexpr i64 k_side_file_lock_timeout_ms = 10'000;

class SideFileLock {
 public:
  SideFileLock() noexcept = default;
  ~SideFileLock() { release(); }
  ENGINE_NON_COPYABLE(SideFileLock);

  // Takes `<native_path>.lock`, making it when absent, waiting up to `timeout_ms` while another
  // holder has it. False, with the reason in `message`, when it cannot be made or the wait ran out.
  bool acquire(std::string_view native_path, i64 timeout_ms, std::string& message);
  void release() noexcept;
  bool held() const noexcept;

 private:
#if ENGINE_PLATFORM_WINDOWS
  void* handle_ = nullptr;  // a HANDLE; null when not held
#else
  int fd_ = -1;
#endif
};

// The lock for the side file at `path` (a Vfs path), as the RPC error a call answers (1005).
bool lock_side_file(const io::Vfs& vfs, std::string_view path, SideFileLock& lock, RpcError& error);

// An id in [1, 2^53) that `taken(context, id)` refuses to call taken.
u64 fresh_side_file_id(bool (*taken)(const void* context, u64 id), const void* context);

namespace detail {
// The file's JSON; `present` false (and `value` null) when there is none.
bool read_side_json(const io::Vfs& vfs, std::string_view path, JsonValue& value, bool& present,
                    RpcError& error);
// Atomically, and not at all when the bytes are the ones there.
bool write_side_json(const io::Vfs& vfs, std::string_view path, const JsonValue& value,
                     RpcError& error);
// Maps `value` onto `object` of `type` at the build's version, then at each older one.
bool map_side_json(const schema::TypeInfo& type, void* object, void (*reset)(void* object),
                   const JsonValue& value, std::string_view path, RpcError& error);
}  // namespace detail

// Reads the side file at `path` into `out`: its type's default, with `present` false, when there
// is none. No lock is taken here (a writer replaces the file whole, so a reader sees the old table
// or the new), but see `read_side_file_locked`.
template <class T>
bool read_side_file(const io::Vfs& vfs, std::string_view path, T& out, bool& present,
                    RpcError& error) {
  JsonValue value;
  out = T{};
  if (!detail::read_side_json(vfs, path, value, present, error)) return false;
  if (!present) return true;
  return detail::map_side_json(
      schema::type_of<T>(), &out, [](void* object) { *static_cast<T*>(object) = T{}; }, value, path,
      error);
}

// The same read under the file's lock, which is how every reader of a table written under it
// reads: on Windows a reader holding the file open makes the writer's rename over it fail (the
// first run of eight engine-cli processes at once lost two of eight to "permission denied" past
// the rename's retries), so a reader outside the lock is a writer's failure.
// A table that is not there is read as its default without the lock, so opening a document that
// has none makes no lock file in it (a writer makes the lock before the table).
template <class T>
bool read_side_file_locked(const io::Vfs& vfs, std::string_view path, T& out, bool& present,
                           RpcError& error) {
  if (!vfs.exists(path)) {
    out = T{};
    present = false;
    return true;
  }
  SideFileLock lock;
  if (!lock_side_file(vfs, path, lock, error)) return false;
  return read_side_file(vfs, path, out, present, error);
}

template <class T>
bool write_side_file(const io::Vfs& vfs, std::string_view path, const T& value, RpcError& error) {
  return detail::write_side_json(vfs, path, schema::to_json(value), error);
}

// The read-modify-write: under the file's lock, `value` is read afresh from the file, then
// `change(value, write)` runs — `bool(T&, bool& write)`, its result the call's — and `value` is
// written back when it set `write`, whether or not it succeeded (a refusal may still have dropped
// lapsed entries). A failed write is the call's error unless `change` had already set one.
template <class T, class Change>
bool update_side_file(const io::Vfs& vfs, std::string_view path, T& value, Change&& change,
                      RpcError& error) {
  SideFileLock lock;
  if (!lock_side_file(vfs, path, lock, error)) return false;
  bool present = false;
  if (!read_side_file(vfs, path, value, present, error)) return false;
  bool write = false;
  const bool ok = std::forward<Change>(change)(value, write);
  if (!write) return ok;
  RpcError write_error;
  if (!write_side_file(vfs, path, value, write_error)) {
    if (ok) error = std::move(write_error);
    return false;
  }
  return ok;
}

}  // namespace engine::protocol
