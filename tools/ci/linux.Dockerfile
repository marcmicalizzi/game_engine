# The Linux toolchain image: what `.github/workflows/ci.yml`'s `linux` job runs on, reproduced
# locally so a Linux build runs on a Windows desktop and does not depend on GitHub being willing
# to run a job. `tools/linux-build.ps1` builds this and drives it; see docs/ci/local-linux.md for
# the command, the volumes, the build lock, and the measured times, cold and warm.
#
# **Ubuntu 24.04 is what `ubuntu-latest` resolves to today** (GitHub's runner image label moved
# to 24.04 in January 2025), and ci.yml names `ubuntu-24.04` explicitly, so this image is the
# same distribution and the same package set as the hosted job rather than a lookalike.
#
# The base is pinned by digest. Refresh it deliberately: `docker pull ubuntu:24.04`, take the
# digest `docker image inspect ubuntu:24.04 --format '{{index .RepoDigests 0}}'` prints, and
# change the line below — which changes this file's hash, which is the image tag
# `linux-build.ps1` computes, so every worktree rebuilds instead of silently disagreeing.
# --- Two targets ------------------------------------------------------------------------------
#
# `desktop` (the default, and what the four ci.yml presets are built in) is the hosted runner's
# package set: the toolchain *plus* the X11, Wayland and Mesa development headers SDL3's Unix
# configure wants. `headless` is the same toolchain with **none** of them, and it exists to make
# the headless build's central claim falsifiable rather than asserted:
# `ENGINE_WINDOW_BACKENDS=none` says the tree builds and its tests pass on a machine that has no
# display development files at all, and the only way to know that is to build it somewhere that
# has none. The two share every layer up to the split, so the second image costs one apt layer.
#
#   docker build -f tools/ci/linux.Dockerfile --target headless -t engine-linux-ci-headless .
#
# `tools/linux-build.ps1` picks the target from the preset and never asks the caller.
FROM ubuntu:24.04@sha256:008173c23f95b170204355c12626cb5a965d779a7e1283b09e9cffbb1bf33ca3 AS base

# Nothing in this image is an engine dependency. It is a toolchain: compilers, a build system and
# a shell. docs/ci/local-linux.md lists every package with its license.
ENV DEBIAN_FRONTEND=noninteractive
ENV LANG=C.UTF-8

# --- The toolchain, shared by both targets ------------------------------------------------------
#
# clang-format is here although ci.yml does not install it: without it
# `tools/new-capability.Tests.ps1` reports its formatting cases as skipped, and a check that
# only ever skips is not a check. With it the scaffold is verified against `.clang-format` on
# every local Linux run. It is Ubuntu's clang-format 18, older than the 19.1.5 and 22.1.3 that
# 08 §8.5 records as measured; if a future template disagrees with 18 alone, that is a third
# data point for that paragraph and not a reason to drop the package.
#
# cmake comes from Ubuntu (3.28.3). CMakePresets.json declares
# `cmakeMinimumRequired` 3.28.0 and uses preset schema version 8, which is exactly CMake 3.28,
# so the distribution package is both new enough and the *floor* the presets promise — a
# stricter test than the hosted runner, whose image ships a newer CMake that would hide a
# feature we accidentally started depending on. It also keeps us on the 3.x line, where the
# third-party CMakeLists files that declare `cmake_minimum_required` below 3.5 still configure.
RUN apt-get update && apt-get install -y --no-install-recommends \
      ca-certificates curl gnupg \
      build-essential cmake ninja-build clang clang-format g++ git rsync \
      pkg-config file unzip xz-utils \
 && rm -rf /var/lib/apt/lists/*

# --- PowerShell 7, from Microsoft's apt repository ---------------------------------------------
#
# `tools/` is PowerShell 7 by policy (ADR-0021, 08 §8.5), and six CTest tests are pwsh scripts:
# lint.banned_patterns, tools.lint, tools.new_capability, docs_check, tools.docs_check and
# tools.machine_lock. A
# container without pwsh does not fail those tests, it silently does not register them
# (cmake/EngineTesting.cmake warns and moves on), which would make a green local run mean less
# than the hosted one. So pwsh is part of the toolchain, not an extra.
#
# The version is pinned. `powershell` in this repository tracks 7.4 LTS; bump the pin and the
# base digest in the same commit so the image tag changes once.
ARG POWERSHELL_VERSION=7.4.12-1.deb
RUN curl -fsSL https://packages.microsoft.com/config/ubuntu/24.04/packages-microsoft-prod.deb \
      -o /tmp/packages-microsoft-prod.deb \
 && dpkg -i /tmp/packages-microsoft-prod.deb \
 && rm -f /tmp/packages-microsoft-prod.deb \
 && apt-get update \
 && apt-get install -y --no-install-recommends "powershell=${POWERSHELL_VERSION}" \
 && rm -rf /var/lib/apt/lists/*

# --- A non-root build user ---------------------------------------------------------------------
#
# uid 1000 is taken by Ubuntu's own `ubuntu` account in 24.04, so the build user takes 1001.
# Nothing here writes to the host: the checkout is mounted read-only and everything else lives
# in named volumes the daemon owns, so the uid only has to be stable, not matched to anyone.
#
# /fetch is the machine-wide dependency cache (tools/ci/fetch-cache.cmake). It has to exist in the
# image, owned by the build user, because an empty named volume takes its mount point's owner
# from the image the first time it is mounted: without it the volume's root is root's and the
# first configure cannot write the cache.
RUN groupadd --gid 1001 build \
 && useradd --uid 1001 --gid 1001 --create-home --shell /bin/bash build \
 && mkdir -p /src /deps /fetch \
 && chown build:build /src /deps /fetch

USER build
WORKDIR /src

# `tools/docs-gate.test.sh` builds throwaway git repositories and commits to them, which needs an
# identity and, since CMake's FetchContent clones into directories this user did not create in
# every layout, a permissive safe.directory. Both are container-local.
RUN git config --global user.email "build@localhost" \
 && git config --global user.name "engine container build" \
 && git config --global init.defaultBranch main \
 && git config --global --add safe.directory '*'

# A build tree that outlives the container: /src is the synced source (plus build/<preset>/),
# /deps is FETCHCONTENT_BASE_DIR (the dependencies' build directories), both per checkout, and
# /fetch is the dependencies' downloaded sources, shared by every checkout. linux-build.ps1 mounts
# a named volume on each.
VOLUME ["/src", "/deps", "/fetch"]

CMD ["/bin/bash"]

# --- headless: the toolchain and nothing that could draw ----------------------------------------
#
# Deliberately empty. Every display development package is in `desktop` below, so this target is
# `base` — and `base` is a machine on which `X11/Xlib.h`, `wayland-client.h`, `xkbcommon/*`,
# `EGL/egl.h`, `GL/gl.h`, `gbm.h` and `libudev.h` do not exist. `linux-server` and
# `linux-server-debug` are built here; if `ENGINE_WINDOW_BACKENDS=none` ever quietly starts
# needing one of those headers again, this target stops building and says which.
#
# **libudev-dev is absent too, on purpose.** SDL's `CheckLibUDev` is a soft check — no header
# means `HAVE_LIBUDEV_H` stays off and SDL polls `/dev/input` instead of subscribing to udev — and
# "soft" is a claim worth a build rather than a reading of somebody's CMake. The Sandy Bridge
# server *has* libudev, so this container is the only place the absent case is exercised.
FROM base AS headless

# --- desktop: what ci.yml's hosted runner has ---------------------------------------------------
#
# The X11, Wayland, Mesa and libdrm entries are **development headers only**, and SDL3's Unix
# configure refuses to proceed without them unless it is told to (`ENGINE_WINDOW_BACKENDS=none`,
# cmake/EngineGraphics.cmake). Nothing in this image starts an X server or a Wayland compositor,
# and no display ever exists. At run time `window::init()` fails, the window and swapchain tests
# skip, and `engine-view` exits 3 — which is what the headless Linux GPU runner does
# (docs/ci/self-hosted-runners.md) and what the tests are written to treat as a skip. Do not read
# this list as "the container has a GUI".
FROM base AS desktop
USER root
RUN apt-get update && apt-get install -y --no-install-recommends \
      libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxfixes-dev libxss-dev \
      libwayland-dev libxkbcommon-dev libegl1-mesa-dev libgl1-mesa-dev libdrm-dev libgbm-dev \
      libxtst-dev libdecor-0-dev libudev-dev \
 && rm -rf /var/lib/apt/lists/*
USER build
