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
| profiling | core | [profiling.md](profiling.md) |
| containers | core | [containers.md](containers.md) |
| time | core | [time.md](time.md) |
| math | core | [math.md](math.md) |
| ids | core | [ids.md](ids.md) |
| json | core | [json.md](json.md) |
| schema (+ tools/schemac) | core | [schema.md](schema.md) |
| platform | core | [platform.md](platform.md) |
| log | core | [log.md](log.md) |
| jobs | core | [jobs.md](jobs.md) |
| tunables | foundation | [tunables.md](tunables.md) |
| io | foundation | [io.md](io.md) |
| window | foundation | [window.md](window.md) |
| image | foundation | [image.md](image.md) |
| input | foundation | [input.md](input.md) |
| bench | foundation | [bench.md](bench.md) |
| store (capability) | foundation | [store.md](store.md) |
| doc | domain | [doc.md](doc.md) |
| geometry | domain | [geometry.md](geometry.md) |
| assets | domain | [assets.md](assets.md) |
| gfx | domain | [gfx.md](gfx.md) |
| protocol | domain | [protocol.md](protocol.md) |
| ecs (capability) | domain | [ecs.md](ecs.md) |
| physics (capability) | domain | [physics.md](physics.md) |
| nav (capability) | domain | [nav.md](nav.md) |
| sim | domain | [sim.md](sim.md) |
| renderer | systems | [renderer.md](renderer.md) |
| engine_host, engine_cli, engine_view, engine_content, engine_input, engine_image | apps | [apps.md](apps.md) |
