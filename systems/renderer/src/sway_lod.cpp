// **A storm's padding is the culling's, not the cut's** (R206; renderer.md, "What a gale builds";
// `gfx::CullParams::sway_lod`, cluster_cull.slang). The padding ladder (W154-2) writes a swaying
// instance's record at its rung so that every test a pair takes holds the sway drawn; the cull pass
// added the same padding to the LOD spheres, so a gale's rung made every swaying tree's cut as fine
// as if each tip were swung at the camera. This table caps the LOD spheres' padding at the calm
// rung's, per mesh, so the cut is the one a calm session draws in any wind.
#include <domain/gfx/resources.h>
#include <foundation/tunables/tunables.h>
#include <systems/renderer/flora_motion.h>
#include <systems/renderer/gpu_scene.h>

#include <algorithm>
#include <cstring>
#include <limits>

namespace engine::renderer {
namespace {

// A mesh no swaying instance has: its deformed instances (skinned, morphed, a terrain level) keep
// their whole padding on the LOD spheres, as before.
constexpr f32 k_no_cap = 3.0e38f;

tunables::Bool calm_cut{
    "renderer.flora.calm_cut", true,
    "A swaying instance's LOD spheres take at most its calm rung's padding, so a "
    "storm's cut is a calm session's (R206); false: the whole rung's padding, "
    "as W154-2 left it, the cut finer as the wind rises"};

}  // namespace

// Made with the pose table, at the load (never in a frame): a region a slot, a float a mesh.
bool GpuScene::make_sway_caps(std::string* error) {
  gfx::destroy_buffer(*device_, sway_caps_);
  // **Every mesh that can sway, and no more** (R206-H1). A scene names meshes past its parts — a
  // terrain ring's slots (`parts + slot parts + s`), the mesh room's tiles — and those are deformed
  // too, but none sways: a tree's record, a field's slot and a tail tree each need the mesh under
  // `flora_meshes_`, which is sized to the parts at the load. The table covers the parts (and the
  // holes' null mesh), `flora_meshes_` and any mesh a caller's padding was noted for, so a swaying
  // mesh is under the count by construction; the cull reads nothing past the count
  // (`CullParams::sway_lod_count`), and a mesh there keeps its whole padding. Sizing it to every
  // index a scene can name was not chosen: the rings' and the mesh room's indices change with a
  // relayout and with streaming, which would remake the table in a frame, for meshes it caps none
  // of.
  sway_cap_count_ = std::max({static_cast<u32>(data_->parts.size()) + 1u,
                              static_cast<u32>(flora_meshes_.size()),
                              static_cast<u32>(sway_base_max_.size())});
  if (sway_base_max_.size() < sway_cap_count_) sway_base_max_.resize(sway_cap_count_, 0.0f);
  const u64 bytes = u64{k_joint_slots} * sway_cap_count_ * sizeof(f32);
  if (!gfx::create_buffer(*device_, bytes,
                          gfx::BufferUsage::Storage | gfx::BufferUsage::ShaderDeviceAddress, true,
                          sway_caps_, error)) {
    return false;
  }
  std::memset(sway_caps_.mapped, 0, bytes);
  ++sway_cap_version_;  // every region stale
  sway_lod_on_ = false;
  return true;
}

// A swaying instance is placed with a caller's padding `base` (its record holds the larger of that
// and the rung's sway bound): the mesh's cap is never under it, so the cap takes from no instance a
// padding its calm record would hold. A running most, never lowered: a cap above the calm bound
// only leaves that mesh's cut finer.
void GpuScene::note_sway_base(u32 mesh, f32 base) noexcept {
  if (mesh >= sway_base_max_.size()) {
    // Only before the table is made (a load's trees are placed first, and the table then covers
    // their meshes); never in a frame, since a tail tree's mesh is under `flora_meshes_`, which the
    // table covers. A mesh noted past a made table would go uncapped (its whole padding: a finer
    // cut, never a wrong one), so a debug build says so.
    ENGINE_ASSERT(sway_caps_.mapped == nullptr, "a swaying mesh past the LOD caps' table");
    sway_base_max_.resize(mesh + 1, 0.0f);
  }
  if (base > sway_base_max_[mesh]) {
    sway_base_max_[mesh] = base;
    ++sway_cap_version_;
  }
}

// The calm rung's padding of a swaying mesh (`flora_padding_of` at rung 0: a distance form's is its
// source's), or more where a caller padded an instance of it more; no cap for any other mesh.
f32 GpuScene::sway_cap_of(u32 mesh) const noexcept {
  if (mesh >= flora_meshes_.size()) return k_no_cap;
  const u32 source_mesh = mesh < data_->form_source.size() ? data_->form_source[mesh] : mesh;
  const u32 m = source_mesh < flora_meshes_.size() && flora_meshes_[source_mesh].part_count() > 0
                    ? source_mesh
                    : mesh;
  // Only a mesh the sway measured: a skinned, morphed or terrain mesh has no sway bound (0), and
  // its instances keep their whole padding.
  if (m >= flora_padding_.size() || !(flora_padding_[m] > 0.0f)) return k_no_cap;
  const f32 calm = flora_rung_padding(flora_padding_[m], flora_meshes_[m].field,
                                      flora_rung_load(0, flora_ladder_top_));
  const f32 base = mesh < sway_base_max_.size() ? sway_base_max_[mesh] : 0.0f;
  return std::max(calm, base);
}

// Once a frame, from `flora_frame`: on while the ladder stands above its calm rung or walks (a
// record may then hold more than the calm padding), its slot's region rewritten only when stale.
void GpuScene::write_sway_caps(u32 slot) noexcept {
  sway_lod_on_ = sway_caps_.mapped != nullptr && slot < k_joint_slots && calm_cut.get() &&
                 (flora_ladder_.hold > 0 || flora_ladder_.target > 0 || ladder_walking_);
  if (!sway_lod_on_ || sway_cap_written_[slot] == sway_cap_version_) return;
  f32* caps = reinterpret_cast<f32*>(static_cast<u8*>(sway_caps_.mapped) +
                                     u64{slot} * sway_cap_count_ * sizeof(f32));
  for (u32 m = 0; m < sway_cap_count_; ++m)
    caps[m] = sway_cap_of(m);
  sway_cap_written_[slot] = sway_cap_version_;
}

// cluster_cull.slang's read, on the CPU: `sway_lod != 0 && mesh < sway_lod_count`, then the
// region's float; anything else is no cap.
f32 GpuScene::sway_lod_cap(u32 slot, u32 mesh) const noexcept {
  if (sway_lod_address(slot) == 0 || slot >= k_joint_slots || mesh >= sway_lod_count())
    return std::numeric_limits<f32>::infinity();
  const f32* caps = reinterpret_cast<const f32*>(static_cast<const u8*>(sway_caps_.mapped) +
                                                 u64{slot} * sway_cap_count_ * sizeof(f32));
  return caps[mesh];
}

}  // namespace engine::renderer
