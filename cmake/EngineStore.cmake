# The persistent-store capability's dependency (docs/plan/03-data-model.md §3.5, ADR-0003,
# experiment E6): the SQLite amalgamation. Public domain.
#
# Pinned by URL and SHA-256 of the published amalgamation archive rather than by a git tag,
# because that archive *is* SQLite's release artefact: the canonical repository is Fossil and
# the GitHub mirror is a mirror. One .c file, compiled here into engine_sqlite3 with the
# defines the engine wants baked in, so no caller can disable them by accident:
#
#   SQLITE_THREADSAFE=2            multi-thread: SQLite is safe across threads, one connection
#                                  per thread. Serialized mode (1) would take a mutex on every
#                                  API call for a guarantee the engine does not need — the store
#                                  hands a connection to one thread at a time.
#   SQLITE_DQS=0                   a double-quoted string is an identifier, never a string
#                                  literal. The misfeature turns a typo'd column name into a
#                                  silently-constant string; a typo should be an error.
#   SQLITE_OMIT_DEPRECATED         nothing in the engine may reach for the pre-3.5 API.
#   SQLITE_DEFAULT_WAL_SYNCHRONOUS=1   WAL + synchronous NORMAL by default: a crash can lose the
#                                  last transactions but never corrupts the database, which is
#                                  the right trade for a save file written every tick.
#   SQLITE_DEFAULT_MEMSTATUS=0     drops the global allocation counters (and their atomics) from
#                                  every malloc; nothing reads them.
#   SQLITE_LIKE_DOESNT_MATCH_BLOBS, SQLITE_OMIT_SHARED_CACHE, SQLITE_USE_ALLOCA,
#   SQLITE_MAX_EXPR_DEPTH=0        the recommended "smaller, faster" set from SQLite's own
#                                  compile-options page.
#
# JSON is *not* behind SQLITE_ENABLE_JSON1: json1 has been built in and always on since 3.38,
# and the flag is a no-op kept only for old build scripts. The document model of
# docs/plan/03-data-model.md §3.2 can use json_extract() on a projection blob today.

engine_capability_enabled(store _engine_store_enabled)
if(NOT _engine_store_enabled)
  return()
endif()

include(FetchContent)
enable_language(C)

set(ENGINE_SQLITE_VERSION "3530400" CACHE STRING "SQLite amalgamation version (MMmmpp00)")
set(ENGINE_SQLITE_YEAR "2026" CACHE STRING "Year directory of the SQLite download")
set(ENGINE_SQLITE_SHA256 "1e71ddf93849c6a6ecf58b827c0692073d2dd7ee40196158068f7b29f422e87d"
  CACHE STRING "SHA-256 of the SQLite amalgamation archive")

# SOURCE_SUBDIR points at a directory that does not exist: the archive carries no CMake files
# and FetchContent must not go looking for any.
FetchContent_Declare(sqlite3
  URL      "https://www.sqlite.org/${ENGINE_SQLITE_YEAR}/sqlite-amalgamation-${ENGINE_SQLITE_VERSION}.zip"
  URL_HASH "SHA256=${ENGINE_SQLITE_SHA256}"
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  SOURCE_SUBDIR cmake-not-used)
FetchContent_MakeAvailable(sqlite3)

add_library(engine_sqlite3 STATIC "${sqlite3_SOURCE_DIR}/sqlite3.c")
add_library(engine::sqlite3 ALIAS engine_sqlite3)
# SYSTEM so sqlite3.h's own warnings never reach our -Werror, for this target and for consumers.
target_include_directories(engine_sqlite3 SYSTEM PUBLIC "${sqlite3_SOURCE_DIR}")
target_compile_definitions(engine_sqlite3 PUBLIC
  SQLITE_THREADSAFE=2
  SQLITE_DQS=0
  SQLITE_OMIT_DEPRECATED
  SQLITE_DEFAULT_WAL_SYNCHRONOUS=1
  SQLITE_DEFAULT_MEMSTATUS=0
  SQLITE_LIKE_DOESNT_MATCH_BLOBS
  SQLITE_OMIT_SHARED_CACHE
  SQLITE_USE_ALLOCA
  SQLITE_MAX_EXPR_DEPTH=0)
set_target_properties(engine_sqlite3 PROPERTIES C_STANDARD 99 POSITION_INDEPENDENT_CODE ON)
if(MSVC)
  target_compile_options(engine_sqlite3 PRIVATE /W0)
else()
  target_compile_options(engine_sqlite3 PRIVATE -w)
endif()
if(UNIX)
  find_package(Threads REQUIRED)
  target_link_libraries(engine_sqlite3 PUBLIC Threads::Threads ${CMAKE_DL_LIBS} m)
endif()
