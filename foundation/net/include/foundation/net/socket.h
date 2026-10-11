#pragma once

// TCP sockets on the loopback interface, and nothing else (docs/subsystems/net.md).
//
// **Why there is no way to bind another address.** A listener on a non-loopback address is one
// Windows Firewall prompt per executable path, this tree makes paths cheaply (every preset, every
// agent's worktree), each pending prompt holds a GPU context, and enough of them stopped the driver
// surviving `vkCreateDevice` on 2026-09-18 (AGENTS.md, the listener rule). So `Listener` binds
// 127.0.0.1 and has no parameter that could say otherwise, `parse_loopback_address` refuses any
// other host with the reason, and a module that does not link this one cannot open a socket at
// all: the rule is the shape of the API rather than a check somebody has to remember. A user's
// opt-in to a reachable listener, when one is wanted, is a new function with its own name.
//
// Blocking sockets with a readiness wait: one thread per connection is what the host needs (a
// handful of local clients), and it keeps every read a plain loop. Sockets are made
// non-inheritable, so a process the host starts (`engine.run_tests`, the content build) never holds
// its port open after it exits.

#include <core/base/macros.h>
#include <core/base/types.h>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace engine::net {

class Socket {
 public:
  Socket() noexcept = default;
  explicit Socket(std::intptr_t handle) noexcept : handle_(handle) {}
  ~Socket() { close(); }
  Socket(Socket&& other) noexcept : handle_(other.handle_) { other.handle_ = k_invalid; }
  Socket& operator=(Socket&& other) noexcept {
    if (this != &other) {
      close();
      handle_ = other.handle_;
      other.handle_ = k_invalid;
    }
    return *this;
  }
  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;

  static constexpr std::intptr_t k_invalid = -1;

  bool valid() const noexcept { return handle_ != k_invalid; }
  std::intptr_t native() const noexcept { return handle_; }

  // Every byte, or false when the connection failed part way.
  bool send_all(std::span<const u8> bytes) noexcept;
  bool send_all(std::string_view bytes) noexcept {
    return send_all(std::span<const u8>(reinterpret_cast<const u8*>(bytes.data()), bytes.size()));
  }
  // Up to `out.size()` bytes: the count, 0 when the peer closed its side, -1 on an error.
  i64 receive(std::span<u8> out) noexcept;
  // 1 when a read would not block, 0 when `timeout_ms` passed first (negative: wait forever),
  // -1 on an error.
  i32 wait_readable(i32 timeout_ms) noexcept;
  // Ends both directions; a thread blocked in `receive` on this socket returns. Safe from another
  // thread while the socket is open.
  void shutdown_both() noexcept;
  void close() noexcept;

 private:
  std::intptr_t handle_ = k_invalid;
};

// Shuts a socket down by its handle, from a thread that does not own it (`Server::stop`).
void shutdown_socket(std::intptr_t handle) noexcept;

// "<host>:<port>" for a listener, where the host must be 127.0.0.1 or localhost. False, with a
// sentence that says why, for any other host — 0.0.0.0, a LAN address, a name — and for a port
// that is not a number in 0..65535. Port 0 asks the system for a free one.
bool parse_loopback_address(std::string_view text, u16& port, std::string& error);

class Listener {
 public:
  Listener() noexcept = default;
  ENGINE_NON_COPYABLE(Listener);
  ~Listener() { close(); }

  // Binds 127.0.0.1:`port` (0: any free port) and listens.
  bool open_loopback(u16 port, std::string& error);
  // The port bound, which is the system's choice after `open_loopback(0)`.
  u16 port() const noexcept { return port_; }
  bool is_open() const noexcept { return socket_.valid(); }
  // Waits up to `timeout_ms` for a connection. True with `out` connected; false when the time ran
  // out (`error` empty) or the listener failed (`error` says how).
  bool accept(i32 timeout_ms, Socket& out, std::string& error);
  void close() noexcept { socket_.close(); }

 private:
  Socket socket_;
  u16 port_ = 0;
};

// A connection to 127.0.0.1:`port`: the client side, for tests and for an engine-side client.
bool connect_loopback(u16 port, Socket& out, std::string& error);

}  // namespace engine::net
