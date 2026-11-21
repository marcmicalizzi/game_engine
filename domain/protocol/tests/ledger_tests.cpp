// Plan 06 §6.8's task ledger and review items over the dispatcher (docs/subsystems/protocol.md,
// "The task ledger and review items"): a task's life from creation to done, read back by another
// host; who may claim, update and close what; a review item for a person that no agent can close;
// and two hosts writing one directory's ledger at once without losing a write.
#include <core/json/json.h>
#include <domain/protocol/clients.h>
#include <domain/protocol/dir_watch.h>
#include <domain/protocol/events.h>
#include <domain/protocol/listener.h>
#include <domain/protocol/policy.h>
#include <domain/protocol/rpc.h>
#include <domain/protocol/session.h>
#include <foundation/io/roots.h>
#include <foundation/io/vfs.h>
#include <foundation/tunables/tunables.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#if ENGINE_PLATFORM_WINDOWS
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <cstring>
#include <windows.h>
#include <winioctl.h>  // FSCTL_SET_REPARSE_POINT, for a junction
#undef far             // windows.h's, which the cases use as a name
#undef near
#else
#include <sys/stat.h>  // mkfifo
#endif

using namespace engine;
using namespace engine::protocol;

namespace {

JsonValue obj(std::initializer_list<std::pair<const char*, JsonValue>> fields) {
  JsonValue o = JsonValue::object();
  for (auto& [k, v] : fields)
    o.set(k, v);
  return o;
}

const JsonValue& at(const JsonValue& o, std::string_view key) {
  const JsonValue* v = o.find(key);
  REQUIRE_MESSAGE(v != nullptr, "no '" << key << "' in " << write_json(o));
  return *v;
}

i64 num(const JsonValue& v) {
  i64 n = -1;
  REQUIRE(v.get_i64(n));
  return n;
}

JsonValue who(const char* actor, const char* role = "") {
  return obj({{"actor", JsonValue(actor)}, {"role", JsonValue(role)}});
}

JsonValue strings(std::initializer_list<const char*> list) {
  JsonValue a = JsonValue::array();
  for (const char* s : list)
    a.push_back(JsonValue(s));
  return a;
}

std::string hex(Id128 id) {
  char buf[Id128::k_hex_length + 1];
  id.to_hex(buf);
  return buf;
}

// Collects what a session publishes, as a socket connection would be sent it.
struct Collect final : EventTransport {
  std::vector<std::string> sent;
  Post post(u64, std::string text, u32) override {
    sent.push_back(std::move(text));
    return Post::Queued;
  }
};

// One host: a session manager and a dispatcher over a document directory.
struct Host {
  io::Vfs vfs;
  SessionManager sessions{vfs};
  Policy policy;
  // Every path a case names is under a TempDir, and every TempDir under one parent: that parent is
  // the host's one writable root, as an operator's --root would be (paths.h).
  test::TempDir roots_anchor{"roots"};
  io::Roots roots;
  Dispatcher dispatcher{Context{&sessions, nullptr, nullptr, nullptr, &policy}};
  std::string session;

  Host() {
    (void)roots.add("tests", std::string(io::parent_path(roots_anchor.path())), true);
    dispatcher.context().roots = &roots;
    add_builtin_methods(dispatcher);
  }

  JsonValue call(std::string_view method, JsonValue params) {
    JsonValue req = obj({{"jsonrpc", JsonValue("2.0")}, {"id", JsonValue(1)}});
    req.set("method", JsonValue(method));
    if (params.is_object() && !session.empty() && params.find("session") == nullptr)
      params.set("session", JsonValue(session));
    req.set("params", std::move(params));
    return dispatcher.dispatch(req);
  }
  JsonValue ok(std::string_view method, JsonValue params = JsonValue::object()) {
    JsonValue resp = call(method, std::move(params));
    if (const JsonValue* e = resp.find("error")) FAIL(method << " failed: " << write_json(*e));
    return *resp.find("result");
  }
  i32 fails(std::string_view method, JsonValue params, JsonValue* out = nullptr) {
    JsonValue resp = call(method, std::move(params));
    const JsonValue* e = resp.find("error");
    REQUIRE_MESSAGE(e != nullptr, method << " succeeded: " << write_json(resp));
    if (out != nullptr) *out = *e;
    return static_cast<i32>(e->find("code")->as_int());
  }
  void open(const std::string& dir, bool create) {
    JsonValue info =
        ok("session.open", obj({{"path", JsonValue(dir)}, {"create", JsonValue(create)}}));
    session = std::string(at(info, "session").as_string());
  }
  void roles(const char* text) {
    std::string error;
    REQUIRE_MESSAGE(policy.load_roles("roles.json", text, error), error);
  }
  // The calls that follow come on socket connection `connection`, bound to the credential `name`
  // the operator issued for `actor` in `role`, as the listener binds one that presented its token
  // (clients.h); connection 0 is stdio again.
  Clients clients;
  void bind(u64 connection, const char* name, const char* actor, const char* role) {
    Client& c = clients.touch(connection, 0);
    c.credential = name;
    c.who.actor = actor;
    c.who.role = role;
    c.identified = true;
    c.bound = true;
    dispatcher.context().clients = &clients;
    dispatcher.context().connection = connection;
  }
  void stdio() { dispatcher.context().connection = 0; }
  // The one task or item a write answers.
  JsonValue task(std::string_view method, JsonValue params) {
    return at(ok(method, std::move(params)), "tasks")[0];
  }
  JsonValue review(std::string_view method, JsonValue params) {
    return at(ok(method, std::move(params)), "reviews")[0];
  }
};

std::string reason_of(const JsonValue& error) {
  return std::string(at(at(error, "data"), "reason").as_string());
}

const char* k_roles = R"({
  "default_role": "qa",
  "roles": [
    {"name": "director", "methods": ["*"], "layers": ["*"], "object_types": ["*"]},
    {"name": "environment", "methods": ["task.*", "review.*"]},
    {"name": "designer", "methods": ["task.*", "review.*"]},
    {"name": "reviewer", "methods": ["review.*", "task.claim"], "human": true},
    {"name": "qa", "methods": ["review.raise"]}
  ]
})";

}  // namespace

TEST_CASE("ledger: a task's life, from creation to done, read back by another host") {
  test::TempDir tmp("protocol_ledger");
  const std::string dir = tmp.file("doc");
  Host h;
  Collect pushed;
  Subscriptions subscriptions;
  subscriptions.attach(&pushed);
  h.sessions.set_events(&subscriptions);
  (void)subscriptions.subscribe(7, "", event_bit(EventKind::Session));
  h.open(dir, true);

  JsonValue t = h.task("task.create",
                       obj({{"title", JsonValue("Lay the north road")},
                            {"goal", JsonValue("A road from the gate to the well.\nKeep the old "
                                               "wall.")},
                            {"acceptance", strings({"the road validates", "no tile past (3, 0)"})},
                            {"links", obj({{"proposals", strings({"roads"})}})},
                            {"attribution", who("director")}}));
  // Ids are drawn, not counted: a task is known by the id its create answered.
  const JsonValue id1 = at(t, "id");
  CHECK(num(id1) >= 1);
  CHECK(at(t, "key").as_string() == "task/" + std::to_string(num(id1)));
  CHECK(at(t, "created_via").as_string() == "stdio");  // a call on stdin names itself
  CHECK(at(at(t, "history")[0], "via").as_string() == "stdio");
  CHECK(at(t, "status").as_string() == "Open");
  CHECK(at(at(t, "created_by"), "actor").as_string() == "director");
  CHECK(at(at(t, "links"), "proposals").size() == 1);
  const JsonValue id2 =
      at(h.task("task.create",
                obj({{"title", JsonValue("Second")}, {"attribution", who("director")}})),
         "id");
  CHECK(num(id2) != num(id1));

  // a claims it; b is refused while the claim stands, and may not update it.
  t = h.task("task.claim", obj({{"task", id1}, {"attribution", who("a")}}));
  CHECK(at(t, "status").as_string() == "Claimed");
  CHECK(at(t, "claimant").as_string() == "a");
  JsonValue error;
  CHECK(h.fails("task.claim", obj({{"task", id1}, {"attribution", who("b")}}), &error) ==
        codes::k_lease_conflict);
  CHECK(at(at(error, "data"), "claimant").as_string() == "a");
  CHECK(h.fails("task.update",
                obj({{"task", id1}, {"note", JsonValue("mine now")}, {"attribution", who("b")}}),
                &error) == codes::k_forbidden);
  CHECK(reason_of(error) == "task owner");
  // The creator may not finish it: only the claimant does.
  CHECK(h.fails("task.update", obj({{"task", id1},
                                    {"status", JsonValue("Done")},
                                    {"attribution", who("director")}})) ==
        codes::k_invalid_argument);

  // a blocks with a note, unblocks, links a checkpoint, finishes.
  t = h.task("task.update", obj({{"task", id1},
                                 {"status", JsonValue("Blocked")},
                                 {"note", JsonValue("the well is not placed yet")},
                                 {"attribution", who("a")}}));
  CHECK(at(t, "status").as_string() == "Blocked");
  t = h.task("task.update", obj({{"task", id1},
                                 {"status", JsonValue("Claimed")},
                                 {"links", obj({{"checkpoints", strings({"abc123"})},
                                                {"proposals", strings({"roads"})}})},
                                 {"attribution", who("a")}}));
  CHECK(at(at(t, "links"), "proposals").size() == 1);  // a duplicate is not added twice
  CHECK(at(at(t, "links"), "checkpoints").size() == 1);
  t = h.task("task.update",
             obj({{"task", id1}, {"status", JsonValue("Done")}, {"attribution", who("a")}}));
  CHECK(at(t, "status").as_string() == "Done");
  CHECK(at(t, "claimant").as_string() == "a");  // who did it stays
  CHECK(h.fails("task.claim", obj({{"task", id1}, {"attribution", who("b")}})) ==
        codes::k_invalid_argument);
  const JsonValue& history = at(t, "history");
  std::vector<std::string> changes;
  for (u32 i = 0; i < history.size(); ++i)
    changes.emplace_back(at(history[i], "change").as_string());
  CHECK(changes == std::vector<std::string>{"created", "claimed", "status", "note", "status",
                                            "linked", "status"});

  // Pushed to a subscriber, in order, naming the task and who changed it.
  REQUIRE(pushed.sent.size() >= 6);
  CHECK(pushed.sent[1].find("\"change\":\"task_created\"") != std::string::npos);
  CHECK(pushed.sent[3].find("\"change\":\"task_claimed\"") != std::string::npos);
  CHECK(pushed.sent[3].find("\"task\":" + std::to_string(num(id1))) != std::string::npos);
  CHECK(pushed.sent[3].find("\"actor\":\"a\"") != std::string::npos);

  // Another host on the same directory reads what the first wrote: nothing lives in a host.
  Host second;
  second.open(dir, false);
  JsonValue listed = second.ok("task.list", obj({{"statuses", strings({"Open"})}}));
  REQUIRE(at(listed, "tasks").size() == 1);
  CHECK(num(at(at(listed, "tasks")[0], "id")) == num(id2));
  listed = second.ok("task.list", obj({{"task", id1}, {"brief", JsonValue(true)}}));
  CHECK(at(at(listed, "tasks")[0], "history").size() == 0);
  CHECK(second.fails("task.list", obj({{"task", JsonValue(9)}})) == codes::k_not_found);
  // The creator reopens it; nobody holds it then.
  t = second.task(
      "task.update",
      obj({{"task", id1}, {"status", JsonValue("Open")}, {"attribution", who("director")}}));
  CHECK(at(t, "status").as_string() == "Open");
  CHECK(at(t, "claimant").as_string().empty());

  // Malformed asks.
  CHECK(h.fails("task.create", obj({{"title", JsonValue("")}, {"attribution", who("d")}})) ==
        codes::k_invalid_argument);
  CHECK(h.fails("task.create", obj({{"title", JsonValue("a\x01")}, {"attribution", who("d")}})) ==
        codes::k_invalid_argument);
  CHECK(h.fails("task.claim",
                obj({{"task", id2}, {"ttl_seconds", JsonValue(0.5)}, {"attribution", who("a")}})) ==
        codes::k_invalid_argument);
}

TEST_CASE("ledger: a claim nobody renews lapses and the task is open again") {
  test::TempDir tmp("protocol_ledger");
  Host h;
  h.open(tmp.file("doc"), true);
  const JsonValue id =
      at(h.task("task.create", obj({{"title", JsonValue("t")}, {"attribution", who("c")}})), "id");
  (void)h.task("task.claim",
               obj({{"task", id}, {"ttl_seconds", JsonValue(1.0)}, {"attribution", who("a")}}));
  std::this_thread::sleep_for(std::chrono::milliseconds(1100));
  const JsonValue listed = h.ok("task.list");
  CHECK(num(at(listed, "lapsed")) == 1);
  const JsonValue& t = at(listed, "tasks")[0];
  CHECK(at(t, "status").as_string() == "Open");
  const JsonValue& history = at(t, "history");
  CHECK(at(history[history.size() - 1], "change").as_string() == "lapsed");
  CHECK(at(history[history.size() - 1], "actor").as_string() == "a");
  CHECK(at(history[history.size() - 1], "via").as_string() ==
        "host");  // the host's clock, no caller
  // b may take it now; a's renewal is a claim of a task b holds.
  (void)h.task("task.claim", obj({{"task", id}, {"attribution", who("b")}}));
  CHECK(h.fails("task.claim", obj({{"task", id}, {"attribution", who("a")}})) ==
        codes::k_lease_conflict);
}

TEST_CASE("ledger: roles, and a review item for a person that no agent closes") {
  test::TempDir tmp("protocol_ledger");
  Host h;
  h.roles(k_roles);
  h.open(tmp.file("doc"), true);
  const Id128 rec = Id128::from_parts(0x10, 1);
  JsonValue cmd = obj({{"kind", JsonValue("CreateObject")},
                       {"id", JsonValue(hex(rec))},
                       {"type", JsonValue("engine.world.Node")},
                       {"value", JsonValue::object()}});
  JsonValue cmds = JsonValue::array();
  cmds.push_back(cmd);
  (void)h.ok("doc.apply", obj({{"commands", cmds}, {"attribution", who("setup", "director")}}));

  // A task for one role: another role is refused its claim; an unknown role is refused at create.
  CHECK(h.fails("task.create", obj({{"title", JsonValue("t")},
                                    {"owner_role", JsonValue("painter")},
                                    {"attribution", who("d", "director")}})) ==
        codes::k_invalid_argument);
  const JsonValue dunes = at(h.task("task.create", obj({{"title", JsonValue("dunes")},
                                                        {"owner_role", JsonValue("environment")},
                                                        {"attribution", who("d", "director")}})),
                             "id");
  JsonValue error;
  CHECK(h.fails("task.claim", obj({{"task", dunes}, {"attribution", who("x", "designer")}}),
                &error) == codes::k_forbidden);
  CHECK(reason_of(error) == "task role");
  (void)h.task("task.claim", obj({{"task", dunes}, {"attribution", who("e", "environment")}}));
  // QA may not make tasks (the gate: its role names review.raise alone), but may raise an item.
  CHECK(h.fails("task.create", obj({{"title", JsonValue("t")}, {"attribution", who("q", "qa")}}),
                &error) == codes::k_forbidden);
  CHECK(reason_of(error) == "method");

  // A check above the view policy: an item for a person, with the numbers and no picture.
  JsonValue report = obj({{"validator", JsonValue("anatomy")}, {"score", JsonValue(0.93)}});
  JsonValue r =
      h.review("review.raise",
               obj({{"kind", JsonValue("classified content review")},
                    {"audience", JsonValue("Human")},
                    {"summary", JsonValue("The figure's proportions passed every numeric check.")},
                    {"subject", obj({{"layer", JsonValue("base")},
                                     {"records", strings({hex(rec).c_str()})},
                                     {"journal_length", JsonValue(99)}})},
                    {"report", report},
                    {"task", dunes},
                    {"attribution", who("q", "qa")}}));
  const JsonValue item = at(r, "id");
  CHECK(at(r, "state").as_string() == "Open");
  CHECK(num(at(r, "revision")) == 1);
  CHECK(at(r, "raised_via").as_string() == "stdio");
  CHECK(num(at(at(r, "subject"), "journal_length")) == 1);  // the host's, not the raiser's 99
  CHECK(at(at(r, "report"), "validator").as_string() == "anatomy");
  CHECK(at(at(h.ok("task.list", obj({{"task", dunes}})), "tasks")[0], "reviews").size() == 1);

  // Nobody but a human role closes it: not the agent roles, not the director, not the raiser.
  for (const char* role : {"environment", "director", "qa"}) {
    CAPTURE(role);
    CHECK(h.fails("review.close",
                  obj({{"review", item},
                       {"resolution", JsonValue("Accepted")},
                       {"attribution", who("someone", role)}}),
                  &error) == codes::k_forbidden);
    if (std::string_view(role) != "qa") CHECK(reason_of(error) == "human review");
  }
  CHECK(h.fails("review.close",
                obj({{"review", item},
                     {"resolution", JsonValue("Withdrawn")},
                     {"attribution", who("q", "environment")}}),
                &error) == codes::k_forbidden);
  CHECK(reason_of(error) == "human review");
  // Naming the human role does not make a call a person's (ADR-0072): before the mend this closed
  // the item, from stdio, engine-cli or any agent that typed the role's name.
  CHECK(h.fails("review.close",
                obj({{"review", item},
                     {"resolution", JsonValue("Accepted")},
                     {"attribution", who("person", "reviewer")}}),
                &error) == codes::k_forbidden);
  CHECK(reason_of(error) == "human review");
  CHECK(at(h.review("review.list", obj({{"review", item}})), "state").as_string() == "Open");
  // A credential the operator issued for the role, presented on a connection, does; but not
  // against a revision of the item its closer did not see.
  h.bind(5, "person-at-desk", "person", "reviewer");
  CHECK(h.fails("review.close",
                obj({{"review", item},
                     {"resolution", JsonValue("Accepted")},
                     {"revision", JsonValue(u64{7})},
                     {"attribution", who("person")}}),
                &error) == codes::k_invalid_argument);
  CHECK(reason_of(error) == "revision moved");
  CHECK(num(at(at(error, "data"), "revision")) == 1);
  // Nor without the subject's digest its detail showed (protocol.md, "What a close is tied to").
  CHECK(h.fails("review.close",
                obj({{"review", item},
                     {"resolution", JsonValue("Accepted")},
                     {"revision", JsonValue(u64{1})},
                     {"attribution", who("person")}}),
                &error) == codes::k_invalid_argument);
  CHECK(reason_of(error) == "seen required");
  const JsonValue seen_digest = at(r, "raised_digest");
  r = h.review("review.close", obj({{"review", item},
                                    {"resolution", JsonValue("Accepted")},
                                    {"note", JsonValue("looked at full policy; fine")},
                                    {"revision", JsonValue(u64{1})},
                                    {"digest", seen_digest},
                                    {"attribution", who("person")}}));
  CHECK(at(r, "decided_at").as_string() == seen_digest.as_string());
  CHECK(at(r, "state").as_string() == "Closed");
  CHECK(at(r, "resolution").as_string() == "Accepted");
  CHECK(at(at(r, "closed_by"), "actor").as_string() == "person");
  CHECK(num(at(r, "revision")) == 2);  // the close moved it
  CHECK(at(r, "closed_via").as_string() == "credential:person-at-desk");
  CHECK(at(r, "closed_human_via").as_string() == "person-at-desk");
  const JsonValue& closed_note = at(r, "history")[at(r, "history").size() - 1];
  CHECK(at(closed_note, "via").as_string() == "credential:person-at-desk");
  CHECK(at(closed_note, "human_via").as_string() == "person-at-desk");
  CHECK(h.fails("review.close", obj({{"review", item},
                                     {"resolution", JsonValue("Rejected")},
                                     {"attribution", who("person")}})) ==
        codes::k_invalid_argument);
  h.stdio();

  // A task for the person's role is claimed by the person, not by a working role of that name.
  const JsonValue person_task = h.task("task.create", obj({{"title", JsonValue("look at it")},
                                                           {"owner_role", JsonValue("reviewer")},
                                                           {"attribution", who("d", "director")}}));
  for (const char* role : {"director", "reviewer"}) {
    CAPTURE(role);
    CHECK(h.fails("task.claim",
                  obj({{"task", at(person_task, "id")}, {"attribution", who("p", role)}}),
                  &error) == codes::k_forbidden);
    CHECK(reason_of(error) == "human review");
  }
  h.bind(5, "person-at-desk", "person", "reviewer");
  CHECK(at(h.task("task.claim", obj({{"task", at(person_task, "id")}})), "claimant").as_string() ==
        "person");
  h.stdio();

  // An item for an agent: its raiser withdraws it, nobody else does, a reviewing agent closes one.
  JsonValue agent_items[2];
  for (JsonValue& id : agent_items)
    id = at(h.review("review.raise", obj({{"kind", JsonValue("validation")},
                                          {"summary", JsonValue("two props overlap")},
                                          {"attribution", who("e", "environment")}})),
            "id");
  CHECK(num(agent_items[0]) != num(agent_items[1]));
  CHECK(h.fails("review.close",
                obj({{"review", agent_items[0]},
                     {"resolution", JsonValue("Withdrawn")},
                     {"attribution", who("other", "designer")}}),
                &error) == codes::k_forbidden);
  CHECK(reason_of(error) == "review raiser");
  CHECK(at(h.review("review.close", obj({{"review", agent_items[0]},
                                         {"resolution", JsonValue("Withdrawn")},
                                         {"attribution", who("e", "environment")}})),
           "resolution")
            .as_string() == "Withdrawn");
  CHECK(at(h.review("review.close", obj({{"review", agent_items[1]},
                                         {"resolution", JsonValue("Rejected")},
                                         {"attribution", who("other", "designer")}})),
           "resolution")
            .as_string() == "Rejected");

  // The lists, read under any role.
  CHECK(at(h.ok("review.list", obj({{"audience", JsonValue("Human")}})), "reviews").size() == 1);
  CHECK(at(h.ok("review.list", obj({{"kind", JsonValue("validation")}})), "reviews").size() == 2);
  CHECK(at(h.ok("review.list", obj({{"state", JsonValue("Open")}})), "reviews").size() == 0);

  // What an item may name: live records, a layer that exists, a task that exists, a bounded
  // report.
  const auto raise = [&](JsonValue subject, JsonValue rep, i64 task) {
    return h.fails("review.raise", obj({{"kind", JsonValue("validation")},
                                        {"summary", JsonValue("s")},
                                        {"subject", std::move(subject)},
                                        {"report", std::move(rep)},
                                        {"task", JsonValue(task)},
                                        {"attribution", who("e", "environment")}}));
  };
  CHECK(raise(obj({{"records", strings({hex(Id128::from_parts(9, 9)).c_str()})}}), JsonValue(),
              0) == codes::k_not_found);
  CHECK(raise(obj({{"layer", JsonValue("nowhere")}}), JsonValue(), 0) == codes::k_not_found);
  CHECK(raise(JsonValue::object(), JsonValue(), 42) == codes::k_not_found);
  CHECK(raise(JsonValue::object(), JsonValue(std::string(70000, 'x')), 0) ==
        codes::k_invalid_argument);
}

// The ledger goes through the side files' one helper (side_file.h): a read of a document with no
// ledger leaves no lock file in it (the first helper made `ledger.json.lock` on every call, reads
// included), and a read that lets nothing lapse does not rewrite the file.
TEST_CASE("ledger: reads leave a document as they found it") {
  test::TempDir tmp("protocol_ledger");
  const std::string dir = tmp.file("doc");
  Host h;
  h.open(dir, true);
  CHECK(at(h.ok("task.list"), "tasks").size() == 0);
  CHECK(at(h.ok("review.list"), "reviews").size() == 0);
  CHECK(!h.vfs.exists(dir + "/ledger.json.lock"));
  CHECK(!h.vfs.exists(dir + "/ledger.json"));
  (void)h.task("task.create", obj({{"title", JsonValue("t")}, {"attribution", who("c")}}));
  std::string before, after;
  REQUIRE(h.vfs.read(dir + "/ledger.json", before) == io::Status::Ok);
  (void)h.ok("task.list");
  REQUIRE(h.vfs.read(dir + "/ledger.json", after) == io::Status::Ok);
  CHECK(before == after);
}

namespace {
// Every file under `dir`, by its path from `dir`, with its bytes: what a read must leave as it was.
std::vector<std::pair<std::string, std::string>> files_under(const std::string& dir) {
  std::vector<std::pair<std::string, std::string>> out;
  const std::filesystem::path root(dir);
  for (const auto& e : std::filesystem::recursive_directory_iterator(root)) {
    if (!e.is_regular_file()) continue;
    std::string bytes;
    REQUIRE(io::read_file(e.path().string(), bytes) == io::Status::Ok);
    out.emplace_back(std::filesystem::relative(e.path(), root).generic_string(), std::move(bytes));
  }
  std::sort(out.begin(), out.end());
  return out;
}
}  // namespace

// A document a client opened from a root the operator named read-only may not be written
// (protocol.md, "The host's file roots"), and that includes the files beside it: before the side
// files' one gate asked that, the open's read of the leases made `leases.json.lock` and removed a
// gone host's mark, `task.list` made `ledger.json.lock`, and `lease.list` was refused outright,
// since it lets lapsed leases go through the writer. The document here came without its lock files,
// as one copied into a library does, with a lease a host that has gone left behind.
TEST_CASE(
    "ledger: a document under a read-only root reads its ledger and leases and writes "
    "nothing") {
  test::TempDir tmp("protocol_ledger_read_only");
  const std::string world = tmp.file("lib") + "/world";
  {
    Host h;
    h.open(world, true);
    (void)h.task("task.create", obj({{"title", JsonValue("survey")}, {"attribution", who("c")}}));
    JsonValue tiles = JsonValue::array();
    tiles.push_back(obj(
        {{"x0", JsonValue(0)}, {"y0", JsonValue(0)}, {"x1", JsonValue(0)}, {"y1", JsonValue(0)}}));
    (void)h.ok("lease.acquire", obj({{"layer", JsonValue("base")},
                                     {"tiles", tiles},
                                     {"ttl_seconds", JsonValue(600.0)},
                                     {"attribution", who("c")}}));
    (void)h.ok("session.close", JsonValue::object());
  }
  for (const auto& e : std::filesystem::directory_iterator(std::filesystem::path(world))) {
    if (e.path().extension() == ".lock") std::filesystem::remove(e.path());
  }
  std::string text;
  REQUIRE(io::read_file(world + "/leases.json", text) == io::Status::Ok);
  JsonValue leases;
  REQUIRE(parse_json(text, leases).ok);
  JsonValue* list = leases.find("leases");
  REQUIRE(list != nullptr);
  REQUIRE(list->size() == 1);
  (*list)[usize{0}].set("connection", JsonValue(u64{7}));
  (*list)[usize{0}].set("host", JsonValue("h-gone"));
  REQUIRE(io::write_file(world + "/leases.json", write_json(leases)) == io::Status::Ok);
  REQUIRE(io::write_file(world + "/leases.h-gone.lock", "") == io::Status::Ok);
  const auto before = files_under(world);

  io::Roots read_only;
  REQUIRE(read_only.add("lib", tmp.file("lib"), false) == io::Status::Ok);
  {
    Host h;
    h.dispatcher.context().roots = &read_only;
    h.open("lib://world", false);
    CHECK(at(h.ok("task.list"), "tasks").size() == 1);
    CHECK(at(h.ok("review.list"), "reviews").size() == 0);
    CHECK(at(h.ok("lease.list"), "leases").size() == 1);
    CHECK(at(h.ok("doc.transactions"), "transactions").size() == 0);
    JsonValue error;
    CHECK(h.fails("task.create", obj({{"title", JsonValue("more")}, {"attribution", who("c")}}),
                  &error) == codes::k_not_found);
    CHECK(at(error, "message").as_string() ==
          "path: no place this host may write at 'lib://world'");
    (void)h.ok("session.close", JsonValue::object());
    h.dispatcher.context().roots = &h.roots;
  }
  CHECK(files_under(world) == before);
}

TEST_CASE("ledger: a host with no human role has nobody who closes a person's item") {
  test::TempDir tmp("protocol_ledger");
  Host h;  // no roles file: nothing restricts a call, and nothing marks one a person's
  h.open(tmp.file("doc"), true);
  const JsonValue item =
      at(h.review("review.raise", obj({{"kind", JsonValue("classified content review")},
                                       {"audience", JsonValue("Human")},
                                       {"summary", JsonValue("s")},
                                       {"attribution", who("a")}})),
         "id");
  JsonValue error;
  CHECK(h.fails("review.close", obj({{"review", item}, {"attribution", who("a")}}), &error) ==
        codes::k_forbidden);
  CHECK(reason_of(error) == "human review");
}

TEST_CASE("ledger: two hosts writing one directory at once lose nothing") {
  // Two session managers on one directory, as engine-cli's host per call and engine-host are,
  // each on its own thread: every create and every note lands, and no id is minted twice. Without
  // the lock around the read-modify-write each would write the ledger it read, and the other's
  // writes since would be gone.
  test::TempDir tmp("protocol_ledger");
  const std::string dir = tmp.file("doc");
  Host a;
  a.open(dir, true);
  const JsonValue shared =
      at(a.task("task.create", obj({{"title", JsonValue("shared")}, {"attribution", who("w")}})),
         "id");
  Host b;
  b.open(dir, false);
  constexpr int k_each = 25;
  std::vector<i64> ids_a, ids_b;
  const auto work = [&](Host& h, const char* actor, std::vector<i64>& ids) {
    for (int i = 0; i < k_each; ++i) {
      ids.push_back(num(
          at(h.task("task.create", obj({{"title", JsonValue(actor)}, {"attribution", who(actor)}})),
             "id")));
      (void)h.task("task.update",
                   obj({{"task", shared}, {"note", JsonValue(actor)}, {"attribution", who("w")}}));
    }
  };
  std::thread second([&] { work(b, "b", ids_b); });
  work(a, "a", ids_a);
  second.join();

  const JsonValue listed = a.ok("task.list");
  const JsonValue& tasks = at(listed, "tasks");
  CHECK(tasks.size() == 1 + 2 * k_each);
  std::set<i64> ids;
  for (u32 i = 0; i < tasks.size(); ++i)
    ids.insert(num(at(tasks[i], "id")));
  CHECK(ids.size() == 1 + 2 * k_each);
  for (const i64 id : ids_a)
    CHECK(ids.count(id) == 1);
  for (const i64 id : ids_b)
    CHECK(ids.count(id) == 1);
  // The shared task's history: its creation and every note of both hosts.
  const JsonValue listed_shared = a.ok("task.list", obj({{"task", shared}}));
  const JsonValue& first = at(listed_shared, "tasks")[0];
  CHECK(at(first, "history").size() == 1 + 2 * k_each);
}

// `ledger.json` version 1 counted its ids (`next_task`, `next_review`) and had no revisions; the
// registered step (session.cpp) drops the counters and gives every item revision 1. Its tasks and
// items keep the ids they were known by, a new one is drawn against them, and the file written
// back is the current version (3 since the ledger's own revision; version 2 to 3 is additive).
TEST_CASE("ledger: a version-1 ledger reads, keeps its ids and draws new ones") {
  test::TempDir tmp("protocol_ledger");
  const std::string dir = tmp.file("doc");
  Host h;
  h.open(dir, true);
  {
    std::ofstream out(dir + "/ledger.json", std::ios::binary);
    out << R"({"next_task":3,"next_review":2,)"
           R"("tasks":[{"id":1,"key":"task/1","title":"old","created_by":{"actor":"a"}},)"
           R"({"id":2,"key":"task/2","title":"older","created_by":{"actor":"a"}}],)"
           R"("reviews":[{"id":1,"kind":"validation","summary":"s","raised_by":{"actor":"a"}}]})";
  }
  const JsonValue tasks = at(h.ok("task.list"), "tasks");
  REQUIRE(tasks.size() == 2);
  CHECK(num(at(tasks[0], "id")) == 1);
  CHECK(num(at(tasks[1], "id")) == 2);
  const JsonValue item = h.review("review.list", obj({{"review", JsonValue(u64{1})}}));
  CHECK(num(at(item, "revision")) == 1);
  const i64 made = num(at(
      h.task("task.create", obj({{"title", JsonValue("new")}, {"attribution", who("b")}})), "id"));
  CHECK(made != 1);
  CHECK(made != 2);
  std::string text;
  REQUIRE(h.vfs.read(dir + "/ledger.json", text) == io::Status::Ok);
  CHECK(text.find("next_task") == std::string::npos);
  CHECK(text.find("next_review") == std::string::npos);
  const JsonValue closed = h.review("review.close", obj({{"review", JsonValue(u64{1})},
                                                         {"resolution", JsonValue("Accepted")},
                                                         {"revision", JsonValue(u64{1})},
                                                         {"attribution", who("b")}}));
  CHECK(num(at(closed, "revision")) == 2);
}
// The lists' order, limit and ids (protocol.md, "The ledger's lists"): by last change, then by id,
// `Newest` the reverse; `limit` and how many more matched; the ids an event named, one gone; a
// limit past 1,024 refused rather than clamped; and the ledger's revision, moved by a write and
// not by a read. The ledger is written by hand so the times are the case's, not the clock's.
TEST_CASE("ledger: a list answers in a stated order, to a limit, and by ids") {
  test::TempDir tmp("protocol_ledger");
  const std::string dir = tmp.file("doc");
  Host h;
  h.open(dir, true);
  {
    std::ofstream out(dir + "/ledger.json", std::ios::binary);
    out << R"({"tasks":[)"
           R"({"id":5,"key":"task/5","title":"e","updated_unix_ms":300,"created_by":{"actor":"a"}},)"
           R"({"id":3,"key":"task/3","title":"c","updated_unix_ms":100,"created_by":{"actor":"a"}},)"
           R"({"id":9,"key":"task/9","title":"i","updated_unix_ms":200,"created_by":{"actor":"a"}},)"
           R"({"id":7,"key":"task/7","title":"g","updated_unix_ms":200,"created_by":{"actor":"a"}}],)"
           R"("reviews":[)"
           R"({"id":2,"kind":"k","summary":"s","raised_unix_ms":10,"revision":1,)"
           R"("history":[{"at_unix_ms":50,"actor":"a","change":"raised"}],"raised_by":{"actor":"a"}},)"
           R"({"id":1,"kind":"k","summary":"s","raised_unix_ms":20,"revision":1,"audience":"Human",)"
           R"("raised_by":{"actor":"a"}}]})";
  }
  const auto ids = [](const JsonValue& list) {
    std::vector<i64> out;
    for (u32 i = 0; i < list.size(); ++i)
      out.push_back(num(at(list[i], "id")));
    return out;
  };

  // Oldest (the default) by last change, the tie at 200 by id; Newest the reverse.
  JsonValue r = h.ok("task.list");
  CHECK(ids(at(r, "tasks")) == std::vector<i64>{3, 7, 9, 5});
  CHECK(num(at(r, "more")) == 0);
  CHECK(num(at(r, "ledger_revision")) == 0);  // a version-2 file has none yet
  r = h.ok("task.list", obj({{"order", JsonValue("Newest")}}));
  CHECK(ids(at(r, "tasks")) == std::vector<i64>{5, 9, 7, 3});
  // A limit answers the first of the order and says how many more matched.
  r = h.ok("task.list", obj({{"order", JsonValue("Newest")}, {"limit", JsonValue(2)}}));
  CHECK(ids(at(r, "tasks")) == std::vector<i64>{5, 9});
  CHECK(num(at(r, "more")) == 2);
  // The ids an event named, in the list's order; one the ledger no longer holds is left out.
  JsonValue named = JsonValue::array();
  for (const u64 id : {u64{9}, u64{5}, u64{42}})
    named.push_back(JsonValue(id));
  r = h.ok("task.list", obj({{"tasks", named}, {"brief", JsonValue(true)}}));
  CHECK(ids(at(r, "tasks")) == std::vector<i64>{9, 5});
  CHECK(num(at(r, "more")) == 0);
  // Past the cap is refused, not clamped, and so is a list of more than 1,024 ids.
  JsonValue error;
  CHECK(h.fails("task.list", obj({{"limit", JsonValue(1025)}}), &error) ==
        codes::k_invalid_argument);
  CHECK(reason_of(error) == "list limit");
  JsonValue many = JsonValue::array();
  for (u64 i = 1; i <= 1025; ++i)
    many.push_back(JsonValue(i));
  CHECK(h.fails("review.list", obj({{"reviews", many}}), &error) == codes::k_invalid_argument);
  CHECK(reason_of(error) == "list limit");
  CHECK(h.ok("task.list", obj({{"limit", JsonValue(1024)}})).find("tasks") != nullptr);

  // An item's last change is its newest history line: item 2 (raised at 10, a line at 50) after
  // item 1 (raised at 20).
  r = h.ok("review.list");
  CHECK(ids(at(r, "reviews")) == std::vector<i64>{1, 2});
  r = h.ok("review.list", obj({{"order", JsonValue("Newest")}, {"limit", JsonValue(1)}}));
  CHECK(ids(at(r, "reviews")) == std::vector<i64>{2});
  CHECK(num(at(r, "more")) == 1);
  // `more` counts what matched the filters, not the ledger.
  r = h.ok("review.list", obj({{"audience", JsonValue("Human")}, {"limit", JsonValue(1)}}));
  CHECK(ids(at(r, "reviews")) == std::vector<i64>{1});
  CHECK(num(at(r, "more")) == 0);

  // A write moves the ledger's revision by one; a read that lets nothing lapse leaves it, and the
  // file, as they were.
  const JsonValue made =
      h.ok("task.create", obj({{"title", JsonValue("new")}, {"attribution", who("b")}}));
  CHECK(num(at(made, "ledger_revision")) == 1);
  std::string before;
  REQUIRE(h.vfs.read(dir + "/ledger.json", before) == io::Status::Ok);
  CHECK(num(at(h.ok("review.list"), "ledger_revision")) == 1);
  std::string after;
  REQUIRE(h.vfs.read(dir + "/ledger.json", after) == io::Status::Ok);
  CHECK(after == before);
  // The new task changed last, so it is the newest.
  r = h.ok("task.list", obj({{"order", JsonValue("Newest")}, {"limit", JsonValue(1)}}));
  CHECK(num(at(at(r, "tasks")[0], "id")) == num(at(at(made, "tasks")[0], "id")));
}

namespace {

// The params of every `ledger_changed` event pushed since `from`.
std::vector<JsonValue> ledger_changes(const Collect& pushed, usize from) {
  std::vector<JsonValue> out;
  for (usize i = from; i < pushed.sent.size(); ++i) {
    JsonValue note;
    REQUIRE(parse_json(pushed.sent[i], note).ok);
    const JsonValue& params = at(note, "params");
    if (at(params, "change").as_string() == "ledger_changed") out.push_back(params);
  }
  return out;
}

std::set<i64> id_set(const JsonValue& list) {
  std::set<i64> out;
  for (u32 i = 0; i < list.size(); ++i)
    out.insert(num(list[i]));
  return out;
}

}  // namespace

// Another process's writes reach a subscriber as `ledger_changed` (protocol.md, "The ledger's
// events"), decided from what the file holds and never from the notices: here the looks are made
// by hand, `all` standing for a notice the system said it dropped, so the case holds the decision
// to the bytes whatever the platform's notices do. Two hosts on one directory, as engine-cli's
// host per call is beside engine-host.
TEST_CASE("ledger: another host's writes are pushed as ledger_changed, decided by content") {
  test::TempDir tmp("protocol_ledger");
  const std::string dir = tmp.file("doc");
  Host h;
  Collect pushed;
  Subscriptions subscriptions;
  subscriptions.attach(&pushed);
  h.sessions.set_events(&subscriptions);
  // A watch is started by a waker, as a listener's looker starts one; this one wakes nobody.
  h.sessions.set_ledger_waker([](void*) {}, nullptr);
  (void)subscriptions.subscribe(7, "", event_bit(EventKind::Session));
  h.open(dir, true);
  Host b;
  b.open(dir, false);
  const auto make = [](Host& on, const char* title) {
    return num(
        at(on.task("task.create", obj({{"title", JsonValue(title)}, {"attribution", who("b")}})),
           "id"));
  };

  // A burst of three of the other host's writes, and one look: one event naming the three.
  usize mark = pushed.sent.size();
  const std::set<i64> burst{make(b, "one"), make(b, "two"), make(b, "three")};
  CHECK(h.sessions.look_at_ledgers(true) == 1);
  std::vector<JsonValue> changes = ledger_changes(pushed, mark);
  REQUIRE(changes.size() == 1);
  CHECK(id_set(at(changes[0], "tasks")) == burst);
  CHECK(num(at(changes[0], "tasks_total")) == 3);
  CHECK(num(at(changes[0], "ledger_revision")) == 3);
  // Nothing changed since: a second look publishes nothing.
  mark = pushed.sent.size();
  CHECK(h.sessions.look_at_ledgers(true) == 0);
  CHECK(pushed.sent.size() == mark);

  // A rewrite with the same bytes, and one with the same content in other spacing, change nothing.
  std::string three;
  REQUIRE(h.vfs.read(dir + "/ledger.json", three) == io::Status::Ok);
  {
    std::ofstream out(dir + "/ledger.json", std::ios::binary);
    out << three;
  }
  CHECK(h.sessions.look_at_ledgers(true) == 0);
  {
    std::ofstream out(dir + "/ledger.json", std::ios::binary);
    out << "  " << three << "\n\n";
  }
  CHECK(h.sessions.look_at_ledgers(true) == 0);
  CHECK(pushed.sent.size() == mark);

  // A fourth, then the ledger restored to the copy of three: its revision went back, and that is a
  // change naming the task that is gone.
  const i64 fourth = make(b, "four");
  CHECK(h.sessions.look_at_ledgers(true) == 1);
  changes = ledger_changes(pushed, mark);
  REQUIRE(changes.size() == 1);
  CHECK(id_set(at(changes[0], "tasks")) == std::set<i64>{fourth});
  CHECK(num(at(changes[0], "ledger_revision")) == 4);
  mark = pushed.sent.size();
  {
    std::ofstream out(dir + "/ledger.json", std::ios::binary);
    out << three;
  }
  CHECK(h.sessions.look_at_ledgers(true) == 1);
  changes = ledger_changes(pushed, mark);
  REQUIRE(changes.size() == 1);
  CHECK(id_set(at(changes[0], "tasks")) == std::set<i64>{fourth});
  CHECK(num(at(changes[0], "ledger_revision")) == 3);  // inequality, not "greater"

  // The other host writes and this one writes before any look: the other's write is told first,
  // then this host's own event, and the notice of its own write publishes nothing more.
  mark = pushed.sent.size();
  const i64 fifth = make(b, "five");
  const i64 sixth = make(h, "six");
  REQUIRE(pushed.sent.size() == mark + 2);
  changes = ledger_changes(pushed, mark);
  REQUIRE(changes.size() == 1);
  CHECK(id_set(at(changes[0], "tasks")) == std::set<i64>{fifth});
  CHECK(pushed.sent[mark + 1].find("\"change\":\"task_created\"") != std::string::npos);
  CHECK(pushed.sent[mark + 1].find("\"task\":" + std::to_string(sixth)) != std::string::npos);
  CHECK(h.sessions.look_at_ledgers(true) == 0);
  CHECK(pushed.sent.size() == mark + 2);
}

// The platform's own notice reaches the session's watch (dir_watch.h): a storm of another host's
// writes wakes the looker, and one look after it is one event naming every write. Where the system
// gives no notice the watch looks once a second, which wakes the looker too.
TEST_CASE("ledger: the system's directory-change notice wakes the look, once for a storm") {
  test::TempDir tmp("protocol_ledger");
  const std::string dir = tmp.file("doc");
  // Before the host, so the session's watch, which calls the waker, stops before this goes.
  struct Woken {
    std::mutex mutex;
    std::condition_variable cv;
    int count = 0;
  } woken;
  Collect pushed;
  Subscriptions subscriptions;
  subscriptions.attach(&pushed);
  Host h;
  h.sessions.set_ledger_waker(
      [](void* context) {
        auto* w = static_cast<Woken*>(context);
        {
          std::lock_guard<std::mutex> lock(w->mutex);
          ++w->count;
        }
        w->cv.notify_all();
      },
      &woken);
  h.sessions.set_events(&subscriptions);
  (void)subscriptions.subscribe(7, "", event_bit(EventKind::Session));
  h.open(dir, true);
  Host b;
  b.open(dir, false);

  const usize mark = pushed.sent.size();
  std::set<i64> made;
  for (int i = 0; i < 20; ++i)
    made.insert(num(
        at(b.task("task.create", obj({{"title", JsonValue("storm")}, {"attribution", who("b")}})),
           "id")));
  {
    std::unique_lock<std::mutex> lock(woken.mutex);
    REQUIRE(woken.cv.wait_for(lock, std::chrono::seconds(10), [&] { return woken.count > 0; }));
  }
#if ENGINE_PLATFORM_WINDOWS || defined(__linux__)
  // The system's own notice woke it, not the once-a-second fallback, which marks itself before its
  // first look: both platforms give one.
  const Session* watched = h.sessions.find(h.session);
  REQUIRE(watched != nullptr);
  CHECK_FALSE(watched->ledger_watch_polling());
#endif
  // Notices may still arrive for the last writes; the look decides from the file all the same.
  CHECK(h.sessions.look_at_ledgers(false) + h.sessions.look_at_ledgers(true) == 1);
  const std::vector<JsonValue> changes = ledger_changes(pushed, mark);
  REQUIRE(changes.size() == 1);
  CHECK(id_set(at(changes[0], "tasks")) == made);
  CHECK(num(at(changes[0], "tasks_total")) == 20);
  h.sessions.set_ledger_waker(nullptr, nullptr);
}

// The ledger's bounds (protocol.md, "The ledger's bounds"): 256 open tasks and 256 open items per
// actor, and 8 MiB of file, each refused 1004 naming the bound. The ledgers are written by hand,
// since filling one through calls would cost each case a few thousand of them.
TEST_CASE("ledger: a ledger refuses work past its bounds, and a full one can be worked down") {
  test::TempDir tmp("protocol_ledger");
  const auto bound_of = [](const JsonValue& error) {
    return std::string(at(at(error, "data"), "bound").as_string());
  };
  JsonValue error;

  SUBCASE("open tasks and open items per actor") {
    const std::string dir = tmp.file("doc");
    Host h;
    h.open(dir, true);
    std::string tasks, items;
    for (int i = 1; i <= 256; ++i) {
      const std::string id = std::to_string(i);
      tasks += std::string(i > 1 ? "," : "") + R"({"id":)" + id + R"(,"key":"task/)" + id +
               R"(","title":"t","created_by":{"actor":"loop"}})";
      items += std::string(i > 1 ? "," : "") + R"({"id":)" + id +
               R"(,"kind":"validation","summary":"s","revision":1,"raised_by":{"actor":"loop"}})";
    }
    {
      std::ofstream out(dir + "/ledger.json", std::ios::binary);
      out << R"({"tasks":[)" << tasks << R"(],"reviews":[)" << items << "]}";
    }
    CHECK(h.fails("task.create",
                  obj({{"title", JsonValue("one more")}, {"attribution", who("loop")}}),
                  &error) == codes::k_invalid_argument);
    CHECK(reason_of(error) == "ledger bound");
    CHECK(bound_of(error) == "open tasks per actor");
    CHECK(num(at(at(error, "data"), "limit")) == 256);
    // Another actor is not held to the first one's count, and a dropped task stops counting.
    (void)h.task("task.create", obj({{"title", JsonValue("mine")}, {"attribution", who("other")}}));
    (void)h.task("task.update", obj({{"task", JsonValue(u64{1})},
                                     {"status", JsonValue("Dropped")},
                                     {"attribution", who("loop")}}));
    (void)h.task("task.create", obj({{"title", JsonValue("now")}, {"attribution", who("loop")}}));
    CHECK(h.fails("review.raise",
                  obj({{"kind", JsonValue("validation")},
                       {"summary", JsonValue("one more")},
                       {"attribution", who("loop")}}),
                  &error) == codes::k_invalid_argument);
    CHECK(bound_of(error) == "open items per actor");
    (void)h.review("review.close", obj({{"review", JsonValue(u64{1})},
                                        {"resolution", JsonValue("Withdrawn")},
                                        {"attribution", who("loop")}}));
    (void)h.review("review.raise", obj({{"kind", JsonValue("validation")},
                                        {"summary", JsonValue("now")},
                                        {"attribution", who("loop")}}));
  }
  SUBCASE("the file's size") {
    const std::string dir = tmp.file("doc");
    Host h;
    h.open(dir, true);
    {
      std::ofstream out(dir + "/ledger.json", std::ios::binary);
      out << R"({"tasks":[{"id":5,"key":"task/5","title":"big","created_by":{"actor":"a"},)"
          << R"("history":[{"actor":"a","change":"note","text":")" << std::string(8u << 20, 'x')
          << R"("}]}]})";
    }
    CHECK(h.fails("task.create", obj({{"title", JsonValue("t")}, {"attribution", who("a")}}),
                  &error) == codes::k_invalid_argument);
    CHECK(bound_of(error) == "ledger bytes");
    CHECK(h.fails("task.update",
                  obj({{"task", JsonValue(u64{5})},
                       {"note", JsonValue("more")},
                       {"attribution", who("a")}}),
                  &error) == codes::k_invalid_argument);
    CHECK(bound_of(error) == "ledger bytes");
    // A claim and a status change still answer: a full ledger is worked down, not locked.
    (void)h.task("task.claim", obj({{"task", JsonValue(u64{5})}, {"attribution", who("a")}}));
    CHECK(at(h.task("task.update", obj({{"task", JsonValue(u64{5})},
                                        {"status", JsonValue("Done")},
                                        {"attribution", who("a")}})),
             "status")
              .as_string() == "Done");
  }
}

namespace {

// A placement under a node, its mesh a file beside the document, for the subject digest's cases
// (protocol.md, "What a close is tied to"): the placement is the subject, the node its ancestor,
// and the mesh an asset its record names.
// A binary glTF (`.glb`) holding `json` as its JSON chunk, padded with spaces to four bytes, and
// `bin` as its binary chunk when given: the 12-byte header, then each chunk's 8-byte header.
std::string glb_bytes(std::string_view json, std::string_view bin = {}) {
  std::string j(json);
  while (j.size() % 4 != 0)
    j.push_back(' ');
  std::string b(bin);
  while (b.size() % 4 != 0)
    b.push_back('\0');
  const auto u32_le = [](std::string& out, usize v) {
    for (u32 i = 0; i < 4; ++i)
      out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
  };
  std::string out = "glTF";
  u32_le(out, 2);
  u32_le(out, 12 + 8 + j.size() + (b.empty() ? 0 : 8 + b.size()));
  u32_le(out, j.size());
  out += "JSON";
  out += j;
  if (!b.empty()) {
    u32_le(out, b.size());
    out += std::string("BIN\0", 4);
    out += b;
  }
  return out;
}

// The placement's mesh: a `.glb` naming nothing outside itself, its one word told apart by `tag`
// (`one`, `two`), so a rewrite keeps the length and the container whole.
std::string crate_glb(const char* tag) {
  return glb_bytes(std::string(R"({"asset":{"version":"2.0"},"extras":")") + tag + "\"}");
}

struct DigestWorld {
  Id128 node = Id128::from_parts(0x20, 1);
  Id128 placement = Id128::from_parts(0x20, 2);
  std::string mesh;  // the asset's native path, for a case to rewrite
  i64 mesh_bytes = static_cast<i64>(crate_glb("one").size());

  void make(Host& h, const std::string& dir) {
    std::filesystem::create_directories(std::filesystem::path(dir) / "props");
    mesh = (std::filesystem::path(dir) / "props" / "crate.glb").string();
    {
      std::ofstream out(mesh, std::ios::binary);
      out << crate_glb("one");
    }
    JsonValue cmds = JsonValue::array();
    cmds.push_back(obj({{"kind", JsonValue("CreateObject")},
                        {"id", JsonValue(hex(node))},
                        {"type", JsonValue("engine.world.Node")},
                        {"value", obj({{"name", JsonValue("yard")}})}}));
    cmds.push_back(obj(
        {{"kind", JsonValue("CreateObject")},
         {"id", JsonValue(hex(placement))},
         {"type", JsonValue("engine.scene.MeshPlacement")},
         {"parent", JsonValue(hex(node))},
         {"value", obj({{"name", JsonValue("crate")}, {"mesh", JsonValue("props/crate.glb")}})}}));
    (void)h.ok("doc.apply", obj({{"commands", cmds}, {"attribution", who("setup", "director")}}));
  }
  void set(Host& h, Id128 id, const char* property, JsonValue value) const {
    JsonValue cmds = JsonValue::array();
    cmds.push_back(obj({{"kind", JsonValue("SetProperty")},
                        {"id", JsonValue(hex(id))},
                        {"name", JsonValue(property)},
                        {"value", std::move(value)}}));
    (void)h.ok("doc.apply", obj({{"commands", cmds}, {"attribution", who("agent", "director")}}));
  }
  JsonValue raise(Host& h, const char* audience = "Human") const {
    return h.ok("review.raise", raise_params(audience));
  }
  // The raise's params: the placement as the subject, and `assets` listed beside it.
  JsonValue raise_params(const char* audience = "Human", JsonValue assets = JsonValue()) const {
    JsonValue subject = obj({{"records", strings({hex(placement).c_str()})}});
    if (!assets.is_null()) subject.set("assets", std::move(assets));
    return obj({{"kind", JsonValue("validation")},
                {"audience", JsonValue(audience)},
                {"summary", JsonValue("the crate sits right")},
                {"subject", std::move(subject)},
                {"attribution", who("checker", "director")}});
  }
};

// A digest's bound lowered for one case and put back after it, whatever the case did
// (protocol.md, "What it costs"): a "large" subject is made by lowering the bound, never by
// writing gigabytes.
struct Bound {
  tunables::Tunable* knob = nullptr;
  Bound(const char* name, const char* value) : knob(tunables::find(name)) {
    REQUIRE_MESSAGE(knob != nullptr, name);
    REQUIRE(knob->set_from_text(value));
  }
  ~Bound() { knob->reset(); }
  Bound(const Bound&) = delete;
  Bound& operator=(const Bound&) = delete;
};

std::string bound_in(const JsonValue& error) {
  return std::string(at(at(error, "data"), "bound").as_string());
}

// The subject as an item's opened detail sees it now.
JsonValue opened_detail(Host& h, const JsonValue& item) {
  return at(h.ok("review.list", obj({{"review", item}, {"digest", JsonValue(true)}})), "subject");
}

// What one call of the digest opened and read, from the session manager's two counters.
struct Reads {
  u64 opens = 0;
  u64 bytes = 0;
};
Reads reads_of(Host& h, const std::function<void()>& call) {
  const u64 opens = h.sessions.digest_files_opened();
  const u64 bytes = h.sessions.digest_bytes_read();
  call();
  return Reads{h.sessions.digest_files_opened() - opens, h.sessions.digest_bytes_read() - bytes};
}

}  // namespace

// The subject digest, stamped at the raise and computed again when a detail is opened
// (protocol.md, "What a close is tied to"): content, so another host on the same directory
// computes the same one, and never for the rows of a list.
TEST_CASE("ledger: a raise stamps the subject's digest, and another host's detail agrees") {
  test::TempDir tmp("protocol_ledger_digest");
  const std::string dir = tmp.file("doc");
  Host h;
  h.open(dir, true);
  DigestWorld w;
  w.make(h, dir);

  const JsonValue raised = w.raise(h, "Agent");
  const JsonValue& item = at(raised, "reviews")[0];
  const std::string stamp(at(item, "raised_digest").as_string());
  CHECK(stamp.size() == 64);
  const JsonValue& stamped = at(raised, "subject");
  CHECK(at(stamped, "digest").as_string() == stamp);
  CHECK(num(at(stamped, "records")) == 2);  // the placement and its node
  CHECK(num(at(stamped, "assets")) == 1);   // the mesh its record names
  CHECK(num(at(stamped, "asset_bytes")) == w.mesh_bytes);
  CHECK(at(stamped, "unreadable").size() == 0);

  JsonValue now = opened_detail(h, at(item, "id"));
  CHECK(at(now, "digest").as_string() == stamp);
  CHECK(at(now, "same").as_bool());
  // A list never computes one, and its rows carry the stamp with no statement.
  JsonValue error;
  CHECK(h.fails("review.list", obj({{"digest", JsonValue(true)}}), &error) ==
        codes::k_invalid_argument);
  {
    const JsonValue rows = h.ok("review.list");
    const JsonValue* subject = rows.find("subject");
    CHECK((subject == nullptr || subject->is_null()));  // nothing compared, nothing stated
  }

  // Another host on the same directory loads the same files and computes the same digest: journal
  // numbers are one process's history, content is not.
  Host other;
  other.open(dir, false);
  CHECK(at(opened_detail(other, at(item, "id")), "digest").as_string() == stamp);

  // A file the subject lists that is not there digests as such; one outside this host's roots is
  // refused at the raise (an unknown root by its spelling, like a place no root admits), since
  // whoever asks a person to judge runs where the thing can be seen.
  const JsonValue missing =
      at(h.ok("review.raise", w.raise_params("Agent", strings({"props/missing.glb"}))), "subject");
  REQUIRE(at(missing, "unreadable").size() == 1);
  CHECK(at(missing, "unreadable")[0].as_string() == "props/missing.glb");
  CHECK(at(missing, "comparable").as_bool());
  CHECK(at(missing, "digest").as_string() != stamp);
  CHECK(h.fails("review.raise", w.raise_params("Agent", strings({"nowhere://x.glb"})), &error) ==
        codes::k_invalid_argument);
  CHECK(reason_of(error) == "subject not readable here");
  CHECK(at(at(error, "data"), "entries")[0].as_string() == "nowhere://x.glb");

  // An edit of the subject moves it.
  w.set(h, w.placement, "name", JsonValue("crate, moved"));
  now = opened_detail(h, at(item, "id"));
  CHECK_FALSE(at(now, "same").as_bool());
  CHECK(at(now, "digest").as_string() != stamp);
}

// A host that cannot see a file says so and never calls it a change (protocol.md, "What a close is
// tied to"): two hosts over one document compare when both can read every file it names. Host A
// has a `content` root and the whole test tree; host B neither, only the case's own folder. A file
// under `content://` (a root B lacks) and one beside the case's folder (a place B's roots do not
// admit, which exists) are outside B's roots: B's detail cannot compare, B's close is refused for
// it rather than called "subject moved", and B refuses to raise the same subject.
TEST_CASE("ledger: a host that cannot see a subject's file says so, and never calls it moved") {
  test::TempDir tmp("protocol_ledger_roots");
  test::TempDir beside("protocol_ledger_beside");
  const std::string dir = tmp.file("doc");
  const std::string content = tmp.file("content");
  std::filesystem::create_directories(content);
  {
    std::ofstream(std::filesystem::path(content) / "rock.png", std::ios::binary) << "png";
  }
  {
    std::ofstream(std::filesystem::path(beside.path()) / "far.bin", std::ios::binary) << "far";
  }
  const std::string far =
      "../../" + std::filesystem::path(beside.path()).filename().string() + "/far.bin";
  Host a;
  REQUIRE(a.roots.add("content", content, false) == io::Status::Ok);
  a.open(dir, true);
  DigestWorld w;
  w.make(a, dir);
  const JsonValue raised =
      a.ok("review.raise", w.raise_params("Agent", strings({"content://rock.png", far.c_str()})));
  const JsonValue item = at(raised, "reviews")[0];
  const std::string stamp(at(item, "raised_digest").as_string());
  CHECK(at(at(raised, "subject"), "comparable").as_bool());
  CHECK(at(at(raised, "subject"), "unreadable").size() == 0);

  Host b;
  b.roots = io::Roots{};
  REQUIRE(b.roots.add("tests", tmp.path(), true) == io::Status::Ok);
  b.open(dir, false);
  const JsonValue there = opened_detail(b, at(item, "id"));
  CHECK_FALSE(at(there, "comparable").as_bool());
  CHECK_FALSE(at(there, "same").as_bool());
  REQUIRE(at(there, "outside").size() == 2);
  CHECK(at(there, "outside")[0].as_string() == far);
  CHECK(at(there, "outside")[1].as_string() == "content://rock.png");
  CHECK(at(there, "unreadable").size() == 0);  // not "not readable": outside, and said so
  JsonValue error;
  CHECK(b.fails("review.close",
                obj({{"review", at(item, "id")},
                     {"resolution", JsonValue("Accepted")},
                     {"digest", JsonValue(stamp)},
                     {"attribution", who("reviewer")}}),
                &error) == codes::k_invalid_argument);
  CHECK(reason_of(error) == "subject not readable here");
  CHECK(b.fails("review.raise",
                w.raise_params("Agent", strings({"content://rock.png", far.c_str()})),
                &error) == codes::k_invalid_argument);
  CHECK(reason_of(error) == "subject not readable here");
  CHECK(at(at(error, "data"), "entries").size() == 2);

  // A, which can read both, compares: the same content is the stamp.
  const JsonValue here = opened_detail(a, at(item, "id"));
  CHECK(at(here, "comparable").as_bool());
  CHECK(at(here, "same").as_bool());
}

// Root names are portable and their places are not (protocol.md, "Which strings are files"): two
// hosts may both have roots named `tests` and `content` and still not admit the same files. The
// item records the stored spellings of the files its raise read as files, so a host that cannot
// admit one of them says it cannot compare and refuses a person's close, whatever names its roots
// have; and one that lacks a root by name says so too. The document is `w/world`; the file its
// record names, `../../c/rock.png`, is under A's `content` and under neither of B's roots.
TEST_CASE(
    "ledger: two hosts with the same root names in different places compare only what both read") {
  test::TempDir tmp("protocol_ledger_places");
  const std::filesystem::path scratch(tmp.path());
  const std::string dir = (scratch / "w" / "world").string();
  std::filesystem::create_directories(scratch / "c");
  std::filesystem::create_directories(scratch / "d");
  {
    std::ofstream(scratch / "c" / "rock.png", std::ios::binary) << "png";
  }
  Host a;
  a.roles(k_roles);
  a.roots = io::Roots{};
  REQUIRE(a.roots.add("tests", (scratch / "w").string(), true) == io::Status::Ok);
  REQUIRE(a.roots.add("content", (scratch / "c").string(), false) == io::Status::Ok);
  a.open(dir, true);
  DigestWorld w;
  w.make(a, dir);
  w.set(a, w.placement, "decal", JsonValue("../../c/rock.png"));
  a.stdio();
  const JsonValue raised = a.ok("review.raise", w.raise_params("Human"));
  const JsonValue item = at(raised, "reviews")[0];
  CHECK(num(at(at(raised, "subject"), "asset_bytes")) == w.mesh_bytes + 3);  // the mesh, the rock
  CHECK(at(opened_detail(a, at(item, "id")), "same").as_bool());

  // B: the same names, `content` somewhere else.
  Host b;
  b.roles(k_roles);
  b.roots = io::Roots{};
  REQUIRE(b.roots.add("tests", (scratch / "w").string(), true) == io::Status::Ok);
  REQUIRE(b.roots.add("content", (scratch / "d").string(), false) == io::Status::Ok);
  b.open(dir, false);
  b.bind(5, "person-at-desk", "person", "reviewer");
  const JsonValue there = opened_detail(b, at(item, "id"));
  CHECK_FALSE(at(there, "comparable").as_bool());
  CHECK_FALSE(at(there, "same").as_bool());
  REQUIRE(at(there, "outside").size() == 1);
  CHECK(at(there, "outside")[0].as_string() == "../../c/rock.png");
  JsonValue error;
  CHECK(b.fails("review.close",
                obj({{"review", at(item, "id")},
                     {"resolution", JsonValue("Accepted")},
                     {"revision", at(item, "revision")},
                     {"digest", at(there, "digest")},
                     {"attribution", who("person")}}),
                &error) == codes::k_invalid_argument);
  CHECK(reason_of(error) == "subject not readable here");

  // C: no `content` root at all, the record's climb to the file beside its root.
  Host c;
  c.roots = io::Roots{};
  REQUIRE(c.roots.add("tests", (scratch / "w").string(), true) == io::Status::Ok);
  c.open(dir, false);
  const JsonValue lacking = opened_detail(c, at(item, "id"));
  CHECK_FALSE(at(lacking, "comparable").as_bool());
  REQUIRE(at(lacking, "outside").size() == 1);
  CHECK(at(lacking, "outside")[0].as_string() == "../../c/rock.png");
}

// An entry of `subject.assets` is a stored path (protocol.md, "Paths a document's records hold";
// T174): `<root>://<relative>` or relative to the document, never absolute, refused at the raise as
// a record's absolute path is, with the same answer. The ledger is beside the document and every
// answer carries the subject, so an absolute path there would be the machine's, everywhere.
TEST_CASE("ledger: a subject's listed file is a stored path, never an absolute one") {
  test::TempDir tmp("protocol_ledger_stored");
  const std::string dir = tmp.file("doc");
  Host h;
  h.open(dir, true);
  DigestWorld w;
  w.make(h, dir);
  const std::string absolute = w.mesh;  // made under the case's own folder, at run time
  JsonValue error;
  const i32 code =
      h.fails("review.raise", w.raise_params("Agent", strings({absolute.c_str()})), &error);
  CHECK(code != 0);
  JsonValue record_error;
  JsonValue cmds = JsonValue::array();
  cmds.push_back(obj({{"kind", JsonValue("SetProperty")},
                      {"id", JsonValue(hex(w.placement))},
                      {"name", JsonValue("mesh")},
                      {"value", JsonValue(absolute)}}));
  // The same rule refuses the same path in a record, with the same code.
  CHECK(h.fails("doc.apply", obj({{"commands", cmds}, {"attribution", who("agent", "director")}}),
                &record_error) == code);
  CHECK(at(h.ok("review.list"), "reviews").size() == 0);  // nothing raised, nothing stored
}

// What a glTF names is part of it (protocol.md, "What a close is tied to"): a `.gltf` the subject
// names has its external buffers and images read too, each a relative URI admitted by the host's
// roots from the glTF's directory (a `..` included, as the importer reads it) and counted under the
// digest's bounds; a `data:` URI is in the file already. The geometry beside a `.gltf` rewritten,
// or the image above its directory, is "subject moved".
TEST_CASE("ledger: the files a glTF subject names are read with it") {
  test::TempDir tmp("protocol_ledger_gltf");
  const std::string dir = tmp.file("doc");
  Host h;
  h.open(dir, true);
  DigestWorld w;
  w.make(h, dir);
  const std::filesystem::path props = std::filesystem::path(dir) / "props";
  {
    std::ofstream(props / "crate.gltf", std::ios::binary)
        << R"({"asset":{"version":"2.0"},)"
        << R"("buffers":[{"uri":"crate.bin","byteLength":4},)"
        << R"({"uri":"data:application/octet-stream;base64,AAAA","byteLength":3}],)"
        << R"("images":[{"uri":"tex%20a.png"},{"uri":"../up.png"}]})";
  }
  {
    std::ofstream(props / "crate.bin", std::ios::binary) << "abcd";
  }
  {
    std::ofstream(props / "tex a.png", std::ios::binary) << "png1";
  }
  {
    std::ofstream(std::filesystem::path(dir) / "up.png", std::ios::binary) << "up";
  }
  w.set(h, w.placement, "mesh", JsonValue("props/crate.gltf"));

  JsonValue error;
  {
    Bound bound("protocol.review.subject_files", "3");  // the glTF and the three it names
    const u64 opens = h.sessions.digest_files_opened();
    const u64 bytes = h.sessions.digest_bytes_read();
    CHECK(h.fails("review.raise", w.raise_params("Agent"), &error) == codes::k_invalid_argument);
    CHECK(bound_in(error) == "protocol.review.subject_files");
    CHECK(num(at(at(error, "data"), "figure")) == 4);
    // A glTF's names are bounded as they are read: the glTF alone was read, held to the bounds
    // by its size and then read, and none of the files it names was opened.
    CHECK(h.sessions.digest_bytes_read() - bytes ==
          static_cast<u64>(std::filesystem::file_size(props / "crate.gltf")));
    CHECK(h.sessions.digest_files_opened() - opens == 2);
  }
  const JsonValue raised = h.ok("review.raise", w.raise_params("Agent"));
  const JsonValue item = at(raised, "reviews")[0];
  const JsonValue& stamped = at(raised, "subject");
  CHECK(num(at(stamped, "assets")) == 4);
  // The glTF's own bytes, the buffer's and the image's four each, and the two of the image above
  // the glTF's directory: a `..` is no way out, the roots are the door, and they admit it.
  CHECK(num(at(stamped, "asset_bytes")) ==
        static_cast<i64>(std::filesystem::file_size(props / "crate.gltf")) + 10);
  CHECK(at(stamped, "unreadable").size() == 0);

  // Each rewritten at the same length, its time put back: the close is refused. The image above
  // the glTF's directory first, then the buffer, each against an item of its own.
  const auto rewrite = [](const std::filesystem::path& file, const char* bytes) {
    const auto when = std::filesystem::last_write_time(file);
    {
      std::ofstream(file, std::ios::binary | std::ios::trunc) << bytes;
    }
    std::filesystem::last_write_time(file, when);
  };
  const JsonValue second = at(h.ok("review.raise", w.raise_params("Agent")), "reviews")[0];
  rewrite(std::filesystem::path(dir) / "up.png", "pu");
  CHECK(h.fails("review.close",
                obj({{"review", at(item, "id")},
                     {"resolution", JsonValue("Accepted")},
                     {"digest", at(item, "raised_digest")},
                     {"attribution", who("reviewer")}}),
                &error) == codes::k_invalid_argument);
  CHECK(reason_of(error) == "subject moved");
  rewrite(std::filesystem::path(dir) / "up.png", "up");
  rewrite(props / "crate.bin", "dcba");
  CHECK(h.fails("review.close",
                obj({{"review", at(second, "id")},
                     {"resolution", JsonValue("Accepted")},
                     {"digest", at(second, "raised_digest")},
                     {"attribution", who("reviewer")}}),
                &error) == codes::k_invalid_argument);
  CHECK(reason_of(error) == "subject moved");
}

namespace {

// A directory link of whichever kind this system lets a test make, as foundation/io's roots cases
// make one: a junction on Windows (any user may make one), a symbolic link on Linux. False when the
// system refuses, and the case says so and goes without it.
#if ENGINE_PLATFORM_WINDOWS
std::wstring wide(const std::string& s) {
  std::wstring w(static_cast<usize>(MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                                        static_cast<int>(s.size()), nullptr, 0)),
                 L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(),
                      static_cast<int>(w.size()));
  for (wchar_t& c : w)
    if (c == L'/') c = L'\\';
  return w;
}
#endif

bool make_dir_link(const std::string& target, const std::string& link) {
#if ENGINE_PLATFORM_WINDOWS
  if (!CreateDirectoryW(wide(link).c_str(), nullptr)) return false;
  const HANDLE h = CreateFileW(wide(link).c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                               FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  const std::wstring substitute = L"\\??\\" + wide(target);
  const std::wstring print = wide(target);
  const usize names = (substitute.size() + 1 + print.size() + 1) * sizeof(wchar_t);
  std::string buf(8 + 8 + names, '\0');
  auto put16 = [&](usize at, usize v) {
    buf[at] = static_cast<char>(v & 0xFF);
    buf[at + 1] = static_cast<char>((v >> 8) & 0xFF);
  };
  const u32 tag = 0xA0000003u;  // IO_REPARSE_TAG_MOUNT_POINT
  std::memcpy(buf.data(), &tag, 4);
  put16(4, 8 + names);                                   // ReparseDataLength
  put16(8, 0);                                           // SubstituteNameOffset
  put16(10, substitute.size() * sizeof(wchar_t));        // SubstituteNameLength
  put16(12, (substitute.size() + 1) * sizeof(wchar_t));  // PrintNameOffset
  put16(14, print.size() * sizeof(wchar_t));             // PrintNameLength
  std::memcpy(buf.data() + 16, substitute.c_str(), (substitute.size() + 1) * sizeof(wchar_t));
  std::memcpy(buf.data() + 16 + (substitute.size() + 1) * sizeof(wchar_t), print.c_str(),
              (print.size() + 1) * sizeof(wchar_t));
  DWORD returned = 0;
  const BOOL ok = DeviceIoControl(h, FSCTL_SET_REPARSE_POINT, buf.data(),
                                  static_cast<DWORD>(buf.size()), nullptr, 0, &returned, nullptr);
  CloseHandle(h);
  return ok != FALSE;
#else
  std::error_code ec;
  std::filesystem::create_directory_symlink(target, link, ec);
  return !ec;
#endif
}

}  // namespace

// Every file a glTF names passes the host's roots as a record's own file does (protocol.md, "What a
// close is tied to"; T174): its URI's own rule first (decoded once; a backslash, a scheme, a drive
// or an absolute form refuses it, and a `..` is left to the roots), then `io::admit_stored_path`
// from the glTF's admitted directory, links resolved and held to the roots. Refused, it is not
// followed: a line saying so, never opened or sized — the detail's opens and bytes are what the
// glTF and its readable buffer cost, and nothing about the place it named — the detail not
// comparable, and a raise over it refused. Answered by its index in the glTF, never by its text.
// The root is a folder of the case's own scratch, and `outside.bin` lies in the scratch above it.
TEST_CASE("ledger: a glTF's named file passes the host's roots, and one refused is never opened") {
  test::TempDir tmp("protocol_ledger_gltf_roots");
  const std::filesystem::path scratch(tmp.path());
  const std::string root = (scratch / "root").string();
  const std::string dir = (scratch / "root" / "doc").string();
  std::filesystem::create_directories(scratch / "root");
  std::filesystem::create_directories(scratch / "far");
  {
    std::ofstream(scratch / "outside.bin", std::ios::binary) << "secret!";
  }
  {
    std::ofstream(scratch / "far" / "x.bin", std::ios::binary) << "far away";
  }
  Host h;
  h.roots = io::Roots{};
  REQUIRE(h.roots.add("tests", root, true) == io::Status::Ok);
  h.open(dir, true);
  DigestWorld w;
  w.make(h, dir);
  const std::filesystem::path props = std::filesystem::path(dir) / "props";
  const auto write_gltf = [&](const char* images) {
    std::ofstream(props / "crate.gltf", std::ios::binary | std::ios::trunc)
        << R"({"asset":{"version":"2.0"},"buffers":[{"uri":"crate.bin","byteLength":4}],)"
        << R"("images":[)" << images << "]}";
    return static_cast<u64>(std::filesystem::file_size(props / "crate.gltf"));
  };
  {
    std::ofstream(props / "crate.bin", std::ios::binary) << "abcd";
  }
  const u64 plain = write_gltf("");
  w.set(h, w.placement, "mesh", JsonValue("props/crate.gltf"));
  const JsonValue item = at(at(w.raise(h, "Agent"), "reviews")[0], "id");
  JsonValue now;
  const Reads base = reads_of(h, [&] { now = opened_detail(h, item); });
  CHECK(base.bytes == plain + 4);
  CHECK(at(now, "unreadable").size() == 0);

  JsonValue error;
  // A plain `..` climb, three levels up from `props` to the file above the root: the URI's rule
  // leaves the climb to the roots, which refuse it, so it is never opened or sized; the detail
  // cannot compare, and a raise over it is refused naming it by its index.
  const u64 above = write_gltf(R"({"uri":"../../../outside.bin"})");
  const Reads up = reads_of(h, [&] { now = opened_detail(h, item); });
  CHECK(up.opens == base.opens);
  CHECK(up.bytes == base.bytes - plain + above);
  REQUIRE(at(now, "outside").size() == 1);
  CHECK(at(now, "outside")[0].as_string() == "props/crate.gltf#images[0]");
  CHECK_FALSE(at(now, "comparable").as_bool());
  const Reads refused = reads_of(h, [&] {
    CHECK(h.fails("review.raise", w.raise_params("Agent"), &error) == codes::k_invalid_argument);
  });
  // A raise sizes the glTF's buffer before it reads it, one open more than the detail's; the
  // refused name, never.
  CHECK(refused.opens == up.opens + 1);
  CHECK(reason_of(error) == "names not read");
  CHECK(at(at(error, "data"), "entries")[0].as_string() == "props/crate.gltf#images[0]");

  // The climb: `\` decoded from `%5C`, four levels up to the file above the root, refused by the
  // URI's own text: not followed either, and the same.
  const u64 climbing = write_gltf(R"({"uri":"x%5C..%5C..%5C..%5C..%5Coutside.bin"})");
  const Reads climb = reads_of(h, [&] { now = opened_detail(h, item); });
  CHECK(climb.opens == base.opens);
  CHECK(climb.bytes == base.bytes - plain + climbing);
  CHECK(num(at(now, "asset_bytes")) == static_cast<i64>(climbing + 4));
  REQUIRE(at(now, "unreadable").size() == 1);
  CHECK(at(now, "unreadable")[0].as_string() == "props/crate.gltf#images[0]");
  CHECK(at(now, "outside").size() == 0);
  CHECK_FALSE(at(now, "comparable").as_bool());
  // A raise over it is refused for the name it never followed, and answers no figure for the file
  // it never sized: the glTF and the buffer fill the subject's bytes exactly.
  {
    Bound bound("protocol.review.subject_bytes", std::to_string(climbing + 4).c_str());
    CHECK(h.fails("review.raise", w.raise_params("Agent"), &error) == codes::k_invalid_argument);
    CHECK(reason_of(error) == "names not read");
    CHECK(at(error, "data").find("figure") == nullptr);
  }

  // A share's name, `\\host\share\x.bin` encoded: refused by the URI's own rule, so nothing is
  // asked of the network; the opens are the glTF's and its buffer's alone.
  (void)write_gltf(R"({"uri":"%5C%5Cnohost.invalid%5Cs%5Cx.bin"})");
  const Reads share = reads_of(h, [&] { now = opened_detail(h, item); });
  CHECK(share.opens == base.opens);
  REQUIRE(at(now, "unreadable").size() == 1);
  CHECK(at(now, "unreadable")[0].as_string() == "props/crate.gltf#images[0]");
  CHECK_FALSE(at(now, "comparable").as_bool());

  // A link under the root to a directory outside it: resolved, and outside the roots.
  // The link alone is removed, before the scratch is, whatever the case did.
  struct Unlink {
    std::string path;
    ~Unlink() {
      std::error_code ec;
      std::filesystem::remove(path, ec);
    }
  } unlink{(props / "sub").string()};
  if (!make_dir_link((scratch / "far").string(), unlink.path)) {
    MESSAGE("this system refused to make a directory link; the link's part is skipped");
  } else {
    (void)write_gltf(R"({"uri":"sub/x.bin"})");
    const Reads linked = reads_of(h, [&] { now = opened_detail(h, item); });
    CHECK(linked.opens == base.opens);
    REQUIRE(at(now, "outside").size() == 1);
    CHECK(at(now, "outside")[0].as_string() == "props/crate.gltf#images[0]");
    CHECK_FALSE(at(now, "comparable").as_bool());
  }
  CHECK(std::filesystem::exists(scratch / "far" / "x.bin"));
}

// The files bound limits what a digest collects and answers, not only what it reads (protocol.md,
// "What it costs"): a glTF's names are taken up to what is left of the bound, each once, and what
// is past it is one entry with its count, whether a glTF or the records named it.
TEST_CASE("ledger: past the files bound a digest collects and answers a handful of names") {
  test::TempDir tmp("protocol_ledger_many");
  const std::string dir = tmp.file("doc");
  Host h;
  h.open(dir, true);
  DigestWorld w;
  w.make(h, dir);
  const JsonValue item = at(at(w.raise(h, "Agent"), "reviews")[0], "id");
  const std::filesystem::path props = std::filesystem::path(dir) / "props";
  {
    std::ofstream out(props / "many.gltf", std::ios::binary);
    out << R"({"asset":{"version":"2.0"},"images":[)";
    for (u32 i = 0; i < 100000; ++i)
      out << (i == 0 ? "" : ",") << R"({"uri":"t)" << i << R"(.png"})";
    out << "]}";
  }
  w.set(h, w.placement, "mesh", JsonValue("props/many.gltf"));
  Bound bound("protocol.review.subject_files", "4");
  u64 names = h.sessions.digest_names_collected();
  JsonValue now = opened_detail(h, item);
  CHECK(h.sessions.digest_names_collected() - names == 3);  // the room the glTF left, and no more
  REQUIRE(at(now, "unreadable").size() == 4);
  CHECK(at(now, "unreadable")[0].as_string() == "props/many.gltf#images[0]");
  CHECK(at(now, "unreadable")[3].as_string() == "99997 more past protocol.review.subject_files");
  CHECK(num(at(now, "assets")) == 4);

  // Record-named strings past the bound: one entry too, and the glTF's names, with no room left,
  // are counted and never collected.
  JsonValue decals = JsonValue::array();
  for (u32 i = 0; i < 10; ++i)
    decals.push_back(JsonValue("r" + std::to_string(i) + ".png"));
  w.set(h, w.placement, "decals", std::move(decals));
  names = h.sessions.digest_names_collected();
  now = opened_detail(h, item);
  CHECK(h.sessions.digest_names_collected() == names);
  REQUIRE(at(now, "unreadable").size() == 4);  // r0 to r2, and the one past the bound
  CHECK(at(now, "unreadable")[3].as_string() == "100007 more past protocol.review.subject_files");
}

// A glTF whose names cannot be followed says so (protocol.md, "What a close is tied to"): one the
// engine's JSON reader refuses (nested past its depth, which the importer may still take), or one
// past `protocol.review.gltf_json_bytes`, is hashed and listed as not followed, and a raise over it
// is refused naming it, so no item is stamped over geometry its digest never saw.
TEST_CASE("ledger: a glTF whose names cannot be followed says so, and no raise is made over it") {
  test::TempDir tmp("protocol_ledger_deep");
  const std::string dir = tmp.file("doc");
  Host h;
  h.open(dir, true);
  DigestWorld w;
  w.make(h, dir);
  const JsonValue item = at(at(w.raise(h, "Agent"), "reviews")[0], "id");
  const std::filesystem::path props = std::filesystem::path(dir) / "props";
  {
    std::ofstream(props / "crate.bin", std::ios::binary) << "abcd";
  }
  {
    std::ofstream out(props / "deep.gltf", std::ios::binary);
    out << R"({"asset":{"version":"2.0"},"buffers":[{"uri":"crate.bin","byteLength":4}],)"
        << R"("extras":)" << std::string(300, '[') << std::string(300, ']') << "}";
  }
  w.set(h, w.placement, "mesh", JsonValue("props/deep.gltf"));
  const JsonValue now = opened_detail(h, item);
  REQUIRE(at(now, "unreadable").size() == 1);
  CHECK(at(now, "unreadable")[0].as_string() == "props/deep.gltf");
  // After a raise, the same: what it names is unseen here, so the detail cannot compare, and a
  // close carrying even this detail's own digest is refused for it, never accepted over the
  // buffer it never read.
  CHECK_FALSE(at(now, "comparable").as_bool());
  REQUIRE(at(now, "not_comparable").size() == 1);
  CHECK(at(now, "not_comparable")[0].as_string() == "props/deep.gltf");
  JsonValue error;
  CHECK(h.fails("review.close",
                obj({{"review", item},
                     {"resolution", JsonValue("Accepted")},
                     {"digest", at(now, "digest")},
                     {"attribution", who("reviewer")}}),
                &error) == codes::k_invalid_argument);
  CHECK(reason_of(error) == "subject not readable here");
  CHECK(h.fails("review.raise", w.raise_params("Agent"), &error) == codes::k_invalid_argument);
  CHECK(reason_of(error) == "names not read");
  CHECK(at(at(error, "data"), "entries")[0].as_string() == "props/deep.gltf");

  // One past the JSON bound: the same, naming the bound and the figure.
  {
    std::ofstream(props / "crate.gltf", std::ios::binary)
        << R"({"asset":{"version":"2.0"},"buffers":[{"uri":"crate.bin","byteLength":4}]})";
  }
  w.set(h, w.placement, "mesh", JsonValue("props/crate.gltf"));
  (void)w.raise(h, "Agent");  // within the bound, it raises
  Bound bound("protocol.review.gltf_json_bytes", "16");
  CHECK(h.fails("review.raise", w.raise_params("Agent"), &error) == codes::k_invalid_argument);
  CHECK(reason_of(error) == "names not read");
  CHECK(bound_in(error) == "protocol.review.gltf_json_bytes");
  CHECK(num(at(at(error, "data"), "limit")) == 16);
  CHECK(num(at(at(error, "data"), "figure")) ==
        static_cast<i64>(std::filesystem::file_size(props / "crate.gltf")));
}

// A `.glb` names files too (protocol.md, "What a close is tied to"): its JSON chunk is read, held
// to the file's size and to the JSON bound, and its buffers' and images' URIs are followed as a
// `.gltf`'s are; buffer 0, the binary chunk, has none. A header that does not hold refuses a raise
// (`names not read`), as a chunk past the JSON bound does.
TEST_CASE("ledger: a .glb's external files are read with it, and a broken one refuses a raise") {
  test::TempDir tmp("protocol_ledger_glb");
  const std::string dir = tmp.file("doc");
  Host h;
  h.open(dir, true);
  DigestWorld w;
  w.make(h, dir);
  const std::filesystem::path props = std::filesystem::path(dir) / "props";
  const std::string json =
      R"({"asset":{"version":"2.0"},"buffers":[{"byteLength":4},{"uri":"ext.bin","byteLength":4}],)"
      R"("images":[{"uri":"ext.png"}]})";
  {
    std::ofstream(props / "ext.glb", std::ios::binary) << glb_bytes(json, "BIN!");
  }
  {
    std::ofstream(props / "ext.bin", std::ios::binary) << "abcd";
  }
  {
    std::ofstream(props / "ext.png", std::ios::binary) << "png";
  }
  w.set(h, w.placement, "mesh", JsonValue("props/ext.glb"));
  const JsonValue raised = h.ok("review.raise", w.raise_params("Agent"));
  const JsonValue item = at(raised, "reviews")[0];
  const JsonValue& stamped = at(raised, "subject");
  CHECK(num(at(stamped, "assets")) == 3);
  CHECK(num(at(stamped, "asset_bytes")) ==
        static_cast<i64>(std::filesystem::file_size(props / "ext.glb")) + 4 + 3);
  CHECK(at(stamped, "unreadable").size() == 0);

  // The external buffer rewritten at the same length, its time put back: the close is refused.
  const auto when = std::filesystem::last_write_time(props / "ext.bin");
  {
    std::ofstream(props / "ext.bin", std::ios::binary | std::ios::trunc) << "dcba";
  }
  std::filesystem::last_write_time(props / "ext.bin", when);
  JsonValue error;
  CHECK(h.fails("review.close",
                obj({{"review", at(item, "id")},
                     {"resolution", JsonValue("Accepted")},
                     {"digest", at(item, "raised_digest")},
                     {"attribution", who("reviewer")}}),
                &error) == codes::k_invalid_argument);
  CHECK(reason_of(error) == "subject moved");

  // A header that does not hold (eight bytes, no chunk): its names are not known.
  {
    std::ofstream(props / "broken.glb", std::ios::binary) << "glTF one";
  }
  w.set(h, w.placement, "mesh", JsonValue("props/broken.glb"));
  CHECK(h.fails("review.raise", w.raise_params("Agent"), &error) == codes::k_invalid_argument);
  CHECK(reason_of(error) == "names not read");
  CHECK(at(at(error, "data"), "entries")[0].as_string() == "props/broken.glb");

  // Its JSON chunk past the JSON bound: the same, naming the bound and the chunk's length.
  w.set(h, w.placement, "mesh", JsonValue("props/ext.glb"));
  Bound bound("protocol.review.gltf_json_bytes", "16");
  CHECK(h.fails("review.raise", w.raise_params("Agent"), &error) == codes::k_invalid_argument);
  CHECK(reason_of(error) == "names not read");
  CHECK(bound_in(error) == "protocol.review.gltf_json_bytes");
  CHECK(num(at(at(error, "data"), "figure")) == static_cast<i64>((json.size() + 3) / 4 * 4));
}

// A string a record holds that no host could admit as a file — a URL, text with a colon, a root no
// host has, a climb out of every root — digests as one constant line for that string and never
// makes the digest incomparable (protocol.md, "Which strings are files"), so it blocks neither a
// raise nor a person's close; an entry the raiser listed in `subject.assets` stays strict.
TEST_CASE("ledger: a string no host could admit never blocks a raise or a close") {
  test::TempDir tmp("protocol_ledger_not_files");
  const std::string dir = tmp.file("doc");
  Host h;
  h.roles(k_roles);
  h.open(dir, true);
  DigestWorld w;
  w.make(h, dir);
  h.stdio();
  const JsonValue item = at(w.raise(h), "reviews")[0];  // a person's
  w.set(h, w.placement, "source",
        strings({"https://example.invalid/crate.png", "fixed: see ruins.png", "x://a.png",
                 "../../../climb.png"}));
  h.bind(5, "person-at-desk", "person", "reviewer");
  const JsonValue now = opened_detail(h, at(item, "id"));
  CHECK(at(now, "comparable").as_bool());
  CHECK(at(now, "outside").size() == 0);
  const JsonValue closed = h.review("review.close", obj({{"review", at(item, "id")},
                                                         {"resolution", JsonValue("Accepted")},
                                                         {"revision", at(item, "revision")},
                                                         {"digest", at(now, "digest")},
                                                         {"attribution", who("person")}}));
  CHECK(at(closed, "state").as_string() == "Closed");
  // The same strings there before a raise: it stands.
  h.stdio();
  (void)w.raise(h, "Agent");
  JsonValue error;
  CHECK(h.fails("review.raise", w.raise_params("Agent", strings({"https://example.invalid/a.png"})),
                &error) == codes::k_invalid_argument);
  CHECK(reason_of(error) == "subject not readable here");
}

// A pass is thrown away only when the subject moved (protocol.md, "What it costs"): an edit of a
// record the subject does not name, and a ledger write about something else, at every pass's
// reads, leave the close to stand in one pass, and the files are read once.
TEST_CASE("ledger: an edit outside the subject during a digest's reads keeps the pass") {
  test::TempDir tmp("protocol_ledger_elsewhere");
  const std::string dir = tmp.file("doc");
  Host h;
  h.open(dir, true);
  DigestWorld w;
  w.make(h, dir);
  const Id128 elsewhere = Id128::from_parts(0x20, 9);
  {
    JsonValue cmds = JsonValue::array();
    cmds.push_back(obj({{"kind", JsonValue("CreateObject")},
                        {"id", JsonValue(hex(elsewhere))},
                        {"type", JsonValue("engine.world.Node")},
                        {"value", obj({{"name", JsonValue("elsewhere")}})}}));
    (void)h.ok("doc.apply", obj({{"commands", cmds}, {"attribution", who("setup", "director")}}));
  }
  const JsonValue item = at(w.raise(h, "Agent"), "reviews")[0];
  const Reads quiet = reads_of(h, [&] { (void)opened_detail(h, at(item, "id")); });

  Dispatcher other{Context{&h.sessions, nullptr, nullptr, nullptr, &h.policy}};
  other.context().roots = &h.roots;
  add_builtin_methods(other);
  std::mutex dispatch;
  struct Seam {
    std::mutex* dispatch = nullptr;
    Dispatcher* other = nullptr;
    std::string session;
    Id128 elsewhere;
    u32 edits = 0;
  } seam{&dispatch, &other, h.session, elsewhere};
  h.sessions.set_digest_read_hook_for_test(
      [](void* context) {
        auto* s = static_cast<Seam*>(context);
        std::lock_guard<std::mutex> held(*s->dispatch);
        ++s->edits;
        JsonValue cmds = JsonValue::array();
        cmds.push_back(obj({{"kind", JsonValue("SetProperty")},
                            {"id", JsonValue(hex(s->elsewhere))},
                            {"name", JsonValue("name")},
                            {"value", JsonValue("elsewhere " + std::to_string(s->edits))}}));
        JsonValue apply = obj({{"jsonrpc", JsonValue("2.0")},
                               {"id", JsonValue(1)},
                               {"method", JsonValue("doc.apply")},
                               {"params", obj({{"session", JsonValue(s->session)},
                                               {"commands", cmds},
                                               {"attribution", who("agent", "director")}})}});
        (void)s->other->dispatch(apply);
        JsonValue task =
            obj({{"jsonrpc", JsonValue("2.0")},
                 {"id", JsonValue(2)},
                 {"method", JsonValue("task.create")},
                 {"params", obj({{"session", JsonValue(s->session)},
                                 {"title", JsonValue("another task " + std::to_string(s->edits))},
                                 {"attribution", who("planner")}})}});
        (void)s->other->dispatch(task);
      },
      &seam);
  JsonValue resp;
  const Reads busy = reads_of(h, [&] {
    std::lock_guard<std::mutex> held(dispatch);
    h.dispatcher.context().dispatch_lock = &dispatch;
    resp = h.call("review.close", obj({{"review", at(item, "id")},
                                       {"resolution", JsonValue("Accepted")},
                                       {"digest", at(item, "raised_digest")},
                                       {"attribution", who("reviewer")}}));
    h.dispatcher.context().dispatch_lock = nullptr;
  });
  h.sessions.set_digest_read_hook_for_test(nullptr, nullptr);
  REQUIRE_MESSAGE(resp.find("result") != nullptr, write_json(resp));
  CHECK(num(at(at(at(resp, "result"), "subject"), "passes")) == 1);
  CHECK(busy.opens == quiet.opens);
  CHECK(seam.edits == 1);
}

// A close that compares nothing — an agent's with no digest, a raiser's withdrawal — decides
// without the digest's passes (protocol.md, "What it costs"), so a subject edited at every pass
// never refuses it `subject changing`, and it reads no file.
TEST_CASE("ledger: a close that compares nothing decides without reading the subject") {
  test::TempDir tmp("protocol_ledger_no_compare");
  const std::string dir = tmp.file("doc");
  Host h;
  h.open(dir, true);
  DigestWorld w;
  w.make(h, dir);
  const JsonValue accepted = at(at(w.raise(h, "Agent"), "reviews")[0], "id");
  const JsonValue withdrawn = at(at(w.raise(h, "Agent"), "reviews")[0], "id");
  Dispatcher other{Context{&h.sessions, nullptr, nullptr, nullptr, &h.policy}};
  other.context().roots = &h.roots;
  add_builtin_methods(other);
  std::mutex dispatch;
  struct Seam {
    std::mutex* dispatch = nullptr;
    Dispatcher* other = nullptr;
    std::string session;
    Id128 placement;
    u32 edits = 0;
  } seam{&dispatch, &other, h.session, w.placement};
  h.sessions.set_digest_read_hook_for_test(
      [](void* context) {
        auto* s = static_cast<Seam*>(context);
        std::lock_guard<std::mutex> held(*s->dispatch);
        JsonValue cmds = JsonValue::array();
        cmds.push_back(obj({{"kind", JsonValue("SetProperty")},
                            {"id", JsonValue(hex(s->placement))},
                            {"name", JsonValue("name")},
                            {"value", JsonValue("crate " + std::to_string(++s->edits))}}));
        JsonValue req = obj({{"jsonrpc", JsonValue("2.0")},
                             {"id", JsonValue(1)},
                             {"method", JsonValue("doc.apply")},
                             {"params", obj({{"session", JsonValue(s->session)},
                                             {"commands", cmds},
                                             {"attribution", who("agent", "director")}})}});
        (void)s->other->dispatch(req);
      },
      &seam);
  const auto close_under_lock = [&](const JsonValue& id, const char* resolution,
                                    const char* actor) {
    std::lock_guard<std::mutex> held(dispatch);
    h.dispatcher.context().dispatch_lock = &dispatch;
    JsonValue resp = h.call(
        "review.close",
        obj({{"review", id}, {"resolution", JsonValue(resolution)}, {"attribution", who(actor)}}));
    h.dispatcher.context().dispatch_lock = nullptr;
    return resp;
  };
  JsonValue resp;
  // Its answer carries no subject statement at all, so nothing in it reads as "could not
  // compare" (a statement's `comparable` false): nothing was compared.
  const auto states_no_subject = [](const JsonValue& result) {
    const JsonValue* subject = result.find("subject");
    return subject == nullptr || subject->is_null();
  };
  const Reads a = reads_of(h, [&] { resp = close_under_lock(accepted, "Accepted", "reviewer"); });
  REQUIRE_MESSAGE(resp.find("result") != nullptr, write_json(resp));
  CHECK(a.opens == 0);
  CHECK(states_no_subject(at(resp, "result")));
  const Reads b = reads_of(h, [&] { resp = close_under_lock(withdrawn, "Withdrawn", "checker"); });
  REQUIRE_MESSAGE(resp.find("result") != nullptr, write_json(resp));
  CHECK(b.opens == 0);
  CHECK(states_no_subject(at(resp, "result")));
  CHECK(seam.edits == 0);
  h.sessions.set_digest_read_hook_for_test(nullptr, nullptr);
}

// An item an older host raised over an absolute path (`subject.assets` before T174's form was held
// at the raise) is not mended, since only tests hold one; but no answer echoes the path: such an
// entry is answered by its index (protocol.md, "What a close is tied to").
TEST_CASE("ledger: an old item's absolute asset is answered by its index, never its spelling") {
  test::TempDir tmp("protocol_ledger_absolute");
  const std::string dir = tmp.file("doc");
  Host h;
  h.roles(k_roles);
  h.open(dir, true);
  DigestWorld w;
  w.make(h, dir);
  h.stdio();
  const JsonValue item =
      at(h.ok("review.raise", w.raise_params("Human", strings({"props/crate.glb"}))), "reviews")[0];
  {
    // The item as the older host wrote it: the listed file by its absolute path.
    std::string text;
    REQUIRE(h.vfs.read(dir + "/ledger.json", text) == io::Status::Ok);
    // Only the listed entry (the ledger holds no record's properties, and the item's
    // `raised_files` keeps the spelling the raise read).
    const std::string listed = R"("props/crate.glb")";
    const usize at_listed = text.find(listed, text.find(R"("assets")"));
    REQUIRE(at_listed != std::string::npos);
    text.replace(at_listed, listed.size(),
                 write_json(JsonValue(w.mesh), JsonWriteOptions{.pretty = false}));
    std::ofstream(dir + "/ledger.json", std::ios::binary | std::ios::trunc) << text;
  }
  const std::string name = std::filesystem::path(tmp.path()).filename().string();
  h.bind(5, "person-at-desk", "person", "reviewer");
  const JsonValue answer =
      h.ok("review.list", obj({{"review", at(item, "id")}, {"digest", JsonValue(true)}}));
  CHECK(write_json(answer).find(name) == std::string::npos);
  REQUIRE(at(at(answer, "subject"), "outside").size() == 1);
  CHECK(at(at(answer, "subject"), "outside")[0].as_string() == "assets[0]");
  CHECK(at(at(at(answer, "reviews")[0], "subject"), "assets")[0].as_string() == "assets[0]");
  JsonValue error;
  CHECK(h.fails("review.close",
                obj({{"review", at(item, "id")},
                     {"resolution", JsonValue("Accepted")},
                     {"revision", at(item, "revision")},
                     {"digest", at(at(answer, "subject"), "digest")},
                     {"attribution", who("person")}}),
                &error) == codes::k_invalid_argument);
  CHECK(reason_of(error) == "subject not readable here");
  CHECK(write_json(error).find(name) == std::string::npos);
  CHECK(write_json(h.ok("review.list")).find(name) == std::string::npos);
}

// What a digest reads is bounded, by four bounds named as tunables (protocol.md, "What it costs"):
// a raise past one is refused naming it and the figure, and a detail or a close past one says so
// in the stream, which differs from the stamp, so a file grown past its bound is "subject moved".
// Only a regular file is read: a directory and, on Linux, a FIFO under a root are not readable,
// and the detail over the FIFO answers (a plain open would wait for a writer under the host's
// lock).
TEST_CASE("ledger: a subject past a digest's bound is refused at the raise, and said at a detail") {
  test::TempDir tmp("protocol_ledger_bounds");
  const std::string dir = tmp.file("doc");
  Host h;
  h.open(dir, true);
  DigestWorld w;
  w.make(h, dir);
  JsonValue error;

  {
    Bound bound("protocol.review.file_bytes", "4");  // the mesh is more
    CHECK(h.fails("review.raise", w.raise_params("Agent"), &error) == codes::k_invalid_argument);
    CHECK(reason_of(error) == "subject too large");
    CHECK(bound_in(error) == "protocol.review.file_bytes");
    CHECK(num(at(at(error, "data"), "limit")) == 4);
    CHECK(num(at(at(error, "data"), "figure")) == w.mesh_bytes);
  }
  {
    // The mesh and a copy of it together, past a bound that holds one of them.
    Bound bound("protocol.review.subject_bytes", std::to_string(2 * w.mesh_bytes - 4).c_str());
    std::filesystem::copy_file(w.mesh, std::filesystem::path(dir) / "props" / "lid.glb");
    CHECK(h.fails("review.raise", w.raise_params("Agent", strings({"props/lid.glb"})), &error) ==
          codes::k_invalid_argument);
    CHECK(bound_in(error) == "protocol.review.subject_bytes");
    CHECK(num(at(at(error, "data"), "figure")) == 2 * w.mesh_bytes);
  }
  {
    Bound bound("protocol.review.subject_files", "1");  // the mesh and one listed
    CHECK(h.fails("review.raise", w.raise_params("Agent", strings({"props/lid.glb"})), &error) ==
          codes::k_invalid_argument);
    CHECK(bound_in(error) == "protocol.review.subject_files");
    CHECK(num(at(at(error, "data"), "figure")) == 2);
  }
  {
    Bound bound("protocol.review.subject_records", "1");  // the placement and its node
    CHECK(h.fails("review.raise", w.raise_params("Agent"), &error) == codes::k_invalid_argument);
    CHECK(bound_in(error) == "protocol.review.subject_records");
  }
  CHECK(h.sessions.digest_bytes_read() == 0);  // every refusal above read nothing

  // Raised within the bounds, then the mesh grown past one: the detail says so, and differs.
  const JsonValue item = at(w.raise(h, "Agent"), "reviews")[0];
  const std::string stamp(at(item, "raised_digest").as_string());
  {
    Bound bound("protocol.review.file_bytes", "4");
    const JsonValue now = opened_detail(h, at(item, "id"));
    CHECK(at(now, "digest").as_string() != stamp);
    CHECK_FALSE(at(now, "same").as_bool());
    REQUIRE(at(now, "unreadable").size() == 1);
    CHECK(at(now, "unreadable")[0].as_string() == "props/crate.glb");
    CHECK(num(at(now, "asset_bytes")) == 0);
    CHECK(h.fails("review.close",
                  obj({{"review", at(item, "id")},
                       {"resolution", JsonValue("Accepted")},
                       {"digest", JsonValue(stamp)},
                       {"attribution", who("reviewer")}}),
                  &error) == codes::k_invalid_argument);
    CHECK(reason_of(error) == "subject moved");
  }
  CHECK(at(opened_detail(h, at(item, "id")), "digest").as_string() == stamp);  // put back

  // A directory and a FIFO are not regular files: they exist and cannot be read, so a raise over
  // one is refused naming them (never opened to wait), and a detail over a file that became one
  // cannot compare, its close refused for it. A raise is made only where every file is read or
  // honestly not there.
  const std::filesystem::path props = std::filesystem::path(dir) / "props";
  std::filesystem::create_directories(props / "set.d");
  JsonValue listed = strings({"props/set.d"});
#if !ENGINE_PLATFORM_WINDOWS
  const std::string fifo = (props / "pipe.glb").string();
  REQUIRE(::mkfifo(fifo.c_str(), 0600) == 0);
  listed.push_back(JsonValue("props/pipe.glb"));
#endif
  CHECK(h.fails("review.raise", w.raise_params("Agent", listed), &error) ==
        codes::k_invalid_argument);
  CHECK(reason_of(error) == "subject not readable here");
  CHECK(at(at(error, "data"), "entries").size() == listed.size());
  {
    std::ofstream(props / "odd.bin", std::ios::binary) << "odd";
  }
  const JsonValue odd =
      at(h.ok("review.raise", w.raise_params("Agent", strings({"props/odd.bin"}))), "reviews")[0];
  std::filesystem::remove(props / "odd.bin");
  std::filesystem::create_directories(props / "odd.bin");
  const JsonValue detail = opened_detail(h, at(odd, "id"));
  CHECK_FALSE(at(detail, "comparable").as_bool());
  REQUIRE(at(detail, "not_comparable").size() == 1);
  CHECK(at(detail, "not_comparable")[0].as_string() == "props/odd.bin");
  CHECK(at(detail, "unreadable").size() == 1);
  CHECK(h.fails("review.close",
                obj({{"review", at(odd, "id")},
                     {"resolution", JsonValue("Accepted")},
                     {"digest", at(detail, "digest")},
                     {"attribution", who("reviewer")}}),
                &error) == codes::k_invalid_argument);
  CHECK(reason_of(error) == "subject not readable here");
}

// Every refusal that reads no file comes before the digest reads one (protocol.md, "What it
// costs"): an item already closed, a revision that moved, a role or a raiser the close refuses, a
// person's close that carries no digest, a session that may not write, and a raise the ledger
// refuses — each answers with no byte of the subject's assets read.
TEST_CASE("ledger: a close or a raise refused before its digest reads no asset") {
  test::TempDir tmp("protocol_ledger_cheap");
  const std::string dir = tmp.file("doc");
  Host h;
  h.roles(k_roles);
  h.open(dir, true);
  DigestWorld w;
  w.make(h, dir);
  h.stdio();
  const JsonValue person_item = at(w.raise(h), "reviews")[0];
  const JsonValue agent_item = at(w.raise(h, "Agent"), "reviews")[0];
  const u64 raised = h.sessions.digest_bytes_read();
  CHECK(raised == static_cast<u64>(2 * w.mesh_bytes));  // each raise read the mesh once
  JsonValue error;
  const char* closer = "agent";
  const auto close = [&](const JsonValue& item, const char* resolution, JsonValue revision,
                         bool with_digest) {
    JsonValue params = obj({{"review", at(item, "id")},
                            {"resolution", JsonValue(resolution)},
                            {"attribution", who(closer)}});
    if (!revision.is_null()) params.set("revision", std::move(revision));
    if (with_digest) params.set("digest", at(item, "raised_digest"));
    return params;
  };

  h.bind(6, "agent-key", "agent", "director");  // a role the roles file does not mark human
  CHECK(h.fails("review.close", close(person_item, "Accepted", JsonValue(), true), &error) ==
        codes::k_forbidden);
  CHECK(reason_of(error) == "human review");
  CHECK(h.fails("review.close", close(agent_item, "Withdrawn", JsonValue(), false), &error) ==
        codes::k_forbidden);
  CHECK(reason_of(error) == "review raiser");
  h.bind(5, "person-at-desk", "person", "reviewer");
  closer = "person";
  CHECK(h.fails("review.close", close(person_item, "Accepted", JsonValue(u64{9}), true), &error) ==
        codes::k_invalid_argument);
  CHECK(reason_of(error) == "revision moved");
  CHECK(h.fails("review.close", close(person_item, "Accepted", JsonValue(u64{1}), false), &error) ==
        codes::k_invalid_argument);
  CHECK(reason_of(error) == "seen required");
  CHECK(h.sessions.digest_bytes_read() == raised);

  // Closed, then closed again: the second is refused before any read.
  (void)h.ok("review.close", close(agent_item, "Accepted", JsonValue(), true));
  const u64 closed = h.sessions.digest_bytes_read();
  CHECK(closed == raised + static_cast<u64>(w.mesh_bytes));
  CHECK(h.fails("review.close", close(agent_item, "Accepted", JsonValue(), true), &error) ==
        codes::k_invalid_argument);
  h.stdio();
  JsonValue params = w.raise_params("Agent");
  params.set("task", JsonValue(u64{12345}));  // no such task
  CHECK(h.fails("review.raise", std::move(params), &error) == codes::k_not_found);
  CHECK(h.sessions.digest_bytes_read() == closed);

  // A session that may not write: the same document opened through a read-only root.
  Host reader;
  reader.roots = io::Roots{};
  REQUIRE(reader.roots.add("tests", std::string(io::parent_path(reader.roots_anchor.path())),
                           false) == io::Status::Ok);
  reader.open(dir, false);
  CHECK(reader.fails("review.raise", w.raise_params("Agent")) != 0);
  CHECK(reader.sessions.digest_bytes_read() == 0);
}

// The files are read with the host's dispatch lock let go (protocol.md, "What it costs"): a second
// client's call is answered while a digest's read is held open, and a document edit made then
// makes the pass compose and read again; an edit at every pass is refused after three.
TEST_CASE("ledger: a digest lets the host go for its reads, and composes again past an edit") {
  test::TempDir tmp("protocol_ledger_unlocked");
  const std::string dir = tmp.file("doc");
  Host h;
  h.open(dir, true);
  DigestWorld w;
  w.make(h, dir);
  const JsonValue item = at(w.raise(h, "Agent"), "reviews")[0];
  const std::string stamp(at(item, "raised_digest").as_string());

  // A second client of the same host: its own dispatcher over the same sessions.
  Dispatcher other{Context{&h.sessions, nullptr, nullptr, nullptr, &h.policy}};
  other.context().roots = &h.roots;
  add_builtin_methods(other);
  std::mutex dispatch;  // the host's dispatch lock, as the listener holds it for a request
  struct Seam {
    std::mutex* dispatch = nullptr;
    Dispatcher* other = nullptr;
    std::string session;
    Id128 placement;
    u32 edits = 0;     // edits to make, one a pass
    u32 answered = 0;  // the second client's calls answered while the read was held
  } seam{&dispatch, &other, h.session, w.placement};
  h.sessions.set_digest_read_hook_for_test(
      [](void* context) {
        auto* s = static_cast<Seam*>(context);
        std::lock_guard<std::mutex> held(*s->dispatch);  // free: the digest let it go
        if (s->edits == 0) {
          // No edit: a read of the session alone, answered.
          JsonValue req = obj({{"jsonrpc", JsonValue("2.0")},
                               {"id", JsonValue(1)},
                               {"method", JsonValue("session.info")},
                               {"params", obj({{"session", JsonValue(s->session)}})}});
          if (s->other->dispatch(req).find("result") != nullptr) ++s->answered;
          return;
        }
        --s->edits;
        JsonValue cmds = JsonValue::array();
        cmds.push_back(obj({{"kind", JsonValue("SetProperty")},
                            {"id", JsonValue(hex(s->placement))},
                            {"name", JsonValue("name")},
                            {"value", JsonValue("crate, edited mid-read")}}));
        JsonValue req = obj({{"jsonrpc", JsonValue("2.0")},
                             {"id", JsonValue(2)},
                             {"method", JsonValue("doc.apply")},
                             {"params", obj({{"session", JsonValue(s->session)},
                                             {"commands", cmds},
                                             {"attribution", who("agent", "director")}})}});
        if (s->other->dispatch(req).find("result") != nullptr) ++s->answered;
      },
      &seam);
  const auto detail_under_lock = [&](JsonValue* error) {
    std::lock_guard<std::mutex> held(dispatch);
    h.dispatcher.context().dispatch_lock = &dispatch;
    JsonValue resp =
        h.call("review.list", obj({{"review", at(item, "id")}, {"digest", JsonValue(true)}}));
    h.dispatcher.context().dispatch_lock = nullptr;
    if (error != nullptr && resp.find("error") != nullptr) *error = at(resp, "error");
    return resp;
  };

  // Nothing edited: one pass, and the other client was answered while the read was held.
  JsonValue resp = detail_under_lock(nullptr);
  CHECK(seam.answered == 1);
  CHECK(num(at(at(at(resp, "result"), "subject"), "passes")) == 1);
  CHECK(at(at(at(resp, "result"), "subject"), "digest").as_string() == stamp);

  // One edit during the first read: the pass composes again and answers the edited state.
  seam.edits = 1;
  resp = detail_under_lock(nullptr);
  const JsonValue& subject = at(at(resp, "result"), "subject");
  CHECK(num(at(subject, "passes")) == 2);
  h.sessions.set_digest_read_hook_for_test(nullptr, nullptr);
  CHECK(at(subject, "digest").as_string() ==
        at(opened_detail(h, at(item, "id")), "digest").as_string());
  CHECK(at(subject, "digest").as_string() != stamp);

  // An edit at every pass: refused after three, as a raise and a close are.
  h.sessions.set_digest_read_hook_for_test(
      [](void* context) {
        auto* s = static_cast<Seam*>(context);
        std::lock_guard<std::mutex> held(*s->dispatch);
        JsonValue cmds = JsonValue::array();
        cmds.push_back(obj({{"kind", JsonValue("SetProperty")},
                            {"id", JsonValue(hex(s->placement))},
                            {"name", JsonValue("name")},
                            {"value", JsonValue("crate " + std::to_string(++s->edits))}}));
        JsonValue req = obj({{"jsonrpc", JsonValue("2.0")},
                             {"id", JsonValue(3)},
                             {"method", JsonValue("doc.apply")},
                             {"params", obj({{"session", JsonValue(s->session)},
                                             {"commands", cmds},
                                             {"attribution", who("agent", "director")}})}});
        (void)s->other->dispatch(req);
      },
      &seam);
  seam.edits = 0;
  JsonValue error;
  resp = detail_under_lock(&error);
  REQUIRE(resp.find("error") != nullptr);
  CHECK(reason_of(error) == "subject changing");
  CHECK(seam.edits == 3);
  h.sessions.set_digest_read_hook_for_test(nullptr, nullptr);
}

// The detail's filters (protocol.md, "What a close is tied to"): beside `digest`, a list's `state`,
// `audience`, `kind` or `task` would hide the item the detail names when it does not match, and
// answer nothing with no error; so they are refused as `reviews`, `limit` and `order` are.
TEST_CASE("ledger: the detail form refuses a list's filters beside it") {
  test::TempDir tmp("protocol_ledger_detail");
  const std::string dir = tmp.file("doc");
  Host h;
  h.open(dir, true);
  DigestWorld w;
  w.make(h, dir);
  const JsonValue id = at(at(w.raise(h, "Agent"), "reviews")[0], "id");
  JsonValue error;
  for (const auto& [name, value] :
       {std::pair<const char*, JsonValue>{"state", JsonValue("Closed")},
        std::pair<const char*, JsonValue>{"audience", JsonValue("Human")},
        std::pair<const char*, JsonValue>{"kind", JsonValue("promotion")},
        std::pair<const char*, JsonValue>{"task", JsonValue(u64{77})}}) {
    CAPTURE(name);
    JsonValue params = obj({{"review", id}, {"digest", JsonValue(true)}});
    params.set(name, value);
    CHECK(h.fails("review.list", std::move(params), &error) == codes::k_invalid_argument);
    CHECK(reason_of(error) == "detail form");
  }
}

// `decided_at` is the digest its closer saw, which the host found equal (protocol.md, "What a close
// is tied to"): a close that sent none — an agent's, or a withdrawal — records none.
TEST_CASE("ledger: a close that sent no digest records none it was decided at") {
  test::TempDir tmp("protocol_ledger_decided");
  const std::string dir = tmp.file("doc");
  Host h;
  h.open(dir, true);
  DigestWorld w;
  w.make(h, dir);
  const JsonValue accepted = at(at(w.raise(h, "Agent"), "reviews")[0], "id");
  const JsonValue withdrawn = at(at(w.raise(h, "Agent"), "reviews")[0], "id");
  const JsonValue a = h.review("review.close", obj({{"review", accepted},
                                                    {"resolution", JsonValue("Accepted")},
                                                    {"attribution", who("reviewer")}}));
  CHECK(at(a, "decided_at").as_string().empty());
  const JsonValue b = h.review("review.close", obj({{"review", withdrawn},
                                                    {"resolution", JsonValue("Withdrawn")},
                                                    {"attribution", who("checker")}}));
  CHECK(at(b, "decided_at").as_string().empty());
}

// Only the judged layer's record's properties are searched for file names, never its type
// (protocol.md, "Which strings are files"): `engine.world.Node` looks like a file with the
// extension `Node`, and was listed as one.
TEST_CASE("ledger: a subject judged in a layer names no file by its record's type") {
  test::TempDir tmp("protocol_ledger_layer");
  const std::string dir = tmp.file("doc");
  Host h;
  h.open(dir, true);
  DigestWorld w;
  w.make(h, dir);
  const JsonValue layers = h.ok("doc.layers");
  const JsonValue base = at(at(layers, "layers")[0], "name");
  JsonValue params = w.raise_params("Agent");
  JsonValue subject = at(params, "subject");
  JsonValue records = JsonValue::array();
  records.push_back(JsonValue(hex(w.node)));
  subject.set("records", std::move(records));
  subject.set("layer", base);
  params.set("subject", std::move(subject));
  const JsonValue stamped = at(h.ok("review.raise", std::move(params)), "subject");
  CHECK(at(stamped, "unreadable").size() == 0);
  CHECK(num(at(stamped, "assets")) == 0);
}

// The detail form is one item by id (protocol.md, "What a close is tied to"): a list's ids, order
// or limit beside `digest` is refused rather than ignored. And a ledger written before digests —
// at version 2, with no ledger revision, or at version 3, with one — reads, its item carrying no
// stamp, which no digest equals, so its detail says what it is about changed since it was raised.
TEST_CASE("ledger: the detail form stands alone, and ledgers from before digests read") {
  test::TempDir tmp("protocol_ledger_digest");
  const std::string dir = tmp.file("doc");
  Host h;
  h.open(dir, true);
  DigestWorld w;
  w.make(h, dir);
  const JsonValue id = at(at(w.raise(h, "Agent"), "reviews")[0], "id");

  JsonValue error;
  JsonValue one = JsonValue::array();
  one.push_back(id);
  CHECK(h.fails("review.list", obj({{"review", id}, {"digest", JsonValue(true)}, {"reviews", one}}),
                &error) == codes::k_invalid_argument);
  CHECK(reason_of(error) == "detail form");
  CHECK(h.fails("review.list",
                obj({{"review", id}, {"digest", JsonValue(true)}, {"limit", JsonValue(1)}}),
                &error) == codes::k_invalid_argument);
  CHECK(reason_of(error) == "detail form");
  CHECK(h.fails("review.list",
                obj({{"review", id}, {"digest", JsonValue(true)}, {"order", JsonValue("Newest")}}),
                &error) == codes::k_invalid_argument);
  CHECK(reason_of(error) == "detail form");
  // Without `digest` the same selection is a list's, as it was.
  CHECK(at(h.ok("review.list", obj({{"review", id}, {"limit", JsonValue(1)}})), "reviews").size() ==
        1);

  // The item as a host before digests wrote it: version 2 (main's hosts before the ledger's
  // revision) has no `revision` of its own; version 3 has one.
  const std::string item = R"({"id":7,"kind":"validation","summary":"s","revision":1,)"
                           R"("raised_via":"declared","raised_by":{"actor":"checker"},)"
                           R"("subject":{"records":[")" +
                           hex(w.placement) + R"("]}})";
  for (const char* ledger_revision : {"", R"("revision":5,)"}) {
    CAPTURE(ledger_revision);
    {
      std::ofstream out(dir + "/ledger.json", std::ios::binary);
      out << "{" << ledger_revision << R"("tasks":[],"reviews":[)" << item << "]}";
    }
    const JsonValue answer =
        h.ok("review.list", obj({{"review", JsonValue(u64{7})}, {"digest", JsonValue(true)}}));
    CHECK(num(at(answer, "ledger_revision")) == (*ledger_revision == 0 ? 0 : 5));
    CHECK(at(at(answer, "reviews")[0], "raised_digest").as_string().empty());
    const JsonValue& now = at(answer, "subject");
    CHECK(at(now, "digest").as_string().size() == 64);
    CHECK_FALSE(at(now, "same").as_bool());
    // Written back by this host at the current version, and read again: the close is in the file,
    // the ledger's revision moved past what it was, and the item kept what the old host wrote.
    const JsonValue closed = h.review("review.close", obj({{"review", JsonValue(u64{7})},
                                                           {"resolution", JsonValue("Accepted")},
                                                           {"attribution", who("reviewer")}}));
    CHECK(at(closed, "state").as_string() == "Closed");
    std::string text;
    REQUIRE(h.vfs.read(dir + "/ledger.json", text) == io::Status::Ok);
    JsonValue written;
    REQUIRE(parse_json(text, written).ok);
    CHECK(num(at(written, "revision")) == (*ledger_revision == 0 ? 1 : 6));
    const JsonValue again =
        at(h.ok("review.list", obj({{"review", JsonValue(u64{7})}})), "reviews")[0];
    CHECK(at(again, "state").as_string() == "Closed");
    CHECK(at(again, "summary").as_string() == "s");
    CHECK(at(again, "raised_digest").as_string().empty());
    CHECK(at(again, "decided_at").as_string().empty());
  }
}

// A person's close is tied to what their detail showed (protocol.md, "What a close is tied to"):
// the review of the queue's design found four ways to make a person accept a state they never
// looked at, each one a close refused here that the item's revision alone let through. Edits come
// on a director's connection (6), closes on the person's (5).
TEST_CASE(
    "ledger: a person's close is refused when what the item is about moved since it was seen") {
  test::TempDir tmp("protocol_ledger_seen");
  const std::string dir = tmp.file("doc");
  Host h;
  h.roles(k_roles);
  h.open(dir, true);
  DigestWorld w;
  w.make(h, dir);
  const auto agent = [&] { h.bind(6, "agent-key", "agent", "director"); };
  const auto person = [&] { h.bind(5, "person-at-desk", "person", "reviewer"); };
  // What the person's detail shows: the item's revision and its subject's digest now.
  struct Seen {
    JsonValue revision;
    JsonValue digest;
  };
  const auto look = [&](const JsonValue& id) {
    const JsonValue listed =
        h.ok("review.list", obj({{"review", id}, {"digest", JsonValue(true)}}));
    return Seen{at(at(listed, "reviews")[0], "revision"), at(at(listed, "subject"), "digest")};
  };
  const auto close = [&](const JsonValue& id, const Seen& seen, JsonValue* error) {
    person();
    JsonValue params = obj({{"review", id},
                            {"resolution", JsonValue("Accepted")},
                            {"revision", seen.revision},
                            {"digest", seen.digest},
                            {"attribution", who("person")}});
    if (error == nullptr) return h.ok("review.close", std::move(params));
    CHECK(h.fails("review.close", std::move(params), error) == codes::k_invalid_argument);
    return JsonValue();
  };
  const auto raise = [&] {
    h.stdio();  // the checker raises over stdio, naming itself
    return at(at(w.raise(h), "reviews")[0], "id");
  };
  JsonValue error;

  SUBCASE("an edit of the subject's own record") {
    const JsonValue id = raise();
    const Seen seen = look(id);
    agent();
    w.set(h, w.placement, "name", JsonValue("crate, swapped"));
    (void)close(id, seen, &error);
    CHECK(reason_of(error) == "subject moved");
    CHECK(at(at(error, "data"), "digest").as_string() != seen.digest.as_string());
    // Looked at again, the person decides on what it is now, and the record says so.
    const Seen again = look(id);
    const JsonValue closed = close(id, again, nullptr);
    CHECK(at(at(closed, "reviews")[0], "decided_at").as_string() == again.digest.as_string());
    CHECK_FALSE(at(at(closed, "subject"), "same").as_bool());
  }
  SUBCASE("an edit of a record the subject hangs under") {
    const JsonValue id = raise();
    const Seen seen = look(id);
    agent();
    w.set(h, w.node, "name", JsonValue("the back yard"));
    (void)close(id, seen, &error);
    CHECK(reason_of(error) == "subject moved");
  }
  SUBCASE("a layer added over the stack that holds nothing of the subject") {
    // Not something its reviewer saw: what composes for the subject is unchanged, so the digest
    // is equal and the close stands.
    const JsonValue id = raise();
    const Seen seen = look(id);
    agent();
    (void)h.ok("doc.add_layer", obj({{"name", JsonValue("late")}, {"edit", JsonValue(false)}}));
    CHECK(look(id).digest.as_string() == seen.digest.as_string());
    const JsonValue closed = close(id, seen, nullptr);
    CHECK(at(at(closed, "reviews")[0], "state").as_string() == "Closed");
  }
  SUBCASE("a stronger layer overriding a property of the subject") {
    const JsonValue id = raise();
    const Seen seen = look(id);
    agent();
    (void)h.ok("doc.add_layer", obj({{"name", JsonValue("late")}, {"edit", JsonValue(true)}}));
    w.set(h, w.placement, "name", JsonValue("crate, from the late layer"));
    (void)close(id, seen, &error);
    CHECK(reason_of(error) == "subject moved");
  }
  SUBCASE("a layer's opinion taken out of the composition") {
    // No method mutes or reorders a layer; a proposal rejected empties its layer, which is the
    // stack's composition changing under the subject without its records being edited.
    agent();
    (void)h.ok("doc.propose_layer",
               obj({{"name", JsonValue("p.crate")},
                    {"target", at(at(h.ok("doc.layers"), "layers")[0], "name")},
                    {"attribution", who("agent", "director")}}));
    w.set(h, w.placement, "name", JsonValue("crate, as proposed"));
    const JsonValue id = raise();
    const Seen seen = look(id);
    agent();
    (void)h.ok("doc.reject", obj({{"proposal", JsonValue("p.crate")},
                                  {"attribution", who("agent", "director")}}));
    (void)close(id, seen, &error);
    CHECK(reason_of(error) == "subject moved");
  }
  SUBCASE("an asset its record names rewritten, with its time put back") {
    const JsonValue id = raise();
    const Seen seen = look(id);
    const auto when = std::filesystem::last_write_time(w.mesh);
    {
      std::ofstream out(w.mesh, std::ios::binary | std::ios::trunc);
      out << crate_glb("two");  // the same length: neither the size nor the time says it changed
    }
    std::filesystem::last_write_time(w.mesh, when);
    (void)close(id, seen, &error);
    CHECK(reason_of(error) == "subject moved");
  }
  SUBCASE("an undo below the stamp, and the redo after it") {
    agent();
    w.set(h, w.placement, "name", JsonValue("crate, second"));
    const JsonValue id = raise();
    const Seen seen = look(id);
    agent();
    (void)h.ok("doc.undo", obj({{"attribution", who("agent", "director")}}));
    (void)close(id, seen, &error);
    CHECK(reason_of(error) == "subject moved");
    // Redone, the document is what the person saw again, and the close is the same digest's: the
    // check is the content, not the journal's position, which an undo then a redo also restores.
    agent();
    (void)h.ok("doc.redo", obj({{"attribution", who("agent", "director")}}));
    const JsonValue closed = close(id, seen, nullptr);
    CHECK(at(at(closed, "reviews")[0], "decided_at").as_string() == seen.digest.as_string());
    CHECK(at(at(closed, "subject"), "same").as_bool());
  }
  SUBCASE("nothing changed: accepted, with the digest it was decided at") {
    const JsonValue id = raise();
    const Seen seen = look(id);
    const JsonValue closed = close(id, seen, nullptr);
    CHECK(at(at(closed, "reviews")[0], "state").as_string() == "Closed");
    CHECK(at(at(closed, "reviews")[0], "decided_at").as_string() == seen.digest.as_string());
  }
  SUBCASE("a digest stamped by another host, of another state") {
    // Another host on the same directory raises and stamps; this host's document then moves, and
    // the other host's stamp is what a page that read it would send.
    Host other;
    other.roles(k_roles);
    other.open(dir, false);
    const JsonValue raised = w.raise(other);
    const JsonValue id = at(at(raised, "reviews")[0], "id");
    const Seen stamped{at(at(raised, "reviews")[0], "revision"),
                       at(at(raised, "reviews")[0], "raised_digest")};
    CHECK(look(id).digest.as_string() == stamped.digest.as_string());  // the same content
    agent();
    w.set(h, w.placement, "name", JsonValue("crate, here only"));
    (void)close(id, stamped, &error);
    CHECK(reason_of(error) == "subject moved");
  }
}

// One write, one event (protocol.md, "The ledger's events" and "What a close is tied to"): the
// raise's stamp and the close's `decided_at` are part of the write that raises or closes, which
// pushes its one event; a close refused for what its closer saw ("seen required", "subject moved")
// writes nothing and pushes nothing.
TEST_CASE("ledger: a stamp and a decision are one write and one event, a refused close neither") {
  test::TempDir tmp("protocol_ledger_seen");
  const std::string dir = tmp.file("doc");
  Host h;
  Collect pushed;
  Subscriptions subscriptions;
  subscriptions.attach(&pushed);
  h.sessions.set_events(&subscriptions);
  (void)subscriptions.subscribe(7, "", event_bit(EventKind::Session));
  h.roles(k_roles);
  h.open(dir, true);
  DigestWorld w;
  w.make(h, dir);
  const auto changes_since = [&](usize from) {
    std::vector<std::string> out;
    for (usize i = from; i < pushed.sent.size(); ++i) {
      JsonValue note;
      REQUIRE(parse_json(pushed.sent[i], note).ok);
      out.emplace_back(at(at(note, "params"), "change").as_string());
    }
    return out;
  };
  const auto ledger_text = [&] {
    std::string text;
    REQUIRE(h.vfs.read(dir + "/ledger.json", text) == io::Status::Ok);
    return text;
  };
  const auto write_mesh = [&](const std::string& bytes) {
    const auto when = std::filesystem::last_write_time(w.mesh);
    {
      std::ofstream out(w.mesh, std::ios::binary | std::ios::trunc);
      out << bytes;
    }
    std::filesystem::last_write_time(w.mesh, when);
  };

  h.stdio();
  usize mark = pushed.sent.size();
  const JsonValue raised = w.raise(h);
  CHECK(changes_since(mark) == std::vector<std::string>{"review_raised"});
  const JsonValue item = at(raised, "reviews")[0];
  CHECK(at(item, "raised_digest").as_string().size() == 64);
  const i64 written = num(at(raised, "ledger_revision"));
  const JsonValue id = at(item, "id");
  const JsonValue listed = h.ok("review.list", obj({{"review", id}, {"digest", JsonValue(true)}}));
  const JsonValue revision = at(at(listed, "reviews")[0], "revision");
  const JsonValue digest = at(at(listed, "subject"), "digest");

  h.bind(5, "person-at-desk", "person", "reviewer");
  const auto close = [&](bool with_digest) {
    JsonValue params = obj({{"review", id},
                            {"resolution", JsonValue("Accepted")},
                            {"revision", revision},
                            {"attribution", who("person")}});
    if (with_digest) params.set("digest", digest);
    return params;
  };
  const std::string before = ledger_text();
  mark = pushed.sent.size();
  JsonValue error;
  CHECK(h.fails("review.close", close(false), &error) == codes::k_invalid_argument);
  CHECK(reason_of(error) == "seen required");
  write_mesh(crate_glb("two"));  // the same length, its time put back
  CHECK(h.fails("review.close", close(true), &error) == codes::k_invalid_argument);
  CHECK(reason_of(error) == "subject moved");
  CHECK(pushed.sent.size() == mark);
  CHECK(ledger_text() == before);

  // Put back, the close is one write carrying `decided_at`, and one event.
  write_mesh(crate_glb("one"));
  const JsonValue closed = h.ok("review.close", close(true));
  CHECK(changes_since(mark) == std::vector<std::string>{"review_closed"});
  CHECK(num(at(closed, "ledger_revision")) == written + 1);
  CHECK(at(at(closed, "reviews")[0], "decided_at").as_string() == digest.as_string());
}

// ---- who is told of a ledger's change, and when (protocol.md, "The ledger's events"; A55) -----

namespace {

// A waker that counts, for a case that waits for the platform's notice.
struct Woken {
  std::mutex mutex;
  std::condition_variable cv;
  int count = 0;
  static void wake(void* context) {
    auto* w = static_cast<Woken*>(context);
    {
      std::lock_guard<std::mutex> lock(w->mutex);
      ++w->count;
    }
    w->cv.notify_all();
  }
  int seen() {
    std::lock_guard<std::mutex> lock(mutex);
    return count;
  }
  // Up to `seconds` for a wake past `after`.
  bool wait_past(int after, int seconds) {
    std::unique_lock<std::mutex> lock(mutex);
    return cv.wait_for(lock, std::chrono::seconds(seconds), [&] { return count > after; });
  }
};

// A session manager watching its ledgers as a listening host's does, `woken` (declared before the
// host, so the watches stop first) told of each notice; the looks are made by hand.
void watch_ledgers(Host& h, Subscriptions& subscriptions, Woken& woken) {
  h.sessions.set_events(&subscriptions);
  h.sessions.set_ledger_waker(&Woken::wake, &woken);
  (void)subscriptions.subscribe(7, "", event_bit(EventKind::Session));
}

// The change of every event pushed since `from`.
std::vector<std::string> changes_of(const std::vector<std::string>& sent, usize from) {
  std::vector<std::string> out;
  for (usize i = from; i < sent.size(); ++i) {
    JsonValue note;
    REQUIRE(parse_json(sent[i], note).ok);
    out.emplace_back(at(at(note, "params"), "change").as_string());
  }
  return out;
}

i64 make_task(Host& on, const char* title, const char* actor = "b") {
  return num(
      at(on.task("task.create", obj({{"title", JsonValue(title)}, {"attribution", who(actor)}})),
         "id"));
}

}  // namespace

// A host's own write is told by its own event and never as `ledger_changed`, which is another
// process's write or a claim this host's clock let lapse: a raise for a task changes the item and
// the task, and both are the raise's.
TEST_CASE("ledger: a write's own event is the one event it pushes, its task included") {
  test::TempDir tmp("protocol_ledger");
  const std::string dir = tmp.file("doc");
  Woken woken;
  Collect pushed;
  Subscriptions subscriptions;
  subscriptions.attach(&pushed);
  Host h;
  watch_ledgers(h, subscriptions, woken);
  h.open(dir, true);
  Host b;
  b.open(dir, false);
  const i64 task = make_task(h, "the road", "director");
  CHECK(h.sessions.look_at_ledgers(true) == 0);

  usize mark = pushed.sent.size();
  const JsonValue item = h.review("review.raise", obj({{"kind", JsonValue("validation")},
                                                       {"summary", JsonValue("the road")},
                                                       {"task", JsonValue(task)},
                                                       {"attribution", who("director")}}));
  CHECK(changes_of(pushed.sent, mark) == std::vector<std::string>{"review_raised"});
  CHECK(at(item, "raised_digest").as_string().size() == 64);  // the stamp rides the same write
  CHECK(h.sessions.look_at_ledgers(true) == 0);
  CHECK(pushed.sent.size() == mark + 1);
  // The close, with what its closer saw, is one write and one event too.
  mark = pushed.sent.size();
  const JsonValue listed =
      h.ok("review.list", obj({{"review", at(item, "id")}, {"digest", JsonValue(true)}}));
  const JsonValue closed =
      h.review("review.close", obj({{"review", at(item, "id")},
                                    {"resolution", JsonValue("Accepted")},
                                    {"revision", at(at(listed, "reviews")[0], "revision")},
                                    {"digest", at(at(listed, "subject"), "digest")},
                                    {"attribution", who("director")}}));
  CHECK(at(closed, "decided_at").as_string() == at(at(listed, "subject"), "digest").as_string());
  CHECK(changes_of(pushed.sent, mark) == std::vector<std::string>{"review_closed"});
  CHECK(h.sessions.look_at_ledgers(true) == 0);
  CHECK(pushed.sent.size() == mark + 1);

  // Another process's write to the same task is `ledger_changed`, naming it.
  mark = pushed.sent.size();
  (void)b.task("task.update", obj({{"task", JsonValue(task)},
                                   {"note", JsonValue("from the other host")},
                                   {"attribution", who("director")}}));
  CHECK(h.sessions.look_at_ledgers(true) == 1);
  const std::vector<JsonValue> changes = ledger_changes(pushed, mark);
  REQUIRE(changes.size() == 1);
  CHECK(id_set(at(changes[0], "tasks")) == std::set<i64>{task});
  CHECK(at(changes[0], "reviews").size() == 0);
}

// A claim this host's clock let lapse is not a call's own change, and nothing else tells it: a
// list that lapses one pushes `ledger_changed` naming it, and a call that lapses another actor's
// claim beside its own write pushes its own event and one `ledger_changed` naming only the lapse.
TEST_CASE("ledger: a claim the host's clock let lapse is told as ledger_changed") {
  test::TempDir tmp("protocol_ledger");
  const std::string dir = tmp.file("doc");
  Woken woken;
  Collect pushed;
  Subscriptions subscriptions;
  subscriptions.attach(&pushed);
  Host h;
  watch_ledgers(h, subscriptions, woken);
  h.open(dir, true);
  // Written by hand, with claims that ran out long ago and one that stands.
  const auto write = [&](const char* tasks) {
    {
      std::ofstream out(dir + "/ledger.json", std::ios::binary);
      out << R"({"tasks":[)" << tasks << R"(],"reviews":[]})";
    }
    (void)h.sessions.look_at_ledgers(true);  // what the session has seen, told as another's write
  };
  const char* k_gone = R"({"id":11,"key":"task/11","title":"t","created_by":{"actor":"c"},)"
                       R"("status":"Claimed","claimant":"gone","claim_expires_unix_ms":1,)"
                       R"("claim_ttl_seconds":60})";
  write(k_gone);
  usize mark = pushed.sent.size();
  CHECK(num(at(h.ok("task.list"), "lapsed")) == 1);
  std::vector<JsonValue> changes = ledger_changes(pushed, mark);
  REQUIRE(changes.size() == 1);
  CHECK(id_set(at(changes[0], "tasks")) == std::set<i64>{11});
  CHECK(pushed.sent.size() == mark + 1);

  const std::string both =
      std::string(k_gone) +
      R"(,{"id":12,"key":"task/12","title":"t","created_by":{"actor":"c"},"status":"Claimed",)"
      R"("claimant":"worker","claim_expires_unix_ms":9000000000000,"claim_ttl_seconds":60})";
  write(both.c_str());
  mark = pushed.sent.size();
  (void)h.task(
      "task.update",
      obj({{"task", JsonValue(12)}, {"note", JsonValue("on it")}, {"attribution", who("worker")}}));
  CHECK(changes_of(pushed.sent, mark) ==
        std::vector<std::string>{"ledger_changed", "task_updated"});
  changes = ledger_changes(pushed, mark);
  REQUIRE(changes.size() == 1);
  CHECK(id_set(at(changes[0], "tasks")) == std::set<i64>{11});
  CHECK(h.sessions.look_at_ledgers(true) == 0);
}

// A write that failed wrote nothing anyone can read, so it is told as nothing: no event for it
// then, and none later naming what it would have made.
TEST_CASE("ledger: a write that failed is not a write") {
  test::TempDir tmp("protocol_ledger");
  const std::string dir = tmp.file("doc");
  Woken woken;
  Collect pushed;
  Subscriptions subscriptions;
  subscriptions.attach(&pushed);
  Host h;
  watch_ledgers(h, subscriptions, woken);
  h.open(dir, true);
  Host b;
  b.open(dir, false);
  (void)make_task(b, "first");
  (void)h.sessions.look_at_ledgers(true);

  const usize mark = pushed.sent.size();
  detail::fail_next_side_writes_for_test(1);
  CHECK(h.fails("task.create", obj({{"title", JsonValue("lost")}, {"attribution", who("h")}})) ==
        codes::k_io_error);
  CHECK(h.sessions.look_at_ledgers(true) == 0);
  CHECK(pushed.sent.size() == mark);
  const i64 kept = make_task(b, "kept");
  CHECK(h.sessions.look_at_ledgers(true) == 1);
  const std::vector<JsonValue> changes = ledger_changes(pushed, mark);
  REQUIRE(changes.size() == 1);
  CHECK(id_set(at(changes[0], "tasks")) == std::set<i64>{kept});
}

namespace {

// Collects what a listener's looker publishes, on its own thread, for a case to wait on.
struct WaitCollect final : EventTransport {
  std::mutex mutex;
  std::condition_variable cv;
  std::vector<std::string> sent;
  Post post(u64, std::string text, u32) override {
    {
      std::lock_guard<std::mutex> lock(mutex);
      sent.push_back(std::move(text));
    }
    cv.notify_all();
    return Post::Queued;
  }
  // Up to `seconds` for `ready(sent)`.
  template <class F>
  bool wait(F&& ready, int seconds) {
    std::unique_lock<std::mutex> lock(mutex);
    return cv.wait_for(lock, std::chrono::seconds(seconds), [&] { return ready(sent); });
  }
  std::vector<std::string> copy() {
    std::lock_guard<std::mutex> lock(mutex);
    return sent;
  }
};

// A host that listens, as engine-host --listen and the editor do: its looker, its watches, and
// what it publishes collected in-process rather than sent on a socket. `pushed` before the
// listener, so the listener's looker stops before what it publishes into goes.
struct Listening {
  Host h;
  std::mutex lock;  // the host's: every call below takes it, as the listener's requests do
  WaitCollect pushed;
  Listener listener{h.dispatcher, lock};
  void start() {
    std::string error;
    REQUIRE_MESSAGE(listener.start(0, "", h.session, error), error);
    std::lock_guard<std::mutex> host(lock);
    listener.events().attach(&pushed);
    (void)listener.events().subscribe(7, "", event_bit(EventKind::Session));
  }
};

}  // namespace

// A watch exists only while someone can be told: engine-cli's host per call builds a listener it
// never starts, and its sessions watch nothing — no thread, no read at open, nothing per write.
TEST_CASE("ledger: a host that tells nobody watches nothing") {
  test::TempDir tmp("protocol_ledger");
  Listening l;
  l.h.open(tmp.file("doc"), true);
  (void)make_task(l.h, "one");
  const Session* s = l.h.sessions.find(l.h.session);
  REQUIRE(s != nullptr);
  CHECK_FALSE(l.h.sessions.watches_ledgers());
  CHECK_FALSE(s->watching_ledger());
  l.start();
  CHECK(l.h.sessions.watches_ledgers());
  CHECK(s->watching_ledger());
  l.listener.stop();
  CHECK_FALSE(l.h.sessions.watches_ledgers());
  CHECK_FALSE(s->watching_ledger());
}

namespace {

// The ids of every `ledger_changed` pushed since `from`, each as often as it was named.
std::multiset<i64> changed_tasks(const std::vector<std::string>& sent, usize from) {
  std::multiset<i64> out;
  for (usize i = from; i < sent.size(); ++i) {
    JsonValue note;
    REQUIRE(parse_json(sent[i], note).ok);
    const JsonValue& params = at(note, "params");
    if (at(params, "change").as_string() != "ledger_changed") continue;
    const JsonValue& tasks = at(params, "tasks");
    for (u32 j = 0; j < tasks.size(); ++j)
      out.insert(num(tasks[j]));
  }
  return out;
}

}  // namespace

// No write falls between the watch's start and what the session has seen: the system's watch is
// registered before the file is read, so a write the moment the start returns raises a notice
// (here the watch's thread is made slow to register, which a start that returned first missed).
TEST_CASE("ledger: a write the moment a watch starts is announced") {
  test::TempDir tmp("protocol_ledger");
  const std::string dir = tmp.file("doc");
  Woken woken;
  Collect pushed;
  Subscriptions subscriptions;
  subscriptions.attach(&pushed);
  Host b;
  b.open(dir, true);
  Host h;
  watch_ledgers(h, subscriptions, woken);
  DirWatch::delay_registration_for_test(300);
  h.open(dir, false);
  const usize mark = pushed.sent.size();
  const i64 made = make_task(b, "now");
  REQUIRE(woken.wait_past(0, 10));
  CHECK(h.sessions.look_at_ledgers(false) == 1);
  const std::vector<JsonValue> changes = ledger_changes(pushed, mark);
  REQUIRE(changes.size() == 1);
  CHECK(id_set(at(changes[0], "tasks")) == std::set<i64>{made});
}

// The same over a listener's own looker: the watches start with it, and a write the moment its
// start returns reaches a subscriber as one `ledger_changed`.
TEST_CASE("ledger: a listener's looker announces a write made as it starts") {
  test::TempDir tmp("protocol_ledger");
  const std::string dir = tmp.file("doc");
  Listening l;
  l.h.open(dir, true);
  Host b;
  b.open(dir, false);
  DirWatch::delay_registration_for_test(300);
  l.start();
  const i64 made = make_task(b, "now");
  REQUIRE(l.pushed.wait(
      [&](const std::vector<std::string>& sent) { return changed_tasks(sent, 0).size() >= 1; },
      10));
  CHECK(changed_tasks(l.pushed.copy(), 0) == std::multiset<i64>{made});
  DirWatch::delay_registration_for_test(0);  // a start that did not take it leaves it for nobody
}
