// Plan 06 §6.8's task ledger and review items (docs/subsystems/protocol.md, "The task ledger and
// review items"; ADR-0072): `task.create`, `task.claim`, `task.update`, `task.list`,
// `review.raise`, `review.close` and `review.list`.
//
// The ledger is `ledger.json` beside the document, and no host keeps it in memory: every call
// takes `ledger.json.lock` between processes, reads the file, changes it and writes it back
// (`update_ledger`, over side_file.h), so two engine-cli calls at once, or engine-host beside
// engine-cli, lose nothing to each other and never mint one id twice.
#include "subject_digest.h"

#include <core/hash/hash.h>
#include <core/json/json.h>
#include <core/schema/json_reflect.h>
#include <core/time/time.h>
#include <domain/doc/document_store.h>
#include <domain/protocol/paths.h>
#include <domain/protocol/policy.h>
#include <domain/protocol/rpc.h>
#include <domain/protocol/session.h>
#include <domain/protocol/side_file.h>

#include <algorithm>
#include <mutex>
#include <string>
#include <string_view>

namespace engine::protocol {

namespace {

constexpr std::string_view k_ledger_file = "ledger.json";
constexpr u32 k_history_max = 256;
constexpr usize k_title_max = 200;
constexpr usize k_text_max = 4096;
constexpr usize k_criterion_max = 1024;
constexpr u32 k_criteria_max = 32;
constexpr usize k_kind_max = 64;
constexpr usize k_summary_max = 1024;
constexpr u32 k_subject_records_max = 1024;
constexpr u32 k_link_max = 256;
constexpr usize k_name_max = 200;
constexpr usize k_report_max = 64 * 1024;
constexpr usize k_asset_path_max = 1024;
constexpr f64 k_claim_ttl_max = 7.0 * 24.0 * 3600.0;
// The ledger's bounds (protocol.md, "The ledger's bounds", where each number's reason is): every
// call reads, parses and rewrites the whole file under a lock other callers wait 10 s for, so a
// ledger that grows without limit makes every call slow and a person's close one of the waits that
// runs out. Each refusal is 1004 naming the bound.
constexpr u32 k_open_tasks_per_actor = 256;
constexpr u32 k_open_items_per_actor = 256;
constexpr u64 k_ledger_bytes_max = 8u << 20;

bool blank(std::string_view s) noexcept { return s.find_first_not_of(" \t") == s.npos; }

bool has_control(std::string_view s, bool allow_newlines) noexcept {
  for (const char c : s) {
    const auto u = static_cast<unsigned char>(c);
    if (allow_newlines && (c == '\n' || c == '\t')) continue;
    if (u < 0x20 || u == 0x7f) return true;
  }
  return false;
}

// Text a client wrote, checked before it is stored: a length and no control characters (a note
// or a goal may break lines). The field's name is in the answer.
bool text_ok(std::string_view field, std::string_view text, usize min, usize max,
             bool allow_newlines, RpcError& error) {
  if (text.size() < min || text.size() > max || has_control(text, allow_newlines)) {
    error = make_error(codes::k_invalid_argument,
                       std::string(field) + " must be " + std::to_string(min) + " to " +
                           std::to_string(max) + " bytes with no control characters" +
                           (allow_newlines ? " but line breaks" : ""));
    return false;
  }
  return true;
}

const char* status_name(TaskStatus s) noexcept {
  switch (s) {
    case TaskStatus::Open: return "Open";
    case TaskStatus::Claimed: return "Claimed";
    case TaskStatus::Blocked: return "Blocked";
    case TaskStatus::Done: return "Done";
    case TaskStatus::Dropped: return "Dropped";
  }
  return "?";
}

const char* resolution_name(ReviewResolution r) noexcept {
  switch (r) {
    case ReviewResolution::None: return "None";
    case ReviewResolution::Accepted: return "Accepted";
    case ReviewResolution::Rejected: return "Rejected";
    case ReviewResolution::Withdrawn: return "Withdrawn";
  }
  return "?";
}

bool held(TaskStatus s) noexcept { return s == TaskStatus::Claimed || s == TaskStatus::Blocked; }

std::string role_of(const Caller& caller) {
  return caller.role != nullptr ? caller.role->name : caller.who.role;
}

void note(Vector<WorkNote>& history, u32& dropped, i64 now_ms, const Caller& caller,
          std::string_view change, std::string_view text) {
  WorkNote n;
  n.at_unix_ms = now_ms;
  n.actor = caller.who.actor;
  n.role = role_of(caller);
  n.change = std::string(change);
  n.text = std::string(text);
  n.via = caller.via;
  n.human_via = caller.human_via;
  history.push_back(std::move(n));
  while (history.size() > k_history_max) {
    history.erase_at(0);
    ++dropped;
  }
}

// Claims whose time ran out are let go: the task is open again, and its history says whose claim
// lapsed. The number let go.
u32 lapse_claims(LedgerFile& ledger, i64 now_ms) {
  u32 lapsed = 0;
  for (TaskInfo& t : ledger.tasks) {
    if (!held(t.status) || t.claim_expires_unix_ms > now_ms) continue;
    WorkNote n;
    n.at_unix_ms = now_ms;
    n.actor = t.claimant;
    n.role = t.claimant_role;
    n.change = "lapsed";
    n.via = "host";  // the host's own clock let it go, not a caller
    t.history.push_back(std::move(n));
    while (t.history.size() > k_history_max) {
      t.history.erase_at(0);
      ++t.history_dropped;
    }
    t.status = TaskStatus::Open;
    t.claimant.clear();
    t.claimant_role.clear();
    t.claimant_via.clear();
    t.claimed_unix_ms = 0;
    t.claim_expires_unix_ms = 0;
    t.claim_ttl_seconds = 0;
    t.updated_unix_ms = now_ms;
    ++lapsed;
  }
  return lapsed;
}

template <class T>
bool contains(const Vector<T>& list, const T& value) {
  for (const T& v : list) {
    if (v == value) return true;
  }
  return false;
}

template <class T>
bool add_links(const char* field, Vector<T>& into, const Vector<T>& added, RpcError& error) {
  for (const T& v : added) {
    if (!contains(into, v)) into.push_back(v);
  }
  if (into.size() > k_link_max) {
    error = make_error(codes::k_invalid_argument, std::string("links.") + field +
                                                      " would hold more than " +
                                                      std::to_string(k_link_max) + " entries");
    return false;
  }
  return true;
}

bool names_ok(const char* field, const Vector<std::string>& names, RpcError& error) {
  for (const std::string& n : names) {
    if (!text_ok(std::string("links.") + field + " entries", n, 1, k_name_max, false, error))
      return false;
  }
  return true;
}

// The links a call adds, merged into what a task already has. A link is a name (a layer, a
// checkpoint) or an id, never a path: a ledger outlives the machine it was written on.
bool merge_links(WorkLinks& into, const WorkLinks& added, RpcError& error) {
  return names_ok("proposals", added.proposals, error) &&
         names_ok("checkpoints", added.checkpoints, error) &&
         add_links("proposals", into.proposals, added.proposals, error) &&
         add_links("leases", into.leases, added.leases, error) &&
         add_links("transactions", into.transactions, added.transactions, error) &&
         add_links("checkpoints", into.checkpoints, added.checkpoints, error) &&
         add_links("records", into.records, added.records, error);
}

bool links_empty(const WorkLinks& l) noexcept {
  return l.proposals.empty() && l.leases.empty() && l.transactions.empty() &&
         l.checkpoints.empty() && l.records.empty();
}

TaskInfo* find_task(LedgerFile& ledger, u64 id, RpcError& error) {
  for (TaskInfo& t : ledger.tasks) {
    if (t.id == id) return &t;
  }
  error = make_error(codes::k_not_found, "no task " + std::to_string(id) +
                                             " in this document's ledger: task.list "
                                             "lists them");
  return nullptr;
}

ReviewInfo* find_review(LedgerFile& ledger, u64 id, RpcError& error) {
  for (ReviewInfo& r : ledger.reviews) {
    if (r.id == id) return &r;
  }
  error = make_error(codes::k_not_found, "no review item " + std::to_string(id) +
                                             " in this document's ledger: review.list lists them");
  return nullptr;
}

// Ids are drawn, never counted (side_file.h): a ledger restored from a copy would count again from
// where the copy stood, and its next id would name a task or item someone already knew by that
// number as a different one. Drawn against the ledger as it is under the lock.
u64 fresh_task_id(const LedgerFile& ledger) {
  return fresh_side_file_id(
      [](const void* l, u64 id) {
        for (const TaskInfo& t : static_cast<const LedgerFile*>(l)->tasks) {
          if (t.id == id) return true;
        }
        return false;
      },
      &ledger);
}

u64 fresh_review_id(const LedgerFile& ledger) {
  return fresh_side_file_id(
      [](const void* l, u64 id) {
        for (const ReviewInfo& r : static_cast<const LedgerFile*>(l)->reviews) {
          if (r.id == id) return true;
        }
        return false;
      },
      &ledger);
}

RpcError bound_error(std::string_view bound, u64 limit, std::string message) {
  JsonValue data = JsonValue::object();
  data.set("reason", JsonValue("ledger bound"));
  data.set("bound", JsonValue(bound));
  data.set("limit", JsonValue(limit));
  return make_error(codes::k_invalid_argument, std::move(message), std::move(data));
}

// ---- the store --------------------------------------------------------------------------------

// Whether a call that grows the ledger may: false with the bound's refusal once the file holds
// `k_ledger_bytes_max`. Looked at under the ledger's lock by `update_ledger`, and before it by a
// raise, so a raise the bound refuses reads none of its subject's files.
bool ledger_has_room(io::Vfs& vfs, const std::string& path, RpcError& error) {
  io::FileInfo info;
  if (vfs.stat(path, info) != io::Status::Ok || info.size < k_ledger_bytes_max) return true;
  error = bound_error("ledger bytes", k_ledger_bytes_max,
                      "the ledger holds " + std::to_string(info.size) +
                          " bytes, past its bound of 8 MiB: close, finish or drop work before "
                          "adding more (a claim, a status change and a close still answer)");
  return false;
}

// The ledger's one read-modify-write: `change(ledger, write, error)` runs on the file as it is on
// disk now (absent: empty) while this process holds `ledger.json.lock`, and when it sets `write`
// the result replaces the file before the lock is let go. False with `error` set when the lock,
// the read, the change or the write fails; nothing is written then.
// Over side_file.h's `update_side_file`, the one helper for every file beside a document (the
// transactions and the leases go through it too): of the two this tree had, it is the one that
// guarantees more — it reads a file of an older version through the registered migrations, writes
// nothing when the bytes are the ones there, and waits with a deadline that grows its pauses — and
// a read of a document that has no ledger takes no lock and makes no lock file, so `task.list` on
// a document nobody gave tasks leaves it as it was. `change` runs on the file as it is on disk now
// (absent: empty), and its `write` is honoured whether or not it succeeded, since a refusal may
// still have let lapsed claims go.
// `grows`: the call adds a task, an item, a note or links, and is refused once the file holds
// `k_ledger_bytes_max` (looked at under the lock, so no two writers both pass it); a claim, a
// status change and a close still answer, so a full ledger can be worked down.
// `revision` is the ledger's revision the call read, or wrote: every write moves it by one, so it
// moves only when the content does (a call that changes nothing sets no write).
template <class F>
bool update_ledger(io::Vfs& vfs, Session& session, bool read_only, bool grows, u64& revision,
                   F&& change, RpcError& error) {
  revision = 0;
  const std::string path = doc::DocumentStore::path_of(session.dir(), k_ledger_file);
  if (read_only && !vfs.exists(path)) {
    LedgerFile none;
    bool write = false;
    return change(none, write, error);
  }
  // A read goes on to take the lock too, since it may let claims lapse and write: the session's
  // one gate on writes beside its document (session.h) decides both. A read on a session the gate
  // refuses (one a client opened from a read-only root) is a plain read without the lock, which
  // makes no lock file there, and what lapses lapses in its answer only.
  if (read_only && !session.writable()) {
    LedgerFile ledger;
    bool present = false;
    if (!read_side_file(vfs, path, ledger, present, error)) return false;
    revision = ledger.revision;
    bool write = false;
    return change(ledger, write, error);
  }
  if (!session.may_write_side_files(error)) return false;
  LedgerFile ledger;
  bool wrote = false;  // the file holds `ledger` because this call wrote it, not only asked to
  const bool ok = update_side_file(
      vfs, path, ledger,
      [&](LedgerFile& l, bool& write) {
        if (grows && !ledger_has_room(vfs, path, error)) {
          revision = l.revision;
          return false;
        }
        const bool changed = change(l, write, error);
        if (write) ++l.revision;
        revision = l.revision;
        return changed;
      },
      error, &wrote);
  // What this process wrote is what its own watch has seen once the call's event goes out
  // (`Session::flush_ledger_written`), so the notice its write raises publishes nothing more, and
  // what else the write carried — another process's writes since, lapsed claims — is told first.
  // Only a write that reached the file counts: one that failed wrote nothing anyone can read, so
  // it is told as nothing (A55). A read that writes nothing serializes nothing here.
  if (wrote && session.watching_ledger()) session.ledger_written(ledger);
  return ok;
}

// ---- the ledger's events (protocol.md, "The ledger's events") ---------------------------------

u64 text_hash(const std::string& text) noexcept { return hash_bytes(text.data(), text.size()); }

// The ledger's bytes as the side-file helper writes them (side_file.cpp, `write_side_json`).
std::string canonical(const LedgerFile& ledger) {
  std::string text = write_json(schema::to_json(ledger));
  text.push_back('\n');
  return text;
}

template <class T>
void entries_of(const Vector<T>& all, Vector<LedgerSeen::Entry>& out) {
  out.clear();
  for (const T& e : all)
    out.push_back(LedgerSeen::Entry{e.id, text_hash(write_json(schema::to_json(e)))});
  std::sort(out.begin(), out.end(),
            [](const LedgerSeen::Entry& a, const LedgerSeen::Entry& b) { return a.id < b.id; });
}

void fill_seen(const LedgerFile& ledger, const std::string& text, LedgerSeen& seen) {
  seen.present = true;
  seen.revision = ledger.revision;
  seen.size = text.size();
  seen.hash = text_hash(text);
  entries_of(ledger.tasks, seen.tasks);
  entries_of(ledger.reviews, seen.reviews);
}

// The ids whose entries differ between two lists sorted by id — made, changed or gone — but
// `own` (the call's own, which its event names; 0 for none), the first `k_event_max_ids` of them,
// and how many in all.
void changed_ids(const Vector<LedgerSeen::Entry>& from, const Vector<LedgerSeen::Entry>& to,
                 u64 own, Vector<u64>& ids, u32& total) {
  const auto add = [&](u64 id) {
    if (id == own && own != 0) return;
    ++total;
    if (ids.size() < k_event_max_ids) ids.push_back(id);
  };
  u32 i = 0;
  u32 j = 0;
  while (i < from.size() || j < to.size()) {
    if (j == to.size() || (i < from.size() && from[i].id < to[j].id)) {
      add(from[i].id);  // gone
      ++i;
    } else if (i == from.size() || to[j].id < from[i].id) {
      add(to[j].id);  // made
      ++j;
    } else {
      if (from[i].hash != to[j].hash) add(to[j].id);  // changed
      ++i;
      ++j;
    }
  }
}

// The order a list answers in (protocol.md, "The ledger's lists"): by last change, then by id, so
// two reads of one ledger agree; `Newest` is that reversed.
i64 changed_ms(const TaskInfo& t) noexcept {
  return t.updated_unix_ms > t.created_unix_ms ? t.updated_unix_ms : t.created_unix_ms;
}

i64 changed_ms(const ReviewInfo& r) noexcept {
  i64 at = r.raised_unix_ms > r.closed_unix_ms ? r.raised_unix_ms : r.closed_unix_ms;
  if (!r.history.empty() && r.history.back().at_unix_ms > at) at = r.history.back().at_unix_ms;
  return at;
}

// A list's `limit` past this is refused rather than clamped: a clamp answers fewer than were asked
// for with only `more` to say so, and a caller that wants every match says 0 (the ledger's own
// 8 MiB bound bounds that answer). The same number bounds the ids a list may name.
constexpr u32 k_list_max = 1024;

bool list_bounds_ok(u32 limit, usize ids, const char* field, RpcError& error) {
  if (limit <= k_list_max && ids <= k_list_max) return true;
  JsonValue data = JsonValue::object();
  data.set("reason", JsonValue("list limit"));
  data.set("limit", JsonValue(k_list_max));
  error = make_error(codes::k_invalid_argument,
                     std::string("limit is 0 (every match) or 1 to 1024, and ") + field +
                         " names at most 1024 ids",
                     std::move(data));
  return false;
}

// The matches `keep` chose, in `order`, the first `limit` of them (0: all) copied into `out` and
// the rest counted in `more`. Sorted by key and index, so nothing is copied that is not answered.
template <class T, class Keep>
void list_in_order(const Vector<T>& all, LedgerOrder order, u32 limit, bool brief, Keep&& keep,
                   Vector<T>& out, u32& more) {
  struct Key {
    i64 at = 0;
    u64 id = 0;
    u32 index = 0;
  };
  Vector<Key> keys;
  for (u32 i = 0; i < all.size(); ++i) {
    if (keep(all[i])) keys.push_back(Key{changed_ms(all[i]), all[i].id, i});
  }
  const bool newest = order == LedgerOrder::Newest;
  std::sort(keys.begin(), keys.end(), [newest](const Key& a, const Key& b) {
    if (a.at != b.at) return newest ? a.at > b.at : a.at < b.at;
    return newest ? a.id > b.id : a.id < b.id;
  });
  const u32 n = limit == 0 || limit > keys.size() ? keys.size() : limit;
  more = keys.size() - n;
  for (u32 i = 0; i < n; ++i) {
    out.push_back(all[keys[i].index]);
    if (brief) out.back().history.clear();
  }
}

// The session and the caller every method starts from.
Session* begin(Context& ctx, const std::string& session, const doc::Attribution* attribution,
               Caller& caller, RpcError& error) {
  Session* s = ctx.sessions->require(session, error);
  if (s == nullptr) return nullptr;
  if (attribution != nullptr && !resolve_caller(ctx, attribution, caller, error)) return nullptr;
  return s;
}

bool require_actor(const Caller& caller, const char* what, RpcError& error) {
  if (!blank(caller.who.actor)) return true;
  error =
      make_error(codes::k_invalid_argument, std::string("attribution.actor is required: ") + what);
  return false;
}

// ---- task.* ------------------------------------------------------------------------------------

bool task_create(Context& ctx, const TaskCreateParams& params, TasksResult& out, RpcError& error) {
  Caller caller;
  Session* s = begin(ctx, params.session, &params.attribution, caller, error);
  if (s == nullptr || !require_actor(caller, "a task records who made it", error)) return false;
  if (!text_ok("title", params.title, 1, k_title_max, false, error) ||
      !text_ok("goal", params.goal, 0, k_text_max, true, error))
    return false;
  if (params.acceptance.size() > k_criteria_max) {
    error = make_error(codes::k_invalid_argument,
                       "acceptance holds at most " + std::to_string(k_criteria_max) + " criteria");
    return false;
  }
  for (const std::string& c : params.acceptance) {
    if (!text_ok("each acceptance criterion", c, 1, k_criterion_max, true, error)) return false;
  }
  if (!params.owner_role.empty()) {
    if (!text_ok("owner_role", params.owner_role, 1, k_name_max, false, error)) return false;
    const Policy* policy = ctx.policy;
    if (policy != nullptr && policy->roles_loaded() &&
        policy->find_role(params.owner_role) == nullptr) {
      error = make_error(codes::k_invalid_argument,
                         "owner_role " + params.owner_role +
                             " is not a role of this host's roles file (engine.roles lists them)");
      return false;
    }
  }
  const i64 now = time::wall_unix_ms();
  TaskInfo made;
  if (!update_ledger(
          ctx.sessions->vfs(), *s, false, true, out.ledger_revision,
          [&](LedgerFile& ledger, bool& write, RpcError& e) {
            out.lapsed = lapse_claims(ledger, now);
            write = out.lapsed > 0;
            u32 open = 0;
            for (const TaskInfo& o : ledger.tasks) {
              if (o.created_by.actor == caller.who.actor && o.status != TaskStatus::Done &&
                  o.status != TaskStatus::Dropped)
                ++open;
            }
            if (open >= k_open_tasks_per_actor) {
              e = bound_error("open tasks per actor", k_open_tasks_per_actor,
                              caller.who.actor +
                                  " has made 256 tasks that are not Done or Dropped: "
                                  "finish or drop some before making more");
              return false;
            }
            TaskInfo t;
            t.id = fresh_task_id(ledger);
            t.key = "task/" + std::to_string(t.id);
            t.title = params.title;
            t.goal = params.goal;
            t.acceptance = params.acceptance;
            t.owner_role = params.owner_role;
            t.created_by = caller.who;
            t.created_by.timestamp_unix_ms = now;
            t.created_via = caller.via;
            t.created_unix_ms = now;
            t.updated_unix_ms = now;
            if (!merge_links(t.links, params.links, e)) return false;
            note(t.history, t.history_dropped, now, caller, "created", {});
            made = t;
            ledger.tasks.push_back(std::move(t));
            write = true;
            return true;
          },
          error))
    return false;
  out.now_unix_ms = now;
  out.tasks.push_back(std::move(made));
  s->publish_ledger("task_created", out.tasks[0].id, 0, caller.who.actor, caller.connection);
  return true;
}

bool task_claim(Context& ctx, const TaskClaimParams& params, TasksResult& out, RpcError& error) {
  Caller caller;
  Session* s = begin(ctx, params.session, &params.attribution, caller, error);
  if (s == nullptr || !require_actor(caller, "a claim is held by an actor", error)) return false;
  if (!(params.ttl_seconds >= 1.0 && params.ttl_seconds <= k_claim_ttl_max)) {
    error = make_error(codes::k_invalid_argument,
                       "ttl_seconds must be from 1 to 604800 (a claim nobody renews lapses after "
                       "it, and the task is open again)");
    return false;
  }
  const i64 now = time::wall_unix_ms();
  const std::string role = role_of(caller);
  bool renewed = false;
  TaskInfo changed;
  if (!update_ledger(
          ctx.sessions->vfs(), *s, false, false, out.ledger_revision,
          [&](LedgerFile& ledger, bool& write, RpcError& e) {
            out.lapsed = lapse_claims(ledger, now);
            write = out.lapsed > 0;
            TaskInfo* t = find_task(ledger, params.task, e);
            if (t == nullptr) return false;
            if (t->status == TaskStatus::Done || t->status == TaskStatus::Dropped) {
              e = make_error(codes::k_invalid_argument,
                             "task " + std::to_string(t->id) + " is " + status_name(t->status) +
                                 ": its creator reopens it (task.update with status Open) "
                                 "before anyone claims it");
              return false;
            }
            if (held(t->status) && t->claimant != caller.who.actor) {
              JsonValue data = JsonValue::object();
              data.set("reason", JsonValue("task claimed"));
              data.set("task", JsonValue(t->id));
              data.set("claimant", JsonValue(t->claimant));
              data.set("claim_expires_unix_ms", JsonValue(t->claim_expires_unix_ms));
              e = make_error(codes::k_lease_conflict,
                             "task " + std::to_string(t->id) + " is claimed by " + t->claimant +
                                 " until its claim lapses or is let go",
                             std::move(data));
              return false;
            }
            // A task for a role marked human is a person's: claimed by the role `human_via`
            // confers, never by a working role that bears the same name.
            const bool for_person =
                ctx.policy != nullptr && ctx.policy->is_human_role(t->owner_role);
            if (for_person && (caller.human_via.empty() || caller.human_role != t->owner_role)) {
              e = forbidden("task " + std::to_string(t->id) + " is for the " + t->owner_role +
                                " role, which is a person's: it is claimed on a connection that "
                                "presented a credential issued for that role",
                            "human review", role);
              return false;
            }
            if (!for_person && !t->owner_role.empty() && t->owner_role != role) {
              e = forbidden("task " + std::to_string(t->id) + " is for the " + t->owner_role +
                                " role, and this call runs as " +
                                (role.empty() ? std::string("no role") : role),
                            "task role", role);
              return false;
            }
            renewed = held(t->status);
            if (!renewed) {
              t->status = TaskStatus::Claimed;
              t->claimant = caller.who.actor;
              t->claimant_role = role;
              t->claimant_via = caller.via;
              t->claimed_unix_ms = now;
            }
            t->claim_ttl_seconds = params.ttl_seconds;
            t->claim_expires_unix_ms = now + static_cast<i64>(params.ttl_seconds * 1000.0);
            t->updated_unix_ms = now;
            note(t->history, t->history_dropped, now, caller, renewed ? "renewed" : "claimed", {});
            changed = *t;
            write = true;
            return true;
          },
          error))
    return false;
  out.now_unix_ms = now;
  out.tasks.push_back(std::move(changed));
  s->publish_ledger(renewed ? "task_updated" : "task_claimed", params.task, 0, caller.who.actor,
                    caller.connection);
  return true;
}

bool task_update(Context& ctx, const TaskUpdateParams& params, TasksResult& out, RpcError& error) {
  Caller caller;
  Session* s = begin(ctx, params.session, &params.attribution, caller, error);
  if (s == nullptr || !require_actor(caller, "an update records who made it", error)) return false;
  if (!text_ok("note", params.note, 0, k_text_max, true, error)) return false;
  const i64 now = time::wall_unix_ms();
  TaskInfo changed;
  if (!update_ledger(
          ctx.sessions->vfs(), *s, false, !blank(params.note) || !links_empty(params.links),
          out.ledger_revision,
          [&](LedgerFile& ledger, bool& write, RpcError& e) {
            out.lapsed = lapse_claims(ledger, now);
            write = out.lapsed > 0;
            TaskInfo* t = find_task(ledger, params.task, e);
            if (t == nullptr) return false;
            const bool claimant = held(t->status) && t->claimant == caller.who.actor;
            const bool creator = t->created_by.actor == caller.who.actor;
            const std::string id = std::to_string(t->id);
            if (!claimant && !creator) {
              e = forbidden("task " + id + " is updated by its claimant" +
                                (held(t->status) ? " (" + t->claimant + ")" : std::string()) +
                                " or its creator (" + t->created_by.actor + ")",
                            "task owner", role_of(caller));
              return false;
            }
            if (!params.status && blank(params.note) && links_empty(params.links) && !claimant) {
              e = make_error(codes::k_invalid_argument,
                             "nothing to update: give a status, a note or links");
              return false;
            }
            if (params.status) {
              const TaskStatus to = *params.status;
              const TaskStatus from = t->status;
              bool allowed = false;
              switch (to) {
                case TaskStatus::Claimed:
                case TaskStatus::Blocked:
                case TaskStatus::Done: allowed = claimant; break;
                case TaskStatus::Dropped:
                  allowed = from != TaskStatus::Done && from != TaskStatus::Dropped;
                  break;
                case TaskStatus::Open:
                  allowed = claimant ||
                            (creator && (from == TaskStatus::Done || from == TaskStatus::Dropped));
                  break;
              }
              if (!allowed) {
                e = make_error(codes::k_invalid_argument,
                               std::string("task ") + id + " cannot go from " + status_name(from) +
                                   " to " + status_name(to) +
                                   " in this call: the claimant blocks, unblocks, finishes, drops "
                                   "or lets go of it; the creator drops it or reopens a Done or "
                                   "Dropped one");
                return false;
              }
              if (to != from) {
                t->status = to;
                if (!held(to)) {
                  // Open: nobody holds it. Done and Dropped keep who held it last.
                  if (to == TaskStatus::Open) {
                    t->claimant.clear();
                    t->claimant_role.clear();
                    t->claimant_via.clear();
                    t->claimed_unix_ms = 0;
                  }
                  t->claim_expires_unix_ms = 0;
                  t->claim_ttl_seconds = 0;
                }
                note(t->history, t->history_dropped, now, caller,
                     to == TaskStatus::Open && claimant ? "released" : "status", status_name(to));
              }
            }
            if (held(t->status) && claimant && t->claim_ttl_seconds > 0) {
              // Any update by the claimant renews the claim it holds.
              t->claim_expires_unix_ms = now + static_cast<i64>(t->claim_ttl_seconds * 1000.0);
            }
            if (!links_empty(params.links)) {
              if (!merge_links(t->links, params.links, e)) return false;
              note(t->history, t->history_dropped, now, caller, "linked", {});
            }
            if (!blank(params.note))
              note(t->history, t->history_dropped, now, caller, "note", params.note);
            t->updated_unix_ms = now;
            changed = *t;
            write = true;
            return true;
          },
          error))
    return false;
  out.now_unix_ms = now;
  out.tasks.push_back(std::move(changed));
  s->publish_ledger("task_updated", params.task, 0, caller.who.actor, caller.connection);
  return true;
}

bool task_list(Context& ctx, const TaskListParams& params, TasksResult& out, RpcError& error) {
  Caller caller;
  Session* s = begin(ctx, params.session, nullptr, caller, error);
  if (s == nullptr) return false;
  if (!list_bounds_ok(params.limit, params.tasks.size(), "tasks", error)) return false;
  const i64 now = time::wall_unix_ms();
  out.now_unix_ms = now;
  const bool ok = update_ledger(
      ctx.sessions->vfs(), *s, true, false, out.ledger_revision,
      [&](LedgerFile& ledger, bool& write, RpcError& e) {
        out.lapsed = lapse_claims(ledger, now);
        write = out.lapsed > 0;
        if (params.task != 0 && find_task(ledger, params.task, e) == nullptr) return false;
        list_in_order(
            ledger.tasks, params.order, params.limit, params.brief,
            [&](const TaskInfo& t) {
              if (params.task != 0 && t.id != params.task) return false;
              if (!params.tasks.empty() && !contains(params.tasks, t.id)) return false;
              if (!params.statuses.empty() && !contains(params.statuses, t.status)) return false;
              if (!params.owner_role.empty() && t.owner_role != params.owner_role) return false;
              if (!params.claimant.empty() && (!held(t.status) || t.claimant != params.claimant))
                return false;
              return true;
            },
            out.tasks, out.more);
        return true;
      },
      error);
  // A list that let claims lapse wrote them, and nobody else tells a subscriber of those.
  s->flush_ledger_written(0, 0);
  return ok;
}

// ---- review.* ----------------------------------------------------------------------------------

// The subject digest's passes (protocol.md, "What it costs"). Each pass, under the dispatch lock:
// `first(session, subject, ledger_revision, error)` makes every refusal that reads no file and
// names the subject, and the document's half is composed; then the files are read with the lock let
// go (`without_dispatch_lock`), so every other client of the host, and the editor's frame, is
// answered meanwhile; then, under the lock again, the session is looked up anew and its document
// compared with what was composed, and `decide(session, state, ledger_revision, moved, error)`
// makes the call's decision on the values of that same pass — setting `moved` when the ledger's
// revision is not the one `first` read. A document or a ledger that moved is composed and read
// again, three passes in all, then refused (`subject changing`).
constexpr u32 k_digest_passes = 3;

RpcError over_error(const DigestOver& over) {
  JsonValue data = JsonValue::object();
  data.set("reason", JsonValue("subject too large"));
  data.set("bound", JsonValue(std::string_view(over.bound)));
  data.set("limit", JsonValue(over.limit));
  data.set("figure", JsonValue(over.figure));
  return make_error(codes::k_invalid_argument,
                    std::string("the subject is past ") + over.bound + " (" +
                        std::to_string(over.limit) + "): it comes to " +
                        std::to_string(over.figure) +
                        "; raise one item per part, or ask the host's operator for a larger bound",
                    std::move(data));
}

// A subject naming a glTF whose names were not followed: by its stored spelling, and for the JSON
// bound the bound and the glTF's size.
RpcError names_not_read_error(const SubjectPlan::Unfollowed& u) {
  JsonValue data = JsonValue::object();
  data.set("reason", JsonValue("names not read"));
  JsonValue entries = JsonValue::array();
  entries.push_back(JsonValue(u.gltf));
  data.set("entries", std::move(entries));
  std::string why = " does not parse as the engine's JSON reader reads it";
  if (u.bound != nullptr) {
    data.set("bound", JsonValue(std::string_view(u.bound)));
    data.set("limit", JsonValue(u.limit));
    data.set("figure", JsonValue(u.figure));
    why = std::string(" is past ") + u.bound + " (" + std::to_string(u.limit) + "): it comes to " +
          std::to_string(u.figure);
  }
  return make_error(codes::k_invalid_argument,
                    "the subject names " + u.gltf + ", which" + why +
                        ", so the files it names could not be read: a raise is stamped over "
                        "what its digest saw",
                    std::move(data));
}

// A subject naming files outside this host's roots: `entries` by their spelling.
RpcError not_readable_here_error(const Vector<std::string>& entries, std::string message) {
  JsonValue data = JsonValue::object();
  data.set("reason", JsonValue("subject not readable here"));
  JsonValue list = JsonValue::array();
  for (const std::string& e : entries)
    list.push_back(JsonValue(e));
  data.set("entries", std::move(list));
  return make_error(codes::k_invalid_argument, std::move(message), std::move(data));
}

template <class First, class Decide>
bool digest_passes(Context& ctx, const std::string& session_id, DigestFor use, First&& first,
                   Decide&& decide, SubjectState& state, Session*& session, RpcError& error) {
  const DigestBounds bounds = DigestBounds::now();
  const i64 started = time::monotonic_ns();
  for (u32 pass = 1;; ++pass) {
    session = ctx.sessions->require(session_id, error);
    if (session == nullptr) return false;
    ReviewSubject subject;
    u64 revision = 0;
    if (!first(*session, subject, revision, error)) return false;
    SubjectPlan plan;
    compose_subject(ctx, *session, subject, bounds, plan);
    plan.refuse_over = use == DigestFor::raise;
    if (use == DigestFor::raise && !plan.over.empty()) {
      error = over_error(plan.over[0]);
      return false;
    }
    // Whoever asks a person to judge runs where the thing can be seen: a raise naming a file
    // outside this host's roots is refused, by the entry's spelling; before the reads for a file
    // the subject names, after them for one a glTF of it names.
    const auto outside_refused = [&] {
      if (use != DigestFor::raise) return false;
      Vector<std::string> outside;
      for (const SubjectPlan::File& f : plan.files) {
        if (f.outside) outside.push_back(f.spelled);
      }
      if (outside.empty()) return false;
      error = not_readable_here_error(
          outside, "the subject names " + outside[0] +
                       ", which is outside this host's roots: raise it on a host whose roots "
                       "hold every file it names");
      return true;
    };
    if (outside_refused()) return false;
    const SessionManager::DigestReadHook hook = ctx.sessions->digest_read_hook();
    void* const hook_context = ctx.sessions->digest_read_context();
    without_dispatch_lock(ctx, [&] {
      if (hook != nullptr) hook(hook_context);
      read_subject_files(plan, bounds);
    });
    ctx.sessions->note_digest_bytes(plan.bytes_read);
    ctx.sessions->note_digest_opens(plan.files_opened);
    ctx.sessions->note_digest_names(plan.names_collected);
    session = ctx.sessions->require(session_id, error);
    if (session == nullptr) return false;
    if (use == DigestFor::raise && !plan.over.empty()) {
      error = over_error(plan.over[0]);
      return false;
    }
    if (outside_refused()) return false;
    // Nor over a glTF whose names its digest could not follow: the item would be stamped over
    // geometry the digest never saw.
    if (use == DigestFor::raise && !plan.unfollowed.empty()) {
      error = names_not_read_error(plan.unfollowed[0]);
      return false;
    }
    bool moved = !same_document(session, plan);
    if (!moved) {
      finish_subject(plan, state);
      state.passes = pass;
      state.ms = static_cast<f64>(time::monotonic_ns() - started) / 1.0e6;
      const bool ok = decide(*session, state, revision, moved, error);
      if (!moved) return ok;
    }
    if (pass == k_digest_passes) {
      JsonValue data = JsonValue::object();
      data.set("reason", JsonValue("subject changing"));
      data.set("passes", JsonValue(pass));
      error = make_error(codes::k_invalid_argument,
                         "what the subject names changed while its files were read, " +
                             std::to_string(pass) +
                             " times running: ask again once the document and the ledger settle",
                         std::move(data));
      return false;
    }
  }
}

bool review_raise(Context& ctx, const ReviewRaiseParams& params, ReviewsResult& out,
                  RpcError& error) {
  Caller caller;
  Session* s = begin(ctx, params.session, &params.attribution, caller, error);
  if (s == nullptr || !require_actor(caller, "a review item records who raised it", error))
    return false;
  if (!text_ok("kind", params.kind, 1, k_kind_max, false, error) ||
      !text_ok("summary", params.summary, 1, k_summary_max, true, error) ||
      !text_ok("subject.checkpoint", params.subject.checkpoint, 0, k_name_max, false, error) ||
      !names_ok("subject.blobs", params.subject.blobs, error))
    return false;
  if (params.subject.records.size() > k_subject_records_max ||
      params.subject.blobs.size() > k_link_max || params.subject.assets.size() > k_link_max) {
    error = make_error(codes::k_invalid_argument,
                       "subject names at most 1024 records, 256 blobs and 256 assets: raise one "
                       "item per part");
    return false;
  }
  for (const std::string& a : params.subject.assets) {
    if (!text_ok("subject.assets entries", a, 1, k_asset_path_max, false, error)) return false;
    // A stored path, as a record's is (T174; protocol.md, "Paths a document's records hold"): the
    // ledger lies beside the document and every answer carries the subject, so an absolute path
    // would be this machine's in both. One rule and one answer with the record's.
    if (io::check_stored_path_form(a) != io::Refusal::none) {
      error = path_not_found("subject.assets", a, io::Access::read);
      return false;
    }
  }
  if (!params.report.is_null() && write_json(params.report).size() > k_report_max) {
    error = make_error(codes::k_invalid_argument,
                       "report is at most 64 KiB of JSON: the numbers, never a picture (a blob "
                       "the subject names carries anything larger)");
    return false;
  }
  io::Vfs& vfs = ctx.sessions->vfs();
  // The ledger's own refusals of a raise, made in the first read and again in the write.
  const auto allowed = [&](LedgerFile& ledger, RpcError& e) {
    u32 open = 0;
    for (const ReviewInfo& o : ledger.reviews) {
      if (o.raised_by.actor == caller.who.actor && o.state == ReviewState::Open) ++open;
    }
    if (open >= k_open_items_per_actor) {
      e = bound_error("open items per actor", k_open_items_per_actor,
                      caller.who.actor +
                          " has raised 256 review items that are still open: a raiser withdraws "
                          "its own agent items, and a person's wait for the person");
      return false;
    }
    return params.task == 0 || find_task(ledger, params.task, e) != nullptr;
  };
  // Every refusal that reads no file, before the digest reads any (protocol.md, "What it costs"):
  // the subject's layer and records, the session's gate on writes, and the ledger's bounds.
  const auto first = [&](Session& session, ReviewSubject& subject, u64& revision, RpcError& e) {
    const doc::Document& d = session.document();
    if (!params.subject.layer.empty() && d.find_layer(params.subject.layer) < 0) {
      e = make_error(codes::k_not_found, "no layer named " + params.subject.layer);
      return false;
    }
    for (const Id128 id : params.subject.records) {
      if (d.exists(id)) continue;
      char hex[Id128::k_hex_length + 1];
      id.to_hex(hex);
      e = make_error(codes::k_not_found, std::string("subject record ") +
                                             std::string(hex, Id128::k_hex_length) +
                                             " is not live: a review item names what stands");
      return false;
    }
    if (!session.may_write_side_files(e) ||
        !ledger_has_room(vfs, doc::DocumentStore::path_of(session.dir(), k_ledger_file), e))
      return false;
    subject = params.subject;
    return update_ledger(
        vfs, session, true, false, revision,
        [&](LedgerFile& ledger, bool& write, RpcError& e2) {
          write = false;
          return allowed(ledger, e2);
        },
        e);
  };
  // The subject's digest, stamped by the host from its own document (protocol.md, "What a close is
  // tied to"), into the write that raises the item.
  const i64 now = time::wall_unix_ms();
  ReviewInfo made;
  const auto decide = [&](Session& session, const SubjectState& stamped, u64 revision, bool& moved,
                          RpcError& e) {
    const doc::Document& d = session.document();
    return update_ledger(
        vfs, session, false, true, out.ledger_revision,
        [&](LedgerFile& ledger, bool& write, RpcError& e2) {
          write = false;
          if (ledger.revision != revision) {
            moved = true;
            return false;
          }
          write = lapse_claims(ledger, now) > 0;
          if (!allowed(ledger, e2)) return false;
          TaskInfo* task = params.task != 0 ? find_task(ledger, params.task, e2) : nullptr;
          {
            ReviewInfo r;
            r.id = fresh_review_id(ledger);
            r.revision = 1;
            r.kind = params.kind;
            r.audience = params.audience;
            r.summary = params.summary;
            r.subject = params.subject;
            // The version is the host's to say, not the raiser's.
            r.subject.journal_length = static_cast<u32>(d.journal().size());
            r.subject.undo_position = session.undo_position();
            r.raised_digest = stamped.digest;
            r.report = params.report;
            r.task = params.task;
            r.raised_by = caller.who;
            r.raised_by.timestamp_unix_ms = now;
            r.raised_via = caller.via;
            r.raised_unix_ms = now;
            note(r.history, r.history_dropped, now, caller, "raised", {});
            if (task != nullptr) {
              task->reviews.push_back(r.id);
              task->updated_unix_ms = now;
            }
            made = r;
            ledger.reviews.push_back(std::move(r));
          }
          write = true;
          return true;
        },
        e);
  };
  SubjectState stamped;
  if (!digest_passes(ctx, params.session, DigestFor::raise, first, decide, stamped, s, error))
    return false;
  out.now_unix_ms = now;
  out.reviews.push_back(std::move(made));
  out.subject = std::move(stamped);
  out.subject.same = true;
  // The item and the task it was raised for are both this call's change, told by its one event.
  s->publish_ledger("review_raised", 0, out.reviews[0].id, caller.who.actor, caller.connection,
                    params.task);
  return true;
}

bool review_close(Context& ctx, const ReviewCloseParams& params, ReviewsResult& out,
                  RpcError& error) {
  Caller caller;
  Session* s = begin(ctx, params.session, &params.attribution, caller, error);
  if (s == nullptr || !require_actor(caller, "a closed review item records who closed it", error))
    return false;
  if (!text_ok("note", params.note, 0, k_text_max, true, error)) return false;
  if (params.resolution == ReviewResolution::None) {
    error =
        make_error(codes::k_invalid_argument, "resolution must be Accepted, Rejected or Withdrawn");
    return false;
  }
  // The refusals of the item and of the caller, which read no file: made in the first read of the
  // ledger, before the digest reads a byte, and again in the write (protocol.md, "What it costs").
  const auto closable = [&](const ReviewInfo& item, RpcError& e) {
    const ReviewInfo* r = &item;
    const std::string id = std::to_string(r->id);
    if (r->state == ReviewState::Closed) {
      e = make_error(codes::k_invalid_argument, "review item " + id + " was closed " +
                                                    resolution_name(r->resolution) + " by " +
                                                    r->closed_by.actor);
      return false;
    }
    if (params.revision != 0 && params.revision != r->revision) {
      JsonValue data = JsonValue::object();
      data.set("reason", JsonValue("revision moved"));
      data.set("review", JsonValue(r->id));
      data.set("revision", JsonValue(r->revision));
      e = make_error(codes::k_invalid_argument,
                     "review item " + id + " is at revision " + std::to_string(r->revision) +
                         ", not the " + std::to_string(params.revision) +
                         " this close was decided on: review.list it again",
                     std::move(data));
      return false;
    }
    const std::string role = role_of(caller);
    if (r->audience == ReviewAudience::Human) {
      // A person's to close, and nobody else's: not its raiser's to take back, and not an
      // agent's whatever methods its role names (AGENTS.md: a check that needs a higher
      // view policy becomes a human review item, never a configuration edit).
      // A person is known by `human_via` alone (policy.h): a credential the operator
      // issued for a role marked human, never a role the call names.
      if (params.resolution == ReviewResolution::Withdrawn || caller.human_via.empty()) {
        e = forbidden("review item " + id +
                          " is for a person: it is closed only on a connection that "
                          "presented a credential the host's operator issued for a role "
                          "the roles file marks human, and nobody withdraws it",
                      "human review", role);
        return false;
      }
    } else if (params.resolution == ReviewResolution::Withdrawn &&
               r->raised_by.actor != caller.who.actor) {
      e = forbidden("review item " + id + " is withdrawn by its raiser (" + r->raised_by.actor +
                        ") alone; a reviewer accepts or rejects it",
                    "review raiser", role);
      return false;
    }
    // What the closer saw. A person's close carries both halves of it, so a page that
    // dropped one could not decide past a change; an agent's checks what it sends.
    if (r->audience == ReviewAudience::Human && (params.revision == 0 || params.digest.empty())) {
      JsonValue data = JsonValue::object();
      data.set("reason", JsonValue("seen required"));
      data.set("review", JsonValue(r->id));
      e = make_error(codes::k_invalid_argument,
                     "review item " + id +
                         " is a person's: its close carries the revision and the subject "
                         "digest its detail was drawn with (review.list with digest)",
                     std::move(data));
      return false;
    }
    return true;
  };
  io::Vfs& vfs = ctx.sessions->vfs();
  const auto first = [&](Session& session, ReviewSubject& subject, u64& revision, RpcError& e) {
    if (!session.may_write_side_files(e)) return false;
    return update_ledger(
        vfs, session, true, false, revision,
        [&](LedgerFile& ledger, bool& write, RpcError& e2) {
          write = false;
          const ReviewInfo* r = find_review(ledger, params.review, e2);
          if (r == nullptr || !closable(*r, e2)) return false;
          // An item's subject never changes after the raise: the one read is the one checked.
          subject = r->subject;
          return true;
        },
        e);
  };
  // The decision, under the dispatch lock and the ledger's, on the digest of this same pass: the
  // computation that decides reuses nothing (protocol.md, "What a close is tied to").
  const i64 now = time::wall_unix_ms();
  ReviewInfo changed;
  const auto decide = [&](Session& session, const SubjectState& seen_now, u64 revision, bool& moved,
                          RpcError& e) {
    return update_ledger(
        vfs, session, false, false, out.ledger_revision,
        [&](LedgerFile& ledger, bool& write, RpcError& e2) {
          write = false;
          if (ledger.revision != revision) {
            moved = true;
            return false;
          }
          ReviewInfo* r = find_review(ledger, params.review, e2);
          if (r == nullptr || !closable(*r, e2)) return false;
          // A digest this host cannot compute in full is never compared, so it never reads as a
          // change (protocol.md, "What a close is tied to").
          if (!params.digest.empty() && !seen_now.comparable) {
            e2 = not_readable_here_error(
                seen_now.outside,
                "review item " + std::to_string(r->id) + " names " + seen_now.outside[0] +
                    ", which is outside this host's roots: close it on a host that can read "
                    "every file it names");
            return false;
          }
          if (!params.digest.empty() && params.digest != seen_now.digest) {
            JsonValue data = JsonValue::object();
            data.set("reason", JsonValue("subject moved"));
            data.set("review", JsonValue(r->id));
            data.set("digest", JsonValue(seen_now.digest));
            data.set("same", JsonValue(seen_now.digest == r->raised_digest));
            e2 = make_error(codes::k_invalid_argument,
                            "what review item " + std::to_string(r->id) +
                                " is about changed since its detail was drawn: review.list it "
                                "with digest again, and decide on what it is now",
                            std::move(data));
            return false;
          }
          // The digest its closer saw, which the host found equal; none when it sent none (an
          // agent's close, a withdrawal), since nothing was compared.
          r->decided_at = params.digest.empty() ? std::string() : seen_now.digest;
          r->state = ReviewState::Closed;
          r->resolution = params.resolution;
          r->closed_by = caller.who;
          r->closed_by.timestamp_unix_ms = now;
          r->closed_via = caller.via;
          r->closed_human_via = caller.human_via;
          r->closed_unix_ms = now;
          ++r->revision;
          note(r->history, r->history_dropped, now, caller, "closed",
               std::string(resolution_name(params.resolution)) +
                   (blank(params.note) ? std::string() : ": " + params.note));
          changed = *r;
          write = true;
          return true;
        },
        e);
  };
  SubjectState seen_now;
  if (!digest_passes(ctx, params.session, DigestFor::close, first, decide, seen_now, s, error))
    return false;
  out.now_unix_ms = now;
  seen_now.same = seen_now.comparable && seen_now.digest == changed.raised_digest;
  out.reviews.push_back(std::move(changed));
  out.subject = std::move(seen_now);
  s->publish_ledger("review_closed", 0, params.review, caller.who.actor, caller.connection);
  return true;
}

bool review_list(Context& ctx, const ReviewListParams& params, ReviewsResult& out,
                 RpcError& error) {
  Caller caller;
  Session* s = begin(ctx, params.session, nullptr, caller, error);
  if (s == nullptr) return false;
  if (!list_bounds_ok(params.limit, params.reviews.size(), "reviews", error)) return false;
  if (params.digest && params.review == 0) {
    error = make_error(codes::k_invalid_argument,
                       "digest is computed for one item (review), the detail a person opens, and "
                       "never for the rows of a list: it reads every asset the subject names");
    return false;
  }
  // The detail form is one item by id: a list's selection beside it (ids, an order, a limit, a
  // state, an audience, a kind, a task) would either change nothing or hide the item it names, so
  // it is refused rather than ignored.
  if (params.digest &&
      (!params.reviews.empty() || params.limit != 0 || params.order != LedgerOrder::Oldest ||
       params.state || params.audience || !params.kind.empty() || params.task != 0)) {
    JsonValue data = JsonValue::object();
    data.set("reason", JsonValue("detail form"));
    error = make_error(codes::k_invalid_argument,
                       "digest opens one item's detail (review): reviews, order, limit, state, "
                       "audience, kind and task select a list's rows and are not given beside it",
                       std::move(data));
    return false;
  }
  out.now_unix_ms = time::wall_unix_ms();
  const auto list = [&](Session& session, u64& revision, RpcError& error_out) {
    out.reviews.clear();
    out.more = 0;
    return update_ledger(
        ctx.sessions->vfs(), session, true, false, revision,
        [&](LedgerFile& ledger, bool& write, RpcError& e) {
          write = false;
          if (params.review != 0 && find_review(ledger, params.review, e) == nullptr) return false;
          list_in_order(
              ledger.reviews, params.order, params.limit, params.brief,
              [&](const ReviewInfo& r) {
                if (params.review != 0 && r.id != params.review) return false;
                if (!params.reviews.empty() && !contains(params.reviews, r.id)) return false;
                if (params.state && r.state != *params.state) return false;
                if (params.audience && r.audience != *params.audience) return false;
                if (!params.kind.empty() && r.kind != params.kind) return false;
                if (params.task != 0 && r.task != params.task) return false;
                return true;
              },
              out.reviews, out.more);
          return true;
        },
        error_out);
  };
  if (!params.digest) return list(*s, out.ledger_revision, error);
  // The detail's statement, its files read with the host let go and with no lock of the ledger's
  // held (the assets' reads are the cost, and every other caller of the ledger waits for that
  // lock); the ledger read again under the dispatch lock says whether the item it answers is the
  // one the pass composed for.
  // With no filter beside it the detail answers its item, which `find_review` found.
  const auto first = [&](Session& session, ReviewSubject& subject, u64& revision, RpcError& e) {
    if (!list(session, revision, e)) return false;
    out.ledger_revision = revision;
    subject = out.reviews[0].subject;
    return true;
  };
  const auto decide = [&](Session& session, const SubjectState&, u64 revision, bool& moved,
                          RpcError& e) {
    u64 now = 0;
    const bool ok = update_ledger(
        ctx.sessions->vfs(), session, true, false, now,
        [](LedgerFile&, bool& write, RpcError&) {
          write = false;
          return true;
        },
        e);
    moved = ok && now != revision;
    return ok;
  };
  if (!digest_passes(ctx, params.session, DigestFor::detail, first, decide, out.subject, s, error))
    return false;
  // A host that cannot see a file cannot say the subject is as it was: it says so (`comparable`).
  out.subject.same = out.subject.comparable && out.subject.digest == out.reviews[0].raised_digest;
  return true;
}

}  // namespace

void add_ledger_methods(Dispatcher& d) {
  d.add(method<TaskCreateParams, TasksResult, &task_create>(
      "task.create",
      "Add a task to the document's ledger: a goal, its acceptance criteria and the role that "
      "should do it, for an agent to claim and resume from the ledger rather than from memory."));
  d.add(method<TaskClaimParams, TasksResult, &task_claim>(
      "task.claim",
      "Claim an open task for a time, or renew one's own claim; refused (1009) while another "
      "actor's claim stands. A claim nobody renews lapses and the task is open again."));
  d.add(method<TaskUpdateParams, TasksResult, &task_update>(
      "task.update",
      "The claimant's progress (a note, links, Blocked, Done, or letting go), or the creator's "
      "drop or reopen."));
  d.add(read_only(method<TaskListParams, TasksResult, &task_list>(
      "task.list", "The document's tasks, by id, status, role or claimant, visible to everyone.")));
  d.add(method<ReviewRaiseParams, ReviewsResult, &review_raise>(
      "review.raise",
      "Raise a review item: a check the caller may not or cannot make, with the numbers and "
      "exactly what is to be reviewed, for an agent or, with audience Human, for a person."));
  d.add(method<ReviewCloseParams, ReviewsResult, &review_close>(
      "review.close",
      "Accept, reject or withdraw a review item. One raised for a person is closed only on a "
      "connection bound to a credential issued for a role the roles file marks human (1008 "
      "otherwise), and never withdrawn."));
  d.add(read_only(method<ReviewListParams, ReviewsResult, &review_list>(
      "review.list",
      "The document's review items, by id, state, audience, kind or task, visible to everyone.")));
}

// ---- the ledger's watch (protocol.md, "The ledger's events") ----------------------------------

void LedgerWaker::set(void (*wake)(void* context), void* context) noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  wake_ = wake;
  context_ = context;
}

void LedgerWaker::notify() noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  if (wake_ != nullptr) wake_(context_);
}

void Session::watch_ledger(LedgerWaker* waker) {
  if (waker == nullptr) {
    ledger_watch_.reset();  // joins its thread before anything it reads goes
    ledger_waker_ = nullptr;
    ledger_notice_.store(false);
    return;
  }
  if (ledger_watch_ != nullptr) return;
  std::string native;
  if (vfs_->resolve(dir_, native, /*for_write=*/false) != io::Status::Ok) return;
  // The waker first, then the system's watch, registered before `start` returns, and only then
  // what the file holds: a write after the read raises a notice someone is told of, and one before
  // it is in what was read, so no write falls between the two (A55).
  ledger_waker_ = waker;
  auto watch = std::make_unique<DirWatch>();
  watch->start(
      std::move(native), std::string(k_ledger_file),
      [](void* context) {
        Session* self = static_cast<Session*>(context);
        self->ledger_notice_.store(true);
        if (self->ledger_waker_ != nullptr) self->ledger_waker_->notify();
      },
      this);
  SessionManager::LedgerLook look;
  look.path = doc::DocumentStore::path_of(dir_, k_ledger_file);
  look.make_lock = writable();
  SessionManager::read_ledger_look(*vfs_, look);
  ledger_seen_ = look.read && !look.same ? std::move(look.now) : LedgerSeen{};
  ++ledger_seen_moves_;
  ledger_watch_ = std::move(watch);
}

void SessionManager::set_ledger_waker(void (*wake)(void* context), void* context) {
  ledger_waker_.set(wake, context);
  ledger_waking_ = wake != nullptr;
  sync_ledger_watches();
}

void SessionManager::sync_ledger_watches() {
  const bool watch = watches_ledgers();
  for (const auto& s : sessions_)
    s->watch_ledger(watch ? &ledger_waker_ : nullptr);
}

bool Session::ledger_watch_polling() const noexcept {
  return ledger_watch_ != nullptr && ledger_watch_->polling();
}

void Session::ledger_written(const LedgerFile& ledger) {
  const std::string text = canonical(ledger);
  ledger_written_ = LedgerSeen{};
  fill_seen(ledger, text, ledger_written_);
  // The file's bytes are the canonical ones: this tree wrote them.
  ledger_written_.raw_size = ledger_written_.size;
  ledger_written_.raw_hash = ledger_written_.hash;
  ledger_has_written_ = true;
  ++ledger_seen_moves_;
}

void Session::flush_ledger_written(u64 own_task, u64 own_review) {
  if (!ledger_has_written_) return;
  (void)publish_ledger_changed(ledger_seen_, ledger_written_, own_task, own_review);
  ledger_seen_ = std::move(ledger_written_);
  ledger_written_ = LedgerSeen{};
  ledger_has_written_ = false;
  ++ledger_seen_moves_;
}

bool Session::publish_ledger_changed(const LedgerSeen& from, const LedgerSeen& to, u64 own_task,
                                     u64 own_review) const {
  if (events_ == nullptr || !events_->wanted(EventKind::Session, id_)) return false;
  SessionChangeEvent event;
  changed_ids(from.tasks, to.tasks, own_task, event.tasks, event.tasks_total);
  changed_ids(from.reviews, to.reviews, own_review, event.reviews, event.reviews_total);
  const bool own_write = own_task != 0 || own_review != 0;
  if (event.tasks_total == 0 && event.reviews_total == 0 &&
      (own_write || from.revision == to.revision))
    return false;
  event.session = id_;
  event.change = "ledger_changed";
  event.path = shown_path_;
  event.ledger_revision = to.revision;
  events_->publish(EventKind::Session, id_, schema::to_json(event));
  return true;
}

void SessionManager::begin_ledger_looks(bool all, Vector<LedgerLook>& looks) {
  looks.clear();
  for (const auto& s : sessions_) {
    if (!s->watching_ledger()) continue;
    const bool noticed = s->take_ledger_notice();
    if (!noticed && !all) continue;
    // A write of this host's whose call was refused after it wrote (a lapse) is told first.
    s->flush_ledger_written(0, 0);
    LedgerLook look;
    look.session = s->id();
    look.path = doc::DocumentStore::path_of(s->dir(), k_ledger_file);
    look.make_lock = s->writable();
    look.seen_moves = s->ledger_seen_moves();
    const LedgerSeen& seen = s->ledger_seen();
    look.seen_present = seen.present;
    look.seen_raw_size = seen.raw_size;
    look.seen_raw_hash = seen.raw_hash;
    looks.push_back(std::move(look));
  }
}

void SessionManager::read_ledger_look(io::Vfs& vfs, LedgerLook& look) {
  look.read = false;
  look.same = false;
  look.now = LedgerSeen{};
  std::string text;
  if (vfs.exists(look.path)) {
    // Under the ledger's own lock between processes, for the read alone: a reader holding the
    // file open on Windows makes a writer's rename over it fail (side_file.h).
    SideFileLock lock;
    RpcError error;
    if (look.make_lock && !lock_side_file(vfs, look.path, lock, error)) return;
    const io::Status s = vfs.read(look.path, text);
    lock.release();
    if (s == io::Status::Ok) {
      look.now.present = true;
    } else if (s != io::Status::NotFound) {
      return;  // looked at again at the next notice
    }
  }
  look.now.raw_size = text.size();
  look.now.raw_hash = look.now.present ? text_hash(text) : 0;
  look.read = true;
  // The bytes already seen: nothing to parse, nothing changed.
  if (look.now.present == look.seen_present && look.now.raw_size == look.seen_raw_size &&
      look.now.raw_hash == look.seen_raw_hash) {
    look.same = true;
    return;
  }
  if (!look.now.present) return;
  JsonValue value;
  RpcError error;
  LedgerFile ledger;
  if (!parse_json(text, value).ok ||
      !detail::map_side_json(
          schema::type_of<LedgerFile>(), &ledger,
          [](void* object) { *static_cast<LedgerFile*>(object) = LedgerFile{}; }, value, look.path,
          error)) {
    look.read = false;  // a file this tree did not write whole: looked at again at the next notice
    return;
  }
  const u64 raw_size = look.now.raw_size;
  const u64 raw_hash = look.now.raw_hash;
  fill_seen(ledger, canonical(ledger), look.now);
  look.now.raw_size = raw_size;
  look.now.raw_hash = raw_hash;
}

u32 SessionManager::finish_ledger_looks(Vector<LedgerLook>& looks, bool& again) {
  again = false;
  u32 published = 0;
  for (LedgerLook& look : looks) {
    Session* s = find(look.session);
    if (s == nullptr || !s->watching_ledger() || !look.read) continue;
    if (s->ledger_seen_moves() != look.seen_moves) {
      // This process read or wrote the ledger after the look read it: what the look holds may be
      // older than what the session has seen, so it looks again rather than step back.
      s->note_ledger_notice();
      again = true;
      continue;
    }
    if (look.same) continue;
    const LedgerSeen& seen = s->ledger_seen();
    // Inequality of the revision, the size and the content hash, never "greater": a ledger restored
    // from an older copy has a revision that went back, and it is a change.
    const bool changed = look.now.present != seen.present || look.now.revision != seen.revision ||
                         look.now.size != seen.size || look.now.hash != seen.hash;
    if (changed && s->publish_ledger_changed(seen, look.now)) ++published;
    s->set_ledger_seen(std::move(look.now));
  }
  return published;
}

u32 SessionManager::look_at_ledgers(bool all) {
  Vector<LedgerLook> looks;
  begin_ledger_looks(all, looks);
  for (LedgerLook& look : looks)
    read_ledger_look(*vfs_, look);
  bool again = false;
  return finish_ledger_looks(looks, again);
}

}  // namespace engine::protocol
