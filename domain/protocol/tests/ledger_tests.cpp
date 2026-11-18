// Plan 06 §6.8's task ledger and review items over the dispatcher (docs/subsystems/protocol.md,
// "The task ledger and review items"): a task's life from creation to done, read back by another
// host; who may claim, update and close what; a review item for a person that no agent can close;
// and two hosts writing one directory's ledger at once without losing a write.
#include <core/json/json.h>
#include <domain/protocol/events.h>
#include <domain/protocol/policy.h>
#include <domain/protocol/rpc.h>
#include <domain/protocol/session.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <chrono>
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
  Dispatcher dispatcher{Context{&sessions, nullptr, nullptr, nullptr, &policy}};
  std::string session;

  Host() { add_builtin_methods(dispatcher); }

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
    {"name": "reviewer", "methods": ["review.*"], "human": true},
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
  CHECK(num(at(t, "id")) == 1);
  CHECK(at(t, "key").as_string() == "task/1");
  CHECK(at(t, "status").as_string() == "Open");
  CHECK(at(at(t, "created_by"), "actor").as_string() == "director");
  CHECK(at(at(t, "links"), "proposals").size() == 1);
  CHECK(num(at(h.task("task.create",
                      obj({{"title", JsonValue("Second")}, {"attribution", who("director")}})),
               "id")) == 2);

  // a claims it; b is refused while the claim stands, and may not update it.
  t = h.task("task.claim", obj({{"task", JsonValue(1)}, {"attribution", who("a")}}));
  CHECK(at(t, "status").as_string() == "Claimed");
  CHECK(at(t, "claimant").as_string() == "a");
  JsonValue error;
  CHECK(h.fails("task.claim", obj({{"task", JsonValue(1)}, {"attribution", who("b")}}), &error) ==
        codes::k_lease_conflict);
  CHECK(at(at(error, "data"), "claimant").as_string() == "a");
  CHECK(
      h.fails(
          "task.update",
          obj({{"task", JsonValue(1)}, {"note", JsonValue("mine now")}, {"attribution", who("b")}}),
          &error) == codes::k_forbidden);
  CHECK(reason_of(error) == "task owner");
  // The creator may not finish it: only the claimant does.
  CHECK(h.fails("task.update", obj({{"task", JsonValue(1)},
                                    {"status", JsonValue("Done")},
                                    {"attribution", who("director")}})) ==
        codes::k_invalid_argument);

  // a blocks with a note, unblocks, links a checkpoint, finishes.
  t = h.task("task.update", obj({{"task", JsonValue(1)},
                                 {"status", JsonValue("Blocked")},
                                 {"note", JsonValue("the well is not placed yet")},
                                 {"attribution", who("a")}}));
  CHECK(at(t, "status").as_string() == "Blocked");
  t = h.task("task.update", obj({{"task", JsonValue(1)},
                                 {"status", JsonValue("Claimed")},
                                 {"links", obj({{"checkpoints", strings({"abc123"})},
                                                {"proposals", strings({"roads"})}})},
                                 {"attribution", who("a")}}));
  CHECK(at(at(t, "links"), "proposals").size() == 1);  // a duplicate is not added twice
  CHECK(at(at(t, "links"), "checkpoints").size() == 1);
  t = h.task(
      "task.update",
      obj({{"task", JsonValue(1)}, {"status", JsonValue("Done")}, {"attribution", who("a")}}));
  CHECK(at(t, "status").as_string() == "Done");
  CHECK(at(t, "claimant").as_string() == "a");  // who did it stays
  CHECK(h.fails("task.claim", obj({{"task", JsonValue(1)}, {"attribution", who("b")}})) ==
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
  CHECK(pushed.sent[3].find("\"task\":1") != std::string::npos);
  CHECK(pushed.sent[3].find("\"actor\":\"a\"") != std::string::npos);

  // Another host on the same directory reads what the first wrote: nothing lives in a host.
  Host second;
  second.open(dir, false);
  JsonValue listed = second.ok("task.list", obj({{"statuses", strings({"Open"})}}));
  REQUIRE(at(listed, "tasks").size() == 1);
  CHECK(num(at(at(listed, "tasks")[0], "id")) == 2);
  listed = second.ok("task.list", obj({{"task", JsonValue(1)}, {"brief", JsonValue(true)}}));
  CHECK(at(at(listed, "tasks")[0], "history").size() == 0);
  CHECK(second.fails("task.list", obj({{"task", JsonValue(9)}})) == codes::k_not_found);
  // The creator reopens it; nobody holds it then.
  t = second.task("task.update", obj({{"task", JsonValue(1)},
                                      {"status", JsonValue("Open")},
                                      {"attribution", who("director")}}));
  CHECK(at(t, "status").as_string() == "Open");
  CHECK(at(t, "claimant").as_string().empty());

  // Malformed asks.
  CHECK(h.fails("task.create", obj({{"title", JsonValue("")}, {"attribution", who("d")}})) ==
        codes::k_invalid_argument);
  CHECK(h.fails("task.create", obj({{"title", JsonValue("a\x01")}, {"attribution", who("d")}})) ==
        codes::k_invalid_argument);
  CHECK(h.fails("task.claim", obj({{"task", JsonValue(2)},
                                   {"ttl_seconds", JsonValue(0.5)},
                                   {"attribution", who("a")}})) == codes::k_invalid_argument);
}

TEST_CASE("ledger: a claim nobody renews lapses and the task is open again") {
  test::TempDir tmp("protocol_ledger");
  Host h;
  h.open(tmp.file("doc"), true);
  (void)h.task("task.create", obj({{"title", JsonValue("t")}, {"attribution", who("c")}}));
  (void)h.task(
      "task.claim",
      obj({{"task", JsonValue(1)}, {"ttl_seconds", JsonValue(1.0)}, {"attribution", who("a")}}));
  std::this_thread::sleep_for(std::chrono::milliseconds(1100));
  const JsonValue listed = h.ok("task.list");
  CHECK(num(at(listed, "lapsed")) == 1);
  const JsonValue& t = at(listed, "tasks")[0];
  CHECK(at(t, "status").as_string() == "Open");
  const JsonValue& history = at(t, "history");
  CHECK(at(history[history.size() - 1], "change").as_string() == "lapsed");
  CHECK(at(history[history.size() - 1], "actor").as_string() == "a");
  // b may take it now; a's renewal is a claim of a task b holds.
  (void)h.task("task.claim", obj({{"task", JsonValue(1)}, {"attribution", who("b")}}));
  CHECK(h.fails("task.claim", obj({{"task", JsonValue(1)}, {"attribution", who("a")}})) ==
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
  (void)h.task("task.create", obj({{"title", JsonValue("dunes")},
                                   {"owner_role", JsonValue("environment")},
                                   {"attribution", who("d", "director")}}));
  JsonValue error;
  CHECK(h.fails("task.claim", obj({{"task", JsonValue(1)}, {"attribution", who("x", "designer")}}),
                &error) == codes::k_forbidden);
  CHECK(reason_of(error) == "task role");
  (void)h.task("task.claim",
               obj({{"task", JsonValue(1)}, {"attribution", who("e", "environment")}}));
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
                    {"task", JsonValue(1)},
                    {"attribution", who("q", "qa")}}));
  CHECK(num(at(r, "id")) == 1);
  CHECK(at(r, "state").as_string() == "Open");
  CHECK(num(at(at(r, "subject"), "journal_length")) == 1);  // the host's, not the raiser's 99
  CHECK(at(at(r, "report"), "validator").as_string() == "anatomy");
  CHECK(at(at(h.ok("task.list", obj({{"task", JsonValue(1)}})), "tasks")[0], "reviews").size() ==
        1);

  // Nobody but a human role closes it: not the agent roles, not the director, not the raiser.
  for (const char* role : {"environment", "director", "qa"}) {
    CAPTURE(role);
    CHECK(h.fails("review.close",
                  obj({{"review", JsonValue(1)},
                       {"resolution", JsonValue("Accepted")},
                       {"attribution", who("someone", role)}}),
                  &error) == codes::k_forbidden);
    if (std::string_view(role) != "qa") CHECK(reason_of(error) == "human review");
  }
  CHECK(h.fails("review.close",
                obj({{"review", JsonValue(1)},
                     {"resolution", JsonValue("Withdrawn")},
                     {"attribution", who("q", "environment")}}),
                &error) == codes::k_forbidden);
  CHECK(reason_of(error) == "human review");
  r = h.review("review.close", obj({{"review", JsonValue(1)},
                                    {"resolution", JsonValue("Accepted")},
                                    {"note", JsonValue("looked at full policy; fine")},
                                    {"attribution", who("person", "reviewer")}}));
  CHECK(at(r, "state").as_string() == "Closed");
  CHECK(at(r, "resolution").as_string() == "Accepted");
  CHECK(at(at(r, "closed_by"), "actor").as_string() == "person");
  CHECK(h.fails("review.close", obj({{"review", JsonValue(1)},
                                     {"resolution", JsonValue("Rejected")},
                                     {"attribution", who("person", "reviewer")}})) ==
        codes::k_invalid_argument);

  // An item for an agent: its raiser withdraws it, nobody else does, a reviewing agent closes one.
  for (int i = 0; i < 2; ++i)
    (void)h.review("review.raise", obj({{"kind", JsonValue("validation")},
                                        {"summary", JsonValue("two props overlap")},
                                        {"attribution", who("e", "environment")}}));
  CHECK(h.fails("review.close",
                obj({{"review", JsonValue(2)},
                     {"resolution", JsonValue("Withdrawn")},
                     {"attribution", who("other", "designer")}}),
                &error) == codes::k_forbidden);
  CHECK(reason_of(error) == "review raiser");
  CHECK(at(h.review("review.close", obj({{"review", JsonValue(2)},
                                         {"resolution", JsonValue("Withdrawn")},
                                         {"attribution", who("e", "environment")}})),
           "resolution")
            .as_string() == "Withdrawn");
  CHECK(at(h.review("review.close", obj({{"review", JsonValue(3)},
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

TEST_CASE("ledger: a host with no human role has nobody who closes a person's item") {
  test::TempDir tmp("protocol_ledger");
  Host h;  // no roles file: nothing restricts a call, and nothing marks one a person's
  h.open(tmp.file("doc"), true);
  (void)h.review("review.raise", obj({{"kind", JsonValue("classified content review")},
                                      {"audience", JsonValue("Human")},
                                      {"summary", JsonValue("s")},
                                      {"attribution", who("a")}}));
  JsonValue error;
  CHECK(h.fails("review.close", obj({{"review", JsonValue(1)}, {"attribution", who("a")}}),
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
  (void)a.task("task.create", obj({{"title", JsonValue("shared")}, {"attribution", who("w")}}));
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
          obj({{"task", JsonValue(1)}, {"note", JsonValue(actor)}, {"attribution", who("w")}}));
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
  CHECK(*ids.rbegin() == 1 + 2 * k_each);
  for (const i64 id : ids_a)
    CHECK(ids.count(id) == 1);
  for (const i64 id : ids_b)
    CHECK(ids.count(id) == 1);
  // The shared task's history: its creation and every note of both hosts.
  CHECK(at(tasks[0], "history").size() == 1 + 2 * k_each);
}
