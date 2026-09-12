# Third-party components

Every dependency of the engine is listed here with its license. Policy: permissive licenses only (MIT, BSD, zlib, Apache-2.0, BSL-1.0, public domain); LGPL only as a dynamically linked, user-replaceable library; no GPL/AGPL/SSPL; proprietary SDKs only as optional developer-supplied plugins outside this repository. See docs/plan/08-toolchain.md section 8.8 and ADR-0014.

| Component | Version | License | Used by | How obtained |
|---|---|---|---|---|
| doctest | v2.4.11 | MIT | tests only | CMake FetchContent at configure time |
