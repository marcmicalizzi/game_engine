# Subsystem pages

One page per module, created in the same change that creates the module. Template:

```markdown
# <module> (<layer>)

**Purpose.** One paragraph.

**Owned data.** What this module is the source of truth for. Nothing else may hold or mutate it.

**Invariants.** Bullet list. Each is checked by `validate()` in debug builds and by tests.

**Public API.** The headers under `include/<module>/` and what each is for.

**Depends on.** Modules (must match `engine_module(... DEPS ...)`).

**Testing.** How to run the module's tests and benchmarks; what the size table covers.

**Performance notes.** Hot paths, layout decisions, tunables.
```

| Module | Layer | Page |
|---|---|---|
| base | core | [base.md](base.md) |
| hash | core | [hash.md](hash.md) |
| memory | core | [memory.md](memory.md) |
| containers | core | [containers.md](containers.md) |
| platform | core | [platform.md](platform.md) |
| jobs | core | [jobs.md](jobs.md) |
