// A client's explicit transaction (plan 06 §6.4; docs/subsystems/protocol.md, "Transactions a
// client opens"): `doc.begin`, `doc.apply` with `transaction`, `doc.commit`, `doc.rollback`, and
// `doc.transactions`. A transaction is staged commands kept beside the document, not a
// `doc::Transaction` held open across calls: a host started for one call (engine-cli) or replaced
// after a deadline (engine-mcp) must commit what an earlier one staged, and a document must never
// hold a client's uncommitted records where another client reads it.
#include <core/time/time.h>
#include <domain/protocol/session.h>

namespace engine::protocol {

namespace {

constexpr f64 k_max_ttl_seconds = 24.0 * 3600.0;

i64 expiry_of(i64 now_ms, f64 ttl_seconds) {
  return now_ms + static_cast<i64>(ttl_seconds * 1000.0);
}

bool blank(std::string_view s) noexcept { return s.find_first_not_of(" \t") == s.npos; }

void remove_entry(TransactionsFile& file, u64 id) {
  for (u32 i = 0; i < file.transactions.size(); ++i) {
    if (file.transactions[i].info.id != id) continue;
    file.transactions.erase(file.transactions.begin() + i);
    return;
  }
}

}  // namespace

u32 Session::expire_transactions(i64 now_ms) {
  u32 dropped = 0;
  Vector<TransactionEntry>& list = transactions_.transactions;
  u32 kept = 0;
  for (u32 i = 0; i < list.size(); ++i) {
    if (list[i].info.expires_unix_ms <= now_ms) {
      ++dropped;
      continue;
    }
    if (kept != i) list[kept] = std::move(list[i]);
    ++kept;
  }
  while (list.size() > kept)
    list.pop_back();
  for (u32 i = transaction_holds_.size(); i > 0; --i) {
    bool open = false;
    for (const TransactionEntry& e : list) {
      if (e.info.id == transaction_holds_[i - 1].transaction) open = true;
    }
    if (!open) transaction_holds_.erase_at(i - 1);
  }
  return dropped;
}

void Session::forget_transaction_hold(u64 transaction) {
  for (u32 i = transaction_holds_.size(); i > 0; --i) {
    if (transaction_holds_[i - 1].transaction == transaction) transaction_holds_.erase_at(i - 1);
  }
}

u32 Session::release_connection_transactions(u64 connection) {
  if (connection == 0) return 0;
  u32 released = 0;
  for (u32 i = transaction_holds_.size(); i > 0; --i) {
    if (transaction_holds_[i - 1].connection != connection) continue;
    transaction_holds_.erase_at(i - 1);
    ++released;
  }
  return released;
}

TransactionEntry* Session::find_transaction(u64 id, const Caller& caller, i64 now_ms,
                                            RpcError& error) {
  bool lapsed = false;
  for (const TransactionEntry& e : transactions_.transactions) {
    if (e.info.id == id && e.info.expires_unix_ms <= now_ms) lapsed = true;
  }
  if (expire_transactions(now_ms) > 0) {
    RpcError ignored;
    (void)save_transactions(ignored);
  }
  for (TransactionEntry& e : transactions_.transactions) {
    if (e.info.id != id) continue;
    if (e.info.attribution.actor != caller.who.actor) {
      error = forbidden("transaction " + std::to_string(id) + " was opened by " +
                            e.info.attribution.actor +
                            ", and only its opener stages into it, commits it or rolls it back",
                        "transaction owner", caller.who.role);
      return nullptr;
    }
    // Two clients of one host may say they are the same actor; while the connection that opened
    // the transaction lasts, it is the opener, and when it ends the actor is again (as a lease's
    // connection is; protocol.md, "Several clients, one host").
    for (const TransactionHold& h : transaction_holds_) {
      if (h.transaction != id || h.connection == caller.connection) continue;
      error = forbidden("transaction " + std::to_string(id) + " was opened on client connection " +
                            std::to_string(h.connection) +
                            ", which is still connected: only that connection stages into it, "
                            "commits it or rolls it back until it closes",
                        "transaction owner", caller.who.role);
      return nullptr;
    }
    return &e;
  }
  error = make_error(codes::k_not_found,
                     "no open transaction " + std::to_string(id) +
                         (lapsed ? ": it lapsed and was rolled back; nothing it staged landed"
                                 : ": doc.transactions lists the open ones"));
  return nullptr;
}

bool Session::begin_transaction(const BeginParams& params, const Caller& caller, i64 now_ms,
                                TransactionInfo& out, RpcError& error) {
  if (blank(caller.who.actor)) {
    error = make_error(codes::k_invalid_argument,
                       "attribution.actor is required: a transaction belongs to whoever opens it");
    return false;
  }
  if (!(params.ttl_seconds >= 1.0 && params.ttl_seconds <= k_max_ttl_seconds)) {
    error = make_error(codes::k_invalid_argument,
                       "ttl_seconds must be from 1 to 86400 (an idle transaction is rolled back "
                       "after it)");
    return false;
  }
  u32 layer = doc_.edit_layer();
  if (!params.layer.empty()) {
    const i32 found = doc_.find_layer(params.layer);
    if (found < 0) {
      error = make_error(codes::k_not_found, "no layer named " + params.layer);
      return false;
    }
    layer = static_cast<u32>(found);
  }
  // Fail fast: a layer the caller may not write (a proposal of someone else's, a closed one, a
  // layer the role does not name) is refused here, not at the commit. An empty staging pass makes
  // exactly the checks that depend on the layer alone.
  {
    Vector<u32> none;
    const Staging staging{0, &none};
    ApplyResult ignored;
    if (!commit_commands({}, caller.who, true, true, layer, &caller, ignored, error, &staging))
      return false;
  }
  (void)expire_transactions(now_ms);
  TransactionEntry entry;
  entry.info.id = transactions_.next_id++;
  entry.info.layer = static_cast<const doc::Document&>(doc_).layer(layer).name();
  entry.info.attribution = caller.who;
  entry.info.attribution.timestamp_unix_ms = 0;
  entry.info.opened_unix_ms = now_ms;
  entry.info.ttl_seconds = params.ttl_seconds;
  entry.info.expires_unix_ms = expiry_of(now_ms, params.ttl_seconds);
  out = entry.info;
  if (caller.connection != 0) transaction_holds_.push_back({entry.info.id, caller.connection});
  transactions_.transactions.push_back(std::move(entry));
  return save_transactions(error);
}

bool Session::stage(u64 transaction, std::span<const doc::Command> commands, const Caller& caller,
                    bool atomic, std::string_view layer, i64 now_ms, ApplyResult& result,
                    RpcError& error) {
  TransactionEntry* entry = find_transaction(transaction, caller, now_ms, error);
  if (entry == nullptr) return false;
  if (!layer.empty() && layer != entry->info.layer) {
    error = make_error(codes::k_invalid_argument,
                       "transaction " + std::to_string(transaction) + " edits layer " +
                           entry->info.layer + ", not " + std::string(layer) +
                           ": a transaction is one layer's, as its patch is");
    return false;
  }
  const i32 found = doc_.find_layer(entry->info.layer);
  if (found < 0) {
    error = make_error(codes::k_not_found, "transaction " + std::to_string(transaction) +
                                               "'s layer " + entry->info.layer + " is gone");
    return false;
  }
  // The transaction's commands, then the call's, checked as one write.
  const u32 held = static_cast<u32>(entry->commands.size());
  Vector<doc::Command> all;
  all.reserve(static_cast<u32>(entry->commands.size() + commands.size()));
  for (const doc::Command& c : entry->commands)
    all.push_back(c);
  for (const doc::Command& c : commands)
    all.push_back(c);
  Vector<u32> accepted;
  const Staging staging{held, &accepted};
  doc::Attribution who = caller.who;
  who.timestamp_unix_ms = now_ms;
  if (!commit_commands({all.data(), all.size()}, std::move(who), atomic, true,
                       static_cast<u32>(found), &caller, result, error, &staging))
    return false;
  result.transaction = transaction;
  if (!accepted.empty()) {
    for (const u32 i : accepted)
      entry->commands.push_back(commands[i]);
    entry->info.commands = static_cast<u32>(entry->commands.size());
    ++entry->info.applies;
    entry->info.expires_unix_ms = expiry_of(now_ms, entry->info.ttl_seconds);
    if (!save_transactions(error)) return false;
  }
  result.staged = entry->info.commands;
  return true;
}

bool Session::commit_transaction(u64 transaction, const Caller& caller, i64 now_ms,
                                 ApplyResult& result, RpcError& error) {
  TransactionEntry* entry = find_transaction(transaction, caller, now_ms, error);
  if (entry == nullptr) return false;
  result.transaction = transaction;
  if (entry->commands.empty()) {
    // Nothing staged: nothing to journal, and nothing left to hold open.
    remove_entry(transactions_, transaction);
    forget_transaction_hold(transaction);
    return save_transactions(error);
  }
  const i32 found = doc_.find_layer(entry->info.layer);
  if (found < 0) {
    error = make_error(codes::k_not_found, "transaction " + std::to_string(transaction) +
                                               "'s layer " + entry->info.layer + " is gone");
    return false;
  }
  // The opener's attribution, as `doc.begin` recorded it; a rationale given now replaces its
  // rationale, and an explicit timestamp is the patch's.
  doc::Attribution who = entry->info.attribution;
  if (!blank(caller.who.rationale)) who.rationale = caller.who.rationale;
  who.timestamp_unix_ms = caller.who.timestamp_unix_ms;
  const Vector<doc::Command> commands = entry->commands;
  if (!commit_commands({commands.data(), commands.size()}, std::move(who), true, true,
                       static_cast<u32>(found), &caller, result, error))
    return false;
  result.transaction = transaction;
  if (!result.committed) {
    // A command that no longer applies (another client committed under it): nothing landed, and
    // the transaction stays open with what it holds.
    result.staged = static_cast<u32>(commands.size());
    return true;
  }
  remove_entry(transactions_, transaction);
  forget_transaction_hold(transaction);
  result.staged = 0;
  return save_transactions(error);
}

bool Session::rollback_transaction(u64 transaction, const Caller& caller, i64 now_ms,
                                   TransactionInfo& out, RpcError& error) {
  const TransactionEntry* entry = find_transaction(transaction, caller, now_ms, error);
  if (entry == nullptr) return false;
  out = entry->info;
  remove_entry(transactions_, transaction);
  forget_transaction_hold(transaction);
  return save_transactions(error);
}

bool Session::list_transactions(std::string_view actor, i64 now_ms, TransactionsResult& out,
                                RpcError& error) {
  out.expired = expire_transactions(now_ms);
  out.now_unix_ms = now_ms;
  for (const TransactionEntry& e : transactions_.transactions) {
    if (!actor.empty() && e.info.attribution.actor != actor) continue;
    out.transactions.push_back(e.info);
  }
  return out.expired == 0 || save_transactions(error);
}

}  // namespace engine::protocol
