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
#include <systems/water/water.h>

#include <filesystem>
#include <memory>
#include <string>

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
    const std::span<const u8> bytes(glb.data(), glb.size());
    scene_gen::PlacementMesh placed;
    placed.path = io::join_path(mesh_dir, mesh_file_name(bytes));
    placed.name = "water/" + (body.name.empty() ? std::to_string(k) : body.name);
    if (!io::exists(placed.path)) {
      if (io::make_directories(mesh_dir) != io::Status::Ok) {
        if (error != nullptr) *error = entry.where + ": cannot create " + mesh_dir;
        return false;
      }
      const io::Status status = io::write_file_atomic(
          placed.path, std::string_view(reinterpret_cast<const char*>(glb.data()), glb.size()));
      if (status != io::Status::Ok) {
        if (error != nullptr)
          *error = entry.where + ": cannot write " + placed.path + ": " + io::status_name(status);
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

}  // namespace

}  // namespace engine::water
