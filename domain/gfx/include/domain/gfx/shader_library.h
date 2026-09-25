#pragma once

// The shader library (ADR-0008, docs/plan/08-toolchain.md §8.4): shaders by name, from files
// during development and from embedded SPIR-V when shipping, with reflection read straight
// from the SPIR-V and hot reload through the pinned slangc.
//
// clang-format off
//     ShaderLibrary library;
//     library.create(&device);
//     library.add_embedded("cluster_mesh", shaders::k_cluster_mesh_spirv, shaders::k_cluster_mesh_spirv_size);
//     library.load_manifest(manifest_path);   // build/<preset>/shaders/manifest.json, optional
//     const Shader* mesh = library.get("cluster_mesh");
//     ...
//     Vector<std::string> changed;
//     if (library.poll_changes(changed, &error) > 0) { /* rebuild pipelines using the changed shaders */ }
// clang-format on
//
// Precedence per name: a manifest entry (compiled from its source when the SPIR-V is missing
// or older) over embedded bytes, so an edited .slang wins while developing and the embedded
// copy serves when no build tree is around. Reflection lists entry points with their stages
// and workgroup sizes, descriptor bindings, and the push-constant block size; it comes from a
// small SPIR-V walk here rather than from the compiler, so any SPIR-V producer works.

#include <core/base/macros.h>
#include <core/base/types.h>
#include <core/containers/vector.h>
#include <domain/gfx/device.h>
#include <domain/gfx/rhi.h>

#include <span>
#include <string>
#include <string_view>

namespace engine::gfx {

struct ShaderEntryPoint {
  std::string name;
  ShaderStage stage = ShaderStage::None;  // None: an execution model the reflection does not know
  u32 local_size[3] = {0, 0, 0};          // compute, task, and mesh stages
};

struct ShaderBinding {
  std::string name;
  u32 set = 0;
  u32 binding = 0;
  DescriptorType type = DescriptorType::Unknown;
  u32 count = 1;  // 0: unbounded (runtime array)
};

struct ShaderReflection {
  Vector<ShaderEntryPoint> entry_points;
  Vector<ShaderBinding> bindings;  // sorted by set, then binding
  u32 push_constant_bytes = 0;
  u32 spirv_version = 0;  // as in the SPIR-V header, e.g. 0x00010600

  const ShaderEntryPoint* entry(std::string_view name) const noexcept;
  const ShaderBinding* binding_at(u32 set, u32 binding) const noexcept;
};

// Walks a SPIR-V module. False with `error` on a malformed module.
bool reflect_spirv(std::span<const u32> words, ShaderReflection& out, std::string* error = nullptr);

struct Shader {
  std::string name;
  ShaderModuleHandle module;  // null when the library has no device
  ShaderReflection reflection;
  Vector<u32> spirv;
  u64 hash = 0;        // of the SPIR-V bytes
  u32 generation = 0;  // increments on every reload
  bool from_file = false;
};

class ShaderLibrary {
 public:
  ShaderLibrary() noexcept = default;
  ~ShaderLibrary();
  ENGINE_NON_COPYABLE(ShaderLibrary);

  // `device` may be null: shaders are then loaded and reflected but no modules are created
  // (tools, tests on machines without a GPU).
  bool create(const Device* device, std::string* error = nullptr);
  void destroy() noexcept;
  bool valid() const noexcept { return created_; }

  // Embedded SPIR-V (engine_shaders() headers). The bytes must outlive the library.
  void add_embedded(std::string_view name, const unsigned char* bytes, usize size);
  // build/<preset>/shaders/manifest.json from engine_write_shader_manifest(). Entries whose
  // name matches an embedded shader shadow it.
  bool load_manifest(std::string_view path, std::string* error = nullptr);

  // Loads on first use. The pointer stays valid until the next add_embedded/load_manifest.
  const Shader* get(std::string_view name, std::string* error = nullptr);
  // Recompiles and reloads every loaded shader whose source changed since it was loaded.
  // Returns how many changed and lists their names; a shader that fails to compile keeps its
  // previous version and its message goes to `error` (several are joined) and the log.
  u32 poll_changes(Vector<std::string>& changed, std::string* error = nullptr);
  // Recompiles one shader from its source now, whatever its timestamps say.
  bool recompile(std::string_view name, std::string* error = nullptr);

  u32 count() const noexcept { return entries_.size(); }
  // How many files a shader's source `#include`s, transitively, as the last load resolved
  // them. Hot reload watches these as well as the source itself, so editing brdf.slang or
  // scene.slang reloads every shader that includes it. Zero for an unknown or embedded-only
  // shader.
  u32 include_count(std::string_view name) const noexcept;
  const std::string& compiler() const noexcept { return compiler_; }
  // One line per loaded shader: entry points, bindings, push-constant size.
  void describe(std::string& out) const;

 private:
  // One file a shader's source reaches through `#include "..."`, with the timestamp it had
  // when the shader was last loaded.
  struct IncludedFile {
    std::string path;
    i64 mtime = 0;
  };
  struct Entry {
    std::string name;
    std::string source;      // manifest
    std::string spirv_path;  // manifest
    Vector<std::string> args;
    const unsigned char* embedded = nullptr;
    usize embedded_size = 0;
    i64 source_mtime = 0;
    Vector<IncludedFile> includes;  // transitive, source excluded
    Shader shader;
    bool loaded = false;
  };
  const Entry* find(std::string_view name) const noexcept;
  Entry* find(std::string_view name) noexcept;
  Entry& entry_for(std::string_view name);
  bool load(Entry& entry, std::string* error);
  bool load_bytes(Entry& entry, std::span<const u8> bytes, bool from_file, std::string* error);
  bool compile(Entry& entry, std::string* error);
  // Walks the source's `#include "..."` graph and records every file it reaches, with its
  // current timestamp. Called whenever the shader's own timestamp is refreshed, so an edit
  // that adds or removes an include is picked up on the next poll.
  void scan_includes(Entry& entry) const;
  void release(Shader& shader) noexcept;

  const Device* device_ = nullptr;
  bool created_ = false;
  std::string compiler_;
  Vector<Entry> entries_;
};

}  // namespace engine::gfx
