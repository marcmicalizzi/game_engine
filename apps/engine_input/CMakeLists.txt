# engine-input: a probe for the input devices attached to a machine. Lists them, streams what
# they emit as JSON lines (optionally recording an input::InputLog), and replays a recorded log
# through an InputState. It is how a device the maintainers do not own gets characterized:
# the owner runs it, sends the output back, and the numbers become bindings and tests.
engine_app(NAME engine_input OUTPUT engine-input
  SOURCES main.cpp
  DEPS base containers log platform window input json io
  E2E_TESTS tests/input_app_tests.cpp)
