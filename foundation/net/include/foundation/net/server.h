#pragma once

// A loopback server: one listener thread, one thread per connection, HTTP GETs and WebSocket
// upgrades handed to a handler the application implements (docs/subsystems/net.md). engine-host
// is its first user: the handler dispatches each text message through the protocol and serves the
// shell's static files (apps/engine_host/listen.cpp).
//
// **Threads, and why that is enough.** The clients are a browser tab or two, a test, an agent's
// script: a handful at once. A thread a connection keeps every read a blocking loop and every
// message's order the order it arrived, and costs nothing at this scale. The handler is called
// from those threads concurrently, so it serializes whatever it shares; engine-host takes one lock
// around the dispatcher, which is what makes a request over a socket indistinguishable from one on
// stdin. `k_max_connections` bounds the threads.

#include <core/base/macros.h>
#include <core/base/types.h>
#include <core/containers/vector.h>
#include <foundation/net/http.h>
#include <foundation/net/socket.h>
#include <foundation/net/websocket.h>

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

namespace engine::net {

class ServerHandler {
 public:
  virtual ~ServerHandler() = default;
  // A request that is not an upgrade. The response is sent and the connection closed.
  virtual void http(const HttpRequest& request, HttpResponse& response) = 0;
  // An upgrade request whose handshake is valid. False refuses it: the handshake still completes,
  // and the connection is closed at once with `close` and `reason` — because a browser's WebSocket
  // reports a close code and its reason to the page, and an HTTP status to nobody.
  virtual bool accept(const HttpRequest& request, u16& close, std::string& reason) = 0;
  // One text message on connection `connection`; the text to send back, or empty for none.
  virtual std::string message(u64 connection, std::string_view text) = 0;
  // A binary message; the connection is closed with 1003 when this returns false (the default).
  virtual bool binary(u64 connection, std::string_view bytes) {
    (void)connection;
    (void)bytes;
    return false;
  }
  // The WebSocket connection `connection` has ended (after its last `message`, before its socket is
  // closed): whatever the handler keeps per connection — the protocol's subscriptions — goes.
  virtual void closed(u64 connection) { (void)connection; }
};

// What `Server::post` did with a message.
enum class Post : u8 {
  Queued,  // in the connection's outbox; its writer sends it in order
  Full,    // the outbox already held `bound` messages: not queued
  Gone,    // no open WebSocket connection by that id
};

class Server {
 public:
  // More connections than this at once are answered 503 and closed.
  static constexpr u32 k_max_connections = 32;
  // How long a connection may take to send its request head.
  static constexpr i32 k_head_timeout_ms = 10000;

  Server() noexcept = default;
  ENGINE_NON_COPYABLE(Server);
  ~Server() { stop(); }

  // Binds 127.0.0.1:`port` (0: any free one) and starts the listener thread. `handler` must outlive
  // `stop()`.
  bool start(u16 port, ServerHandler& handler, std::string& error);
  u16 port() const noexcept { return listener_.port(); }
  // Stops accepting, shuts every connection down, and joins every thread.
  // Idempotent.
  void stop();
  // Connections open now.
  u32 connections() const;
  // Shuts every open connection down and goes on listening: what a peer sees when the network
  // drops under it, and how engine-editor's `--test-drop-socket-at` proves the shell reconnects.
  // Returns how many it shut.
  u32 drop_connections();
  // Sends one text message to every open WebSocket connection, between the messages their own
  // threads send (a lock per connection), and returns how many it reached: a host's notification
  // to its pages (engine-editor's `editor.relayout`). Small and rare by contract — it is sent on
  // the caller's thread — so a slow peer is the caller's wait, never a frame's queue.
  u32 send_text_all(std::string_view text);

  // **Pushing to a connection** (docs/subsystems/net.md): queues `text` as one text message on
  // WebSocket connection `connection`, to be sent after everything queued before it by a writer
  // thread of that connection's own — started on its first post — so the caller never waits on the
  // socket. A connection whose peer has stopped reading fills its socket's buffers, then its
  // outbox; past `bound` messages a post is refused (`Full`) rather than queued or waited on, and
  // the caller decides what a refused message means (the protocol counts it and says so in the next
  // one). Safe from any thread; never waits on a peer.
  Post post(u64 connection, std::string text, u32 bound);

 private:
  struct Connection {
    u64 id = 0;
    std::intptr_t handle = Socket::k_invalid;  // for stop(), valid while `done` is false
    std::atomic<bool> done{false};
    std::thread thread;
    // The outbox (post), guarded by `out_mutex`. `ws` is the connection's WebSocket while its
    // message loop runs, and null before and after, which is what makes a post `Gone`.
    std::mutex out_mutex;
    std::condition_variable out_ready;
    Vector<std::string> outbox;
    u32 out_head = 0;  // the next message to send; the outbox is cleared when it catches up
    bool out_stop = false;
    WebSocket* ws = nullptr;
    std::thread writer;
    std::mutex send;  // one direct sender at a time on `ws` (replies); pushes go through the outbox
  };

  void listen_loop();
  void serve(Connection& connection, Socket socket);
  void write_loop(Connection& connection);
  void reap(bool all);

  Listener listener_;
  ServerHandler* handler_ = nullptr;
  std::thread listen_thread_;
  std::atomic<bool> stopping_{false};
  mutable std::mutex mutex_;  // connections_, and every socket close of a connection
  Vector<std::unique_ptr<Connection>> connections_;
  u64 next_id_ = 1;
};

}  // namespace engine::net
