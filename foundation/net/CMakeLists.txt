# Loopback networking (docs/subsystems/net.md): TCP sockets that can bind 127.0.0.1 and nothing
# else, the HTTP/1.1 subset a local listener needs, RFC 6455 WebSocket framing for both ends, and a
# thread-per-connection server. engine-host's `--listen` is its first user (plan 06 §6.2). No
# third-party dependency: SHA-1 and base64 are core/hash's, the random source core/platform's.
engine_module(NAME net LAYER foundation
  DEPS base containers hash platform
  SOURCES src/socket.cpp src/http.cpp src/websocket.cpp src/server.cpp)

# Winsock on Windows; the BSD sockets are in the C library elsewhere.
if(WIN32)
  target_link_libraries(engine_net PRIVATE ws2_32)
endif()

engine_module_tests(NAME net
  SOURCES tests/net_tests.cpp)
