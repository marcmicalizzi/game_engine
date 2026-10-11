#pragma once

// The operating system's shared-library loader, the only platform code in the module.

#include <core/base/types.h>

#include <string>
#include <string_view>

namespace engine::hotload::detail {

// Null with the loader's message in `error` when the library cannot be loaded.
void* open_library(const std::string& path, std::string& error);
void* find_symbol(void* library, const char* name);
void close_library(void* library);
// This process's id, which keeps two processes' per-load copies of one library apart.
u64 process_id();

}  // namespace engine::hotload::detail
