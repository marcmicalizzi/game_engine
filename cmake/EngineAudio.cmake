# miniaudio (public domain or MIT-0, docs/plan/08-toolchain.md, plan 05 §5.11): the device layer
# and the decoders behind domain/audio. Fetched only when the audio capability is on, so a minimal
# build (ADR-0027) neither downloads nor compiles it.
#
# miniaudio is one header plus a two-line `miniaudio.c` that includes it with the implementation
# switched on. It is compiled here, as C, into `engine_miniaudio`, and the engine never compiles
# the implementation itself: exactly one engine translation unit (domain/audio/src/backend.cpp)
# includes the header for its declarations, and that target links this one PRIVATE, so neither
# the header nor the defines below reach anything that includes a domain/audio public header.
#
# The defines are PUBLIC on this target on purpose: several of them change the size and layout of
# `ma_context` and `ma_device`, which backend.cpp allocates, so the library and the one file that
# includes its header must see the same set. Each one is here because the engine does not use
# what it removes (docs/subsystems/audio.md, "What miniaudio is asked to do"):
#
#   MA_NO_DSOUND MA_NO_WINMM   WASAPI is the one Windows backend. DirectSound and WinMM are its
#                              predecessors and every Windows the engine supports has WASAPI.
#   MA_NO_JACK                 JACK is a pro-audio routing server, not a game output.
#   MA_NO_NULL                 miniaudio's null backend is a device that plays to nowhere on a
#                              thread of its own. The engine's null backend is not a device at
#                              all — the caller pulls the mix into its own buffer — and enumerating
#                              miniaudio's would report a "device" on a machine that has none.
#   MA_NO_ENCODING MA_NO_GENERATION MA_NO_ENGINE MA_NO_NODE_GRAPH MA_NO_RESOURCE_MANAGER
#                              the high-level engine, its node graph and resource manager, the
#                              encoders and the waveform generators. The engine mixes itself and
#                              owns its clips; none of this is linked or reachable.
#
# What is left: WASAPI on Windows; PulseAudio and ALSA on Linux, both **loaded at run time**
# (miniaudio's default), so building needs no audio headers or libraries and a machine without
# them — the CI runners, the headless GPU server — builds and runs the same binary and simply
# finds no devices. Decoding: WAV (dr_wav), FLAC (dr_flac) and MP3 (dr_mp3), all inside the header;
# and the linear resampler with its low-pass filter, which decoding uses to bring a clip to the
# mix rate.

include_guard(GLOBAL)
include(FetchContent)

# 0.11.25 (2026-03-04) is commit 9634bedb5b5a2ca38c1ee7108a9358a4e233f14d.
set(ENGINE_MINIAUDIO_TAG "0.11.25" CACHE STRING "miniaudio tag")

function(engine_fetch_miniaudio)
  if(TARGET engine_miniaudio)
    return()
  endif()

  # SOURCE_SUBDIR points at a directory that does not exist: miniaudio's own CMakeLists builds its
  # examples, tests and optional libvorbis/libopus decoders and must not be added.
  FetchContent_Declare(miniaudio
    GIT_REPOSITORY https://github.com/mackron/miniaudio.git
    GIT_TAG        ${ENGINE_MINIAUDIO_TAG}
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  cmake-not-used)
  FetchContent_MakeAvailable(miniaudio)

  enable_language(C)
  add_library(engine_miniaudio STATIC "${miniaudio_SOURCE_DIR}/miniaudio.c")
  add_library(engine::miniaudio ALIAS engine_miniaudio)
  # SYSTEM so the header's own warnings never reach the engine's -Werror in backend.cpp.
  target_include_directories(engine_miniaudio SYSTEM PUBLIC "${miniaudio_SOURCE_DIR}")
  target_compile_definitions(engine_miniaudio PUBLIC
    MA_NO_DSOUND
    MA_NO_WINMM
    MA_NO_JACK
    MA_NO_NULL
    MA_NO_ENCODING
    MA_NO_GENERATION
    MA_NO_ENGINE
    MA_NO_NODE_GRAPH
    MA_NO_RESOURCE_MANAGER)
  set_target_properties(engine_miniaudio PROPERTIES C_STANDARD 99 POSITION_INDEPENDENT_CODE ON
                                                    FOLDER "third_party")
  # Third-party code is judged by its own policy, not ours.
  if(MSVC)
    target_compile_options(engine_miniaudio PRIVATE /W0)
  else()
    target_compile_options(engine_miniaudio PRIVATE -w)
  endif()
  if(UNIX)
    # dlopen for the run-time-linked backends, pthreads for the device thread, libm for the
    # resampler's filter design.
    find_package(Threads REQUIRED)
    target_link_libraries(engine_miniaudio PUBLIC Threads::Threads ${CMAKE_DL_LIBS} m)
  endif()
endfunction()
