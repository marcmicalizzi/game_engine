// The scene's generator consumers in their declared order (scene_consumers.h).
#include <systems/renderer/scene_renderer.h>
#include <systems/world/scene_consumers.h>

namespace engine::world {

namespace {

bool mesh_fits(void* context, u32 clusters, u32 materials) {
  return static_cast<renderer::SceneRenderer*>(context)->scene().mesh_fits(clusters, materials);
}
bool mesh_admit(void* context, u32 mesh, const renderer::PlacementMeshData& data,
                std::string* error) {
  return static_cast<renderer::SceneRenderer*>(context)->admit_mesh(mesh, data, error);
}
void mesh_retire(void* context, u32 mesh) {
  (void)static_cast<renderer::SceneRenderer*>(context)->retire_mesh(mesh);
}
TileMeshSink::Texture mesh_texture(void* context, const renderer::PlacementTexture& texture,
                                   std::string* why) {
  switch (static_cast<renderer::SceneRenderer*>(context)->acquire_texture(texture, why)) {
    case renderer::GpuScene::TextureAdmit::held: return TileMeshSink::Texture::held;
    case renderer::GpuScene::TextureAdmit::need_bytes: return TileMeshSink::Texture::need_bytes;
    case renderer::GpuScene::TextureAdmit::refused: break;
  }
  return TileMeshSink::Texture::refused;
}
void mesh_release_texture(void* context, u64 key) {
  static_cast<renderer::SceneRenderer*>(context)->release_texture(key);
}
bool mesh_texture_resident(void* context, u64 key) {
  return static_cast<renderer::SceneRenderer*>(context)->texture_resident(key);
}

}  // namespace

void connect_scene_meshes(PlacementTiles& placements, renderer::SceneRenderer& renderer,
                          jobs::JobSystem* jobs) {
  // The room is the renderer's, as its scene made it; the pace is the world's tunables.
  const renderer::GpuScene::MeshRoom& room = renderer.scene().mesh_room();
  TileMeshConfig config;
  config.first_mesh = room.first_mesh;
  config.slots = room.slots;
  config.clusters = room.clusters;
  config.max_clusters = room.max_clusters;
  config.cluster_bytes = room.cluster_bytes;
  config.linger = mesh_linger_tunable();
  config.admissions = mesh_admissions_tunable();
  config.admission_bytes = mesh_admission_bytes_tunable();
  placements.set_meshes(config,
                        TileMeshSink{&renderer, &mesh_fits, &mesh_admit, &mesh_retire,
                                     &mesh_texture, &mesh_release_texture, &mesh_texture_resident},
                        jobs);
}

void add_scene_consumers(World& world, GroundTiles* ground, PlacementTiles* placements) {
  // Ground first: activation calls consumers in registration order, so a tile's ground is built
  // before the placements that stand on it, and deactivation, in reverse, drops them first.
  if (ground != nullptr && ground->valid()) world.add_consumer(ground->consumer());
  if (placements != nullptr) {
    placements->set_ring(&world.ring());
    world.add_consumer(placements->consumer());
  }
}

}  // namespace engine::world
