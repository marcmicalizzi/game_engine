// foundation/net (docs/subsystems/net.md): the loopback-only address rule, the HTTP subset, the
// WebSocket frame codec against RFC 6455's own examples, and a real server and client on loopback
// — echo, fragmentation with a ping in the middle, a refused upgrade's close code, an unmasked
// client frame refused, two connections at once, and the static-path guard.
#include <foundation/net/http.h>
#include <foundation/net/server.h>
#include <foundation/net/socket.h>
#include <foundation/net/websocket.h>

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

using namespace engine;
using namespace engine::net;

namespace {

// An echo handler: "refuse" in the query refuses the upgrade with 1008, a GET answers its path.
class EchoHandler final : public ServerHandler {
 public:
  void http(const HttpRequest& request, HttpResponse& response) override {
    response.content_type = "text/plain";
    response.body = "path " + request.path;
  }
  bool accept(const HttpRequest& request, u16& close, std::string& reason) override {
    std::string value;
    if (request.query_param("refuse", value)) {
      close = close_code::k_policy;
      reason = "refused: " + value;
      return false;
    }
    return true;
  }
  std::string message(u64, std::string_view text) override {
    messages.fetch_add(1);
    return "echo:" + std::to_string(text.size()) + ":" + std::string(text.substr(0, 16));
  }
  std::atomic<u32> messages{0};
};

}  // namespace

TEST_CASE("net: a listener address must be loopback, and says why when it is not") {
  u16 port = 1;
  std::string error;
  CHECK(parse_loopback_address("127.0.0.1:0", port, error));
  CHECK(port == 0);
  CHECK(parse_loopback_address("localhost:52341", port, error));
  CHECK(port == 52341);
  for (const char* bad : {"0.0.0.0:8080", "192.168.1.10:80", "example.com:80", "[::]:80"}) {
    error.clear();
    CHECK_FALSE(parse_loopback_address(bad, port, error));
    CHECK(error.find("loopback only") != std::string::npos);
  }
  CHECK_FALSE(parse_loopback_address("127.0.0.1:65536", port, error));
  CHECK_FALSE(parse_loopback_address("127.0.0.1:http", port, error));
  CHECK_FALSE(parse_loopback_address("127.0.0.1", port, error));
}

TEST_CASE("net: an HTTP request head parses, and malformed ones do not") {
  HttpRequest r;
  REQUIRE(parse_http_request(
      "GET /shell/a%20b.js?token=x%2By+z&n=1 HTTP/1.1\r\nHost: 127.0.0.1:1\r\nX-Thing:  v  \r\n",
      r));
  CHECK(r.method == "GET");
  CHECK(r.path == "/shell/a b.js");
  CHECK(r.query == "token=x%2By+z&n=1");
  REQUIRE(r.header("x-thing") != nullptr);
  CHECK(*r.header("x-thing") == "v");
  std::string token;
  REQUIRE(r.query_param("token", token));
  CHECK(token == "x+y z");
  CHECK_FALSE(r.query_param("absent", token));
  CHECK_FALSE(parse_http_request("GET http://elsewhere/ HTTP/1.1\r\n", r));  // not origin-form
  CHECK_FALSE(parse_http_request("GET / HTTP/1.1\r\nno colon here\r\n", r));
  CHECK_FALSE(parse_http_request("GET /%zz HTTP/1.1\r\n", r));
  CHECK_FALSE(parse_http_request("GET /%00 HTTP/1.1\r\n", r));
  CHECK_FALSE(parse_http_request("GET / HTTP/1.1\r\nA: b\r\n folded\r\n", r));
}

TEST_CASE("net: a static path never leaves its directory") {
  CHECK(safe_relative_path("index.html"));
  CHECK(safe_relative_path("js/main.js"));
  for (const char* bad :
       {"", "/etc/passwd", "../x", "a/../../x", "a/./b", "a//b", "a\\b", "C:/x", "a/b/", ".."}) {
    CHECK_MESSAGE(!safe_relative_path(bad), bad);
  }
  CHECK(content_type_for("a/b.JS") == "text/javascript; charset=utf-8");
  CHECK(content_type_for("x.png") == "image/png");
  CHECK(content_type_for("x.exe") == "application/octet-stream");
}

TEST_CASE("net: frames as RFC 6455 section 5.7 spells them") {
  // A single-frame unmasked text message "Hello".
  std::string out;
  encode_frame(Opcode::Text, "Hello", true, nullptr, out);
  CHECK(out == std::string("\x81\x05Hello", 7));
  // The same, masked with 37 fa 21 3d.
  const u8 mask[4] = {0x37, 0xfa, 0x21, 0x3d};
  out.clear();
  encode_frame(Opcode::Text, "Hello", true, mask, out);
  CHECK(out == std::string("\x81\x85\x37\xfa\x21\x3d\x7f\x9f\x4d\x51\x58", 11));
  Frame f;
  usize used = 0;
  u16 code = 0;
  std::string why;
  REQUIRE(decode_frame(out, 1 << 20, f, used, code, why) == FrameParse::Complete);
  CHECK(used == 11);
  CHECK(f.masked);
  CHECK(f.payload == "Hello");
  // 256 bytes takes the 16-bit length, 65536 the 64-bit one; a truncated buffer needs more.
  out.clear();
  encode_frame(Opcode::Binary, std::string(256, 'x'), true, nullptr, out);
  CHECK(out.substr(0, 4) == std::string("\x82\x7e\x01\x00", 4));
  out.clear();
  encode_frame(Opcode::Binary, std::string(65536, 'x'), true, nullptr, out);
  CHECK(out.substr(0, 10) == std::string("\x82\x7f\x00\x00\x00\x00\x00\x01\x00\x00", 10));
  CHECK(decode_frame(std::string_view(out).substr(0, 100), u64{1} << 20, f, used, code, why) ==
        FrameParse::NeedMore);
  CHECK(decode_frame(out, 1000, f, used, code, why) == FrameParse::Error);
  CHECK(code == close_code::k_too_big);
  // A reserved bit (what permessage-deflate would set) and a long ping are errors.
  CHECK(decode_frame(std::string("\xc1\x00", 2), 100, f, used, code, why) == FrameParse::Error);
  CHECK(code == close_code::k_protocol_error);
  std::string long_ping;
  encode_frame(Opcode::Ping, std::string(126, 'p'), true, nullptr, long_ping);
  CHECK(decode_frame(long_ping, 1000, f, used, code, why) == FrameParse::Error);
}

TEST_CASE("net: UTF-8 is checked as RFC 6455 requires of text") {
  CHECK(valid_utf8("plain"));
  CHECK(valid_utf8("\xc3\xa9t\xc3\xa9 \xe2\x82\xac \xf0\x9f\x98\x80"));
  CHECK_FALSE(valid_utf8("\xc0\xaf"));          // overlong '/'
  CHECK_FALSE(valid_utf8("\xed\xa0\x80"));      // a surrogate
  CHECK_FALSE(valid_utf8("\xf4\x90\x80\x80"));  // above U+10FFFF
  CHECK_FALSE(valid_utf8("\xe2\x82"));          // truncated
}

TEST_CASE("net: a server and a client on loopback") {
  EchoHandler handler;
  Server server;
  std::string error;
  REQUIRE_MESSAGE(server.start(0, handler, error), error);
  REQUIRE(server.port() != 0);

  SUBCASE("echo, then a fragmented 200 KB message with a ping between every two frames") {
    WebSocket ws;
    REQUIRE_MESSAGE(websocket_connect(server.port(), "/", {}, ws, error), error);
    REQUIRE(ws.send_text("hello"));
    Message m;
    REQUIRE(ws.receive(m, 10000));
    CHECK(m.data == "echo:5:hello");
    std::string big(200 * 1024, 'a');
    big.replace(0, 7, "{\"big\":");
    REQUIRE(ws.send_fragmented(Opcode::Text, big, 4096, true));
    REQUIRE(ws.receive(m, 10000));
    CHECK(m.data == "echo:204800:{\"big\":aaaaaaaaa");
    ws.close(close_code::k_normal, "done");
    CHECK(ws.close_code() == close_code::k_normal);
  }

  SUBCASE("a refused upgrade completes the handshake and closes with the handler's code") {
    WebSocket ws;
    REQUIRE_MESSAGE(websocket_connect(server.port(), "/?refuse=no%20token", {}, ws, error), error);
    Message m;
    CHECK_FALSE(ws.receive(m, 10000));
    CHECK(ws.close_code() == close_code::k_policy);
    CHECK(ws.close_reason() == "refused: no token");
  }

  SUBCASE("a client frame without a mask is a protocol error") {
    WebSocket ws;
    REQUIRE_MESSAGE(websocket_connect(server.port(), "/", {}, ws, error), error);
    std::string raw;
    encode_frame(Opcode::Text, "unmasked", true, nullptr, raw);
    REQUIRE(ws.socket().send_all(raw));
    Message m;
    CHECK_FALSE(ws.receive(m, 10000));
    CHECK(ws.close_code() == close_code::k_protocol_error);
  }

  SUBCASE("two connections are served at once") {
    WebSocket a;
    WebSocket b;
    REQUIRE_MESSAGE(websocket_connect(server.port(), "/", {}, a, error), error);
    REQUIRE_MESSAGE(websocket_connect(server.port(), "/", {}, b, error), error);
    // b is answered while a stays open and idle: one connection never waits on another.
    REQUIRE(b.send_text("second"));
    Message m;
    REQUIRE(b.receive(m, 10000));
    CHECK(m.data == "echo:6:second");
    REQUIRE(a.send_text("first"));
    REQUIRE(a.receive(m, 10000));
    CHECK(m.data == "echo:5:first");
    CHECK(server.connections() >= 2);
  }

  SUBCASE("a plain GET is the handler's, and a bad upgrade is a 400") {
    Socket s;
    REQUIRE_MESSAGE(connect_loopback(server.port(), s, error), error);
    REQUIRE(s.send_all(std::string_view("GET /x%20y HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n")));
    std::string reply;
    u8 chunk[1024];
    while (s.wait_readable(10000) > 0) {
      const i64 n = s.receive(std::span<u8>(chunk, sizeof(chunk)));
      if (n <= 0) break;
      reply.append(reinterpret_cast<const char*>(chunk), static_cast<usize>(n));
    }
    CHECK(reply.starts_with("HTTP/1.1 200 OK\r\n"));
    CHECK(reply.ends_with("path /x y"));
    u32 status = 0;
    WebSocket ws;
    const std::pair<std::string, std::string> wrong[] = {{"X-Unused", "1"}};
    CHECK(websocket_connect(server.port(), "/", wrong, ws, error, &status));  // extra headers ok
  }

  server.stop();
  CHECK(server.connections() == 0);
}

namespace {

// Answers "ok" and remembers which connection asked, so a test can push to it.
class PushHandler final : public ServerHandler {
 public:
  void http(const HttpRequest&, HttpResponse& response) override { response.body = "no"; }
  bool accept(const HttpRequest&, u16&, std::string&) override { return true; }
  std::string message(u64 connection, std::string_view) override {
    last.store(connection);
    return "ok";
  }
  void closed(u64) override { closed_count.fetch_add(1); }
  std::atomic<u64> last{0};
  std::atomic<u32> closed_count{0};
};

}  // namespace

TEST_CASE("net: a server pushes to a connection, bounded, and never waits on a peer") {
  PushHandler handler;
  Server server;
  std::string error;
  REQUIRE_MESSAGE(server.start(0, handler, error), error);
  const auto hello = [&](WebSocket& ws) {
    REQUIRE_MESSAGE(websocket_connect(server.port(), "/", {}, ws, error), error);
    REQUIRE(ws.send_text("hi"));
    Message m;
    REQUIRE(ws.receive(m, 10000));
    CHECK(m.data == "ok");
    return handler.last.load();
  };

  // A connection that reads: what is posted to it arrives, in order.
  WebSocket reader;
  const u64 id = hello(reader);
  CHECK(server.post(id, "first", 4) == Post::Queued);
  CHECK(server.post(id, "second", 4) == Post::Queued);
  Message m;
  REQUIRE(reader.receive(m, 10000));
  CHECK(m.data == "first");
  REQUIRE(reader.receive(m, 10000));
  CHECK(m.data == "second");
  CHECK(server.post(id + 1000, "nobody", 4) == Post::Gone);

  // A connection that never reads: its writer blocks once the socket's buffers are full, the
  // outbox fills to the bound, and a post is then refused at once rather than queued or waited on.
  WebSocket stalled;
  const u64 sid = hello(stalled);
  const std::string chunk(256 * 1024, 'x');
  // Posted until the outbox has stayed full for a fifth of a second: the writer is then blocked in
  // its send, not merely behind.
  u32 queued = 0;
  u32 full_in_a_row = 0;
  while (queued < 4000 && full_in_a_row < 10) {
    if (server.post(sid, chunk, 2) == Post::Queued) {
      ++queued;
      full_in_a_row = 0;
    } else {
      ++full_in_a_row;
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  }
  CHECK(full_in_a_row == 10);
  MESSAGE("a stalled connection took " << queued
                                       << " posts of 256 KB before its outbox of 2 stayed full");
  // The other connection is served all the while.
  REQUIRE(reader.send_text("again"));
  REQUIRE(reader.receive(m, 10000));
  CHECK(m.data == "ok");
  reader.close(close_code::k_normal, "done");

  // Stopping with the stalled writer blocked in its send returns: the send is woken (shutdown, and
  // on Windows the cancel), the writer joined, and every connection's end reported to the handler.
  server.stop();
  CHECK(server.connections() == 0);
  CHECK(handler.closed_count.load() == 2);
}
