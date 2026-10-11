#pragma once

// RFC 6455 WebSocket, both ends (docs/subsystems/net.md): the opening handshake (SHA-1 and base64
// from core/hash), frames with fragmentation, ping and pong, close codes, and the masking rule —
// a client masks every frame it sends and a server refuses an unmasked one. **No extensions**
// (permessage-deflate is never offered or accepted, so a reserved bit set is a protocol error), no
// subprotocols, no TLS: it runs on loopback (socket.h) and carries JSON-RPC text.
//
// The client half is here, not in a test, for two reasons: the end-to-end tests drive engine-host
// through it, and a future engine-side client (a native window that is a client of a host, plan 06
// §6.13) needs it. Because a framing bug shared by this server and this client would be invisible
// to tests that use both, a second, independent client drives the same host in
// `tools/host-ws.Tests.ps1` (.NET's ClientWebSocket).

#include <core/base/types.h>
#include <foundation/net/http.h>
#include <foundation/net/socket.h>

#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace engine::net {

// RFC 6455 §1.3.
inline constexpr std::string_view k_websocket_guid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

// base64(SHA-1(key + GUID)): what the server's `Sec-WebSocket-Accept` must say for `key`.
std::string websocket_accept(std::string_view client_key);

// Whether `request` is a version-13 upgrade: GET, `Upgrade: websocket`, a `Connection` naming
// `upgrade`, `Sec-WebSocket-Version: 13`, and a `Sec-WebSocket-Key` that is base64 of 16 bytes.
// False with the reason otherwise.
bool is_websocket_upgrade(const HttpRequest& request, std::string& why);
// The 101 response to an upgrade `is_websocket_upgrade` accepted.
std::string websocket_upgrade_response(const HttpRequest& request);

// RFC 6455 §7.4.1.
namespace close_code {
inline constexpr u16 k_normal = 1000;
inline constexpr u16 k_going_away = 1001;
inline constexpr u16 k_protocol_error = 1002;
inline constexpr u16 k_unsupported_data = 1003;
inline constexpr u16 k_no_status = 1005;  // never sent: the peer's close carried no code
inline constexpr u16 k_abnormal = 1006;   // never sent: the connection ended without a close
inline constexpr u16 k_invalid_data = 1007;
inline constexpr u16 k_policy = 1008;
inline constexpr u16 k_too_big = 1009;
inline constexpr u16 k_internal = 1011;
}  // namespace close_code

enum class Opcode : u8 {
  Continuation = 0x0,
  Text = 0x1,
  Binary = 0x2,
  Close = 0x8,
  Ping = 0x9,
  Pong = 0xA,
};

struct Frame {
  bool fin = true;
  Opcode opcode = Opcode::Text;
  bool masked = false;
  std::string payload;  // unmasked
};

// Appends one frame to `out`. `mask` is the client's four masking bytes; null sends unmasked (a
// server). A control frame must be final and carry at most 125 bytes; the caller keeps to that.
void encode_frame(Opcode opcode, std::string_view payload, bool fin, const u8* mask,
                  std::string& out);

enum class FrameParse : u8 { Complete, NeedMore, Error };

// One frame from the front of `buffer`. Complete with `consumed` set, NeedMore when the buffer
// holds less than a frame, or Error with `close` the code to close with and `why` the reason: a
// reserved bit, an unknown opcode, a fragmented or oversized control frame, a 64-bit length with
// its top bit set, or a payload longer than `max_payload` (1009).
FrameParse decode_frame(std::string_view buffer, u64 max_payload, Frame& out, usize& consumed,
                        u16& close, std::string& why);

// Well-formed UTF-8 (no overlong forms, no surrogates, nothing above U+10FFFF), which RFC 6455
// §8.1 requires of a text message.
bool valid_utf8(std::string_view text) noexcept;

enum class Role : u8 { Server, Client };

struct Message {
  Opcode opcode = Opcode::Text;  // Text or Binary
  std::string data;
};

class WebSocket {
 public:
  WebSocket() noexcept = default;
  // A connection whose handshake is done. `leftover` is what arrived after the handshake's head.
  WebSocket(Socket socket, Role role, std::string leftover = {}) noexcept
      : socket_(std::move(socket)), role_(role), buffer_(std::move(leftover)) {}
  WebSocket(WebSocket&&) noexcept = default;
  WebSocket& operator=(WebSocket&&) noexcept = default;
  WebSocket(const WebSocket&) = delete;
  WebSocket& operator=(const WebSocket&) = delete;

  // The largest message accepted, fragments together (default 64 MiB); a larger one closes 1009.
  void set_max_message(u64 bytes) noexcept { max_message_ = bytes; }

  // The next text or binary message, reassembled from its fragments, with every ping in between
  // answered and every pong dropped. False when the connection has ended — the peer closed it
  // (its close is answered), it broke the protocol (closed with 1002, 1007 or 1009), or the socket
  // failed — and `close_code()` and `close_reason()` say how. `timeout_ms` bounds each wait for
  // bytes (negative: none); a wait that runs out ends the connection with 1001.
  bool receive(Message& out, i32 timeout_ms = -1);

  bool send_text(std::string_view text) { return send_message(Opcode::Text, text); }
  bool send_binary(std::string_view bytes) { return send_message(Opcode::Binary, bytes); }
  bool send_ping(std::string_view payload = {}) { return send_frame(Opcode::Ping, payload, true); }
  // One message as frames of at most `fragment` bytes, with a ping between every two when
  // `ping_between`: what the tests use to prove reassembly, and nothing else needs.
  bool send_fragmented(Opcode opcode, std::string_view data, usize fragment, bool ping_between);
  // Sends a close frame with `code` and `reason` (at most 123 bytes of it), once, and waits up to
  // `linger_ms` for the peer's close before the socket is shut. `close_code()` is then `code`.
  void close(u16 code, std::string_view reason, i32 linger_ms = 1000);

  bool open() const noexcept { return socket_.valid() && !closed_; }
  u16 close_code() const noexcept { return close_code_; }
  const std::string& close_reason() const noexcept { return close_reason_; }
  Socket& socket() noexcept { return socket_; }

 private:
  bool send_message(Opcode opcode, std::string_view data) { return send_frame(opcode, data, true); }
  bool send_frame(Opcode opcode, std::string_view payload, bool fin);
  // The next frame, reading as needed. False when the connection ended (close_code_ set).
  bool next_frame(Frame& out, i32 timeout_ms);
  void fail(u16 code, std::string_view reason);

  Socket socket_;
  Role role_ = Role::Server;
  std::string buffer_;
  u64 max_message_ = u64{64} << 20;
  bool close_sent_ = false;
  bool closed_ = false;
  u16 close_code_ = close_code::k_abnormal;
  std::string close_reason_;
};

// A client's opening handshake on a fresh loopback connection: GET `target` (path and query) with
// `headers` added, the response checked for 101 and the right accept value. False with `error`;
// when the server answered with something other than 101, `refused_status` is its status.
bool websocket_connect(u16 port, std::string_view target,
                       std::span<const std::pair<std::string, std::string>> headers, WebSocket& out,
                       std::string& error, u32* refused_status = nullptr);

}  // namespace engine::net
