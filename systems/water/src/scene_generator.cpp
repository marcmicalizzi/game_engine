// The water bodies as a placement generator (scene_gen.h; docs/subsystems/water.md, "Where it
// enters the renderer"): the renderer's scene reader hands a scene's `water` block to the generator
// named "water" (systems/renderer/src/scene_water.cpp), and `expand` makes one mesh and one
// placement per body, in the block's order. The meshes are GLB files written once, each under the
// hash of its bytes, into the repository's derived-data root (`<repo>/ddc/water`, found from the
// scene's directory as the renderer's cache is), or `water` beside a scene outside any checkout;
// the renderer's derived-data cache clusters each like any mesh. Nothing else of the renderer is
// named here, and the renderer names nothing of this capability but the generator's name.
#include <core/schema/json_reflect.h>
#include <domain/geometry/cluster_file.h>
#include <domain/scene_gen/scene_gen.h>
#include <foundation/io/vfs.h>
#include <systems/water/channel.h>
#include <systems/water/water.h>

#include <filesystem>
#include <initializer_list>
#include <memory>
#include <string>
#include <utility>

namespace engine::water {

namespace {

struct Entry {
  std::string where;
  std::string dir;
  Vector<scene::WaterBody> blocks;
  Vector<Body> bodies;
};

std::string schema_errors(const schema::ReadContext& ctx) {
  std::string text;
  for (const schema::Diagnostic& d : ctx.diagnostics) {
    if (!text.empty()) text += "; ";
    text += d.path.empty() ? d.message : d.path + ": " + d.message;
  }
  return text.empty() ? std::string("not a water body") : text;
}

std::string resolve(std::string_view dir, const std::string& path) {
  return io::is_absolute_path(path) || dir.empty() ? path : io::join_path(dir, path);
}

bool open(const JsonValue& params, const scene_gen::Context& context, void** state,
          std::string* error) {
  auto entry = std::make_unique<Entry>();
  entry->where = std::string(context.where);
  entry->dir = std::string(context.dir);
  const JsonValue* bodies = params.find("bodies");
  if (bodies == nullptr || !bodies->is_array()) {
    if (error != nullptr)
      *error = entry->where + ": the water generator's params have no \"bodies\"";
    return false;
  }
  for (usize i = 0; i < bodies->size(); ++i) {
    scene::WaterBody block;
    schema::ReadContext ctx;
    if (!schema::from_json(block, (*bodies)[i], ctx) || !ctx.ok()) {
      if (error != nullptr)
        *error = entry->where + " " + std::to_string(i) + ": " + schema_errors(ctx);
      return false;
    }
    Body body;
    std::string why;
    if (!read_body(block, body, &why)) {
      if (error != nullptr) *error = entry->where + " " + std::to_string(i) + ": " + why;
      return false;
    }
    // One open body a scene (water.md, "To the horizon"): the resolve draws the sea past its
    // outline as one plane at its level, and a second would be a second sea at another level.
    if (body.open) {
      for (const Body& earlier : entry->bodies) {
        if (!earlier.open) continue;
        if (error != nullptr)
          *error = entry->where + " " + std::to_string(i) + ": water body \"" + body.name +
                   "\" is open, and so is \"" + earlier.name +
                   "\": a scene has at most one open body, whose sea is one plane at its level";
        return false;
      }
    }
    entry->blocks.push_back(std::move(block));
    entry->bodies.push_back(std::move(body));
  }
  *state = entry.release();
  return true;
}

void close(void* state) noexcept { delete static_cast<Entry*>(state); }

bool expand(void* state, const scene_gen::Context& context, scene_gen::Placements& out,
            std::string* error) {
  const Entry& entry = *static_cast<const Entry*>(state);
  // Where the meshes go: the host's derived-data root (`Context::ddc`, `--ddc`), so a scene in a
  // game repository writes them where its host keeps derived data; with none named, rock.cpp's
  // rule — the repository's root found from the scene's directory, else `water` beside the scene,
  // which is where a scene outside any checkout (a test's scratch directory) keeps it.
  std::string ddc(context.ddc);
  if (ddc.empty()) {
    std::error_code ec;
    const std::filesystem::path from = std::filesystem::absolute(
        std::filesystem::path(entry.dir.empty() ? std::string(".") : entry.dir), ec);
    if (!ec) ddc = geometry::find_ddc_root(from.generic_string());
  }
  const std::string mesh_dir =
      !ddc.empty() ? io::join_path(ddc, "water") : resolve(entry.dir, "water");
  // The depth is the surface's as the ground stands now: the moving ground's, when the host's has
  // moved on from the scene's time, and the load's either way — a body over migrating dunes keeps
  // the depths of the read until the scene is read again (water.md, "The mesh").
  const scene_gen::Ground& ground = context.stand.present() ? context.stand : context.ground;
  for (u32 k = 0; k < entry.bodies.size(); ++k) {
    const Body& body = entry.bodies[k];
    const scene::WaterBody& block = entry.blocks[k];
    Mesh mesh;
    build_mesh(body, ground, mesh);
    const Vec3 shallow = block.shallow.has_value() ? *block.shallow : Vec3{0.30f, 0.55f, 0.50f};
    const Vector<u8> glb = mesh_glb(mesh, shallow, block.roughness);
    // The depth map beside it (water.md, "The depth map"): the same ground at a quarter of the
    // mesh's spacing, under one name with the mesh, so the reader finds it from the mesh's path.
    DepthMap map;
    build_depth_map(body, mesh, ground, map);
    const Vector<u8> map_file = depth_map_bytes(map);
    const std::span<const u8> bytes(glb.data(), glb.size());
    const std::span<const u8> map_bytes(map_file.data(), map_file.size());
    const std::string stem = io::join_path(mesh_dir, body_file_stem(bytes, map_bytes));
    scene_gen::PlacementMesh placed;
    placed.path = stem + ".glb";
    placed.name = "water/" + (body.name.empty() ? std::to_string(k) : body.name);
    // The map first and the mesh last: a mesh under its name means both are there.
    const std::string map_path = stem + ".depth";
    for (const auto& [path, file] :
         {std::pair<const std::string*, std::span<const u8>>{&map_path, map_bytes},
          std::pair<const std::string*, std::span<const u8>>{&placed.path, bytes}}) {
      if (io::exists(*path)) continue;
      if (io::make_directories(mesh_dir) != io::Status::Ok) {
        if (error != nullptr) *error = entry.where + ": cannot create " + mesh_dir;
        return false;
      }
      const io::Status status = io::write_file_atomic(
          *path, std::string_view(reinterpret_cast<const char*>(file.data()), file.size()));
      if (status != io::Status::Ok) {
        if (error != nullptr)
          *error = entry.where + ": cannot write " + *path + ": " + io::status_name(status);
        return false;
      }
    }
    out.meshes.push_back(std::move(placed));
    scene_gen::Placement placement;
    placement.position = body.center;
    placement.mesh = k;
    out.instances.push_back(placement);
  }
  return true;
}

constexpr scene_gen::PlacementGeneratorDesc k_water{
    .name = "water", .open = &open, .close = &close, .expand = &expand};
const scene_gen::Registrar k_water_registrar{k_water};

// **Channels** (channel.h; water.md, "Channels"; roadmap S96): the scene's `water_channels`, which
// the renderer's reader hands to the generator "water_channel" after the bodies, as
// `{"channels": [...]}`. One mesh and one placement per channel, in order — mesh k and instance k
// are channel k — the instance at the channel's origin, unrotated. The files are a body's: the GLB
// and the depth map beside it under one hash of both, in the same directory.
struct ChannelEntry {
  std::string where;
  std::string dir;
  Vector<Channel> channels;
};

bool open_channels(const JsonValue& params, const scene_gen::Context& context, void** state,
                   std::string* error) {
  auto entry = std::make_unique<ChannelEntry>();
  entry->where = std::string(context.where);
  entry->dir = std::string(context.dir);
  const JsonValue* channels = params.find("channels");
  if (channels == nullptr || !channels->is_array()) {
    if (error != nullptr)
      *error = entry->where + ": the water channel generator's params have no \"channels\"";
    return false;
  }
  for (usize i = 0; i < channels->size(); ++i) {
    scene::WaterChannel block;
    schema::ReadContext ctx;
    if (!schema::from_json(block, (*channels)[i], ctx) || !ctx.ok()) {
      if (error != nullptr)
        *error = entry->where + " " + std::to_string(i) + ": " + schema_errors(ctx);
      return false;
    }
    Channel channel;
    std::string why;
    if (!read_channel(block, channel, &why)) {
      if (error != nullptr) *error = entry->where + " " + std::to_string(i) + ": " + why;
      return false;
    }
    entry->channels.push_back(std::move(channel));
  }
  *state = entry.release();
  return true;
}

void close_channels(void* state) noexcept { delete static_cast<ChannelEntry*>(state); }

bool expand_channels(void* state, const scene_gen::Context& context, scene_gen::Placements& out,
                     std::string* error) {
  const ChannelEntry& entry = *static_cast<const ChannelEntry*>(state);
  std::string ddc(context.ddc);
  if (ddc.empty()) {
    std::error_code ec;
    const std::filesystem::path from = std::filesystem::absolute(
        std::filesystem::path(entry.dir.empty() ? std::string(".") : entry.dir), ec);
    if (!ec) ddc = geometry::find_ddc_root(from.generic_string());
  }
  const std::string mesh_dir =
      !ddc.empty() ? io::join_path(ddc, "water") : resolve(entry.dir, "water");
  for (u32 k = 0; k < entry.channels.size(); ++k) {
    const Channel& channel = entry.channels[k];
    ChannelMesh mesh;
    build_channel_mesh(channel, mesh);
    const Vector<u8> glb = mesh_glb(mesh.mesh, Vec3{0.30f, 0.55f, 0.50f}, 0.08f);
    DepthMap map;
    build_channel_depth_map(channel, mesh, map);
    const Vector<u8> map_file = depth_map_bytes(map);
    const std::span<const u8> bytes(glb.data(), glb.size());
    const std::span<const u8> map_bytes(map_file.data(), map_file.size());
    const std::string stem = io::join_path(mesh_dir, body_file_stem(bytes, map_bytes));
    scene_gen::PlacementMesh placed;
    placed.path = stem + ".glb";
    placed.name = "water/" + (channel.name.empty() ? "channel" + std::to_string(k) : channel.name);
    const std::string map_path = stem + ".depth";
    for (const auto& [path, file] :
         {std::pair<const std::string*, std::span<const u8>>{&map_path, map_bytes},
          std::pair<const std::string*, std::span<const u8>>{&placed.path, bytes}}) {
      if (io::exists(*path)) continue;
      if (io::make_directories(mesh_dir) != io::Status::Ok) {
        if (error != nullptr) *error = entry.where + ": cannot create " + mesh_dir;
        return false;
      }
      const io::Status status = io::write_file_atomic(
          *path, std::string_view(reinterpret_cast<const char*>(file.data()), file.size()));
      if (status != io::Status::Ok) {
        if (error != nullptr)
          *error = entry.where + ": cannot write " + *path + ": " + io::status_name(status);
        return false;
      }
    }
    out.meshes.push_back(std::move(placed));
    scene_gen::Placement placement;
    placement.position = channel.origin;
    placement.mesh = k;
    out.instances.push_back(placement);
  }
  return true;
}

constexpr scene_gen::PlacementGeneratorDesc k_water_channel{.name = "water_channel",
                                                            .open = &open_channels,
                                                            .close = &close_channels,
                                                            .expand = &expand_channels};
const scene_gen::Registrar k_water_channel_registrar{k_water_channel};

}  // namespace

}  // namespace engine::water
