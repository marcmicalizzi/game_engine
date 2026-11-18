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

// `extra`: header lines, each ending in CRLF.
HttpReply http_get(u16 port, std::string_view target, std::string_view host = {},
                   std::string_view extra = {}) {
  net::Socket s;
  std::string error;
  HttpReply reply;
  REQUIRE_MESSAGE(net::connect_loopback(port, s, error), error);
  const std::string host_line =
      host.empty() ? "127.0.0.1:" + std::to_string(port) : std::string(host);
  REQUIRE(s.send_all("GET " + std::string(target) + " HTTP/1.1\r\nHost: " + host_line + "\r\n" +
                     std::string(extra) + "\r\n"));
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
  // A one-time ticket in the address, never the token (ADR-0069); each call makes another.
  const std::string shell = listener.shell_url();
  CHECK(shell.starts_with("http://127.0.0.1:" + std::to_string(port) + "/?ticket="));
  CHECK(shell.ends_with("&session=s1"));
  CHECK(shell.find(listener.token()) == std::string::npos);
  CHECK(listener.shell_url() != shell);
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
    CHECK(none.close_reason().find("a credential is required") != std::string::npos);
    net::WebSocket wrong;
    const std::pair<std::string, std::string> guess[] = {
        {"X-Engine-Token", "AAAAAAAAAAAAAAAAAAAAAA"}};
    REQUIRE(net::websocket_connect(port, "/", guess, wrong, error));
    CHECK_FALSE(wrong.receive(m, 30000));
    CHECK(wrong.close_code() == net::close_code::k_policy);
    CHECK(wrong.close_reason().find("names no token or credential") != std::string::npos);
    CHECK(wrong.close_reason().find("AAAA") == std::string::npos);  // what was sent is never said
    // The token in the URL is not read at all: a credential is never in a URL.
    net::WebSocket in_url;
    REQUIRE(net::websocket_connect(port, "/?token=" + listener.token(), {}, in_url, error));
    CHECK_FALSE(in_url.receive(m, 30000));
    CHECK(in_url.close_code() == net::close_code::k_policy);
  }
  SUBCASE("a page from another origin is refused even with the token") {
    net::WebSocket ws;
    const std::pair<std::string, std::string> origin[] = {{"Origin", "http://evil.example"},
                                                          {"X-Engine-Token", listener.token()}};
    REQUIRE(net::websocket_connect(port, "/", origin, ws, error));
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
    CHECK(http_get(port, "/capture/c1?token=" + listener.token()).status == 403);  // not in a URL
    const std::string with_token = "X-Engine-Token: " + listener.token() + "\r\n";
    const HttpReply shot = http_get(port, "/capture/c1", {}, with_token);
    CHECK(shot.status == 200);
    CHECK(shot.body == "PNGBYTES");
    CHECK(captures.held);
    CHECK(http_get(port, "/capture/c2", {}, with_token).status == 404);
    CHECK(captures.asked == 2);
  }
  listener.stop();
}

// Roadmap A4, ADR-0069 (protocol.md, "Who a connection is"): a credential the operator issued binds
// its connection's identity, a call naming another is refused before its handler, a token nobody
// issued is refused before any method, and a browser page buys its cookie with a one-time ticket.
TEST_CASE("listener: a credential binds who a connection is; the shell's ticket buys a cookie") {
  const test::TempDir tmp("protocol_listener_credentials");
  {
    std::ofstream f(tmp.file("index.html"), std::ios::binary);
    f << "<!doctype html><title>shell</title>";
  }
  protocol::Policy policy;
  std::string error;
  REQUIRE_MESSAGE(policy.load_roles("roles.json", R"({"default_role":"director","roles":[
    {"name":"director","methods":["*"],"layers":["*"],"object_types":["*"]},
    {"name":"designer","methods":["doc.*"],"layers":["*"],"object_types":["*"]}]})",
                                    error),
                  error);
  std::string agent_token;
  REQUIRE(protocol::make_token(agent_token));
  CHECK(agent_token.size() == 22);
  const std::string digest = protocol::token_digest(agent_token);
  CHECK(digest.size() == 64);
  // SHA-256, by FIPS 180-4's own example.
  CHECK(protocol::token_digest("abc") ==
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  protocol::Credentials credentials;
  // Malformed files are refused with the reason, never quoting a digest.
  CHECK_FALSE(credentials.load(
      "c.json", R"({"credentials":[{"name":"a","token_sha256":")" + digest + R"("}]})", &policy,
      error));
  CHECK(error.find("actor") != std::string::npos);
  CHECK_FALSE(credentials.load("c.json", R"({"credentials":[{"name":"a","actor":"x",
    "token_sha256":"abc"}]})",
                               &policy, error));
  CHECK_FALSE(credentials.load("c.json", R"({"credentials":[{"name":"a","actor":"x","role":"pilot",
    "token_sha256":")" + digest + R"("}]})",
                               &policy, error));
  CHECK(error.find(digest) == std::string::npos);
  REQUIRE_MESSAGE(credentials.load("c.json",
                                   R"({"credentials":[{"name":"agent-1","actor":"agent",
    "role":"designer","task":"t-a","token_sha256":")" +
                                       digest + R"("}]})",
                                   &policy, error),
                  error);
  CHECK(credentials.match(agent_token) != nullptr);
  CHECK(credentials.match("AAAAAAAAAAAAAAAAAAAAAA") == nullptr);

  io::Vfs vfs;
  protocol::SessionManager sessions(vfs);
  protocol::Dispatcher dispatcher(protocol::Context{&sessions, nullptr, nullptr, nullptr, &policy});
  protocol::add_builtin_methods(dispatcher);
  std::mutex lock;
  Captures captures;
  captures.lock = &lock;
  protocol::Listener listener(dispatcher, lock, &captures);
  listener.set_credentials(&credentials);
  REQUIRE_MESSAGE(listener.start(0, tmp.path(), "s1", error), error);
  const u16 port = listener.port();

  SUBCASE("the credential's connection is its actor and role, and may not call as another") {
    net::WebSocket ws;
    const std::pair<std::string, std::string> header[] = {{"X-Engine-Token", agent_token}};
    REQUIRE_MESSAGE(net::websocket_connect(port, "/", header, ws, error), error);
    const JsonValue list = call(ws, R"({"jsonrpc":"2.0","id":1,"method":"client.list"})");
    const JsonValue& me = (*list.find("result")->find("clients"))[0];
    CHECK(me.find("credential")->as_string() == "agent-1");
    CHECK(me.find("bound")->as_bool());
    CHECK(me.find("actor")->as_string() == "agent");
    CHECK(me.find("role")->as_string() == "designer");
    CHECK(write_json(list).find(agent_token) == std::string::npos);
    const JsonValue opened = call(
        ws, std::string(R"({"jsonrpc":"2.0","id":2,"method":"session.open","params":)") +
                R"({"path":)" + write_json(JsonValue(tmp.file("doc"))) + R"(,"create":true}})");
    const std::string session(opened.find("result")->find("session")->as_string());
    const auto apply = [&](u32 id, std::string_view attribution) {
      return call(
          ws, R"({"jsonrpc":"2.0","id":)" + std::to_string(id) +
                  R"(,"method":"doc.apply","params":{"session":")" + session +
                  R"(","commands":[{"kind":"CreateObject","id":"0000000000000000000000000000be)" +
                  std::to_string(10 + id) +
                  R"(","type":"engine.world.Node","value":{"name":"n"}}],"attribution":)" +
                  std::string(attribution) + "}}");
    };
    // Naming another actor or role is refused before the handler, whatever the method.
    const JsonValue as_other = apply(3, R"({"actor":"owner","rationale":"x"})");
    REQUIRE(as_other.find("error") != nullptr);
    CHECK(as_other.find("error")->find("code")->as_int() == 1008);
    CHECK(as_other.find("error")->find("data")->find("reason")->as_string() == "identity");
    CHECK(apply(4, R"({"role":"director","rationale":"x"})").find("error") != nullptr);
    CHECK(call(ws, R"({"jsonrpc":"2.0","id":5,"method":"client.hello","params":)"
                   R"({"attribution":{"actor":"owner"}}})")
              .find("error") != nullptr);
    // Blank, or its own, is the credential's; the journal says so.
    const JsonValue own = apply(6, R"({"rationale":"mine"})");
    REQUIRE_MESSAGE(own.find("result") != nullptr, write_json(own));
    CHECK(own.find("result")->find("committed")->as_bool());
    CHECK(
        apply(7, R"({"actor":"agent","role":"designer","rationale":"mine too"})").find("result") !=
        nullptr);
    const JsonValue journal = call(ws, R"({"jsonrpc":"2.0","id":8,"method":"doc.journal",)"
                                       R"("params":{"session":")" +
                                           session + R"("}})");
    const JsonValue& first = (*journal.find("result")->find("patches"))[0];
    CHECK(first.find("attribution")->find("actor")->as_string() == "agent");
    CHECK(first.find("attribution")->find("task")->as_string() == "t-a");
  }
  SUBCASE("the operator's token is not bound") {
    net::WebSocket ws;
    const std::pair<std::string, std::string> header[] = {{"X-Engine-Token", listener.token()}};
    REQUIRE_MESSAGE(net::websocket_connect(port, "/", header, ws, error), error);
    const JsonValue hello = call(ws, R"({"jsonrpc":"2.0","id":1,"method":"client.hello","params":)"
                                     R"({"name":"cli","attribution":{"actor":"owner"}}})");
    REQUIRE_MESSAGE(hello.find("result") != nullptr, write_json(hello));
    CHECK(hello.find("result")->find("credential")->as_string() == "operator");
    CHECK_FALSE(hello.find("result")->find("bound")->as_bool());
  }
  SUBCASE("a shell address's ticket is spent once, for a cookie the page's socket carries") {
    const std::string shell = listener.shell_url();
    const std::string target = shell.substr(shell.find('/', 7));  // "/?ticket=...&session=s1"
    const HttpReply first = http_get(port, target + "&embedded=1");
    CHECK(first.status == 303);
    CHECK(first.head.find("Location: /?session=s1&embedded=1") != std::string::npos);
    CHECK(first.head.find("ticket") == std::string::npos);
    // Named for the port (ADR-0069 decision 6): two hosts' shells in one browser keep a cookie
    // each.
    const std::string name = "engine_session_" + std::to_string(port) + "=";
    const usize at = first.head.find(name);
    REQUIRE_MESSAGE(at != std::string::npos, first.head.substr(0, first.head.find("Set-Cookie")));
    CHECK(first.head.find("HttpOnly; SameSite=Strict") != std::string::npos);
    const std::string cookie = first.head.substr(at, first.head.find(';', at) - at);
    CHECK(http_get(port, target).status == 403);                             // spent
    CHECK(http_get(port, "/?ticket=AAAAAAAAAAAAAAAAAAAAAA").status == 403);  // never issued
    // The page's own GETs need nothing; its captures and its socket carry the cookie.
    CHECK(http_get(port, "/?session=s1").status == 200);
    CHECK(http_get(port, "/capture/c1", {}, "Cookie: " + cookie + "\r\n").status == 200);
    CHECK(http_get(port, "/capture/c1", {}, "Cookie: " + name + "nope\r\n").status == 403);
    // Another port's name is not this host's, whatever its value.
    const std::string value = cookie.substr(name.size());
    CHECK(http_get(port, "/capture/c1", {}, "Cookie: engine_session_1=" + value + "\r\n").status ==
          403);
    net::WebSocket page;
    const std::pair<std::string, std::string> headers[] = {
        {"Cookie", "theme=dark; " + cookie},
        {"Origin", "http://127.0.0.1:" + std::to_string(port)}};
    REQUIRE_MESSAGE(net::websocket_connect(port, "/", headers, page, error), error);
    const JsonValue pong = call(page, R"({"jsonrpc":"2.0","id":1,"method":"engine.info"})");
    CHECK(pong.find("result") != nullptr);
    // A browser that holds the cookie (a second tab opened from the same printed address) is sent
    // on without a new one; one that does not is refused.
    const HttpReply again = http_get(port, target, {}, "Cookie: " + cookie + "\r\n");
    CHECK(again.status == 303);
    CHECK(again.head.find("Set-Cookie") == std::string::npos);
    net::WebSocket stale;
    const std::pair<std::string, std::string> wrong[] = {{"Cookie", name + "nope"}};
    REQUIRE(net::websocket_connect(port, "/", wrong, stale, error));
    net::Message m;
    CHECK_FALSE(stale.receive(m, 30000));
    CHECK(stale.close_code() == net::close_code::k_policy);
  }
  SUBCASE("a ticket left unspent past its lifetime buys nothing") {
    listener.set_ticket_lifetime_ms(0);
    const std::string shell = listener.shell_url();
    CHECK(http_get(port, shell.substr(shell.find('/', 7))).status == 403);
  }
  listener.stop();
}

namespace {

// A listener over the built-in methods with `policy`, accepting `credentials`.
struct BoundHost {
  io::Vfs vfs;
  protocol::SessionManager sessions{vfs};
  protocol::Dispatcher dispatcher;
  std::mutex lock;
  protocol::Listener listener;
  BoundHost(const protocol::Policy& policy, const protocol::Credentials& credentials)
      : dispatcher(protocol::Context{&sessions, nullptr, nullptr, nullptr, &policy}),
        listener(dispatcher, lock) {
    protocol::add_builtin_methods(dispatcher);
    listener.set_credentials(&credentials);
    std::string error;
    REQUIRE_MESSAGE(listener.start(0, {}, {}, error), error);
  }
  ~BoundHost() { listener.stop(); }
};

// What a credential's connection may do on `host`: the reason a doc.apply naming nobody is refused
// for ("" when it committed), and whether a read (doc.journal) answered.
struct Outcome {
  std::string refused;
  std::string role;  // as client.list shows it
  bool read = false;
};

Outcome try_write(BoundHost& host, const std::string& token, const std::string& doc) {
  net::WebSocket ws;
  std::string error;
  const std::pair<std::string, std::string> header[] = {{"X-Engine-Token", token}};
  REQUIRE_MESSAGE(net::websocket_connect(host.listener.port(), "/", header, ws, error), error);
  Outcome out;
  const JsonValue list = call(ws, R"({"jsonrpc":"2.0","id":1,"method":"client.list"})");
  out.role = std::string((*list.find("result")->find("clients"))[0].find("role")->as_string());
  // session.open is a read: it changes no document.
  const JsonValue opened =
      call(ws, std::string(R"({"jsonrpc":"2.0","id":2,"method":"session.open","params":)") +
                   R"({"path":)" + write_json(JsonValue(doc)) + R"(,"create":true}})");
  REQUIRE_MESSAGE(opened.find("result") != nullptr, write_json(opened));
  const std::string session(opened.find("result")->find("session")->as_string());
  const JsonValue applied = call(
      ws,
      R"({"jsonrpc":"2.0","id":3,"method":"doc.apply","params":{"session":")" + session +
          R"(","commands":[{"kind":"CreateObject","id":"0000000000000000000000000000be01",)"
          R"("type":"engine.world.Node","value":{"name":"n"}}],"attribution":{"rationale":"w"}}})");
  if (const JsonValue* e = applied.find("error"); e != nullptr) {
    CHECK(e->find("code")->as_int() == 1008);
    out.refused = std::string(e->find("data")->find("reason")->as_string());
    CHECK(write_json(applied).find(token) == std::string::npos);
  }
  const JsonValue journal = call(ws, R"({"jsonrpc":"2.0","id":4,"method":"doc.journal",)"
                                     R"("params":{"session":")" +
                                         session + R"("}})");
  out.read = journal.find("result") != nullptr;
  return out;
}

}  // namespace

// The role a handler checks a bound connection against is its credential's, not one the call named
// (ADR-0069 decision 3a): resolve_caller refuses a named actor or role that is not the
// credential's itself, so the dispatcher's look at a top-level `attribution` is not the only thing
// between a call and another role's layers, tiles and types.
TEST_CASE("listener: resolve_caller holds a bound connection to its credential's role") {
  protocol::Policy policy;
  std::string error;
  REQUIRE_MESSAGE(policy.load_roles("roles.json", R"({"default_role":"director","roles":[
    {"name":"director","methods":["*"],"layers":["*"],"object_types":["*"]},
    {"name":"qa","methods":["doc.*"],"layers":["qa"],"object_types":["*"]}]})",
                                    error),
                  error);
  protocol::Clients clients;
  protocol::Client& bot = clients.touch(7, 0);
  bot.credential = "qa-bot";
  bot.who.actor = "bot";
  bot.who.role = "qa";
  bot.identified = true;
  bot.bound = true;
  protocol::Context ctx;
  ctx.policy = &policy;
  ctx.clients = &clients;
  ctx.connection = 7;
  const auto refused = [&](const char* actor, const char* role) {
    doc::Attribution given;
    given.actor = actor;
    given.role = role;
    protocol::Caller caller;
    protocol::RpcError rpc;
    const bool ok = protocol::resolve_caller(ctx, &given, caller, rpc);
    if (ok) return std::string("resolved as ") + (caller.role ? caller.role->name : "(null)");
    CHECK(rpc.code == 1008);
    const JsonValue* reason = rpc.data.is_object() ? rpc.data.find("reason") : nullptr;
    return reason != nullptr ? std::string(reason->as_string()) : std::string("no reason");
  };
  CHECK(refused("", "director") == "identity");
  CHECK(refused("owner", "") == "identity");
  CHECK(refused("bot", "director") == "identity");
  // Blank, or its own, is the credential's role as the roles file defines it.
  for (const char* role : {"", "qa"}) {
    doc::Attribution given;
    given.role = role;
    protocol::Caller caller;
    protocol::RpcError rpc;
    REQUIRE(protocol::resolve_caller(ctx, &given, caller, rpc));
    CHECK(caller.role == policy.find_role("qa"));
    CHECK(caller.who.actor == "bot");
  }
}

// ADR-0069 decision 3a ("a role a host cannot enforce is never a label"): a credential's role
// binds on every host that accepts the credential, roles file or none, and a credential that names
// no role runs neither in the host's own --role nor in the file's default role: it may only read.
TEST_CASE("listener: a credential's role binds with or without a roles file; no role only reads") {
  const test::TempDir tmp("protocol_listener_bound_roles");
  std::string qa_token;
  std::string reader_token;
  REQUIRE(protocol::make_token(qa_token));
  REQUIRE(protocol::make_token(reader_token));
  const std::string file = R"({"credentials":[{"name":"qa-bot","actor":"bot","role":"qa",)"
                           R"("token_sha256":")" +
                           protocol::token_digest(qa_token) +
                           R"("},{"name":"reader","actor":"reader","token_sha256":")" +
                           protocol::token_digest(reader_token) + R"("}]})";
  std::string error;

  SUBCASE("no roles file: the role named is not a label, it is a connection that only reads") {
    // engine-editor's case, and engine-host --listen --credentials without --roles.
    protocol::Policy policy;
    policy.identity.actor = "owner";
    protocol::Credentials credentials;
    REQUIRE_MESSAGE(credentials.load("c.json", file, &policy, error), error);
    CHECK(credentials.read_only_count(&policy) == 2);
    BoundHost host(policy, credentials);
    const Outcome qa = try_write(host, qa_token, tmp.file("doc-a"));
    CHECK(qa.role == "qa");
    CHECK(qa.refused == "unenforced role");
    CHECK(qa.read);
    const Outcome reader = try_write(host, reader_token, tmp.file("doc-b"));
    CHECK(reader.refused == "no role");
    CHECK(reader.read);
  }
  SUBCASE("a roles file and a director host: a credential with no role gets neither") {
    protocol::Policy policy;
    REQUIRE_MESSAGE(policy.load_roles("roles.json", R"({"default_role":"director","roles":[
      {"name":"director","methods":["*"],"layers":["*"],"object_types":["*"]},
      {"name":"qa","methods":["lease.*"],"layers":["*"],"object_types":["*"]}]})",
                                      error),
                    error);
    policy.identity.actor = "owner";
    policy.identity.role = "director";  // engine-host --role director, as a director's host runs
    protocol::Credentials credentials;
    REQUIRE_MESSAGE(credentials.load("c.json", file, &policy, error), error);
    CHECK(credentials.read_only_count(&policy) == 1);
    BoundHost host(policy, credentials);
    const Outcome reader = try_write(host, reader_token, tmp.file("doc-c"));
    CHECK(reader.role.empty());  // client.list tells the truth: it has none
    CHECK(reader.refused == "no role");
    CHECK(reader.read);
    const Outcome qa = try_write(host, qa_token, tmp.file("doc-d"));
    CHECK(qa.refused == "method");  // the role binds, and does not list doc.apply
    CHECK(qa.read);
  }
}
