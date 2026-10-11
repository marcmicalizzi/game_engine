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
