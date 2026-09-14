# schemac: the schema compiler. A standalone tool (standard library only) so it builds before
# any engine module and on any host.
add_executable(engine_schemac
  src/main.cpp
  src/parser.cpp
  src/resolve.cpp
  src/emit_cpp.cpp
  src/emit_json_schema.cpp
  src/emit_docs.cpp)
target_compile_features(engine_schemac PRIVATE cxx_std_20)
target_include_directories(engine_schemac PRIVATE src)
engine_apply_warnings(engine_schemac)
set_target_properties(engine_schemac PROPERTIES OUTPUT_NAME schemac)
