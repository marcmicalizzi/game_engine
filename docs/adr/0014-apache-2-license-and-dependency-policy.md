# ADR-0014: Apache-2.0 engine license and the dependency policy that follows

- **Status:** Accepted
- **Date:** 2026-09-12
- **Plan references:** docs/plan/08-toolchain.md §8.8

## Context

The owner wants games built on the engine to owe nothing but attribution and to be free to remain closed source, and rejects GPL outright. A permissive license constrains which dependencies may be linked or redistributed.

## Decision

The engine is licensed under Apache-2.0 (chosen over MIT for its explicit patent grant). Dependency policy: MIT, BSD, zlib, Apache-2.0, BSL-1.0, and public-domain code may be vendored or linked; LGPL only as a dynamically linked, user-replaceable library and avoided where a permissive alternative exists; GPL, AGPL, SSPL, and other copyleft or source-available licenses are not allowed; proprietary SDKs (NVIDIA RTX SDKs License, DLSS/FSR/XeSS binaries, DirectStorage) never enter the repository or the engine binary and are exposed only as optional plugin modules the game developer fetches under the vendor's terms; model weights only under licenses permitting redistribution and commercial use. A license check runs in CI. Every dependency is recorded in `third_party/LICENSES.md`.

## Consequences

ReSTIR and a baseline denoiser are implemented in-house. Vendor upscalers and neural denoisers are plugins. The engine is fully functional without any proprietary component.

## Revisit when

Not planned.
