#include <foundation/net/server.h>

namespace engine::net {

bool Server::start(u16 port, ServerHandler& handler, std::string& error) {
  stop();
  if (!listener_.open_loopback(port, error)) return false;
  handler_ = &handler;
  stopping_.store(false);
  listen_thread_ = std::thread([this] { listen_loop(); });
  return true;
}

void Server::stop() {
  if (!listen_thread_.joinable()) return;
  stopping_.store(true);
  listen_thread_.join();
  listener_.close();
  {
    // A connection thread closes its socket under this lock and marks itself done in the same
    // breath, so every handle shut down here is still the connection's own.
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& c : connections_) {
      if (!c->done.load()) shutdown_socket(c->handle);
    }
  }
  reap(true);
  handler_ = nullptr;
}

u32 Server::connections() const {
  std::lock_guard<std::mutex> lock(mutex_);
  u32 open = 0;
  for (const auto& c : connections_)
    open += c->done.load() ? 0u : 1u;
  return open;
}

Post Server::post(u64 connection, std::string text, u32 bound) {
  // Under the server's lock, so the connection cannot be reaped between the lookup and the queue;
  // nothing here waits on a peer, so holding it is brief.
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& c : connections_) {
    if (c->id != connection) continue;
    if (c->done.load()) return Post::Gone;
    std::lock_guard<std::mutex> out(c->out_mutex);
    if (c->ws == nullptr || c->out_stop) return Post::Gone;
    if (c->outbox.size() - c->out_head >= bound) return Post::Full;
    c->outbox.push_back(std::move(text));
    if (!c->writer.joinable()) {
      Connection& target = *c;
      c->writer = std::thread([this, &target] { write_loop(target); });
    }
    c->out_ready.notify_one();
    return Post::Queued;
  }
  return Post::Gone;
}

void Server::write_loop(Connection& c) {
  for (;;) {
    std::string next;
    WebSocket* ws = nullptr;
    {
      std::unique_lock<std::mutex> out(c.out_mutex);
      c.out_ready.wait(out, [&] { return c.out_stop || c.out_head < c.outbox.size(); });
      if (c.out_stop) return;
      next = std::move(c.outbox[c.out_head++]);
      if (c.out_head == c.outbox.size()) {
        c.outbox.clear();
        c.out_head = 0;
      }
      ws = c.ws;
    }
    // Outside the outbox's lock: a peer that has stopped reading blocks this send, and only this
    // connection's writer waits on it, while posts keep queueing until the bound refuses them.
    // `ws` stays valid until serve() has joined this thread.
    if (ws == nullptr || !ws->send_text(next)) {
      std::lock_guard<std::mutex> out(c.out_mutex);
      c.out_stop = true;
      return;
    }
  }
}

void Server::reap(bool all) {
  Vector<std::unique_ptr<Connection>> finished;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    Vector<std::unique_ptr<Connection>> kept;
    for (auto& c : connections_) {
      if (all || c->done.load()) {
        finished.push_back(std::move(c));
      } else {
        kept.push_back(std::move(c));
      }
    }
    connections_ = std::move(kept);
  }
  for (auto& c : finished) {
    if (c->thread.joinable()) c->thread.join();
  }
}

void Server::listen_loop() {
  while (!stopping_.load()) {
    Socket socket;
    std::string error;
    // A quarter of a second between looks at `stopping_`: the one wait in this module that is not
    // on a peer, and the cheapest way to stop a blocked accept on both systems alike.
    if (!listener_.accept(250, socket, error)) {
      if (!error.empty()) break;  // the listener itself failed
      continue;
    }
    reap(false);
    if (connections() >= k_max_connections) {
      (void)socket.send_all(format_http_response(http_error(503, "too many connections")));
      continue;
    }
    auto connection = std::make_unique<Connection>();
    Connection& c = *connection;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      c.id = next_id_++;
      c.handle = socket.native();
      connections_.push_back(std::move(connection));
    }
    c.thread = std::thread([this, &c, s = std::move(socket)]() mutable { serve(c, std::move(s)); });
  }
}

void Server::serve(Connection& connection, Socket socket) {
  HttpRequest request;
  std::string leftover;
  const HttpRead read = read_http_request(socket, k_head_timeout_ms, request, leftover);
  WebSocket ws;
  bool upgraded = false;
  if (read == HttpRead::Ok) {
    std::string why;
    if (request.header("upgrade") != nullptr) {
      if (!is_websocket_upgrade(request, why)) {
        (void)socket.send_all(format_http_response(http_error(400, why)));
      } else {
        (void)socket.send_all(websocket_upgrade_response(request));
        ws = WebSocket(std::move(socket), Role::Server, std::move(leftover));
        upgraded = true;
      }
    } else {
      HttpResponse response;
      handler_->http(request, response);
      (void)socket.send_all(format_http_response(response));
    }
  } else if (read == HttpRead::TooLarge) {
    (void)socket.send_all(format_http_response(http_error(431)));
  } else if (read == HttpRead::Malformed) {
    (void)socket.send_all(format_http_response(http_error(400)));
  } else if (read == HttpRead::TimedOut) {
    (void)socket.send_all(format_http_response(http_error(408)));
  }

  if (upgraded) {
    u16 code = close_code::k_policy;
    std::string reason;
    if (!handler_->accept(request, code, reason)) {
      ws.close(code, reason);
    } else {
      {
        std::lock_guard<std::mutex> out(connection.out_mutex);
        connection.ws = &ws;
      }
      Message message;
      while (!stopping_.load() && ws.receive(message)) {
        if (message.opcode == Opcode::Text) {
          const std::string reply = handler_->message(connection.id, message.data);
          if (!reply.empty() && !ws.send_text(reply)) break;
        } else if (!handler_->binary(connection.id, message.data)) {
          ws.close(close_code::k_unsupported_data, "this server takes text messages");
          break;
        }
      }
      handler_->closed(connection.id);
      // The writer goes before the socket does: told to stop, woken from a send the peer is not
      // reading (shutdown, and on Windows the cancel in shutdown_socket), and joined, so it never
      // touches a closed handle.
      {
        std::lock_guard<std::mutex> out(connection.out_mutex);
        connection.out_stop = true;
        connection.ws = nullptr;
      }
      connection.out_ready.notify_all();
      if (connection.writer.joinable()) {
        ws.socket().shutdown_both();
        connection.writer.join();
      }
    }
  }

  std::lock_guard<std::mutex> lock(mutex_);
  if (upgraded) {
    ws.socket().close();
  } else {
    socket.close();
  }
  connection.handle = Socket::k_invalid;
  connection.done.store(true);
}

}  // namespace engine::net
