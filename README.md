# Agent-Native High-Performance Game Engine

An engine built from first principles around four priorities: rendering efficiency and fidelity (cluster geometry, hybrid ray tracing, extreme resolutions), hardware-aware execution, agent-native development tooling where every developer operation is a structured API, and persistent, systemic, physically reactive worlds.

Status: Phase 0 (foundations). Nothing renders yet.

- **Plan**: [docs/plan/README.md](docs/plan/README.md) is the first-pass technical plan and the source of intent.
- **Decisions**: [docs/adr/](docs/adr/) holds the architecture decision records.
- **Working in this repo**: [AGENTS.md](AGENTS.md) covers build, test, layering, and conventions for humans and agents alike.

## Build

Requirements: Visual Studio 2026 with the C++ workload (bundled CMake and Ninja are used automatically), PowerShell 7. On Linux: Clang 18+ or GCC 14+, CMake 3.28+, Ninja.

```powershell
tools/dev.ps1 configure
tools/dev.ps1 build
tools/dev.ps1 test
```

## License

Apache-2.0. See [LICENSE](LICENSE) and [NOTICE](NOTICE). Games built with the engine owe attribution only.
