// Plan 06 §6.8's task ledger and review items over the dispatcher (docs/subsystems/protocol.md,
// "The task ledger and review items"): a task's life from creation to done, read back by another
// host; who may claim, update and close what; a review item for a person that no agent can close;
// and two hosts writing one directory's ledger at once without losing a write.
#include <core/json/json.h>
#include <domain/protocol/clients.h>
#include <domain/protocol/events.h>
#include <domain/protocol/policy.h>
#include <domain/protocol/rpc.h>
#include <domain/protocol/session.h>
#include <foundation/io/roots.h>
#include <foundation/io/vfs.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <chrono>
#include <fstream>
#include <set>
#include <string>
#include <thread>
#include <vector>

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
  const JsonValue id2 = at(
      h.task("task.create", obj({{"title", JsonValue("Second")}, {"attribution", who("director")}})),
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
  CHECK(
      h.fails(
          "task.update",
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
  t = h.task(
      "task.update",
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
  t = second.task("task.update", obj({{"task", id1},
                                      {"status", JsonValue("Open")},
                                      {"attribution", who("director")}}));
  CHECK(at(t, "status").as_string() == "Open");
  CHECK(at(t, "claimant").as_string().empty());

  // Malformed asks.
  CHECK(h.fails("task.create", obj({{"title", JsonValue("")}, {"attribution", who("d")}})) ==
        codes::k_invalid_argument);
  CHECK(h.fails("task.create", obj({{"title", JsonValue("a\x01")}, {"attribution", who("d")}})) ==
        codes::k_invalid_argument);
  CHECK(h.fails("task.claim", obj({{"task", id2},
                                   {"ttl_seconds", JsonValue(0.5)},
                                   {"attribution", who("a")}})) == codes::k_invalid_argument);
}

TEST_CASE("ledger: a claim nobody renews lapses and the task is open again") {
  test::TempDir tmp("protocol_ledger");
  Host h;
  h.open(tmp.file("doc"), true);
  const JsonValue id =
      at(h.task("task.create", obj({{"title", JsonValue("t")}, {"attribution", who("c")}})), "id");
  (void)h.task(
      "task.claim",
      obj({{"task", id}, {"ttl_seconds", JsonValue(1.0)}, {"attribution", who("a")}}));
  std::this_thread::sleep_for(std::chrono::milliseconds(1100));
  const JsonValue listed = h.ok("task.list");
  CHECK(num(at(listed, "lapsed")) == 1);
  const JsonValue& t = at(listed, "tasks")[0];
  CHECK(at(t, "status").as_string() == "Open");
  const JsonValue& history = at(t, "history");
  CHECK(at(history[history.size() - 1], "change").as_string() == "lapsed");
  CHECK(at(history[history.size() - 1], "actor").as_string() == "a");
  CHECK(at(history[history.size() - 1], "via").as_string() == "host");  // the host's clock, no caller
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
  (void)h.task("task.claim",
               obj({{"task", dunes}, {"attribution", who("e", "environment")}}));
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
  CHECK(at(at(h.ok("task.list", obj({{"task", dunes}})), "tasks")[0], "reviews").size() ==
        1);

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
  CHECK(at(h.review("review.list", obj({{"review", item}})), "state").as_string() ==
        "Open");
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
  r = h.review("review.close", obj({{"review", item},
                                    {"resolution", JsonValue("Accepted")},
                                    {"note", JsonValue("looked at full policy; fine")},
                                    {"revision", JsonValue(u64{1})},
                                    {"attribution", who("person")}}));
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
  CHECK(h.fails("review.close", obj({{"review", item}, {"attribution", who("a")}}),
                &error) == codes::k_forbidden);
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
  const JsonValue shared = at(
      a.task("task.create", obj({{"title", JsonValue("shared")}, {"attribution", who("w")}})), "id");
  Host b;
  b.open(dir, false);
  constexpr int k_each = 25;
  std::vector<i64> ids_a, ids_b;
  const auto work = [&](Host& h, const char* actor, std::vector<i64>& ids) {
    for (int i = 0; i < k_each; ++i) {
      ids.push_back(num(
          at(h.task("task.create", obj({{"title", JsonValue(actor)}, {"attribution", who(actor)}})),
             "id")));
      (void)h.task(
          "task.update",
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
// back is version 2.
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
  const i64 made =
      num(at(h.task("task.create", obj({{"title", JsonValue("new")}, {"attribution", who("b")}})),
             "id"));
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
    CHECK(h.fails("task.create", obj({{"title", JsonValue("one more")}, {"attribution", who("loop")}}),
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
    CHECK(h.fails("review.raise", obj({{"kind", JsonValue("validation")},
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
    CHECK(h.fails("task.update", obj({{"task", JsonValue(u64{5})},
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