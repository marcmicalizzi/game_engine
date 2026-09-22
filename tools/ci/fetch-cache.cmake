# A machine-wide cache of FetchContent's downloaded sources, for the Linux container build
# (tools/linux-build.ps1, docs/ci/local-linux.md "The dependency cache"). Not used by any other
# build: linux-build.ps1 names it with -DCMAKE_PROJECT_TOP_LEVEL_INCLUDES and says where the cache
# is with -DENGINE_FETCH_CACHE; a configure that names neither never reads this file.
#
# Why. linux-build.ps1 keeps one build tree per checkout, and every agent works in a fresh
# worktree, so every agent's first run used to download every dependency again — Jolt, flecs,
# SDL3, SQLite, Recast, Tracy, meshoptimizer, Slang and the rest, once per preset — into a volume
# nobody else could read. The sources are a function of the pins alone, so they are kept once per
# pin, in one volume every checkout mounts, and a fresh worktree's configure downloads nothing that
# any checkout on the machine has already fetched. Build trees, including the dependencies' own
# build directories, stay per checkout: they are what differs.
#
# How. A CMake dependency provider (cmake_language(SET_DEPENDENCY_PROVIDER), CMake 3.24) sees every
# FetchContent_MakeAvailable() with the details its FetchContent_Declare() gave. It keys an entry
# by a hash of those details — repository, tag, URL, hash, and everything else declared — so a
# pin bump is a new entry and never a stale hit, fetches the entry once if it is missing, and then
# hands FetchContent the entry as FETCHCONTENT_SOURCE_DIR_<NAME> for exactly one
# FetchContent_MakeAvailable() call, which does everything else (add_subdirectory, SOURCE_SUBDIR,
# EXCLUDE_FROM_ALL, SYSTEM, the <name>_SOURCE_DIR variables) the way it always does.
#
# Three details that are not obvious:
#
# - The override is removed from the cache again after the call. FetchContent_Populate() writes
#   FETCHCONTENT_SOURCE_DIR_<NAME> into CMakeCache.txt as a side effect, and a cached override is
#   consulted *before* the provider on the next configure — so after a pin bump the build would
#   quietly keep the old sources. Unset, every configure asks the provider, which asks the pins.
# - An entry is filled in a `.partial-*` directory and renamed into place, so an interrupted
#   download never leaves a directory that looks complete. The build lock already stops two
#   builds filling one entry at once; the rename would settle it if it did not.
# - A filled entry is made read-only. Every checkout compiles from the same files, so a
#   dependency whose build wrote into its own source tree would be one checkout changing another
#   checkout's sources. None of the current dependencies does; if one starts to, its build fails
#   here with a permission error rather than corrupting the cache.
#
# Deliberately not in the key: the preset, the compiler, the checkout. None of them changes what
# a `git clone` of a tag or a download of a URL produces.

include_guard(GLOBAL)

if(NOT ENGINE_FETCH_CACHE)
  return()
endif()
if(NOT IS_DIRECTORY "${ENGINE_FETCH_CACHE}")
  message(WARNING "fetch-cache: ENGINE_FETCH_CACHE=${ENGINE_FETCH_CACHE} is not a directory; "
                  "FetchContent downloads into the build tree as usual")
  return()
endif()

include(FetchContent)

# Bump to invalidate every entry, when the way an entry is filled changes.
set(ENGINE_FETCH_CACHE_FORMAT 1)

# Makes sure the entry for one dependency exists and returns its path in _engine_fetch_cache_dir.
# `packed` is the declared details as one ;-list; `foreach(IN LISTS)` keeps empty elements, which
# matter to FetchContent (an empty GIT_SUBMODULES means "none", CMP0097).
function(_engine_fetch_cache_entry dep_name packed)
  string(TOLOWER "${dep_name}" lower)

  # FetchContent_Declare() records *where* it would have put the content alongside what the
  # content is: SOURCE_DIR, BINARY_DIR and SUBBUILD_DIR, defaulted from FETCHCONTENT_BASE_DIR,
  # which linux-build.ps1 sets per preset. They say nothing about the bytes, so they are left out
  # of the key — with them in it, every preset had its own copy of every download — and out of
  # what the download is told, which gets directories of its own below.
  set(details "")
  set(skip_value FALSE)
  foreach(item IN LISTS packed)
    if(skip_value)
      set(skip_value FALSE)
    elseif(item STREQUAL "SOURCE_DIR" OR item STREQUAL "BINARY_DIR" OR item STREQUAL "SUBBUILD_DIR")
      set(skip_value TRUE)
    elseif(NOT item STREQUAL "OVERRIDE_FIND_PACKAGE")
      list(APPEND details "${item}")
    endif()
  endforeach()

  string(SHA256 key "engine-fetch-cache-${ENGINE_FETCH_CACHE_FORMAT};${lower};${details}")
  string(SUBSTRING "${key}" 0 16 key)
  set(entry "${ENGINE_FETCH_CACHE}/${lower}-${key}")
  # `cmake --log-level=VERBOSE` says what an entry was keyed on, for the day a miss is a surprise.
  message(VERBOSE "fetch-cache: ${dep_name} is keyed on: ${details}")

  if(IS_DIRECTORY "${entry}")
    message(STATUS "fetch-cache: ${dep_name} from ${entry}")
    set(_engine_fetch_cache_dir "${entry}" PARENT_SCOPE)
    return()
  endif()

  message(STATUS "fetch-cache: ${dep_name} is not cached yet; fetching it into ${entry}")
  file(GLOB leftovers LIST_DIRECTORIES true "${ENGINE_FETCH_CACHE}/.partial-${lower}-${key}-*")
  foreach(leftover IN LISTS leftovers)
    file(REMOVE_RECURSE "${leftover}")
  endforeach()
  string(RANDOM LENGTH 12 ALPHABET "0123456789abcdef" nonce)
  set(staging "${ENGINE_FETCH_CACHE}/.partial-${lower}-${key}-${nonce}")
  set(scratch "${CMAKE_BINARY_DIR}/_fetch-cache/${lower}")
  file(REMOVE_RECURSE "${scratch}")

  # The direct form of FetchContent_Populate, with the details as declared: the same download
  # FetchContent would have done, into the staging directory instead of the build tree. Quoted
  # item by item, as FetchContent itself forwards them, so nothing is split or dropped.
  set(forward "")
  foreach(item IN LISTS details)
    string(APPEND forward " [==[${item}]==]")
  endforeach()
  cmake_language(EVAL CODE "
    FetchContent_Populate(${lower}_fetch_cache QUIET ${forward}
      SUBBUILD_DIR [==[${scratch}/subbuild]==]
      SOURCE_DIR   [==[${staging}]==]
      BINARY_DIR   [==[${scratch}/build]==])")

  # Plain rename(2), not NO_REPLACE: CMake implements NO_REPLACE with link() and unlink(), which
  # no filesystem allows on a directory ("Operation not permitted"), while rename(2) itself
  # already refuses to replace a directory that is not empty — which a filled entry never is.
  file(RENAME "${staging}" "${entry}" RESULT renamed)
  if(NOT renamed STREQUAL "0")
    # Somebody else filled it first; theirs is as good as ours.
    file(REMOVE_RECURSE "${staging}")
  else()
    execute_process(COMMAND chmod -R a-w "${entry}" RESULT_VARIABLE chmod_result)
    if(NOT chmod_result EQUAL 0)
      message(WARNING "fetch-cache: could not make ${entry} read-only")
    endif()
  endif()
  file(REMOVE_RECURSE "${scratch}")
  set(_engine_fetch_cache_dir "${entry}" PARENT_SCOPE)
endfunction()

# A macro, not a function, because FetchContent_MakeAvailable() must run in its caller's scope:
# add_subdirectory() and the <name>_SOURCE_DIR variables are expected there. FetchContent does
# not call the provider again for a dependency the provider itself asks for, which is what makes
# the inner FetchContent_MakeAvailable() the built-in one.
macro(engine_fetch_cache_provide method dep_name)
  _engine_fetch_cache_entry("${dep_name}" "${ARGN}")
  string(TOUPPER "${dep_name}" _engine_fetch_cache_upper)
  set(FETCHCONTENT_SOURCE_DIR_${_engine_fetch_cache_upper} "${_engine_fetch_cache_dir}")
  FetchContent_MakeAvailable(${dep_name})
  unset(FETCHCONTENT_SOURCE_DIR_${_engine_fetch_cache_upper} CACHE)
  unset(FETCHCONTENT_SOURCE_DIR_${_engine_fetch_cache_upper})
  unset(_engine_fetch_cache_upper)
  unset(_engine_fetch_cache_dir)
endmacro()

cmake_language(SET_DEPENDENCY_PROVIDER engine_fetch_cache_provide
  SUPPORTED_METHODS FETCHCONTENT_MAKEAVAILABLE_SERIAL)
message(STATUS "fetch-cache: dependency sources come from ${ENGINE_FETCH_CACHE}")
