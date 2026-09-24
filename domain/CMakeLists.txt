# Layer L2: domain. Modules here own one kind of data behind a schema-declared interface and
# depend only on core and generated schemas.
add_subdirectory(doc)
add_subdirectory(geometry)
add_subdirectory(anim)
add_subdirectory(assets)
# After assets: the atlas step rewrites an imported mesh (`assets::MeshData`) in place.
add_subdirectory(atlas)
# After geometry: the built texture, whose per-image records a `.clusters` container carries.
add_subdirectory(texture)
add_subdirectory(gfx)
add_subdirectory(protocol)
# sim before ecs: ADR-0028 seam 2 makes `ecs::TickPhase` an alias of `sim::TickPhase`, so the
# scheduler's module is declared first and the entity store depends on it, never the other way
# round — `domain/sim` has to stay free of flecs for the executor to be replaceable.
add_subdirectory(sim)
add_subdirectory(ecs)
add_subdirectory(physics)
add_subdirectory(nav)
# After geometry (the limit surface and the binding) and physics (the cage-size verdict): the
# tissue definition and its validators.
add_subdirectory(tissue)
# After protocol (audio.devices): the device and mixing half of audio. Its ECS half is
# systems/audio_system, because flecs belongs to systems/ (ADR-0028 seam 5).
add_subdirectory(audio)
