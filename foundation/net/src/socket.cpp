#include <foundation/net/socket.h>

#include <cstring>
#include <limits>

#if ENGINE_PLATFORM_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
// winsock2.h first: ws2tcpip.h builds on it.
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace engine::net {

namespace {

#if ENGINE_PLATFORM_WINDOWS
using Native = SOCKET;
Native native_of(std::intptr_t h) noexcept { return static_cast<SOCKET>(static_cast<UINT_PTR>(h)); }
std::intptr_t handle_of(Native s) noexcept { return static_cast<std::intptr_t>(s); }
bool is_invalid(Native s) noexcept { return s == INVALID_SOCKET; }

// Winsock is started once per process and never stopped: the process's exit is its cleanup, and a
// WSACleanup racing a connection thread's last send would be a bug with nothing to gain.
bool start_sockets() noexcept {
  static const bool started = [] {
    WSADATA data{};
    return WSAStartup(MAKEWORD(2, 2), &data) == 0;
  }();
  return started;
}

std::string last_error(const char* what) {
  return std::string(what) + " failed (WSA error " + std::to_string(WSAGetLastError()) + ")";
}

Native open_stream_socket() noexcept {
  // WSA_FLAG_NO_HANDLE_INHERIT: a child process must not keep the port open (socket.h).
  return WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0,
                    WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
}

void close_native(Native s) noexcept { ::closesocket(s); }
#else
using Native = int;
Native native_of(std::intptr_t h) noexcept { return static_cast<int>(h); }
std::intptr_t handle_of(Native s) noexcept { return static_cast<std::intptr_t>(s); }
bool is_invalid(Native s) noexcept { return s < 0; }
bool start_sockets() noexcept { return true; }

std::string last_error(const char* what) {
  return std::string(what) + " failed (errno " + std::to_string(errno) + ")";
}

Native open_stream_socket() noexcept { return ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0); }

void close_native(Native s) noexcept { ::close(s); }
#endif

// Small frames go out at once: a JSON-RPC answer is one write, and Nagle's delay would be the
// largest part of a loopback round trip.
void set_no_delay(Native s) noexcept {
  int one = 1;
  (void)::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
}

sockaddr_in loopback_address(u16 port) noexcept {
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  return address;
}

}  // namespace

bool Socket::send_all(std::span<const u8> bytes) noexcept {
  if (!valid()) return false;
  usize sent = 0;
  while (sent < bytes.size()) {
    const usize left = bytes.size() - sent;
    const int chunk = left > (1u << 30) ? (1 << 30) : static_cast<int>(left);
#if ENGINE_PLATFORM_WINDOWS
    const int n =
        ::send(native_of(handle_), reinterpret_cast<const char*>(bytes.data() + sent), chunk, 0);
#else
    // MSG_NOSIGNAL: a peer that went away is an error to return, not a SIGPIPE that ends the host.
    const ssize_t n =
        ::send(native_of(handle_), bytes.data() + sent, static_cast<size_t>(chunk), MSG_NOSIGNAL);
    if (n < 0 && errno == EINTR) continue;
#endif
    if (n <= 0) return false;
    sent += static_cast<usize>(n);
  }
  return true;
}

i64 Socket::receive(std::span<u8> out) noexcept {
  if (!valid()) return -1;
  const int want = out.size() > (1u << 30) ? (1 << 30) : static_cast<int>(out.size());
  for (;;) {
#if ENGINE_PLATFORM_WINDOWS
    const int n = ::recv(native_of(handle_), reinterpret_cast<char*>(out.data()), want, 0);
#else
    const ssize_t n = ::recv(native_of(handle_), out.data(), static_cast<size_t>(want), 0);
    if (n < 0 && errno == EINTR) continue;
#endif
    return n < 0 ? -1 : static_cast<i64>(n);
  }
}

i32 Socket::wait_readable(i32 timeout_ms) noexcept {
  if (!valid()) return -1;
#if ENGINE_PLATFORM_WINDOWS
  WSAPOLLFD fd{};
  fd.fd = native_of(handle_);
  fd.events = POLLRDNORM;
  const int n = ::WSAPoll(&fd, 1, timeout_ms);
#else
  pollfd fd{};
  fd.fd = native_of(handle_);
  fd.events = POLLIN;
  int n = 0;
  do {
    n = ::poll(&fd, 1, timeout_ms);
  } while (n < 0 && errno == EINTR);
#endif
  if (n < 0) return -1;
  return n == 0 ? 0 : 1;
}

void Socket::shutdown_both() noexcept { shutdown_socket(handle_); }

void Socket::close() noexcept {
  if (!valid()) return;
  close_native(native_of(handle_));
  handle_ = k_invalid;
}

void shutdown_socket(std::intptr_t handle) noexcept {
  if (handle == Socket::k_invalid) return;
#if ENGINE_PLATFORM_WINDOWS
  (void)::shutdown(native_of(handle), SD_BOTH);
  // shutdown wakes a thread blocked in recv on Windows, but not one blocked in send to a peer that
  // stopped reading (a stalled subscriber, Server::post); cancelling the handle's pending I/O does,
  // for any thread. Linux's shutdown wakes both.
  (void)::CancelIoEx(reinterpret_cast<HANDLE>(native_of(handle)), nullptr);
#else
  (void)::shutdown(native_of(handle), SHUT_RDWR);
#endif
}

bool parse_loopback_address(std::string_view text, u16& port, std::string& error) {
  const usize colon = text.rfind(':');
  if (colon == std::string_view::npos) {
    error = "expected <host>:<port>, such as 127.0.0.1:0; got '" + std::string(text) + "'";
    return false;
  }
  const std::string_view host = text.substr(0, colon);
  const std::string_view digits = text.substr(colon + 1);
  if (host != "127.0.0.1" && host != "localhost") {
    error = "a listener binds loopback only (127.0.0.1 or localhost), never '" + std::string(host) +
            "': any other address is reachable from the network and raises a firewall prompt per "
            "executable path, and those prompts have taken the GPU driver down (AGENTS.md, the "
            "listener rule)";
    return false;
  }
  u32 value = 0;
  if (digits.empty() || digits.size() > 5) {
    error = "the port must be a number in 0..65535; got '" + std::string(digits) + "'";
    return false;
  }
  for (const char c : digits) {
    if (c < '0' || c > '9') {
      error = "the port must be a number in 0..65535; got '" + std::string(digits) + "'";
      return false;
    }
    value = value * 10 + static_cast<u32>(c - '0');
  }
  if (value > 65535u) {
    error = "the port must be a number in 0..65535; got '" + std::string(digits) + "'";
    return false;
  }
  port = static_cast<u16>(value);
  return true;
}

bool Listener::open_loopback(u16 port, std::string& error) {
  close();
  if (!start_sockets()) {
    error = "the socket library did not start";
    return false;
  }
  const Native s = open_stream_socket();
  if (is_invalid(s)) {
    error = last_error("socket");
    return false;
  }
  socket_ = Socket(handle_of(s));
#if ENGINE_PLATFORM_WINDOWS
  // Nobody else may bind the same port while this listener holds it.
  int one = 1;
  (void)::setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&one),
                     sizeof(one));
#else
  // A host restarted on the same port is not refused for the minutes TIME_WAIT lasts.
  int one = 1;
  (void)::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#endif
  const sockaddr_in address = loopback_address(port);
  if (::bind(s, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
    error = last_error("bind") + " on 127.0.0.1:" + std::to_string(port);
    close();
    return false;
  }
  if (::listen(s, SOMAXCONN) != 0) {
    error = last_error("listen");
    close();
    return false;
  }
  sockaddr_in bound{};
#if ENGINE_PLATFORM_WINDOWS
  int length = sizeof(bound);
#else
  socklen_t length = sizeof(bound);
#endif
  if (::getsockname(s, reinterpret_cast<sockaddr*>(&bound), &length) != 0) {
    error = last_error("getsockname");
    close();
    return false;
  }
  port_ = ntohs(bound.sin_port);
  return true;
}

bool Listener::accept(i32 timeout_ms, Socket& out, std::string& error) {
  error.clear();
  const i32 ready = socket_.wait_readable(timeout_ms);
  if (ready < 0) {
    error = last_error("poll");
    return false;
  }
  if (ready == 0) return false;
#if ENGINE_PLATFORM_WINDOWS
  const Native s = ::accept(native_of(socket_.native()), nullptr, nullptr);
  if (!is_invalid(s)) {
    (void)::SetHandleInformation(reinterpret_cast<HANDLE>(s), HANDLE_FLAG_INHERIT, 0);
  }
#else
  const Native s = ::accept4(native_of(socket_.native()), nullptr, nullptr, SOCK_CLOEXEC);
#endif
  if (is_invalid(s)) {
    error = last_error("accept");
    return false;
  }
  set_no_delay(s);
  out = Socket(handle_of(s));
  return true;
}

bool connect_loopback(u16 port, Socket& out, std::string& error) {
  if (!start_sockets()) {
    error = "the socket library did not start";
    return false;
  }
  const Native s = open_stream_socket();
  if (is_invalid(s)) {
    error = last_error("socket");
    return false;
  }
  Socket socket(handle_of(s));
  const sockaddr_in address = loopback_address(port);
  if (::connect(s, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
    error = last_error("connect") + " to 127.0.0.1:" + std::to_string(port);
    return false;
  }
  set_no_delay(s);
  out = std::move(socket);
  return true;
}

}  // namespace engine::net
