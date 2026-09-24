#pragma once

// Which texture each image of a mesh becomes (docs/subsystems/texture.md, "Formats per slot"):
// the roles the materials give an image, the build options those roles ask for, and the record a
// `.clusters` container keeps per image so that the renderer finds the built texture from the
// container alone (geometry::ClusterFileTexture, section kind 32).
//
// This is the one place the per-slot defaults live. `engine-content build` records them and
// builds from them, `engine-view` records them when it writes a cache entry of its own, and the
// renderer reads them back — three callers that must agree byte for byte, because the record is
// part of a container whose bytes are pinned (geometry.md, "The same bytes from every
// toolchain").

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <domain/geometry/cluster_file.h>
#include <domain/texture/texture_build.h>

#include <span>

namespace engine::texture {

// geometry::ClusterFileTexture::roles: which material slots name the image.
inline constexpr u32 k_role_base_color = 1u;
inline constexpr u32 k_role_normal = 2u;
inline constexpr u32 k_role_metallic_roughness = 4u;
inline constexpr u32 k_role_occlusion = 8u;
inline constexpr u32 k_role_emissive = 16u;

// The roles every image plays across `materials`, parallel to the images (`image_count` of them).
// A slot naming an image past the end is ignored here; the container reader refuses it anyway.
void image_roles(std::span<const geometry::ClusterFileMaterial> materials, u32 image_count,
                 Vector<u32>& out);

// The per-slot defaults, as build options, and zero options (`pack_texture_options` never gives
// zero) for an image no slot names:
//
//   base colour, emissive   BC7, sRGB   colour a person looks at; BC7 is the only desktop format
//                                       that keeps it at 8 bits a texel without BC1's banding
//   normal                  BC5         x and y at 8 bits each through two independent BC4 blocks;
//                                       z is reconstructed in the shader (material.slang)
//   metallic-roughness      BC7, linear glTF packs G roughness and B metallic, and exporters put
//                                       occlusion in R of the same image ("ORM"); BC7 keeps all
//                                       three where the shader reads them, at BC5's size
//   occlusion alone         BC4         one channel, R, half of BC7's size
//
// An image given two roles takes the first of that list it has, so an ORM image named by both the
// metallic-roughness and the occlusion slot is BC7 linear and serves both.
TextureBuildOptions options_for_roles(u32 roles) noexcept;

// Fills `data.textures`, parallel to `data.image_paths`, from `data.materials` and the embedded
// bytes in `data.images`: the roles, the packed options, and — for an image whose bytes the
// container carries — the source hash and the cache key. An image named by path records options
// and roles and a zero key, because its bytes are not the container's and the container has to be
// a function of what its own key covers (the glTF and its buffers): the renderer and the build
// hash the file when they need the key. An image no slot names records all zeros.
void fill_cluster_texture_records(geometry::ClusterFileData& data);

// The same for images held as `assets::ImageRef`-shaped pairs, for a caller that has no
// ClusterFileData yet: `embedded[i]` is image i's bytes, empty for an image named by path.
void cluster_texture_records(std::span<const geometry::ClusterFileMaterial> materials,
                             std::span<const std::span<const u8>> embedded,
                             Vector<geometry::ClusterFileTexture>& out);

}  // namespace engine::texture
