// protocol::Listener (docs/subsystems/protocol.md, "Transport"), in process, through
// foundation/net's own client: the same assertions engine-host's transport tests make of
// `--listen`, made of the listener itself, which every host now runs (engine-host beside stdin,
// engine-editor in its window). The token both ways and refused without it or wrong, a page from
// another origin and a request naming another Host refused, the shell's files served and nothing
// outside them, a capture source asked under the host's lock, and a doc.apply through the
// dispatcher round trip. engine-host's transport_tests.cpp and tools/host-ws.Tests.ps1 (an
// independent .NET client) hold engine-host to the same behaviour on top of it.

#include <core/json/json.h>
#include <core/json/json_value.h>
#include <domain/protocol/listener.h>
#include <domain/protocol/rpc.h>
#include <domain/protocol/session.h>
#include <foundation/io/vfs.h>
#include <foundation/net/socket.h>
#include <foundation/net/websocket.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <fstream>
#include <mutex>
#include <string>

using namespace engine;

namespace {

struct HttpReply {
  u32 status = 0;
  std::string head;
  std::string body;
};

HttpReply http_get(u16 port, std::string_view target, std::string_view host = {}) {
  net::Socket s;
  std::string error;
  HttpReply reply;
  REQUIRE_MESSAGE(net::connect_loopback(port, s, error), error);
  const std::string host_line =
      host.empty() ? "127.0.0.1:" + std::to_string(port) : std::string(host);
  REQUIRE(
      s.send_all("GET " + std::string(target) + " HTTP/1.1\r\nHost: " + host_line + "\r\n\r\n"));
  std::string all;
  u8 chunk[16384];
  while (s.wait_readable(30000) > 0) {
    const i64 n = s.receive(std::span<u8>(chunk, sizeof(chunk)));
    if (n <= 0) break;
    all.append(reinterpret_cast<const char*>(chunk), static_cast<usize>(n));
  }
  const usize end = all.find("\r\n\r\n");
  REQUIRE(end != std::string::npos);
  reply.head = all.substr(0, end);
  reply.body = all.substr(end + 4);
  REQUIRE(reply.head.size() > 12);
  reply.status = static_cast<u32>(std::stoul(reply.head.substr(9, 3)));
  return reply;
}

// A host's captures, as engine-host's are: asked under the lock, so it checks that it holds.
struct Captures final : protocol::CaptureSource {
  std::mutex* lock = nullptr;
  u32 asked = 0;
  bool held = false;
  bool capture(std::string_view id, std::string_view part, net::HttpResponse& response) override {
    ++asked;
    held = !lock->try_lock();
    if (!held) lock->unlock();
    if (id != "c1" || !part.empty()) {
      response = net::http_error(404, "no such capture");
      return false;
    }
    response.status = 200;
    response.content_type = "image/png";
    response.body = "PNGBYTES";
    return true;
  }
};

JsonValue call(net::WebSocket& ws, std::string_view text) {
  REQUIRE(ws.send_text(text));
  net::Message m;
  REQUIRE_MESSAGE(ws.receive(m, 60000), "closed " << ws.close_code() << " " << ws.close_reason());
  JsonValue response;
  REQUIRE_MESSAGE(parse_json(m.data, response).ok, m.data);
  return response;
}

}  // namespace

TEST_CASE("listener: the protocol over a loopback WebSocket, with the token, from any host") {
  const test::TempDir tmp("protocol_listener");
  {
    std::ofstream f(tmp.file("index.html"), std::ios::binary);
    f << "<!doctype html><title>shell</title>";
  }
  io::Vfs vfs;
  protocol::SessionManager sessions(vfs);
  protocol::Dispatcher dispatcher(protocol::Context{&sessions, nullptr, nullptr, nullptr, nullptr});
  protocol::add_builtin_methods(dispatcher);
  std::mutex lock;
  Captures captures;
  captures.lock = &lock;
  protocol::Listener listener(dispatcher, lock, &captures);
  std::string error;
  REQUIRE_MESSAGE(listener.start(0, tmp.path(), "s1", error), error);
  const u16 port = listener.port();
  REQUIRE(port != 0);
  CHECK(listener.token().size() == 22);
  CHECK(listener.shell_url() ==
        "http://127.0.0.1:" + std::to_string(port) + "/?token=" + listener.token() + "&session=s1");
  JsonValue announcement;
  REQUIRE(parse_json(listener.announcement(), announcement).ok);
  CHECK(std::string(announcement.find("listen")->as_string()) ==
        "ws://127.0.0.1:" + std::to_string(port) + "/");
  CHECK(listener.announcement().find('\n') == std::string::npos);

  SUBCASE("the token is required, and must match") {
    net::WebSocket none;
    REQUIRE_MESSAGE(net::websocket_connect(port, "/", {}, none, error), error);
    net::Message m;
    CHECK_FALSE(none.receive(m, 30000));
    CHECK(none.close_code() == net::close_code::k_policy);
    CHECK(none.close_reason().find("token is required") != std::string::npos);
    net::WebSocket wrong;
    REQUIRE(net::websocket_connect(port, "/?token=AAAAAAAAAAAAAAAAAAAAAA", {}, wrong, error));
    CHECK_FALSE(wrong.receive(m, 30000));
    CHECK(wrong.close_code() == net::close_code::k_policy);
    CHECK(wrong.close_reason().find("does not match") != std::string::npos);
  }
  SUBCASE("a page from another origin is refused even with the token") {
    net::WebSocket ws;
    const std::pair<std::string, std::string> origin[] = {{"Origin", "http://evil.example"}};
    REQUIRE(net::websocket_connect(port, "/?token=" + listener.token(), origin, ws, error));
    net::Message m;
    CHECK_FALSE(ws.receive(m, 30000));
    CHECK(ws.close_code() == net::close_code::k_policy);
  }
  SUBCASE("the listener's own origin and the header form are accepted, and a request runs") {
    net::WebSocket ws;
    const std::pair<std::string, std::string> headers[] = {
        {"X-Engine-Token", listener.token()},
        {"Origin", "http://127.0.0.1:" + std::to_string(port)}};
    REQUIRE_MESSAGE(net::websocket_connect(port, "/", headers, ws, error), error);
    const JsonValue opened = call(
        ws, std::string(R"({"jsonrpc":"2.0","id":1,"method":"session.open","params":)") +
                R"({"path":)" + write_json(JsonValue(tmp.file("doc"))) + R"(,"create":true}})");
    const JsonValue* result = opened.find("result");
    REQUIRE_MESSAGE(result != nullptr, write_json(opened));
    const std::string session(result->find("session")->as_string());
    const JsonValue applied = call(
        ws, R"({"jsonrpc":"2.0","id":2,"method":"doc.apply","params":{"session":")" + session +
                R"(","commands":[{"kind":"CreateObject","id":"0000000000000000000000000000beef",)"
                R"("type":"engine.world.Node","value":{"name":"over the socket"}}],)"
                R"("attribution":{"actor":"t","role":"director","task":"t","rationale":"why"}}})");
    const JsonValue* applied_result = applied.find("result");
    REQUIRE_MESSAGE(applied_result != nullptr, write_json(applied));
    CHECK_MESSAGE(applied_result->find("committed")->as_bool(), write_json(applied));
    const JsonValue journal = call(ws, R"({"jsonrpc":"2.0","id":3,"method":"doc.journal",)"
                                       R"("params":{"session":")" +
                                           session + R"("}})");
    CHECK_MESSAGE(write_json(journal).find("\"why\"") != std::string::npos, write_json(journal));
    // A notification is dispatched and answered with nothing; the next request still is.
    REQUIRE(ws.send_text(R"({"jsonrpc":"2.0","method":"engine.info"})"));
    const JsonValue info = call(ws, R"({"jsonrpc":"2.0","id":4,"method":"engine.info"})");
    CHECK(write_json(*info.find("id")) == "4");
  }
  SUBCASE("the shell's files, and nothing outside them; another Host is refused") {
    const HttpReply index = http_get(port, "/");
    CHECK(index.status == 200);
    CHECK(index.body.find("<title>shell</title>") != std::string::npos);
    CHECK(http_get(port, "/shell/../../secret.txt").status == 404);
    CHECK(http_get(port, "/shell/missing.js").status == 404);
    CHECK(http_get(port, "/", "evil.example:80").status == 403);
  }
  SUBCASE("a capture needs the token and is the source's, asked under the host's lock") {
    CHECK(http_get(port, "/capture/c1").status == 403);
    CHECK(captures.asked == 0);
    const HttpReply shot = http_get(port, "/capture/c1?token=" + listener.token());
    CHECK(shot.status == 200);
    CHECK(shot.body == "PNGBYTES");
    CHECK(captures.held);
    CHECK(http_get(port, "/capture/c2?token=" + listener.token()).status == 404);
    CHECK(captures.asked == 2);
  }
  listener.stop();
}
