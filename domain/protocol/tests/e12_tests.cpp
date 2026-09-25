// Experiment E12 (docs/plan/10-roadmap-risks.md §10.5; docs/experiments/e12-proposal-layers-and-
// leases.md): structural three-way merge of proposal layers under simulated multi-agent edits —
// what is the conflict rate with tile leases, and without them?
//
// N agents share one document through one dispatcher, as agents sharing one host would: a `world`
// layer tiled 10 m a side, 8 x 8 tiles, six props a tile. Each agent owns a block of tiles, keeps
// a proposal layer over `world`, and makes its edits — create, set a property, reparent, delete —
// mostly in its own block and, with probability `leak`, in a tile of the ring around it, which
// belongs to a neighbour. After every `per_cycle` edits it asks a director to promote the proposal
// and starts the next cycle. Three modes:
//
//   none        no leases: every edit is applied, and conflicts surface at promotion.
//   give_up     leases required: an agent leases its block for the cycle; a leak edit asks for a
//               one-tile lease, and when the neighbour holds it the edit is dropped.
//   retry       the same, but a refused edit is kept and asked for again each turn until the
//               neighbour lets its block go at the end of its cycle; the wait is counted in rounds.
//
// A promotion refused for conflicts or validation counts as needing a human; the simulation then
// settles it the way a director most often would (the target's side of a conflict wins; a record
// that breaks validation is taken out of the proposal) and counts every edit that loses.
//
// The always-on case runs a small configuration and holds the invariants the page's conclusions
// rest on (leases leave nothing to conflict at merge; the accounting closes; the world validates
// afterwards). The full matrix is skipped unless asked for:
//
//   engine_protocol_tests.exe -ns -tc="E12: the matrix"      (ENGINE_E12_OUT=<file.jsonl> for rows)
#include <core/json/json.h>
#include <core/time/time.h>
#include <domain/protocol/rpc.h>
#include <domain/protocol/session.h>
#include <foundation/bench/machine_state.h>

#include <doctest/doctest.h>
#include <test_paths.h>
#include <test_temp_dir.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <random>
#include <string>
#include <vector>

using namespace engine;
using namespace engine::protocol;

namespace {

constexpr i32 k_grid = 8;          // tiles a side
constexpr u32 k_per_tile = 6;      // props a tile at the start, in the matrix
constexpr f64 k_tile = 10.0;       // metres a tile
constexpr u32 k_round_cap = 4000;  // a run that has not finished by then is reported as such

enum class Mode : u8 { none, give_up, retry };
const char* mode_name(Mode m) {
  return m == Mode::none ? "none" : (m == Mode::give_up ? "give_up" : "retry");
}

struct Config {
  u32 agents = 4;
  u32 edits = 40;  // per agent
  u32 cycles = 2;  // promotions per agent
  f64 leak = 0.1;
  Mode mode = Mode::none;
  u64 seed = 1;
  u32 per_tile = k_per_tile;  // props a tile at the start
};

struct Result {
  u32 attempted = 0;      // edits the agents set out to make
  u32 applied = 0;        // committed by doc.apply
  u32 refused_apply = 0;  // doc.apply committed nothing: a strict precondition failed
  u32 stale = 0;          //   ... because an object or a parent was gone (a neighbour's proposal)
  u32 cycle = 0;          //   ... because a reparent would close a cycle
  u32 lease_calls = 0;    // lease.acquire calls
  u32 lease_refused = 0;  // ... refused (1009)
  u32 dropped = 0;        // edits never made: give_up's refusals, and a retry that ran out
  u32 deferred = 0;       // edits retry kept for later
  u64 wait_rounds = 0;    // summed over deferred edits that were made
  u32 wait_max = 0;
  u32 waited = 0;          // deferred edits that were made in the end
  u32 home_waits = 0;      // rounds an agent waited for its own block at a cycle's start
  u32 edit_conflicts = 0;  // 1009 at an edit (a tile lease against a type lease; none here)
  u32 edit_uncovered = 0;  // 1010 at an edit (none here: an agent leases before it edits)
  u32 promotions = 0;
  u32 needed_human = 0;             // promotions refused on the first try
  u32 conflicts[4] = {0, 0, 0, 0};  // by MergeConflictKind
  u32 validation_refusals = 0;
  u32 validation_problems = 0;
  u32 lost = 0;      // edits that lost: a conflict's losing side, a record taken out
  u32 rejected = 0;  // proposals no settlement could promote
  u32 rounds = 0;
  bool finished = true;
  bool world_valid = true;
  f64 wall_ms = 0;
  std::vector<f64> merge_ms, validate_ms, commit_ms, promote_ms, apply_ms;

  u32 conflicts_total() const { return conflicts[0] + conflicts[1] + conflicts[2] + conflicts[3]; }
};

JsonValue obj(std::initializer_list<std::pair<const char*, JsonValue>> fields) {
  JsonValue o = JsonValue::object();
  for (auto& [k, v] : fields)
    o.set(k, v);
  return o;
}

std::string hex(Id128 id) {
  char buf[Id128::k_hex_length + 1];
  id.to_hex(buf);
  return buf;
}

f64 number(const JsonValue* v) {
  f64 d = 0;
  return v != nullptr && v->get_f64(d) ? d : 0.0;
}

f64 percentile(std::vector<f64> v, f64 p) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  const usize i = static_cast<usize>(p * static_cast<f64>(v.size() - 1) + 0.5);
  return v[std::min(i, v.size() - 1)];
}

struct Tile {
  i32 x = 0, y = 0;
  friend bool operator==(Tile a, Tile b) { return a.x == b.x && a.y == b.y; }
  friend bool operator<(Tile a, Tile b) { return a.x != b.x ? a.x < b.x : a.y < b.y; }
};

struct Pending {
  u8 kind = 0;
  Tile tile;
  u32 since = 0;  // the round it was first refused
};

struct Agent {
  u32 index = 0;
  std::string actor, layer;
  i32 x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // home block, inclusive
  std::vector<Tile> ring;              // neighbours' tiles next to the block
  std::mt19937_64 rng;
  u32 made = 0;      // new edits started
  u32 in_cycle = 0;  // new edits started this cycle
  u32 cycles_done = 0;
  bool open = false;  // proposal open this cycle
  bool holds_home = false;
  u64 home_lease = 0;
  std::vector<u64> held;  // one-tile leases for edits next door, kept until the promotion
  u64 next_id = 0;
  std::vector<Pending> pending;
  // What the agent knows: the live props per tile, read from the document at each cycle's start,
  // plus what it made since.
  std::map<Tile, std::vector<Id128>> known;
  bool finished = false;
};

class Simulation {
 public:
  Simulation(const Config& config, const std::string& dir) : config_(config), dir_(dir) {
    add_builtin_methods(dispatcher_);
  }

  Result run() {
    const i64 started = time::monotonic_ns();
    build_world();
    make_agents();
    std::mt19937_64 order_rng(config_.seed * 7919u + 17u);
    std::vector<u32> order(agents_.size());
    for (u32 i = 0; i < order.size(); ++i)
      order[i] = i;
    u32 round = 0;
    for (; round < k_round_cap; ++round) {
      bool any = false;
      std::shuffle(order.begin(), order.end(), order_rng);
      for (const u32 i : order) {
        Agent& a = agents_[i];
        if (a.finished) continue;
        any = true;
        turn(a, round);
      }
      if (!any) break;
    }
    result_.rounds = round;
    for (const Agent& a : agents_) {
      if (a.finished) continue;
      result_.finished = false;
      result_.dropped += static_cast<u32>(a.pending.size());
    }
    // The world the promotions left must validate as well as it did at the start (it did fully).
    JsonValue v = ok("doc.validate", obj({}));
    result_.world_valid = *v.find("ok") == JsonValue(true);
    result_.wall_ms = static_cast<f64>(time::monotonic_ns() - started) / 1.0e6;
    return result_;
  }

 private:
  // ---- the wire
  // ----------------------------------------------------------------------------------

  JsonValue call(std::string_view method, JsonValue params) {
    JsonValue req = obj({{"jsonrpc", JsonValue("2.0")}, {"id", JsonValue(1)}});
    req.set("method", JsonValue(method));
    if (params.is_object() && !session_.empty() && params.find("session") == nullptr)
      params.set("session", JsonValue(session_));
    req.set("params", std::move(params));
    return dispatcher_.dispatch(req);
  }
  JsonValue ok(std::string_view method, JsonValue params) {
    JsonValue resp = call(method, std::move(params));
    if (const JsonValue* e = resp.find("error")) FAIL(method << ": " << write_json(*e));
    return *resp.find("result");
  }
  static i32 code_of(const JsonValue& resp) {
    const JsonValue* e = resp.find("error");
    return e != nullptr ? static_cast<i32>(e->find("code")->as_int()) : 0;
  }
  JsonValue who(const Agent& a) const {
    return obj({{"actor", JsonValue(a.actor)},
                {"role", JsonValue("environment")},
                {"task", JsonValue("e12")},
                {"rationale", JsonValue("simulated edit")}});
  }
  JsonValue director() const {
    return obj({{"actor", JsonValue("director")},
                {"role", JsonValue("director")},
                {"task", JsonValue("e12")},
                {"rationale", JsonValue("reviewed")}});
  }

  // ---- the world
  // ---------------------------------------------------------------------------------

  static JsonValue position_in(Tile t, std::mt19937_64& rng) {
    std::uniform_real_distribution<f64> u(0.5, k_tile - 0.5);
    JsonValue p = JsonValue::array();
    p.push_back(JsonValue(t.x * k_tile + u(rng)));
    p.push_back(JsonValue(0.0));
    p.push_back(JsonValue(t.y * k_tile + u(rng)));
    return p;
  }

  void build_world() {
    const JsonValue info =
        ok("session.open", obj({{"path", JsonValue(dir_)}, {"create", JsonValue(true)}}));
    session_ = std::string(info.find("session")->as_string());
    ok("doc.add_layer",
       obj({{"name", JsonValue("world")}, {"partition", obj({{"tile_size", JsonValue(k_tile)}})}}));
    std::mt19937_64 rng(config_.seed);
    JsonValue commands = JsonValue::array();
    u64 n = 0;
    for (i32 x = 0; x < k_grid; ++x) {
      for (i32 y = 0; y < k_grid; ++y) {
        for (u32 k = 0; k < config_.per_tile; ++k) {
          const Id128 id = Id128::from_parts(0xE12, ++n);
          commands.push_back(obj({{"kind", JsonValue("CreateObject")},
                                  {"id", JsonValue(hex(id))},
                                  {"type", JsonValue("engine.protocol.test.Prop")},
                                  {"value", obj({{"name", JsonValue("prop")},
                                                 {"position", position_in({x, y}, rng)}})}}));
        }
      }
    }
    ok("doc.apply",
       obj({{"commands", commands}, {"attribution", director()}, {"layer", JsonValue("world")}}));
    if (config_.mode != Mode::none)
      ok("lease.require", obj({{"required", JsonValue(true)}, {"attribution", director()}}));
  }

  void make_agents() {
    const u32 n = config_.agents;
    const u32 cols = n == 4 ? 2 : 4;
    const u32 rows = n / cols;
    const i32 w = k_grid / static_cast<i32>(cols), h = k_grid / static_cast<i32>(rows);
    agents_.resize(n);
    for (u32 i = 0; i < n; ++i) {
      Agent& a = agents_[i];
      a.index = i;
      a.actor = "agent" + std::to_string(i);
      a.layer = "p." + a.actor;
      a.x0 = static_cast<i32>(i % cols) * w;
      a.y0 = static_cast<i32>(i / cols) * h;
      a.x1 = a.x0 + w - 1;
      a.y1 = a.y0 + h - 1;
      a.rng.seed(config_.seed * 1000003u + i);
      for (i32 x = a.x0 - 1; x <= a.x1 + 1; ++x) {
        for (i32 y = a.y0 - 1; y <= a.y1 + 1; ++y) {
          const bool inside = x >= a.x0 && x <= a.x1 && y >= a.y0 && y <= a.y1;
          if (inside || x < 0 || y < 0 || x >= k_grid || y >= k_grid) continue;
          a.ring.push_back({x, y});
        }
      }
    }
  }

  static Tile tile_of(const JsonValue* position) {
    if (position == nullptr || !position->is_array() || position->size() != 3) return {-1, -1};
    f64 x = 0, z = 0;
    (*position)[0].get_f64(x);
    (*position)[2].get_f64(z);
    return {static_cast<i32>(x / k_tile), static_cast<i32>(z / k_tile)};
  }

  // What an agent reads at a cycle's start: the live props of its block and its ring, composed —
  // neighbours' open proposals included, because that is what everyone reads. With `only`, one
  // tile, read again just before the agent edits in it.
  void refresh(Agent& a, const Tile* only = nullptr) {
    if (only != nullptr) {
      a.known[*only].clear();
    } else {
      a.known.clear();
    }
    const JsonValue r = ok("doc.objects", obj({{"limit", JsonValue(u32{100000})}}));
    const JsonValue& list = *r.find("objects");
    for (usize i = 0; i < list.size(); ++i) {
      const JsonValue& o = list[i];
      const Tile t = tile_of(o.find("properties")->find("position"));
      if (only != nullptr && !(t == *only)) continue;
      const bool home = t.x >= a.x0 && t.x <= a.x1 && t.y >= a.y0 && t.y <= a.y1;
      const bool ring = std::find(a.ring.begin(), a.ring.end(), t) != a.ring.end();
      if (!home && !ring) continue;
      Id128 id;
      Id128::from_hex(o.find("id")->as_string(), id);
      a.known[t].push_back(id);
    }
  }
  void refresh(Agent& a, Tile only) { refresh(a, &only); }

  // ---- one turn
  // ------------------------------------------------------------------------------------

  // A lease, held until the proposal that carries the edits it covers is promoted: a lease
  // released right after the edit protects nothing, because the edit sits in the proposal
  // unmerged and a neighbour who takes the tile next can change the record under it (the first
  // run of this experiment found exactly that: a conflict at merge with leases on).
  bool acquire(Agent& a, i32 x0, i32 y0, i32 x1, i32 y1, bool home) {
    ++result_.lease_calls;
    JsonValue tiles = JsonValue::array();
    tiles.push_back(obj({{"x0", JsonValue(x0)},
                         {"y0", JsonValue(y0)},
                         {"x1", JsonValue(x1)},
                         {"y1", JsonValue(y1)}}));
    const JsonValue resp = call("lease.acquire", obj({{"layer", JsonValue("world")},
                                                      {"tiles", tiles},
                                                      {"ttl_seconds", JsonValue(3600.0)},
                                                      {"attribution", who(a)}}));
    if (code_of(resp) == codes::k_lease_conflict) {
      ++(home ? result_.home_waits : result_.lease_refused);
      return false;
    }
    REQUIRE_MESSAGE(resp.find("result") != nullptr, write_json(resp));
    u64 lease = 0;
    REQUIRE(resp.find("result")->find("id")->get_u64(lease));
    (home ? a.home_lease : a.held.emplace_back()) = lease;
    return true;
  }

  void release_all(Agent& a) {
    if (a.holds_home) {
      ok("lease.release", obj({{"lease", JsonValue(a.home_lease)}, {"attribution", who(a)}}));
      a.holds_home = false;
    }
    for (const u64 lease : a.held)
      ok("lease.release", obj({{"lease", JsonValue(lease)}, {"attribution", who(a)}}));
    a.held.clear();
  }

  void turn(Agent& a, u32 round) {
    const bool leases = config_.mode != Mode::none;
    const u32 per_cycle = config_.edits / config_.cycles;
    const bool quota_done = a.made >= config_.edits;
    if (!a.open) {
      if (quota_done && a.pending.empty()) {
        a.finished = true;
        return;
      }
      ok("doc.propose_layer", obj({{"name", JsonValue(a.layer)},
                                   {"target", JsonValue("world")},
                                   {"edit", JsonValue(false)},
                                   {"attribution", who(a)}}));
      a.open = true;
      a.in_cycle = 0;
      refresh(a);
    }
    // An agent holds its block while it has new edits to make. One that has made them all and
    // still owes kept edits hands the block back (by promoting what it has) and works off what it
    // owes one edit at a time, each in a proposal of its own promoted at once: waiting for a
    // neighbour's tile while holding its own, or while holding a tile it took earlier, is how two
    // such agents deadlock.
    if (quota_done && a.holds_home) {
      promote(a);
      return;
    }
    if (leases && !a.holds_home && !quota_done) {
      if (!acquire(a, a.x0, a.y0, a.x1, a.y1, true)) return;  // a neighbour's promotion frees it
      a.holds_home = true;
      refresh(a);  // read under the lease: until now a neighbour could still change the block
    }
    // A kept edit first: when its tile can be had now, it is this turn's edit.
    if (config_.mode == Mode::retry && !a.pending.empty()) {
      const Pending p = a.pending.front();
      if (acquire(a, p.tile.x, p.tile.y, p.tile.x, p.tile.y, false)) {
        a.pending.erase(a.pending.begin());
        const u32 waited = round - p.since;
        result_.wait_rounds += waited;
        result_.wait_max = std::max(result_.wait_max, waited);
        ++result_.waited;
        refresh(a, p.tile);
        edit(a, p.kind, p.tile);
        if (quota_done) promote(a);
        return;
      }
    }
    if (quota_done) {
      if (!a.pending.empty()) {
        if (round < 2 * k_round_cap / 3) return;                // wait for the tiles
        result_.dropped += static_cast<u32>(a.pending.size());  // it is not going to happen
        a.pending.clear();
      }
      promote(a);
      return;
    }
    if (a.in_cycle >= per_cycle) {
      promote(a);
      return;
    }
    // A new edit: which tile, which kind.
    ++a.made;
    ++a.in_cycle;
    ++result_.attempted;
    std::uniform_real_distribution<f64> u(0.0, 1.0);
    Tile t;
    const bool leak = !a.ring.empty() && u(a.rng) < config_.leak;
    if (leak) {
      t = a.ring[std::uniform_int_distribution<usize>(0, a.ring.size() - 1)(a.rng)];
    } else {
      t = {std::uniform_int_distribution<i32>(a.x0, a.x1)(a.rng),
           std::uniform_int_distribution<i32>(a.y0, a.y1)(a.rng)};
    }
    const f64 k = u(a.rng);
    const u8 kind = k < 0.25 ? 0 : (k < 0.70 ? 1 : (k < 0.85 ? 2 : 3));
    if (leases && leak) {
      if (!acquire(a, t.x, t.y, t.x, t.y, false)) {
        if (config_.mode == Mode::give_up) {
          ++result_.dropped;
        } else {
          ++result_.deferred;
          a.pending.push_back({kind, t, round});
        }
        return;
      }
      refresh(a, t);  // acquire, read, then edit: the tile cannot change under the lease
      edit(a, kind, t);
      return;
    }
    // Every mode reads a neighbour's tile before it edits there; only a lease keeps it that way.
    if (leak) refresh(a, t);
    edit(a, kind, t);
  }

  // One edit of `kind` (0 create, 1 set, 2 reparent, 3 delete) in tile `t`, as one doc.apply into
  // the agent's proposal.
  void edit(Agent& a, u8 kind, Tile t) {
    std::vector<Id128>& here = a.known[t];
    std::uniform_int_distribution<usize> pick(0, here.empty() ? 0 : here.size() - 1);
    std::uniform_real_distribution<f64> u(0.0, 1.0);
    if (here.empty() || (kind == 2 && here.size() < 2)) kind = 0;
    JsonValue c;
    Id128 created;
    usize deleted_at = here.size();
    switch (kind) {
      case 0: {
        created = Id128::from_parts(0xA000 + a.index, ++a.next_id);
        c = obj(
            {{"kind", JsonValue("CreateObject")},
             {"id", JsonValue(hex(created))},
             {"type", JsonValue("engine.protocol.test.Prop")},
             {"value", obj({{"name", JsonValue(a.actor)}, {"position", position_in(t, a.rng)}})}});
        if (!here.empty() && u(a.rng) < 0.3) c.set("parent", JsonValue(hex(here[pick(a.rng)])));
        break;
      }
      case 1: {
        const Id128 id = here[pick(a.rng)];
        const u32 which = std::uniform_int_distribution<u32>(0, 3)(a.rng);
        JsonValue value;
        const char* name = "color";
        if (which == 0) {
          value = JsonValue(std::uniform_int_distribution<u32>(0, 15)(a.rng));
        } else if (which == 1) {
          name = "name";
          value = JsonValue(a.actor + "-" + std::to_string(a.made));
        } else if (which == 2) {
          name = "scale";
          value = JsonValue(0.5 + 0.25 * std::uniform_int_distribution<u32>(0, 6)(a.rng));
        } else {
          name = "position";
          value = position_in(t, a.rng);
        }
        c = obj({{"kind", JsonValue("SetProperty")},
                 {"id", JsonValue(hex(id))},
                 {"name", JsonValue(name)},
                 {"value", std::move(value)}});
        break;
      }
      case 2: {
        const usize i = pick(a.rng);
        usize j = pick(a.rng);
        if (j == i) j = (j + 1) % here.size();
        c = obj({{"kind", JsonValue("SetParent")}, {"id", JsonValue(hex(here[i]))}});
        if (u(a.rng) < 0.8) c.set("parent", JsonValue(hex(here[j])));
        break;
      }
      default: {
        // An agent reads before it deletes: an object with children is not deleted out from
        // under them, which validation would refuse at the promotion anyway.
        deleted_at = pick(a.rng);
        if (!document().children(here[deleted_at]).empty()) {
          kind = 1;
          c = obj({{"kind", JsonValue("SetProperty")},
                   {"id", JsonValue(hex(here[deleted_at]))},
                   {"name", JsonValue("color")},
                   {"value", JsonValue(std::uniform_int_distribution<u32>(0, 15)(a.rng))}});
          deleted_at = here.size();
          break;
        }
        c = obj({{"kind", JsonValue("DeleteObject")}, {"id", JsonValue(hex(here[deleted_at]))}});
        break;
      }
    }
    JsonValue commands = JsonValue::array();
    commands.push_back(std::move(c));
    const i64 t0 = time::monotonic_ns();
    const JsonValue resp =
        call("doc.apply",
             obj({{"commands", commands}, {"attribution", who(a)}, {"layer", JsonValue(a.layer)}}));
    result_.apply_ms.push_back(static_cast<f64>(time::monotonic_ns() - t0) / 1.0e6);
    if (const i32 code = code_of(resp); code != 0) {
      if (code == codes::k_lease_conflict) {
        ++result_.edit_conflicts;
      } else if (code == codes::k_lease_required) {
        ++result_.edit_uncovered;
      } else {
        FAIL("doc.apply: " << write_json(resp));
      }
      return;
    }
    const JsonValue& r = *resp.find("result");
    if (*r.find("committed") != JsonValue(true)) {
      ++result_.refused_apply;
      const JsonValue& d = *r.find("diagnostics");
      const std::string_view message = d.size() > 0 ? d[0].find("message")->as_string() : "";
      if (message.find("cycle") != std::string_view::npos) {
        ++result_.cycle;
      } else {
        ++result_.stale;
      }
      return;
    }
    ++result_.applied;
    if (kind == 0) here.push_back(created);
    if (kind == 3 && deleted_at < here.size())
      here.erase(here.begin() + static_cast<i64>(deleted_at));
  }

  // ---- promotion
  // ---------------------------------------------------------------------------------

  JsonValue promote_call(const Agent& a, const char* prefer) {
    const i64 t0 = time::monotonic_ns();
    JsonValue r = ok("doc.promote", obj({{"proposal", JsonValue(a.layer)},
                                         {"prefer", JsonValue(prefer)},
                                         {"attribution", director()}}));
    result_.promote_ms.push_back(static_cast<f64>(time::monotonic_ns() - t0) / 1.0e6);
    result_.merge_ms.push_back(number(r.find("merge_ms")));
    result_.validate_ms.push_back(number(r.find("validate_ms")));
    result_.commit_ms.push_back(number(r.find("commit_ms")));
    return r;
  }

  const doc::Document& document() { return sessions_.find(session_)->document(); }

  // A person settling a refused promotion: every record a new validation problem names comes out
  // of the proposal, or the proposal's opinion about that record's parent does when the record
  // itself is the target's. What comes out is counted as lost.
  u32 take_out(Agent& a, const JsonValue& validation) {
    const doc::Document& d = document();
    const i32 p = d.find_layer(a.layer);
    REQUIRE(p >= 0);
    const doc::Layer& proposal = d.layer(static_cast<u32>(p));
    std::vector<Id128> out;
    for (usize i = 0; i < validation.size(); ++i) {
      const std::string_view path = validation[i].find("path")->as_string();
      const usize slash = path.find('/');
      Id128 id;
      if (slash == std::string_view::npos ||
          !Id128::from_hex(path.substr(slash + 1, Id128::k_hex_length), id))
        continue;
      Id128 culprit = id;
      if (proposal.find(id) == nullptr) culprit = d.parent_of(id);
      if (proposal.find(culprit) == nullptr) {
        // The target's record points at something the proposal deleted: find the tombstone.
        for (auto [pid, record] : proposal.records()) {
          if (record.deleted && pid == d.parent_of(id)) culprit = pid;
        }
      }
      if (proposal.find(culprit) != nullptr &&
          std::find(out.begin(), out.end(), culprit) == out.end())
        out.push_back(culprit);
    }
    if (out.empty()) return 0;
    JsonValue commands = JsonValue::array();
    for (const Id128 id : out)
      commands.push_back(obj({{"kind", JsonValue("RemoveRecord")}, {"id", JsonValue(hex(id))}}));
    // The person lifts the lease rule for the fix: the records may stand outside the agent's lease.
    const bool leases = config_.mode != Mode::none;
    if (leases)
      ok("lease.require", obj({{"required", JsonValue(false)}, {"attribution", director()}}));
    ok("doc.apply",
       obj({{"commands", commands}, {"attribution", who(a)}, {"layer", JsonValue(a.layer)}}));
    if (leases)
      ok("lease.require", obj({{"required", JsonValue(true)}, {"attribution", director()}}));
    return static_cast<u32>(out.size());
  }

  void promote(Agent& a) {
    ++result_.promotions;
    JsonValue r = promote_call(a, "Refuse");
    bool promoted = *r.find("promoted") == JsonValue(true);
    if (!promoted) {
      ++result_.needed_human;
      const JsonValue& conflicts = *r.find("conflicts");
      for (usize i = 0; i < conflicts.size(); ++i) {
        const std::string_view kind = conflicts[i].find("kind")->as_string();
        const u32 k = kind == "PropertyBothChanged"    ? 0
                      : kind == "DeletedAndModified"   ? 1
                      : kind == "CreatedBothDifferent" ? 2
                                                       : 3;
        ++result_.conflicts[k];
      }
      result_.lost += static_cast<u32>(conflicts.size());
      if (r.find("validation")->size() > 0) ++result_.validation_refusals;
      for (u32 attempt = 0; attempt < 4 && !promoted; ++attempt) {
        const JsonValue& validation = *r.find("validation");
        result_.validation_problems += static_cast<u32>(validation.size());
        result_.lost += take_out(a, validation);
        r = promote_call(a, "Target");
        promoted = *r.find("promoted") == JsonValue(true);
      }
      if (!promoted) {
        ++result_.rejected;
        result_.lost += static_cast<u32>(
            document().layer(static_cast<u32>(document().find_layer(a.layer))).size());
        ok("doc.reject", obj({{"proposal", JsonValue(a.layer)}, {"attribution", director()}}));
      }
    }
    release_all(a);  // the edits are merged: nothing is left for the leases to protect
    a.open = false;
    ++a.cycles_done;
  }

  Config config_;
  std::string dir_;
  io::Vfs vfs_;
  SessionManager sessions_{vfs_};
  Dispatcher dispatcher_{Context{&sessions_, nullptr, nullptr, nullptr, nullptr}};
  std::string session_;
  std::vector<Agent> agents_;
  Result result_;
};

Result simulate(const Config& config) {
  test::TempDir tmp("e12");
  Simulation s(config, tmp.file("world"));
  return s.run();
}

// Everything the page's tables are made of, one row per configuration and seed.
JsonValue row(const Config& c, const Result& r) {
  JsonValue o = obj({{"agents", JsonValue(c.agents)},
                     {"edits_per_agent", JsonValue(c.edits)},
                     {"cycles", JsonValue(c.cycles)},
                     {"leak", JsonValue(c.leak)},
                     {"mode", JsonValue(mode_name(c.mode))},
                     {"seed", JsonValue(c.seed)},
                     {"attempted", JsonValue(r.attempted)},
                     {"applied", JsonValue(r.applied)},
                     {"refused_apply", JsonValue(r.refused_apply)},
                     {"stale", JsonValue(r.stale)},
                     {"cycle", JsonValue(r.cycle)},
                     {"lease_calls", JsonValue(r.lease_calls)},
                     {"lease_refused", JsonValue(r.lease_refused)},
                     {"dropped", JsonValue(r.dropped)},
                     {"deferred", JsonValue(r.deferred)},
                     {"wait_rounds", JsonValue(r.wait_rounds)},
                     {"wait_max", JsonValue(r.wait_max)},
                     {"home_waits", JsonValue(r.home_waits)},
                     {"edit_conflicts", JsonValue(r.edit_conflicts)},
                     {"edit_uncovered", JsonValue(r.edit_uncovered)},
                     {"promotions", JsonValue(r.promotions)},
                     {"needed_human", JsonValue(r.needed_human)},
                     {"conflicts_property", JsonValue(r.conflicts[0])},
                     {"conflicts_deleted_modified", JsonValue(r.conflicts[1])},
                     {"conflicts_created", JsonValue(r.conflicts[2])},
                     {"conflicts_cycle", JsonValue(r.conflicts[3])},
                     {"validation_refusals", JsonValue(r.validation_refusals)},
                     {"validation_problems", JsonValue(r.validation_problems)},
                     {"lost", JsonValue(r.lost)},
                     {"rejected", JsonValue(r.rejected)},
                     {"rounds", JsonValue(r.rounds)},
                     {"finished", JsonValue(r.finished)},
                     {"world_valid", JsonValue(r.world_valid)},
                     {"wall_ms", JsonValue(r.wall_ms)},
                     {"apply_ms_median", JsonValue(percentile(r.apply_ms, 0.5))},
                     {"promote_ms_median", JsonValue(percentile(r.promote_ms, 0.5))},
                     {"promote_ms_p95", JsonValue(percentile(r.promote_ms, 0.95))},
                     {"merge_ms_median", JsonValue(percentile(r.merge_ms, 0.5))},
                     {"validate_ms_median", JsonValue(percentile(r.validate_ms, 0.5))},
                     {"commit_ms_median", JsonValue(percentile(r.commit_ms, 0.5))}});
  return o;
}

// The invariants every configuration keeps, whatever the numbers.
void check(const Config& c, const Result& r) {
  INFO("agents " << c.agents << " leak " << c.leak << " mode " << mode_name(c.mode) << " seed "
                 << c.seed);
  CHECK(r.finished);
  CHECK(r.world_valid);
  // Every edit set out on was applied, refused at apply, dropped, or kept and then applied or
  // refused: nothing disappears unaccounted.
  CHECK(r.attempted ==
        r.applied + r.refused_apply + r.dropped + r.edit_conflicts + r.edit_uncovered);
  CHECK(r.edit_uncovered == 0);  // an agent leases before it edits
  CHECK(r.edit_conflicts == 0);  // tile leases only: nothing for a type lease to meet
  if (c.mode != Mode::none) {
    // The claim E12 tests: with tile leases, nothing is left to conflict at merge.
    CHECK(r.conflicts_total() == 0);
    CHECK(r.stale == 0);
  } else {
    CHECK(r.lease_calls == 0);
  }
  // Kept to their own tiles, agents never need a person: nothing to conflict with, and an agent
  // that reads before it deletes leaves no child without its parent.
  if (c.leak == 0) CHECK(r.needed_human == 0);
}

}  // namespace

TEST_CASE("E12: leases leave nothing to conflict at merge (small run)") {
  // Small enough for every CTest run in a debug build — one prop a tile, six edits an agent, half
  // of them next door — because every commit is a save, and a save is most of an edit's time
  // (the page has the numbers).
  for (const Mode mode : {Mode::none, Mode::give_up, Mode::retry}) {
    Config c;
    c.agents = 4;
    c.edits = 6;
    c.cycles = 2;
    c.leak = 0.5;
    c.mode = mode;
    c.seed = 3;
    c.per_tile = 1;
    const Result r = simulate(c);
    check(c, r);
    // One promotion a cycle, and a retrying agent's last one for what it owed.
    CHECK(r.promotions >= c.agents * c.cycles);
    if (mode == Mode::give_up) CHECK(r.dropped == r.lease_refused);
    if (mode != Mode::none) CHECK(r.lease_refused > 0);  // the leak met a neighbour's lease
  }
}

TEST_CASE("E12: the matrix" * doctest::skip()) {
  const std::string out_path = test::detail::environment("ENGINE_E12_OUT");
  std::ofstream out;
  if (!out_path.empty()) out.open(out_path, std::ios::binary | std::ios::app);
  const u32 seeds = [] {
    const std::string s = test::detail::environment("ENGINE_E12_SEEDS");
    return s.empty() ? 3u : static_cast<u32>(std::strtoul(s.c_str(), nullptr, 10));
  }();

  const bench::MachineState before = bench::sample_machine_state(bench::k_sample_window_ms);
  std::printf("# E12: %u seeds a configuration, %d x %d tiles of %.0f m, %u props a tile\n", seeds,
              k_grid, k_grid, k_tile, k_per_tile);
  std::printf("# machine before: %s\n", bench::describe(before).c_str());
  std::printf(
      "| agents | leak | mode | attempted | applied | refused at apply | lease refusals "
      "| dropped | deferred | wait mean/max (rounds) | merge conflicts (prop/del-mod) "
      "| validation refusals | needed a human | lost | wall s | promote ms (median) |\n");
  bench::MachineState worst = before;
  for (const u32 agents : {4u, 8u, 16u}) {
    for (const f64 leak : {0.0, 0.1, 0.3}) {
      for (const Mode mode : {Mode::none, Mode::give_up, Mode::retry}) {
        Result sum;
        std::vector<f64> promote_ms;
        for (u32 s = 1; s <= seeds; ++s) {
          Config c;
          c.agents = agents;
          c.edits = 40;
          c.cycles = 2;
          c.leak = leak;
          c.mode = mode;
          c.seed = s;
          const Result r = simulate(c);
          check(c, r);
          if (out.is_open()) {
            JsonValue line = row(c, r);
            out << write_json(line, JsonWriteOptions{.pretty = false}) << '\n';
          }
          sum.attempted += r.attempted;
          sum.applied += r.applied;
          sum.refused_apply += r.refused_apply;
          sum.lease_refused += r.lease_refused;
          sum.dropped += r.dropped;
          sum.deferred += r.deferred;
          sum.wait_rounds += r.wait_rounds;
          sum.waited += r.waited;
          sum.wait_max = std::max(sum.wait_max, r.wait_max);
          for (u32 k = 0; k < 4; ++k)
            sum.conflicts[k] += r.conflicts[k];
          sum.validation_refusals += r.validation_refusals;
          sum.needed_human += r.needed_human;
          sum.promotions += r.promotions;
          sum.lost += r.lost;
          sum.wall_ms += r.wall_ms;
          promote_ms.insert(promote_ms.end(), r.promote_ms.begin(), r.promote_ms.end());
        }
        const f64 wait_mean =
            sum.waited > 0 ? static_cast<f64>(sum.wait_rounds) / static_cast<f64>(sum.waited) : 0.0;
        std::printf(
            "| %u | %.0f%% | %s | %u | %u | %u | %u | %u | %u | %.1f / %u | %u (%u/%u) | %u "
            "| %u of %u | %u | %.1f | %.1f |\n",
            agents, leak * 100, mode_name(mode), sum.attempted, sum.applied, sum.refused_apply,
            sum.lease_refused, sum.dropped, sum.deferred, wait_mean, sum.wait_max,
            sum.conflicts_total(), sum.conflicts[0], sum.conflicts[1], sum.validation_refusals,
            sum.needed_human, sum.promotions, sum.lost, sum.wall_ms / 1000.0,
            percentile(promote_ms, 0.5));
        std::fflush(stdout);
        worst = bench::worst_of(worst, bench::sample_machine_state(0));
      }
    }
  }
  const bench::MachineState after = bench::sample_machine_state(bench::k_sample_window_ms);
  worst = bench::worst_of(worst, after);
  std::printf("# machine after: %s\n", bench::describe(after).c_str());
  (void)bench::warn_if_busy(worst, bench::QuietThresholds{}, stdout);
  if (out.is_open()) {
    JsonValue m = obj({{"machine_state_before", bench::machine_state_json(before)},
                       {"machine_state_after", bench::machine_state_json(after)}});
    out << write_json(m, JsonWriteOptions{.pretty = false}) << '\n';
  }
}
