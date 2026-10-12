#include <core/json/json.h>
#include <domain/protocol/events.h>
#include <foundation/tunables/tunables.h>

namespace engine::protocol {

namespace {

// 256 events a connection: a journal event is a few hundred bytes (a thousand ids at most), so the
// bound costs at most a few megabytes a subscriber while it is stalled, and a subscriber that is
// merely slow — a browser tab in the background, an agent between turns — falls 256 changes
// behind before it loses one. It loses none silently: the next event says how many.
tunables::Int queue_tunable{"protocol.events.queue", 256, 1, 1 << 16,
                            "Events a socket connection's queue holds before it drops them"};

constexpr const char* k_names[k_event_kind_count] = {"journal", "selection", "scene", "session"};

}  // namespace

const char* event_kind_name(EventKind kind) noexcept { return k_names[static_cast<u32>(kind)]; }

bool event_kind_from(std::string_view name, EventKind& out) noexcept {
  for (u32 i = 0; i < k_event_kind_count; ++i) {
    if (name == k_names[i]) {
      out = static_cast<EventKind>(i);
      return true;
    }
  }
  return false;
}

void Subscriptions::attach(EventTransport* transport) noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  transport_ = transport;
}

u32 Subscriptions::queue_bound() noexcept { return static_cast<u32>(queue_tunable.get()); }

u32 Subscriptions::subscribers() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return static_cast<u32>(subscribers_.size());
}

void Subscriptions::snapshot(Vector<Snapshot>& out) const {
  std::lock_guard<std::mutex> lock(mutex_);
  out.clear();
  for (const Subscriber& s : subscribers_) {
    for (const Interest& i : s.interests) {
      Snapshot snap;
      snap.connection = s.connection;
      snap.session = i.session;
      snap.kinds = i.kinds;
      snap.dropped = s.dropped;
      out.push_back(std::move(snap));
    }
  }
}

u32 Subscriptions::subscribe(u64 connection, std::string_view session, u32 kinds) {
  std::lock_guard<std::mutex> lock(mutex_);
  Subscriber* found = nullptr;
  for (Subscriber& s : subscribers_) {
    if (s.connection == connection) found = &s;
  }
  if (found == nullptr) {
    Subscriber s;
    s.connection = connection;
    subscribers_.push_back(std::move(s));
    found = &subscribers_.back();
  }
  for (Interest& i : found->interests) {
    if (i.session == session) {
      i.kinds |= kinds & k_all_event_kinds;
      return i.kinds;
    }
  }
  Interest interest;
  interest.session = std::string(session);
  interest.kinds = kinds & k_all_event_kinds;
  found->interests.push_back(std::move(interest));
  return found->interests.back().kinds;
}

u32 Subscriptions::unsubscribe(u64 connection, std::string_view session, u32 kinds) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (u32 s = 0; s < subscribers_.size(); ++s) {
    Subscriber& subscriber = subscribers_[s];
    if (subscriber.connection != connection) continue;
    for (u32 i = 0; i < subscriber.interests.size(); ++i) {
      Interest& interest = subscriber.interests[i];
      if (interest.session != session) continue;
      interest.kinds &= ~kinds;
      const u32 left = interest.kinds;
      if (left == 0) subscriber.interests.erase_at(i);
      if (subscriber.interests.empty()) subscribers_.erase_at(s);
      return left;
    }
    return 0;
  }
  return 0;
}

void Subscriptions::forget(u64 connection) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (u32 s = 0; s < subscribers_.size(); ++s) {
    if (subscribers_[s].connection == connection) {
      subscribers_.erase_at(s);
      return;
    }
  }
}

u32 Subscriptions::mask_for(const Subscriber& s, std::string_view session) noexcept {
  u32 mask = 0;
  for (const Interest& i : s.interests) {
    if (i.session.empty() || i.session == session) mask |= i.kinds;
  }
  return mask;
}

bool Subscriptions::wanted(EventKind kind, std::string_view session) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (transport_ == nullptr) return false;
  for (const Subscriber& s : subscribers_) {
    if ((mask_for(s, session) & event_bit(kind)) != 0) return true;
  }
  return false;
}

void Subscriptions::publish(EventKind kind, std::string_view session, const JsonValue& params) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (transport_ == nullptr || subscribers_.empty()) return;
  const u32 bound = queue_bound();
  JsonValue message = JsonValue::object();
  message.set("jsonrpc", "2.0");
  message.set("method", std::string("event.") + event_kind_name(kind));
  message.set("params", params);
  JsonWriteOptions compact;
  compact.pretty = false;
  std::string plain;  // the text for a connection that missed nothing, made once
  for (u32 s = 0; s < subscribers_.size();) {
    Subscriber& subscriber = subscribers_[s];
    if ((mask_for(subscriber, session) & event_bit(kind)) == 0) {
      ++s;
      continue;
    }
    std::string text;
    if (subscriber.dropped == 0) {
      if (plain.empty()) plain = write_json(message, compact);
      text = plain;
    } else {
      JsonValue counted = message;
      if (JsonValue* p = counted.find("params"); p != nullptr && p->is_object()) {
        p->set("dropped", subscriber.dropped);
      }
      text = write_json(counted, compact);
    }
    const EventTransport::Post posted =
        transport_->post(subscriber.connection, std::move(text), bound);
    if (posted == EventTransport::Post::Gone) {
      // The connection ended between its last message and the listener's word of it.
      subscribers_.erase_at(s);
      continue;
    }
    if (posted == EventTransport::Post::Full) {
      ++subscriber.dropped;
    } else {
      subscriber.dropped = 0;
    }
    ++s;
  }
}

}  // namespace engine::protocol
