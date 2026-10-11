#pragma once

// For a module that keeps state: names its state type in the description. The type is a struct
// declared in a `.schema` file beside the module; `engine_game_module(... SCHEMAS ...)` runs
// schemac over it and compiles the generated tables into the module without their registration
// (ENGINE_SCHEMA_NO_REGISTRAR), so this include costs the module headers and constant data, not a
// library. The host migrates the state through this descriptor on every reload.

#include <core/schema/type_info.h>
#include <foundation/hotload/module_abi.h>

namespace engine::hotload {

template <class State>
void describe_state(ModuleDesc* out) noexcept {
  out->state_type = &schema::type_of<State>();
  out->type_info_size = static_cast<u32>(sizeof(schema::TypeInfo));
}

}  // namespace engine::hotload
