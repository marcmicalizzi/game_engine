// Plan 06 §6.8's task ledger and review items (docs/subsystems/protocol.md, "The task ledger and
// review items"; ADR-0072): `task.create`, `task.claim`, `task.update`, `task.list`,
// `review.raise`, `review.close` and `review.list`.
//
// The ledger is `ledger.json` beside the document, and no host keeps it in memory: every call
// takes `ledger.json.lock` between processes, reads the file, changes it and writes it back
// (`update_ledger`, over side_file.h), so two engine-cli calls at once, or engine-host beside
// engine-cli, lose nothing to each other and never mint one id twice.
#include <core/json/json.h>
#include <core/schema/json_reflect.h>
#include <core/time/time.h>
#include <domain/doc/document_store.h>
#include <domain/protocol/policy.h>
#include <domain/protocol/rpc.h>
#include <domain/protocol/session.h>
#include <domain/protocol/side_file.h>

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
template <class F>
bool update_ledger(io::Vfs& vfs, const Session& session, bool read_only, bool grows, F&& change,
                   RpcError& error) {
  const std::string path = doc::DocumentStore::path_of(session.dir(), k_ledger_file);
  if (read_only && !vfs.exists(path)) {
    LedgerFile none;
    bool write = false;
    return change(none, write, error);
  }
  // A read goes on to take the lock too, since it may let claims lapse and write: the session's
  // one gate on writes beside its document (session.h) decides both.
  if (!session.may_write_side_files(error)) return false;
  LedgerFile ledger;
  return update_side_file(
      vfs, path, ledger,
      [&](LedgerFile& l, bool& write) {
        io::FileInfo info;
        if (grows && vfs.stat(path, info) == io::Status::Ok && info.size >= k_ledger_bytes_max) {
          error = bound_error("ledger bytes", k_ledger_bytes_max,
                              "the ledger holds " + std::to_string(info.size) +
                                  " bytes, past its bound of 8 MiB: close, finish or drop work "
                                  "before adding more (a claim, a status change and a close "
                                  "still answer)");
          return false;
        }
        return change(l, write, error);
      },
      error);
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
  if (!update_ledger(ctx.sessions->vfs(), *s, false, true,
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
                              caller.who.actor + " has made 256 tasks that are not Done or Dropped: "
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
  if (!update_ledger(ctx.sessions->vfs(), *s, false, false,
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
  if (!update_ledger(ctx.sessions->vfs(), *s, false, !blank(params.note) || !links_empty(params.links),
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
  const i64 now = time::wall_unix_ms();
  out.now_unix_ms = now;
  return update_ledger(ctx.sessions->vfs(), *s, true, false,
      [&](LedgerFile& ledger, bool& write, RpcError& e) {
        out.lapsed = lapse_claims(ledger, now);
        write = out.lapsed > 0;
        if (params.task != 0 && find_task(ledger, params.task, e) == nullptr) return false;
        for (const TaskInfo& t : ledger.tasks) {
          if (params.task != 0 && t.id != params.task) continue;
          if (!params.statuses.empty() && !contains(params.statuses, t.status)) continue;
          if (!params.owner_role.empty() && t.owner_role != params.owner_role) continue;
          if (!params.claimant.empty() && (!held(t.status) || t.claimant != params.claimant))
            continue;
          out.tasks.push_back(t);
          if (params.brief) out.tasks.back().history.clear();
        }
        return true;
      },
      error);
}

// ---- review.* ----------------------------------------------------------------------------------

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
  const doc::Document& d = s->document();
  if (!params.subject.layer.empty() && d.find_layer(params.subject.layer) < 0) {
    error = make_error(codes::k_not_found, "no layer named " + params.subject.layer);
    return false;
  }
  if (params.subject.records.size() > k_subject_records_max ||
      params.subject.blobs.size() > k_link_max) {
    error = make_error(codes::k_invalid_argument,
                       "subject names at most 1024 records and 256 blobs: raise one item per part");
    return false;
  }
  for (const Id128 id : params.subject.records) {
    if (d.exists(id)) continue;
    char hex[Id128::k_hex_length + 1];
    id.to_hex(hex);
    error = make_error(codes::k_not_found, std::string("subject record ") +
                                               std::string(hex, Id128::k_hex_length) +
                                               " is not live: a review item names what stands");
    return false;
  }
  if (!params.report.is_null() && write_json(params.report).size() > k_report_max) {
    error = make_error(codes::k_invalid_argument,
                       "report is at most 64 KiB of JSON: the numbers, never a picture (a blob "
                       "the subject names carries anything larger)");
    return false;
  }
  const i64 now = time::wall_unix_ms();
  ReviewInfo made;
  if (!update_ledger(ctx.sessions->vfs(), *s, false, true,
          [&](LedgerFile& ledger, bool& write, RpcError& e) {
            write = lapse_claims(ledger, now) > 0;
            u32 open = 0;
            for (const ReviewInfo& o : ledger.reviews) {
              if (o.raised_by.actor == caller.who.actor && o.state == ReviewState::Open) ++open;
            }
            if (open >= k_open_items_per_actor) {
              e = bound_error("open items per actor", k_open_items_per_actor,
                              caller.who.actor + " has raised 256 review items that are still "
                                                 "open: a raiser withdraws its own agent items, "
                                                 "and a person's wait for the person");
              return false;
            }
            TaskInfo* task = nullptr;
            if (params.task != 0) {
              task = find_task(ledger, params.task, e);
              if (task == nullptr) return false;
            }
            ReviewInfo r;
            r.id = fresh_review_id(ledger);
            r.revision = 1;
            r.kind = params.kind;
            r.audience = params.audience;
            r.summary = params.summary;
            r.subject = params.subject;
            // The version is the host's to say, not the raiser's.
            r.subject.journal_length = static_cast<u32>(d.journal().size());
            r.subject.undo_position = s->undo_position();
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
            write = true;
            return true;
          },
          error))
    return false;
  out.now_unix_ms = now;
  out.reviews.push_back(std::move(made));
  s->publish_ledger("review_raised", 0, out.reviews[0].id, caller.who.actor, caller.connection);
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
  const i64 now = time::wall_unix_ms();
  ReviewInfo changed;
  if (!update_ledger(ctx.sessions->vfs(), *s, false, false,
          [&](LedgerFile& ledger, bool& write, RpcError& e) {
            ReviewInfo* r = find_review(ledger, params.review, e);
            if (r == nullptr) return false;
            const std::string id = std::to_string(r->id);
            if (r->state == ReviewState::Closed) {
              e = make_error(codes::k_invalid_argument, "review item " + id + " was closed " +
                                                            resolution_name(r->resolution) +
                                                            " by " + r->closed_by.actor);
              return false;
            }
            if (params.revision != 0 && params.revision != r->revision) {
              JsonValue data = JsonValue::object();
              data.set("reason", JsonValue("revision moved"));
              data.set("review", JsonValue(r->id));
              data.set("revision", JsonValue(r->revision));
              e = make_error(codes::k_invalid_argument,
                             "review item " + id + " is at revision " +
                                 std::to_string(r->revision) + ", not the " +
                                 std::to_string(params.revision) +
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
              e = forbidden("review item " + id + " is withdrawn by its raiser (" +
                                r->raised_by.actor + ") alone; a reviewer accepts or rejects it",
                            "review raiser", role);
              return false;
            }
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
          error))
    return false;
  out.now_unix_ms = now;
  out.reviews.push_back(std::move(changed));
  s->publish_ledger("review_closed", 0, params.review, caller.who.actor, caller.connection);
  return true;
}

bool review_list(Context& ctx, const ReviewListParams& params, ReviewsResult& out,
                 RpcError& error) {
  Caller caller;
  Session* s = begin(ctx, params.session, nullptr, caller, error);
  if (s == nullptr) return false;
  out.now_unix_ms = time::wall_unix_ms();
  return update_ledger(ctx.sessions->vfs(), *s, true, false,
      [&](LedgerFile& ledger, bool& write, RpcError& e) {
        write = false;
        if (params.review != 0 && find_review(ledger, params.review, e) == nullptr) return false;
        for (const ReviewInfo& r : ledger.reviews) {
          if (params.review != 0 && r.id != params.review) continue;
          if (params.state && r.state != *params.state) continue;
          if (params.audience && r.audience != *params.audience) continue;
          if (!params.kind.empty() && r.kind != params.kind) continue;
          if (params.task != 0 && r.task != params.task) continue;
          out.reviews.push_back(r);
          if (params.brief) out.reviews.back().history.clear();
        }
        return true;
      },
      error);
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

}  // namespace engine::protocol
