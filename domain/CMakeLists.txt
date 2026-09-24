# Layer L2: domain. Modules here own one kind of data behind a schema-declared interface and
# depend only on core and generated schemas.
add_subdirectory(doc)
add_subdirectory(geometry)
add_subdirectory(anim)
add_subdirectory(assets)
# After assets: the atlas step rewrites an imported mesh (`assets::MeshData`) in place.
add_subdirectory(atlas)
add_subdirectory(gfx)
add_subdirectory(protocol)
# sim before ecs: ADR-0028 seam 2 makes `ecs::TickPhase` an alias of `sim::TickPhase`, so the
# scheduler's module is declared first and the entity store depends on it, never the other way
# round — `domain/sim` has to stay free of flecs for the executor to be replaceable.
add_subdirectory(sim)
add_subdirectory(ecs)
add_subdirectory(physics)
add_subdirectory(nav)
