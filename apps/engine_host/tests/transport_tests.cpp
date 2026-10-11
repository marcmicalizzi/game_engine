// engine-host --listen end to end (docs/subsystems/apps.md, "engine-host --listen"; protocol.md,
// "Transport"): the real executable, driven through foundation/net's own WebSocket client. The
// handshake and the token's refusal; a fragmented 200 KB request with pings between its frames; a
// journal entry written over the socket that is byte for byte the one stdin writes; a role refused
// over the socket exactly as on stdin; a second connection answered while the first is open; a
// non-loopback --listen refused before anything binds; the static files and their path guard; and
// `render.load {session}` with A32's camera, the records of the ids channel and a kept capture
// fetched over HTTP. tools/host-ws.Tests.ps1 drives the same host with an independent client.
#include <core/json/json.h>
#include <core/math/math.h>
#include <core/platform/process.h>
#include <foundation/net/socket.h>
#include <foundation/net/websocket.h>

#include <doctest/doctest.h>
#include <test_paths.h>
#include <test_temp_dir.h>

#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using namespace engine;

namespace {

constexpr i32 k_render_unavailable = 1007;

const std::string& host_exe() {
  static const std::string path = test::app_path(ENGINE_APP_PATH);
  return path;
}

// A JSON string literal of `text` (not `quoted`, which ADL would find in std for a std::string).
std::string json_text(std::string_view text) { return write_json(JsonValue(text)); }

const JsonValue& at(const JsonValue& o, std::string_view key) {
  const JsonValue* v = o.find(key);
  REQUIRE_MESSAGE(v != nullptr, "no '" << key << "' in " << write_json(o));
  return *v;
}

i32 error_code(const JsonValue& response) {
  const JsonValue* e = response.find("error");
  return e != nullptr ? static_cast<i32>(at(*e, "code").as_int()) : 0;
}

std::string request(u32 id, std::string_view method, std::string_view params) {
  return "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(id) + ",\"method\":\"" +
         std::string(method) + "\",\"params\":" + std::string(params) + "}";
}

// One engine-host with --listen and the given flags: the announcement read, stdin kept for
// requests on the stdio side, and closed at the end, which is what ends the host.
struct ListeningHost {
  platform::Process process;
  u16 port = 0;
  std::string token;
  std::string session;
  u32 next_id = 1000;
  bool ok = false;

  explicit ListeningHost(std::vector<std::string> flags) {
    std::vector<std::string> args = {host_exe(), "--listen", "127.0.0.1:0"};
    args.insert(args.end(), flags.begin(), flags.end());
    std::vector<std::string_view> argv(args.begin(), args.end());
    std::string error;
    if (!process.spawn(std::span<const std::string_view>(argv.data(), argv.size()), &error)) {
      FAIL("cannot spawn engine-host: " << error);
      return;
    }
    ok = true;
    std::string line;
    REQUIRE(process.read_line(line));
    JsonValue announcement;
    REQUIRE_MESSAGE(parse_json(line, announcement).ok, line);
    const std::string_view listen = at(announcement, "listen").as_string();
    REQUIRE(listen.starts_with("ws://127.0.0.1:"));
    REQUIRE(listen.ends_with("/"));
    std::string why;
    const std::string_view address = listen.substr(5, listen.size() - 6);
    REQUIRE_MESSAGE(net::parse_loopback_address(address, port, why), why);
    REQUIRE(port != 0);
    token = std::string(at(announcement, "token").as_string());
    CHECK(token.size() == 22);  // 128 bits in base64url without padding
    if (const JsonValue* s = announcement.find("session"); s != nullptr) {
      session = std::string(s->as_string());
    }
    const std::string_view shell = at(announcement, "shell").as_string();
    CHECK(shell.find("token=" + token) != std::string_view::npos);
  }
  ~ListeningHost() {
    if (!ok) return;
    process.close_stdin();
    CHECK(process.wait() == 0);
  }
  JsonValue stdio(std::string_view method, std::string_view params) {
    REQUIRE(process.write(request(next_id++, method, params) + "\n"));
    std::string text;
    REQUIRE(process.read_line(text));
    JsonValue response;
    REQUIRE_MESSAGE(parse_json(text, response).ok, text);
    return response;
  }
};

struct Client {
  net::WebSocket ws;
  u32 next_id = 1;

  bool connect(u16 port, std::string_view token) {
    std::string error;
    const bool connected =
        net::websocket_connect(port, "/?token=" + std::string(token), {}, ws, error);
    CHECK_MESSAGE(connected, error);
    return connected;
  }
  JsonValue call(std::string_view method, std::string_view params) {
    REQUIRE(ws.send_text(request(next_id++, method, params)));
    net::Message m;
    REQUIRE_MESSAGE(ws.receive(m, 600000),
                    "closed " << ws.close_code() << " " << ws.close_reason());
    JsonValue response;
    REQUIRE_MESSAGE(parse_json(m.data, response).ok, m.data);
    return response;
  }
};

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

void write_file(const std::string& path, std::string_view text) {
  std::ofstream f(path, std::ios::binary);
  f << text;
}

void put_u32(std::vector<u8>& out, u32 v) {
  for (u32 i = 0; i < 4; ++i)
    out.push_back(static_cast<u8>((v >> (8 * i)) & 0xffu));
}

void put_f32(std::vector<u8>& out, f32 v) {
  u32 bits = 0;
  std::memcpy(&bits, &v, 4);
  put_u32(out, bits);
}

// A unit cube as a GLB (apps/engine_editor/tests/editor_tests.cpp's).
bool write_cube_glb(const std::string& path) {
  const Vec3 normals[6] = {Vec3{1, 0, 0},  Vec3{-1, 0, 0}, Vec3{0, 1, 0},
                           Vec3{0, -1, 0}, Vec3{0, 0, 1},  Vec3{0, 0, -1}};
  const Vec3 tangents[6] = {Vec3{0, 1, 0}, Vec3{0, 1, 0}, Vec3{0, 0, 1},
                            Vec3{0, 0, 1}, Vec3{1, 0, 0}, Vec3{1, 0, 0}};
  std::vector<u8> bin;
  for (u32 f = 0; f < 6; ++f) {
    const Vec3 nrm = normals[f];
    const Vec3 t = tangents[f];
    const Vec3 b = cross(nrm, t);
    const Vec3 corners[4] = {nrm * 0.5f - t * 0.5f - b * 0.5f, nrm * 0.5f + t * 0.5f - b * 0.5f,
                             nrm * 0.5f + t * 0.5f + b * 0.5f, nrm * 0.5f - t * 0.5f + b * 0.5f};
    for (const Vec3& c : corners) {
      put_f32(bin, c.x);
      put_f32(bin, c.y);
      put_f32(bin, c.z);
    }
  }
  const u64 normal_offset = bin.size();
  for (u32 f = 0; f < 6; ++f) {
    for (u32 c = 0; c < 4; ++c) {
      put_f32(bin, normals[f].x);
      put_f32(bin, normals[f].y);
      put_f32(bin, normals[f].z);
    }
  }
  const u64 uv_offset = bin.size();
  for (u32 f = 0; f < 6; ++f) {
    const f32 uvs[8] = {0, 0, 1, 0, 1, 1, 0, 1};
    for (const f32 v : uvs)
      put_f32(bin, v);
  }
  const u64 index_offset = bin.size();
  for (u32 f = 0; f < 6; ++f) {
    const u32 base = f * 4;
    const u32 tris[6] = {base, base + 1, base + 2, base, base + 2, base + 3};
    for (const u32 i : tris) {
      bin.push_back(static_cast<u8>(i & 0xffu));
      bin.push_back(static_cast<u8>(i >> 8));
    }
  }
  const u64 index_bytes = bin.size() - index_offset;
  while (bin.size() % 4 != 0)
    bin.push_back(0);
  auto n = [](u64 v) { return std::to_string(v); };
  std::string json =
      "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
      "\"nodes\":[{\"mesh\":0}],"
      "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"NORMAL\":1,"
      "\"TEXCOORD_0\":2},\"indices\":3,\"material\":0}]}],"
      "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorFactor\":[0.8,0.5,0.2,1],"
      "\"metallicFactor\":0,\"roughnessFactor\":0.6}}],"
      "\"accessors\":["
      "{\"bufferView\":0,\"componentType\":5126,\"count\":24,\"type\":\"VEC3\","
      "\"min\":[-0.5,-0.5,-0.5],\"max\":[0.5,0.5,0.5]},"
      "{\"bufferView\":1,\"componentType\":5126,\"count\":24,\"type\":\"VEC3\"},"
      "{\"bufferView\":2,\"componentType\":5126,\"count\":24,\"type\":\"VEC2\"},"
      "{\"bufferView\":3,\"componentType\":5123,\"count\":36,\"type\":\"SCALAR\"}],"
      "\"bufferViews\":["
      "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":" +
      n(normal_offset) + "},{\"buffer\":0,\"byteOffset\":" + n(normal_offset) +
      ",\"byteLength\":" + n(uv_offset - normal_offset) +
      "},{\"buffer\":0,\"byteOffset\":" + n(uv_offset) +
      ",\"byteLength\":" + n(index_offset - uv_offset) +
      "},{\"buffer\":0,\"byteOffset\":" + n(index_offset) + ",\"byteLength\":" + n(index_bytes) +
      "}],\"buffers\":[{\"byteLength\":" + n(bin.size()) + "}]}";
  while (json.size() % 4 != 0)
    json += ' ';
  std::vector<u8> glb;
  put_u32(glb, 0x46546c67u);
  put_u32(glb, 2u);
  put_u32(glb, static_cast<u32>(12 + 8 + json.size() + 8 + bin.size()));
  put_u32(glb, static_cast<u32>(json.size()));
  put_u32(glb, 0x4e4f534au);
  glb.insert(glb.end(), json.begin(), json.end());
  put_u32(glb, static_cast<u32>(bin.size()));
  put_u32(glb, 0x004e4942u);
  glb.insert(glb.end(), bin.begin(), bin.end());
  std::ofstream f(path, std::ios::binary);
  if (!f.is_open()) return false;
  f.write(reinterpret_cast<const char*>(glb.data()), static_cast<std::streamsize>(glb.size()));
  return f.good();
}

constexpr const char* k_left = "00000000000000200000000000000001";
constexpr const char* k_right = "00000000000000200000000000000002";

std::string create_placement(const char* id, const char* name, const char* translation) {
  return std::string(R"({"kind":"CreateObject","id":")") + id +
         R"(","type":"engine.scene.MeshPlacement","value":{"name":")" + name +
         R"(","mesh":"cube.glb","translation":)" + translation + "}}";
}

}  // namespace

TEST_CASE("listen: the handshake needs the token, and refuses without it with 1008") {
  ListeningHost host({});
  {
    net::WebSocket ws;
    std::string error;
    REQUIRE_MESSAGE(net::websocket_connect(host.port, "/", {}, ws, error), error);
    net::Message m;
    CHECK_FALSE(ws.receive(m, 30000));
    CHECK(ws.close_code() == net::close_code::k_policy);
    CHECK(ws.close_reason().find("token is required") != std::string::npos);
  }
  {
    net::WebSocket ws;
    std::string error;
    REQUIRE(net::websocket_connect(host.port, "/?token=AAAAAAAAAAAAAAAAAAAAAA", {}, ws, error));
    net::Message m;
    CHECK_FALSE(ws.receive(m, 30000));
    CHECK(ws.close_code() == net::close_code::k_policy);
    CHECK(ws.close_reason().find("does not match") != std::string::npos);
  }
  {
    // A browser page from elsewhere is refused even with the token.
    net::WebSocket ws;
    std::string error;
    const std::pair<std::string, std::string> origin[] = {{"Origin", "http://evil.example"}};
    REQUIRE(net::websocket_connect(host.port, "/?token=" + host.token, origin, ws, error));
    net::Message m;
    CHECK_FALSE(ws.receive(m, 30000));
    CHECK(ws.close_code() == net::close_code::k_policy);
  }
  {
    // The header form, for clients that would rather not put the token in a URL.
    net::WebSocket ws;
    std::string error;
    const std::pair<std::string, std::string> header[] = {{"X-Engine-Token", host.token}};
    REQUIRE(net::websocket_connect(host.port, "/", header, ws, error));
    Client c{std::move(ws)};
    const JsonValue pong = c.call("engine.ping", "null");
    CHECK(at(at(pong, "result"), "pid").as_int() > 0);
  }
}

TEST_CASE("listen: a fragmented 200 KB request with pings between its frames is one request") {
  test::TempDir tmp("engine_host_listen_frag");
  ListeningHost host({"--doc", tmp.file("doc"), "--create", "--actor", "frag"});
  REQUIRE_FALSE(host.session.empty());
  Client c;
  REQUIRE(c.connect(host.port, host.token));
  const std::string big_name(200 * 1024, 'n');
  const std::string apply = request(7, "doc.apply",
                                    R"({"session":")" + host.session + R"(","commands":[)" +
                                        create_placement(k_left, big_name.c_str(), "[0,0,0]") +
                                        R"(],"attribution":{"rationale":"a 200 KB name"}})");
  REQUIRE(apply.size() > 200 * 1024);
  REQUIRE(c.ws.send_fragmented(net::Opcode::Text, apply, 4096, true));
  net::Message m;
  REQUIRE(c.ws.receive(m, 60000));
  JsonValue response;
  REQUIRE_MESSAGE(parse_json(m.data, response).ok, m.data.substr(0, 400));
  CHECK(at(at(response, "result"), "committed").as_bool());
  CHECK(at(response, "id").as_int() == 7);
}

TEST_CASE("listen: a journal entry written over the socket is the one stdin writes") {
  test::TempDir tmp("engine_host_listen_journal");
  ListeningHost host(
      {"--doc", tmp.file("over_socket"), "--create", "--actor", "marc", "--task", "t-socket"});
  Client c;
  REQUIRE(c.connect(host.port, host.token));
  // The same transaction, with the same explicit timestamp, once over the socket and once on stdin
  // against a second document: the two journal entries must be the same bytes.
  const std::string commands =
      std::string("[") + create_placement(k_left, "left", "[-1.5,0,0]") + "]";
  const std::string attribution =
      R"({"rationale":"the same edit both ways","timestamp_unix_ms":1760000000000})";
  const JsonValue a = c.call("doc.apply", R"({"session":")" + host.session + R"(","commands":)" +
                                              commands + R"(,"attribution":)" + attribution + "}");
  REQUIRE_MESSAGE(at(at(a, "result"), "committed").as_bool(), write_json(a));
  const JsonValue opened = host.stdio(
      "session.open", R"({"path":)" + json_text(tmp.file("over_stdin")) + R"(,"create":true})");
  const std::string other = std::string(at(at(opened, "result"), "session").as_string());
  REQUIRE(other != host.session);
  const JsonValue b =
      host.stdio("doc.apply", R"({"session":")" + other + R"(","commands":)" + commands +
                                  R"(,"attribution":)" + attribution + "}");
  REQUIRE_MESSAGE(at(at(b, "result"), "committed").as_bool(), write_json(b));
  const JsonValue ja = c.call("doc.journal", R"({"session":")" + host.session + "\"}");
  const JsonValue jb = host.stdio("doc.journal", R"({"session":")" + other + "\"}");
  const JsonValue& pa = at(at(ja, "result"), "patches");
  const JsonValue& pb = at(at(jb, "result"), "patches");
  REQUIRE(pa.size() == 1);
  REQUIRE(pb.size() == 1);
  CHECK(write_json(pa[0]) == write_json(pb[0]));
  CHECK(at(at(pa[0], "attribution"), "actor").as_string() == "marc");
  CHECK(at(at(pa[0], "attribution"), "rationale").as_string() == "the same edit both ways");
}

TEST_CASE("listen: a role that may not write is refused over the socket as on stdin") {
  test::TempDir tmp("engine_host_listen_roles");
  const std::string roles = tmp.file("roles.json");
  write_file(roles, R"({"default_role":"qa","roles":[{"name":"director","methods":["*"],
    "layers":["*"],"object_types":["*"]},{"name":"qa","methods":[]}]})");
  ListeningHost host({"--roles", roles, "--doc", tmp.file("doc"), "--create"});
  Client c;
  REQUIRE(c.connect(host.port, host.token));
  const std::string params = R"({"session":")" + host.session + R"(","commands":[)" +
                             create_placement(k_left, "left", "[0,0,0]") +
                             R"(],"attribution":{"rationale":"not mine to make"}})";
  const JsonValue over_socket = c.call("doc.apply", params);
  const JsonValue on_stdin = host.stdio("doc.apply", params);
  CHECK(error_code(over_socket) == 1008);
  CHECK(error_code(on_stdin) == 1008);
  CHECK(write_json(at(over_socket, "error")) == write_json(at(on_stdin, "error")));
  // A read is not restricted, over either.
  CHECK(error_code(c.call("doc.objects", R"({"session":")" + host.session + "\"}")) == 0);
}

TEST_CASE("listen: a second connection is served while the first is open") {
  ListeningHost host({});
  Client a;
  Client b;
  REQUIRE(a.connect(host.port, host.token));
  REQUIRE(b.connect(host.port, host.token));
  CHECK(error_code(b.call("engine.ping", "null")) == 0);
  CHECK(error_code(a.call("engine.ping", "null")) == 0);
  CHECK(error_code(b.call("engine.methods", "null")) == 0);
  // And stdin is still the host's, beside both.
  CHECK(error_code(host.stdio("engine.ping", "null")) == 0);
}

TEST_CASE("listen: a non-loopback address is refused before anything binds") {
  for (const char* address : {"0.0.0.0:0", "192.168.1.10:8080", "[::]:0"}) {
    platform::Process p;
    const std::string_view argv[] = {host_exe(), "--listen", address};
    std::string error;
    REQUIRE(p.spawn(argv, &error, /*merge_stderr=*/true));
    p.close_stdin();
    std::string out;
    p.read_all(out);
    CHECK(p.wait() == 2);
    CHECK_MESSAGE(out.find("loopback only") != std::string::npos, out);
    CHECK(out.find("\"listen\"") == std::string::npos);  // nothing was announced
  }
}

TEST_CASE("listen: the shell's files, and nothing outside them") {
  ListeningHost host({});
  const HttpReply index = http_get(host.port, "/");
  CHECK(index.status == 200);
  CHECK(index.head.find("text/html") != std::string::npos);
  CHECK(index.body.find("<html") != std::string::npos);
  CHECK(http_get(host.port, "/shell/shell.css").status == 200);
  CHECK(http_get(host.port, "/shell/../CMakeCache.txt").status == 404);
  CHECK(http_get(host.port, "/shell/%2e%2e/CMakeCache.txt").status == 404);
  CHECK(http_get(host.port, "/shell/").status == 404);
  CHECK(http_get(host.port, "/elsewhere").status == 404);
  CHECK(http_get(host.port, "/capture/c1").status == 403);                      // no token
  CHECK(http_get(host.port, "/capture/c1?token=" + host.token).status == 404);  // none kept yet
  // A name that is not the listener's own (DNS rebinding) is refused.
  CHECK(http_get(host.port, "/", "evil.example:" + std::to_string(host.port)).status == 403);
}

TEST_CASE("listen: render.load of a session, the camera it framed, records, a kept capture") {
  test::TempDir tmp("engine_host_listen_render");
  const std::string doc = tmp.file("doc");
  ListeningHost host({"--doc", doc, "--create", "--actor", "shell"});
  REQUIRE(write_cube_glb(doc + "/cube.glb"));
  Client c;
  REQUIRE(c.connect(host.port, host.token));
  const std::string session = host.session;
  const JsonValue made =
      c.call("doc.apply", R"({"session":")" + session + R"(","commands":[)" +
                              create_placement(k_left, "left", "[-1.5,0,0]") + "," +
                              create_placement(k_right, "right", "[1.5,0,0]") +
                              R"(],"attribution":{"rationale":"two cubes"}})");
  REQUIRE_MESSAGE(at(at(made, "result"), "committed").as_bool(), write_json(made));

  const JsonValue loaded = c.call("render.load", R"({"session":")" + session + "\"}");
  if (error_code(loaded) == k_render_unavailable) {
    MESSAGE("render.* unavailable here: " << write_json(loaded));
    return;
  }
  REQUIRE_MESSAGE(error_code(loaded) == 0, write_json(loaded));
  const JsonValue& info = at(loaded, "result");
  const std::string scene = std::string(at(info, "scene").as_string());
  CHECK(at(info, "session").as_string() == session);
  CHECK(at(info, "instances").as_int() == 2);
  CHECK(at(info, "rebuilt").as_bool());
  // Exactly one of mesh, scene and session.
  CHECK(error_code(c.call("render.load", R"({"session":")" + session + R"(","mesh":"x.glb"})")) ==
        1004);

  const std::string capture_params =
      R"({"scene":")" + scene +
      R"(","width":96,"height":64,"channels":["color","ids"],"keep":true})";
  const JsonValue shot = c.call("render.capture", capture_params);
  REQUIRE_MESSAGE(error_code(shot) == 0, write_json(shot));
  const JsonValue& result = at(shot, "result");
  // A32: the camera the host framed, which given back draws the same picture.
  const JsonValue& camera = at(result, "camera");
  CHECK(at(camera, "fov_deg").as_float() > 1.0);
  CHECK(at(camera, "position").size() == 3);
  // The ids channel answers in records: instance i is records[i], in id order.
  const JsonValue& records = at(result, "records");
  REQUIRE(records.size() == 2);
  CHECK(records[0].as_string() == k_left);
  CHECK(records[1].as_string() == k_right);
  const JsonValue* files = result.find("files");
  CHECK((files == nullptr || files->size() == 0));  // kept, with no directory: nothing on disk
  const std::string id = std::string(at(result, "capture").as_string());
  REQUIRE_FALSE(id.empty());
  const HttpReply png = http_get(host.port, "/capture/" + id + "?token=" + host.token);
  CHECK(png.status == 200);
  CHECK(png.head.find("image/png") != std::string::npos);
  CHECK(png.body.substr(1, 3) == "PNG");
  const HttpReply ids = http_get(host.port, "/capture/" + id + "/ids?token=" + host.token);
  REQUIRE(ids.status == 200);
  REQUIRE(ids.body.size() == usize{96} * 64 * 3 * 4);
  // Some pixel shows each cube, and every covered pixel's instance has a record.
  bool saw[2] = {false, false};
  for (usize p = 0; p < usize{96} * 64; ++p) {
    u32 instance = 0;
    std::memcpy(&instance, ids.body.data() + p * 12, 4);
    if (instance < 2) saw[instance] = true;
  }
  CHECK(saw[0]);
  CHECK(saw[1]);

  // The same camera given back, as a RenderCamera, draws the same bytes.
  JsonValue given = JsonValue::object();
  for (const char* field : {"position", "target", "fov_deg", "znear"})
    given.set(field, at(camera, field));
  const std::string again_params = R"({"scene":")" + scene +
                                   R"(","width":96,"height":64,"keep":true,"camera":)" +
                                   write_json(given) + "}";
  const JsonValue again = c.call("render.capture", again_params);
  REQUIRE_MESSAGE(error_code(again) == 0, write_json(again));
  CHECK(write_json(at(at(again, "result"), "camera")) == write_json(camera));
  const HttpReply png2 = http_get(
      host.port, "/capture/" + std::string(at(at(again, "result"), "capture").as_string()) +
                     "?token=" + host.token);
  CHECK(png2.body == png.body);

  // A transform edit moves the instance in place: the same scene id, one moved, no rebuild.
  const JsonValue moved = c.call(
      "doc.apply",
      R"({"session":")" + session + R"(","commands":[{"kind":"SetProperty","id":")" + k_right +
          R"(","name":"translation","value":[3,0,0]}],"attribution":{"rationale":"move right"}})");
  REQUIRE_MESSAGE(at(at(moved, "result"), "committed").as_bool(), write_json(moved));
  const JsonValue reloaded = c.call("render.load", R"({"session":")" + session + "\"}");
  REQUIRE_MESSAGE(error_code(reloaded) == 0, write_json(reloaded));
  CHECK(at(at(reloaded, "result"), "scene").as_string() == scene);
  CHECK(at(at(reloaded, "result"), "moved").as_int() == 1);
  CHECK_FALSE(at(at(reloaded, "result"), "rebuilt").as_bool());
  const JsonValue after = c.call("render.capture", again_params);
  REQUIRE(error_code(after) == 0);
  const HttpReply png3 = http_get(
      host.port, "/capture/" + std::string(at(at(after, "result"), "capture").as_string()) +
                     "?token=" + host.token);
  CHECK(png3.body != png.body);  // the right cube is somewhere else now

  // A new placement is a rebuild into the same scene id.
  const JsonValue third = c.call(
      "doc.apply", R"({"session":")" + session + R"(","commands":[)" +
                       create_placement("00000000000000200000000000000003", "third", "[0,2,0]") +
                       R"(],"attribution":{"rationale":"a third"}})");
  REQUIRE(at(at(third, "result"), "committed").as_bool());
  const JsonValue rebuilt = c.call("render.load", R"({"session":")" + session + "\"}");
  REQUIRE_MESSAGE(error_code(rebuilt) == 0, write_json(rebuilt));
  CHECK(at(at(rebuilt, "result"), "scene").as_string() == scene);
  CHECK(at(at(rebuilt, "result"), "rebuilt").as_bool());
  CHECK(at(at(rebuilt, "result"), "instances").as_int() == 3);
}
