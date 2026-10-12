#pragma once

// Subscriptions (docs/subsystems/protocol.md, "Events"; plan 06 §6.2; roadmap A2): which socket
// connection wants which kinds of event for which session, and the one place an event is turned
// into a JSON-RPC notification and handed to the transport.
//
// **Who feeds it.** The places that already know a change happened, under the host's dispatch lock:
// the session's commit, undo and redo (`journal`), its opens, closes and leases (`session`),
// engine-host's `render.load {session}` (`scene`), and engine-editor's selection (`selection`).
// Being under the one lock every dispatch takes is what orders the events: they are published in
// the order the changes were made, each after its change is committed.
//
// **Who drains it.** `publish` formats the notification once and posts it to every subscriber's
// connection through an `EventTransport` — the listener, whose server queues it on that connection
// and sends it from the connection's own writer thread (foundation/net/server.h, `Server::post`).
// So publishing never waits on a socket, and a subscriber that has stopped reading stalls nobody:
// its queue fills to the bound (`queue_bound()`, the tunable `protocol.events.queue`), further
// events for it are dropped and counted, and the next one it is sent carries `dropped`, the count,
// so it knows to read the state again.
//
// stdio has no channel to push on: a dispatch that did not come from a connection has connection 0,
// and `session.subscribe` refuses it.

#include <core/base/macros.h>
#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/json/json_value.h>

#include <mutex>
#include <span>
#include <string>
#include <string_view>

namespace engine::protocol {

enum class EventKind : u8 { Journal = 0, Selection = 1, Scene = 2, Session = 3 };
inline constexpr u32 k_event_kind_count = 4;
inline constexpr u32 k_all_event_kinds = (1u << k_event_kind_count) - 1;

constexpr u32 event_bit(EventKind kind) noexcept { return 1u << static_cast<u32>(kind); }
// "journal", "selection", "scene", "session".
const char* event_kind_name(EventKind kind) noexcept;
bool event_kind_from(std::string_view name, EventKind& out) noexcept;

// A journal event names at most this many records; `ids_total` says how many the patch touched.
inline constexpr u32 k_event_max_ids = 1024;

// Where an event goes: one connection's queue. The listener implements it over its server.
class EventTransport {
 public:
  enum class Post : u8 { Queued, Full, Gone };
  virtual ~EventTransport() = default;
  // Queues `text` on `connection` unless `bound` messages are already waiting there. Never waits.
  virtual Post post(u64 connection, std::string text, u32 bound) = 0;
};

class Subscriptions {
 public:
  Subscriptions() = default;
  ENGINE_NON_COPYABLE(Subscriptions);

  // The transport events are posted through; null (the default) publishes nothing.
  void attach(EventTransport* transport) noexcept;

  // `connection` now receives `kinds` (a mask of event_bit) for `session` (empty: every session),
  // in addition to whatever it already had. The mask it holds for that session afterwards.
  u32 subscribe(u64 connection, std::string_view session, u32 kinds);
  // Takes `kinds` away; the mask left for that session.
  u32 unsubscribe(u64 connection, std::string_view session, u32 kinds);
  // The connection has gone: everything it held goes with it.
  void forget(u64 connection);

  // Whether anyone wants `kind` for `session` — so a publisher can skip building an event nobody
  // will be sent.
  bool wanted(EventKind kind, std::string_view session) const;
  // Sends `{"jsonrpc":"2.0","method":"event.<kind>","params":params}` to every connection
  // subscribed to `kind` for `session` (or for every session), with `dropped` set in `params` to
  // what that connection has missed since its last event.
  void publish(EventKind kind, std::string_view session, const JsonValue& params);

  // The bound of every connection's queue, from the tunable `protocol.events.queue`.
  static u32 queue_bound() noexcept;
  // How many connections hold a subscription.
  u32 subscribers() const;
  // Every (connection, session) interest, with the connection's dropped count: what
  // `session.subscriptions` reports.
  struct Snapshot {
    u64 connection = 0;
    std::string session;
    u32 kinds = 0;
    u64 dropped = 0;
  };
  void snapshot(Vector<Snapshot>& out) const;

 private:
  struct Interest {
    std::string session;  // empty: every session
    u32 kinds = 0;
  };
  struct Subscriber {
    u64 connection = 0;
    u64 dropped = 0;
    Vector<Interest> interests;
  };

  static u32 mask_for(const Subscriber& s, std::string_view session) noexcept;

  mutable std::mutex mutex_;
  EventTransport* transport_ = nullptr;
  Vector<Subscriber> subscribers_;
};

}  // namespace engine::protocol
