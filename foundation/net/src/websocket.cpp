#include <core/hash/base64.h>
#include <core/hash/sha1.h>
#include <core/platform/random.h>
#include <foundation/net/websocket.h>

namespace engine::net {

namespace {

char lower(char c) noexcept { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

bool equals_ci(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size()) return false;
  for (usize i = 0; i < a.size(); ++i) {
    if (lower(a[i]) != lower(b[i])) return false;
  }
  return true;
}

// Whether a comma-separated header value names `token`, ignoring case and blanks.
bool lists_token(std::string_view value, std::string_view token) noexcept {
  while (!value.empty()) {
    const usize comma = value.find(',');
    std::string_view item = value.substr(0, comma);
    while (!item.empty() && (item.front() == ' ' || item.front() == '\t'))
      item.remove_prefix(1);
    while (!item.empty() && (item.back() == ' ' || item.back() == '\t'))
      item.remove_suffix(1);
    if (equals_ci(item, token)) return true;
    if (comma == std::string_view::npos) break;
    value.remove_prefix(comma + 1);
  }
  return false;
}

// The close codes a peer may send (RFC 6455 §7.4): the defined ones that are not reserved for
// "no frame", and the ranges for libraries and applications.
bool sendable_close_code(u16 code) noexcept {
  return (code >= 1000 && code <= 1003) || (code >= 1007 && code <= 1011) ||
         (code >= 3000 && code <= 4999);
}

bool is_control(Opcode opcode) noexcept { return static_cast<u8>(opcode) >= 0x8; }

}  // namespace

std::string websocket_accept(std::string_view client_key) {
  Sha1 h;
  h.update(client_key);
  h.update(k_websocket_guid);
  const Sha1Digest digest = h.finish();
  return base64_encode(std::span<const u8>(digest.data(), digest.size()));
}

bool is_websocket_upgrade(const HttpRequest& request, std::string& why) {
  if (request.method != "GET") {
    why = "an upgrade is a GET";
    return false;
  }
  const std::string* upgrade = request.header("upgrade");
  if (upgrade == nullptr || !lists_token(*upgrade, "websocket")) {
    why = "no 'Upgrade: websocket'";
    return false;
  }
  const std::string* connection = request.header("connection");
  if (connection == nullptr || !lists_token(*connection, "upgrade")) {
    why = "no 'Connection: Upgrade'";
    return false;
  }
  const std::string* version = request.header("sec-websocket-version");
  if (version == nullptr || *version != "13") {
    why = "Sec-WebSocket-Version must be 13";
    return false;
  }
  const std::string* key = request.header("sec-websocket-key");
  std::string decoded;
  if (key == nullptr || !base64_decode(*key, decoded) || decoded.size() != 16) {
    why = "Sec-WebSocket-Key must be base64 of 16 bytes";
    return false;
  }
  return true;
}

std::string websocket_upgrade_response(const HttpRequest& request) {
  const std::string* key = request.header("sec-websocket-key");
  return "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
         "Sec-WebSocket-Accept: " +
         websocket_accept(key != nullptr ? *key : std::string()) + "\r\n\r\n";
}

void encode_frame(Opcode opcode, std::string_view payload, bool fin, const u8* mask,
                  std::string& out) {
  out.push_back(static_cast<char>((fin ? 0x80u : 0x00u) | static_cast<u8>(opcode)));
  const u8 mask_bit = mask != nullptr ? 0x80u : 0x00u;
  const u64 n = payload.size();
  if (n < 126) {
    out.push_back(static_cast<char>(mask_bit | static_cast<u8>(n)));
  } else if (n <= 0xffffu) {
    out.push_back(static_cast<char>(mask_bit | 126u));
    out.push_back(static_cast<char>((n >> 8) & 0xffu));
    out.push_back(static_cast<char>(n & 0xffu));
  } else {
    out.push_back(static_cast<char>(mask_bit | 127u));
    for (int shift = 56; shift >= 0; shift -= 8)
      out.push_back(static_cast<char>((n >> shift) & 0xffu));
  }
  if (mask == nullptr) {
    out.append(payload);
    return;
  }
  for (u32 i = 0; i < 4; ++i)
    out.push_back(static_cast<char>(mask[i]));
  const usize start = out.size();
  out.append(payload);
  for (usize i = 0; i < payload.size(); ++i)
    out[start + i] = static_cast<char>(static_cast<u8>(out[start + i]) ^ mask[i & 3]);
}

FrameParse decode_frame(std::string_view buffer, u64 max_payload, Frame& out, usize& consumed,
                        u16& close, std::string& why) {
  if (buffer.size() < 2) return FrameParse::NeedMore;
  const u8 b0 = static_cast<u8>(buffer[0]);
  const u8 b1 = static_cast<u8>(buffer[1]);
  if ((b0 & 0x70u) != 0) {
    close = close_code::k_protocol_error;
    why = "a reserved bit is set, and no extension was negotiated";
    return FrameParse::Error;
  }
  const u8 op = b0 & 0x0fu;
  if (op != 0x0 && op != 0x1 && op != 0x2 && op != 0x8 && op != 0x9 && op != 0xA) {
    close = close_code::k_protocol_error;
    why = "unknown opcode " + std::to_string(op);
    return FrameParse::Error;
  }
  out.fin = (b0 & 0x80u) != 0;
  out.opcode = static_cast<Opcode>(op);
  out.masked = (b1 & 0x80u) != 0;
  u64 length = b1 & 0x7fu;
  usize header = 2;
  if (length == 126) {
    if (buffer.size() < 4) return FrameParse::NeedMore;
    length = (u64{static_cast<u8>(buffer[2])} << 8) | u64{static_cast<u8>(buffer[3])};
    header = 4;
  } else if (length == 127) {
    if (buffer.size() < 10) return FrameParse::NeedMore;
    length = 0;
    for (usize i = 2; i < 10; ++i)
      length = (length << 8) | u64{static_cast<u8>(buffer[i])};
    if ((length >> 63) != 0) {
      close = close_code::k_protocol_error;
      why = "a 64-bit length with its top bit set";
      return FrameParse::Error;
    }
    header = 10;
  }
  if (is_control(out.opcode) && (!out.fin || length > 125)) {
    close = close_code::k_protocol_error;
    why = "a control frame must be final and at most 125 bytes";
    return FrameParse::Error;
  }
  if (length > max_payload) {
    close = close_code::k_too_big;
    why = "a frame of " + std::to_string(length) + " bytes is over the limit";
    return FrameParse::Error;
  }
  u8 mask[4] = {};
  if (out.masked) {
    if (buffer.size() < header + 4) return FrameParse::NeedMore;
    for (usize i = 0; i < 4; ++i)
      mask[i] = static_cast<u8>(buffer[header + i]);
    header += 4;
  }
  if (buffer.size() - header < length) return FrameParse::NeedMore;
  out.payload.assign(buffer.data() + header, static_cast<usize>(length));
  if (out.masked) {
    for (usize i = 0; i < out.payload.size(); ++i)
      out.payload[i] = static_cast<char>(static_cast<u8>(out.payload[i]) ^ mask[i & 3]);
  }
  consumed = header + static_cast<usize>(length);
  return FrameParse::Complete;
}

bool valid_utf8(std::string_view text) noexcept {
  usize i = 0;
  while (i < text.size()) {
    const u8 c = static_cast<u8>(text[i]);
    if (c < 0x80) {
      ++i;
      continue;
    }
    u32 need = 0;
    u32 code = 0;
    u32 min = 0;
    if ((c & 0xe0u) == 0xc0u) {
      need = 1;
      code = c & 0x1fu;
      min = 0x80;
    } else if ((c & 0xf0u) == 0xe0u) {
      need = 2;
      code = c & 0x0fu;
      min = 0x800;
    } else if ((c & 0xf8u) == 0xf0u) {
      need = 3;
      code = c & 0x07u;
      min = 0x10000;
    } else {
      return false;
    }
    if (i + need >= text.size()) return false;
    for (u32 k = 1; k <= need; ++k) {
      const u8 cc = static_cast<u8>(text[i + k]);
      if ((cc & 0xc0u) != 0x80u) return false;
      code = (code << 6) | (cc & 0x3fu);
    }
    if (code < min || code > 0x10ffffu || (code >= 0xd800u && code <= 0xdfffu)) return false;
    i += need + 1;
  }
  return true;
}

void WebSocket::fail(u16 code, std::string_view reason) {
  if (!closed_ && !close_sent_ && socket_.valid()) {
    std::string payload;
    payload.push_back(static_cast<char>(code >> 8));
    payload.push_back(static_cast<char>(code & 0xffu));
    payload.append(reason.substr(0, 123));
    (void)send_frame(Opcode::Close, payload, true);
  }
  close_sent_ = true;
  closed_ = true;
  close_code_ = code;
  close_reason_ = std::string(reason);
  socket_.shutdown_both();
}

bool WebSocket::send_frame(Opcode opcode, std::string_view payload, bool fin) {
  if (!socket_.valid() || closed_ || close_sent_) return false;
  std::string bytes;
  bytes.reserve(payload.size() + 14);
  if (role_ == Role::Client) {
    u8 mask[4] = {};
    if (!platform::secure_random(std::span<u8>(mask, 4))) return false;
    encode_frame(opcode, payload, fin, mask, bytes);
  } else {
    encode_frame(opcode, payload, fin, nullptr, bytes);
  }
  if (opcode == Opcode::Close) close_sent_ = true;
  return socket_.send_all(bytes);
}

bool WebSocket::send_fragmented(Opcode opcode, std::string_view data, usize fragment,
                                bool ping_between) {
  if (fragment == 0) fragment = data.size() > 0 ? data.size() : 1;
  usize offset = 0;
  bool first = true;
  do {
    const usize n = data.size() - offset < fragment ? data.size() - offset : fragment;
    const bool fin = offset + n >= data.size();
    if (!send_frame(first ? opcode : Opcode::Continuation, data.substr(offset, n), fin))
      return false;
    if (!fin && ping_between && !send_frame(Opcode::Ping, "mid-message", true)) return false;
    offset += n;
    first = false;
  } while (offset < data.size());
  return true;
}

bool WebSocket::next_frame(Frame& out, i32 timeout_ms) {
  u8 chunk[64 * 1024];
  for (;;) {
    usize consumed = 0;
    u16 code = 0;
    std::string why;
    const FrameParse parsed = decode_frame(buffer_, max_message_, out, consumed, code, why);
    if (parsed == FrameParse::Complete) {
      buffer_.erase(0, consumed);
      const bool want_masked = role_ == Role::Server;
      if (out.masked != want_masked) {
        fail(close_code::k_protocol_error,
             want_masked ? "a client's frames must be masked" : "a server's frames are not masked");
        return false;
      }
      return true;
    }
    if (parsed == FrameParse::Error) {
      fail(code, why);
      return false;
    }
    const i32 ready = socket_.wait_readable(timeout_ms);
    if (ready == 0) {
      fail(close_code::k_going_away, "timed out waiting for the peer");
      return false;
    }
    const i64 n = ready < 0 ? -1 : socket_.receive(std::span<u8>(chunk, sizeof(chunk)));
    if (n <= 0) {
      closed_ = true;
      close_code_ = close_code::k_abnormal;
      close_reason_ = "the connection ended without a close frame";
      return false;
    }
    buffer_.append(reinterpret_cast<const char*>(chunk), static_cast<usize>(n));
  }
}

bool WebSocket::receive(Message& out, i32 timeout_ms) {
  if (!socket_.valid() || closed_) return false;
  bool assembling = false;
  out.data.clear();
  for (;;) {
    Frame frame;
    if (!next_frame(frame, timeout_ms)) return false;
    switch (frame.opcode) {
      case Opcode::Ping: (void)send_frame(Opcode::Pong, frame.payload, true); continue;
      case Opcode::Pong: continue;
      case Opcode::Close: {
        u16 code = close_code::k_no_status;
        std::string reason;
        if (frame.payload.size() == 1) {
          fail(close_code::k_protocol_error, "a close frame's payload is a code or nothing");
          return false;
        }
        if (frame.payload.size() >= 2) {
          code = static_cast<u16>((static_cast<u8>(frame.payload[0]) << 8) |
                                  static_cast<u8>(frame.payload[1]));
          reason = frame.payload.substr(2);
          if (!sendable_close_code(code)) {
            fail(close_code::k_protocol_error, "the peer sent a close code nobody may send");
            return false;
          }
          if (!valid_utf8(reason)) {
            fail(close_code::k_invalid_data, "a close reason must be UTF-8");
            return false;
          }
        }
        if (!close_sent_) {
          // Echo the code back (RFC 6455 §5.5.1); a close without one is answered with 1000.
          const u16 echo = code == close_code::k_no_status ? close_code::k_normal : code;
          std::string payload;
          payload.push_back(static_cast<char>(echo >> 8));
          payload.push_back(static_cast<char>(echo & 0xffu));
          (void)send_frame(Opcode::Close, payload, true);
        }
        closed_ = true;
        close_code_ = code;
        close_reason_ = std::move(reason);
        socket_.shutdown_both();
        return false;
      }
      case Opcode::Text:
      case Opcode::Binary:
        if (assembling) {
          fail(close_code::k_protocol_error, "a new message began inside a fragmented one");
          return false;
        }
        out.opcode = frame.opcode;
        out.data = std::move(frame.payload);
        assembling = !frame.fin;
        break;
      case Opcode::Continuation:
        if (!assembling) {
          fail(close_code::k_protocol_error, "a continuation frame with no message to continue");
          return false;
        }
        if (out.data.size() + frame.payload.size() > max_message_) {
          fail(close_code::k_too_big, "the message is over the limit");
          return false;
        }
        out.data += frame.payload;
        assembling = !frame.fin;
        break;
    }
    if (!assembling) {
      if (out.opcode == Opcode::Text && !valid_utf8(out.data)) {
        fail(close_code::k_invalid_data, "a text message must be UTF-8");
        return false;
      }
      return true;
    }
  }
}

void WebSocket::close(u16 code, std::string_view reason, i32 linger_ms) {
  if (closed_ || !socket_.valid()) return;
  if (!close_sent_) {
    std::string payload;
    payload.push_back(static_cast<char>(code >> 8));
    payload.push_back(static_cast<char>(code & 0xffu));
    payload.append(reason.substr(0, 123));
    (void)send_frame(Opcode::Close, payload, true);
  }
  // Wait for the peer's close, dropping anything before it; a peer that never answers gets the
  // linger and no more.
  Frame frame;
  u8 chunk[4096];
  for (;;) {
    usize consumed = 0;
    u16 ignored = 0;
    std::string why;
    const FrameParse parsed = decode_frame(buffer_, max_message_, frame, consumed, ignored, why);
    if (parsed == FrameParse::Error) break;
    if (parsed == FrameParse::Complete) {
      buffer_.erase(0, consumed);
      if (frame.opcode == Opcode::Close) break;
      continue;
    }
    if (socket_.wait_readable(linger_ms) <= 0) break;
    const i64 n = socket_.receive(std::span<u8>(chunk, sizeof(chunk)));
    if (n <= 0) break;
    buffer_.append(reinterpret_cast<const char*>(chunk), static_cast<usize>(n));
  }
  closed_ = true;
  close_code_ = code;
  close_reason_ = std::string(reason);
  socket_.shutdown_both();
}

bool websocket_connect(u16 port, std::string_view target,
                       std::span<const std::pair<std::string, std::string>> headers, WebSocket& out,
                       std::string& error, u32* refused_status) {
  Socket socket;
  if (!connect_loopback(port, socket, error)) return false;
  u8 nonce[16] = {};
  if (!platform::secure_random(std::span<u8>(nonce, 16))) {
    error = "the system's random source did not answer";
    return false;
  }
  const std::string key = base64_encode(std::span<const u8>(nonce, 16));
  std::string request =
      "GET " + std::string(target) + " HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) +
      "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + key +
      "\r\nSec-WebSocket-Version: 13\r\n";
  for (const auto& h : headers)
    request += h.first + ": " + h.second + "\r\n";
  request += "\r\n";
  if (!socket.send_all(request)) {
    error = "could not send the upgrade request";
    return false;
  }
  // The response head, read like a request's.
  std::string buffer;
  u8 chunk[4096];
  usize end = std::string::npos;
  while ((end = buffer.find("\r\n\r\n")) == std::string::npos) {
    if (buffer.size() > k_max_http_head || socket.wait_readable(10000) <= 0) {
      error = "no answer to the upgrade request";
      return false;
    }
    const i64 n = socket.receive(std::span<u8>(chunk, sizeof(chunk)));
    if (n <= 0) {
      error = "the server closed the connection during the handshake";
      return false;
    }
    buffer.append(reinterpret_cast<const char*>(chunk), static_cast<usize>(n));
  }
  const std::string_view head = std::string_view(buffer).substr(0, end);
  const usize line_end = head.find("\r\n");
  const std::string_view status_line = head.substr(0, line_end);
  u32 status = 0;
  if (status_line.size() >= 12 && status_line.starts_with("HTTP/1.")) {
    for (usize i = 9; i < 12; ++i)
      status = status * 10 + static_cast<u32>(status_line[i] - '0');
  }
  if (status != 101) {
    if (refused_status != nullptr) *refused_status = status;
    error = "the server answered '" + std::string(status_line) + "'";
    return false;
  }
  std::string accept;
  bool upgraded = false;
  std::string_view rest =
      line_end == std::string_view::npos ? std::string_view() : head.substr(line_end + 2);
  while (!rest.empty()) {
    const usize eol = rest.find("\r\n");
    const std::string_view line = rest.substr(0, eol);
    rest = eol == std::string_view::npos ? std::string_view() : rest.substr(eol + 2);
    const usize colon = line.find(':');
    if (colon == std::string_view::npos) continue;
    std::string_view value = line.substr(colon + 1);
    while (!value.empty() && value.front() == ' ')
      value.remove_prefix(1);
    if (equals_ci(line.substr(0, colon), "sec-websocket-accept")) accept = std::string(value);
    if (equals_ci(line.substr(0, colon), "upgrade") && lists_token(value, "websocket"))
      upgraded = true;
  }
  if (!upgraded || accept != websocket_accept(key)) {
    error = "the server's handshake does not answer this key";
    return false;
  }
  out = WebSocket(std::move(socket), Role::Client, buffer.substr(end + 4));
  return true;
}

}  // namespace engine::net
