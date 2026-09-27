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
# After atlas and texture: the content build calls the atlas step and the texture build, so it
# sits above geometry, assets, atlas and texture, and engine-content and engine-host both link it.
add_subdirectory(content_build)
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
# After geometry (a ground's ring chunks are cluster LOD DAGs), before every generator: the registry
# the ground providers and placement generators below register into, and the renderer's scene reader
# and the world ring look them up in (ADR-0046).
add_subdirectory(scene_gen)
add_subdirectory(ruins)
add_subdirectory(terrain)
add_subdirectory(city)
