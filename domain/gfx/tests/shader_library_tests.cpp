// SPIR-V reflection against the shaders this module ships, the library's embedded and file
// paths (through the build's manifest), and hot reload through slangc with a temporary shader.
// Everything but module creation runs without a GPU, so CI covers the compiler round trip.
#include <domain/gfx/device.h>
#include <domain/gfx/shader_library.h>
#include <foundation/io/vfs.h>

#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <shaders/bindless_copy.spv.h>
#include <shaders/cluster_mesh.spv.h>
#include <shaders/fill.spv.h>
#include <shaders/scale.spv.h>
#include <shaders/triangle.spv.h>
#include <string>

using namespace engine;

namespace {

std::span<const u32> words_of(const unsigned char* bytes, usize size) {
  return std::span<const u32>(reinterpret_cast<const u32*>(bytes), size / 4);
}

std::string forward_slashes(std::string path) {
  for (char& c : path) {
    if (c == '\\') c = '/';
  }
  return path;
}

}  // namespace

TEST_CASE("shader reflection: entry points, workgroup sizes, bindings, push constants") {
  gfx::ShaderReflection r;
  std::string error;

  REQUIRE_MESSAGE(
      gfx::reflect_spirv(words_of(shaders::k_fill_spirv, shaders::k_fill_spirv_size), r, &error),
      error);
  CHECK(r.spirv_version >= 0x00010600u);
  REQUIRE(r.entry_points.size() == 1);
  CHECK(r.entry_points[0].name == "fill");
  CHECK(r.entry_points[0].stage == VK_SHADER_STAGE_COMPUTE_BIT);
  CHECK(r.entry_points[0].local_size[0] == 64);
  CHECK(r.entry_points[0].local_size[1] == 1);
  CHECK(r.push_constant_bytes == 12);
  REQUIRE(r.bindings.size() == 1);
  CHECK(r.bindings[0].set == 0);
  CHECK(r.bindings[0].binding == 0);
  CHECK(r.bindings[0].type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
  CHECK(r.bindings[0].count == 1);
  CHECK(r.bindings[0].name == "g_data");

  REQUIRE(
      gfx::reflect_spirv(words_of(shaders::k_scale_spirv, shaders::k_scale_spirv_size), r, &error));
  REQUIRE(r.bindings.size() == 2);
  CHECK(r.binding_at(0, 0) != nullptr);
  CHECK(r.binding_at(0, 1) != nullptr);
  CHECK(r.binding_at(0, 1)->type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
  CHECK(r.binding_at(1, 0) == nullptr);

  REQUIRE(gfx::reflect_spirv(
      words_of(shaders::k_bindless_copy_spirv, shaders::k_bindless_copy_spirv_size), r, &error));
  REQUIRE(r.bindings.size() == 3);
  CHECK(r.bindings[0].type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
  CHECK(r.bindings[0].count == 0);  // unbounded
  CHECK(r.bindings[1].type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
  CHECK(r.bindings[1].count == 0);
  CHECK(r.bindings[2].type == VK_DESCRIPTOR_TYPE_SAMPLER);
  CHECK(r.bindings[2].binding == 2);
  REQUIRE(r.entry_points.size() == 1);
  CHECK(r.entry_points[0].local_size[0] == 8);
  CHECK(r.entry_points[0].local_size[1] == 8);

  REQUIRE(gfx::reflect_spirv(
      words_of(shaders::k_cluster_mesh_spirv, shaders::k_cluster_mesh_spirv_size), r, &error));
  REQUIRE(r.entry_points.size() == 4);
  const gfx::ShaderEntryPoint* mesh = r.entry("mesh_main");
  REQUIRE(mesh != nullptr);
  CHECK(mesh->stage == VK_SHADER_STAGE_MESH_BIT_EXT);
  CHECK(mesh->local_size[0] == 128);
  REQUIRE(r.entry("fs_main") != nullptr);
  CHECK(r.entry("fs_main")->stage == VK_SHADER_STAGE_FRAGMENT_BIT);
  CHECK(r.entry("fs_color") != nullptr);
  CHECK(r.entry("fs_visibility") != nullptr);
  CHECK(r.entry("missing") == nullptr);
  // float4x4, three addresses, count and pad, the visible list, the visibility buffer, width,
  // height.
  CHECK(r.push_constant_bytes == 120);
  CHECK(r.bindings.empty());

  REQUIRE(gfx::reflect_spirv(words_of(shaders::k_triangle_spirv, shaders::k_triangle_spirv_size), r,
                             &error));
  REQUIRE(r.entry("vs_main") != nullptr);
  CHECK(r.entry("vs_main")->stage == VK_SHADER_STAGE_VERTEX_BIT);
  CHECK(r.entry("vs_main")->local_size[0] == 0);
  CHECK(r.push_constant_bytes == 16);

  const u32 garbage[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  CHECK_FALSE(gfx::reflect_spirv(garbage, r, &error));
  CHECK(error.find("SPIR-V") != std::string::npos);
}

TEST_CASE("shader library: embedded shaders, the build manifest, and hot reload through slangc") {
  gfx::Device device;
  std::string error;
  const bool have_device = device.create(gfx::DeviceOptions{}, &error);
  if (!have_device) MESSAGE("no device, reflection only: " << error);

  gfx::ShaderLibrary library;
  REQUIRE(library.create(have_device ? &device : nullptr, &error));
  library.add_embedded("cluster_mesh", shaders::k_cluster_mesh_spirv,
                       shaders::k_cluster_mesh_spirv_size);
  library.add_embedded("fill", shaders::k_fill_spirv, shaders::k_fill_spirv_size);
  CHECK(library.count() == 2);

  const gfx::Shader* fill = library.get("fill", &error);
  REQUIRE_MESSAGE(fill != nullptr, error);
  CHECK(fill->hash != 0);
  CHECK_FALSE(fill->from_file);
  CHECK(fill->reflection.push_constant_bytes == 12);
  CHECK((fill->module != VK_NULL_HANDLE) == have_device);
  CHECK(library.get("nope", &error) == nullptr);
  CHECK(error.find("unknown shader") != std::string::npos);

  // The manifest the build wrote: the same shader from its .spv file hashes identically.
  REQUIRE_MESSAGE(library.load_manifest(ENGINE_SHADER_MANIFEST, &error), error);
  CHECK(library.count() >= 6);
  CHECK_FALSE(library.compiler().empty());
  const gfx::Shader* embedded_mesh = nullptr;
  {
    gfx::ShaderLibrary embedded_only;
    REQUIRE(embedded_only.create(nullptr));
    embedded_only.add_embedded("cluster_mesh", shaders::k_cluster_mesh_spirv,
                               shaders::k_cluster_mesh_spirv_size);
    embedded_mesh = embedded_only.get("cluster_mesh");
    REQUIRE(embedded_mesh != nullptr);
    const gfx::Shader* file_mesh = library.get("cluster_mesh", &error);
    REQUIRE_MESSAGE(file_mesh != nullptr, error);
    CHECK(file_mesh->from_file);
    CHECK(file_mesh->hash == embedded_mesh->hash);
    CHECK(file_mesh->reflection.entry_points.size() == 4);
  }
  std::string description;
  library.describe(description);
  CHECK(description.find("cluster_mesh (file) entries: mesh_main[128x1x1]") != std::string::npos);
  CHECK(description.find("push:120") != std::string::npos);

  // Hot reload: a temporary shader compiled on first use, recompiled when its source changes,
  // and kept at the last good version when a save does not compile.
  const auto dir = std::filesystem::temp_directory_path() / "engine_shader_library_tests";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const std::string source = forward_slashes((dir / "hot.slang").string());
  const std::string spirv = forward_slashes((dir / "hot.spv").string());
  const std::string manifest = forward_slashes((dir / "manifest.json").string());
  auto write_source = [&](const char* push_type) {
    const std::string text = std::string("struct Params { ") + push_type +
                             " value; };\n[[vk::push_constant]] ConstantBuffer<Params> g_params;\n"
                             "[[vk::binding(0, 0)]] RWStructuredBuffer<uint> g_out;\n"
                             "[shader(\"compute\")]\n[numthreads(32, 1, 1)]\n"
                             "void hot_main(uint3 id : SV_DispatchThreadID) { g_out[id.x] = id.x + "
                             "uint(g_params.value.x); }\n";
    REQUIRE(io::write_file(source, text) == io::Status::Ok);
  };
  write_source("uint2");
  REQUIRE(io::write_file(manifest,
                         std::string("{\"slangc\": \"") + library.compiler() +
                             "\", \"shaders\": [{\"name\": \"hot\", \"source\": \"" + source +
                             "\", \"spirv\": \"" + spirv +
                             "\", \"args\": [\"-target\", \"spirv\", \"-profile\", \"spirv_1_6\", "
                             "\"-emit-spirv-directly\", \"-fvk-use-entrypoint-name\", \"-O2\", "
                             "\"-warnings-disable\", \"41012\"]}]}\n") == io::Status::Ok);

  gfx::ShaderLibrary hot;
  REQUIRE(hot.create(have_device ? &device : nullptr));
  REQUIRE_MESSAGE(hot.load_manifest(manifest, &error), error);
  const gfx::Shader* shader = hot.get("hot", &error);
  REQUIRE_MESSAGE(shader != nullptr, error);  // compiled because hot.spv did not exist
  CHECK(std::filesystem::exists(spirv));
  CHECK(shader->from_file);
  CHECK(shader->generation == 0);
  CHECK(shader->reflection.push_constant_bytes == 8);
  REQUIRE(shader->reflection.entry("hot_main") != nullptr);
  CHECK(shader->reflection.entry("hot_main")->local_size[0] == 32);
  const u64 first_hash = shader->hash;

  Vector<std::string> changed;
  CHECK(hot.poll_changes(changed, &error) == 0);  // nothing changed yet
  CHECK(error.empty());

  // A newer source: push the file time two seconds ahead so the change is unambiguous.
  write_source("float4");
  std::filesystem::last_write_time(
      source, std::filesystem::file_time_type::clock::now() + std::chrono::seconds(2));
  REQUIRE(hot.poll_changes(changed, &error) == 1);
  CHECK(error.empty());
  REQUIRE(changed.size() == 1);
  CHECK(changed[0] == "hot");
  shader = hot.get("hot");
  REQUIRE(shader != nullptr);
  CHECK(shader->generation == 1);
  CHECK(shader->reflection.push_constant_bytes == 16);
  CHECK(shader->hash != first_hash);

  // A broken save: reported once, previous version kept.
  REQUIRE(io::write_file(source, "this is not slang\n") == io::Status::Ok);
  std::filesystem::last_write_time(
      source, std::filesystem::file_time_type::clock::now() + std::chrono::seconds(4));
  CHECK(hot.poll_changes(changed, &error) == 0);
  CHECK(error.find("hot: slangc exited with") != std::string::npos);
  CHECK(changed.empty());
  shader = hot.get("hot");
  REQUIRE(shader != nullptr);
  CHECK(shader->generation == 1);
  CHECK(shader->reflection.push_constant_bytes == 16);
  CHECK(hot.poll_changes(changed, &error) == 0);  // the same broken save is not reported again
  CHECK(error.empty());
  CHECK_FALSE(hot.recompile("hot", &error));
  CHECK_FALSE(hot.recompile("nope", &error));

  hot.destroy();
  library.destroy();
  std::filesystem::remove_all(dir);
  if (have_device) device.destroy();
}
