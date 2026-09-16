#include <core/hash/hash.h>
#include <core/json/json.h>
#include <core/log/log.h>
#include <core/platform/process.h>
#include <domain/gfx/device.h>
#include <domain/gfx/shader_library.h>
#include <foundation/io/vfs.h>

#include <algorithm>
#include <cstring>

namespace engine::gfx {

ENGINE_LOG_CATEGORY_DEFINE(log_shaders, "gfx.shaders");

// ---- SPIR-V reflection -------------------------------------------------------------------------

namespace {

constexpr u32 k_spirv_magic = 0x07230203u;

enum Op : u16 {
  OpName = 5,
  OpEntryPoint = 15,
  OpExecutionMode = 16,
  OpTypeBool = 20,
  OpTypeInt = 21,
  OpTypeFloat = 22,
  OpTypeVector = 23,
  OpTypeMatrix = 24,
  OpTypeImage = 25,
  OpTypeSampler = 26,
  OpTypeSampledImage = 27,
  OpTypeArray = 28,
  OpTypeRuntimeArray = 29,
  OpTypeStruct = 30,
  OpTypePointer = 32,
  OpConstant = 43,
  OpVariable = 59,
  OpDecorate = 71,
  OpMemberDecorate = 72,
  OpTypeAccelerationStructureKHR = 5341,
};

enum Decoration : u32 {
  DecBlock = 2,
  DecBufferBlock = 3,
  DecRowMajor = 4,
  DecArrayStride = 6,
  DecMatrixStride = 7,
  DecBinding = 33,
  DecDescriptorSet = 34,
  DecOffset = 35,
};

enum StorageClass : u32 {
  ScUniformConstant = 0,
  ScUniform = 2,
  ScPushConstant = 9,
  ScStorageBuffer = 12,
};

VkShaderStageFlagBits stage_of(u32 execution_model) noexcept {
  switch (execution_model) {
    case 0: return VK_SHADER_STAGE_VERTEX_BIT;
    case 1: return VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
    case 2: return VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
    case 3: return VK_SHADER_STAGE_GEOMETRY_BIT;
    case 4: return VK_SHADER_STAGE_FRAGMENT_BIT;
    case 5: return VK_SHADER_STAGE_COMPUTE_BIT;
    case 5313: return VK_SHADER_STAGE_RAYGEN_BIT_KHR;
    case 5314: return VK_SHADER_STAGE_INTERSECTION_BIT_KHR;
    case 5315: return VK_SHADER_STAGE_ANY_HIT_BIT_KHR;
    case 5316: return VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;
    case 5317: return VK_SHADER_STAGE_MISS_BIT_KHR;
    case 5318: return VK_SHADER_STAGE_CALLABLE_BIT_KHR;
    case 5364: return VK_SHADER_STAGE_TASK_BIT_EXT;
    case 5365: return VK_SHADER_STAGE_MESH_BIT_EXT;
    default: return VK_SHADER_STAGE_FLAG_BITS_MAX_ENUM;
  }
}

// Reads the null-terminated, word-aligned literal string starting at `word`.
std::string literal_string(std::span<const u32> words, u32 word, u32 end) {
  std::string out;
  for (u32 w = word; w < end; ++w) {
    const u32 v = words[w];
    for (u32 byte = 0; byte < 4; ++byte) {
      const char c = static_cast<char>((v >> (8 * byte)) & 0xFF);
      if (c == '\0') return out;
      out.push_back(c);
    }
  }
  return out;
}

struct Module {
  std::span<const u32> words;
  Vector<u32> def;  // result id -> word offset of the defining instruction (0: none)
  struct MemberDecoration {
    u32 target;
    u32 member;
    u32 decoration;
    u32 value;
  };
  Vector<MemberDecoration> member_decorations;
  struct IdDecoration {
    u32 target;
    u32 decoration;
    u32 value;
  };
  Vector<IdDecoration> decorations;
  Vector<std::pair<u32, u32>> names;  // id -> word offset of OpName

  u16 opcode(u32 at) const noexcept { return static_cast<u16>(words[at] & 0xFFFF); }
  u16 length(u32 at) const noexcept { return static_cast<u16>(words[at] >> 16); }
  u32 operand(u32 at, u32 index) const noexcept { return words[at + index]; }
  u32 def_of(u32 id) const noexcept { return id < def.size() ? def[id] : 0; }

  bool decoration(u32 target, u32 decoration, u32* value = nullptr) const noexcept {
    for (const IdDecoration& d : decorations) {
      if (d.target == target && d.decoration == decoration) {
        if (value != nullptr) *value = d.value;
        return true;
      }
    }
    return false;
  }
  bool member_decoration(u32 target, u32 member, u32 decoration,
                         u32* value = nullptr) const noexcept {
    for (const MemberDecoration& d : member_decorations) {
      if (d.target == target && d.member == member && d.decoration == decoration) {
        if (value != nullptr) *value = d.value;
        return true;
      }
    }
    return false;
  }
  std::string name_of(u32 id) const {
    for (const auto& [target, at] : names) {
      if (target == id) return literal_string(words, at + 2, at + length(at));
    }
    return {};
  }
  u32 constant_value(u32 id) const noexcept {
    const u32 at = def_of(id);
    if (at == 0 || opcode(at) != OpConstant || length(at) < 4) return 0;
    return operand(at, 3);
  }

  // Size in bytes of a type as laid out in a push-constant or buffer block.
  u32 type_size(u32 type_id, u32 depth = 0) const noexcept {
    const u32 at = def_of(type_id);
    if (at == 0 || depth > 16) return 0;
    switch (opcode(at)) {
      case OpTypeBool: return 4;
      case OpTypeInt:
      case OpTypeFloat: return operand(at, 2) / 8;
      case OpTypeVector: return type_size(operand(at, 2), depth + 1) * operand(at, 3);
      case OpTypeMatrix: return type_size(operand(at, 2), depth + 1) * operand(at, 3);
      case OpTypePointer: return 8;  // physical storage buffer addresses
      case OpTypeArray: {
        const u32 count = constant_value(operand(at, 3));
        u32 stride = 0;
        if (!decoration(type_id, DecArrayStride, &stride))
          stride = type_size(operand(at, 2), depth + 1);
        return stride * count;
      }
      case OpTypeRuntimeArray: return 0;
      case OpTypeStruct: {
        const u32 member_count = length(at) - 2;
        u32 size = 0;
        u32 running = 0;
        for (u32 m = 0; m < member_count; ++m) {
          const u32 member_type = operand(at, 2 + m);
          u32 member_size = 0;
          u32 stride = 0;
          if (member_decoration(type_id, m, DecMatrixStride, &stride)) {
            const u32 matrix_at = def_of(member_type);
            const bool row_major = member_decoration(type_id, m, DecRowMajor);
            u32 vectors = 0;
            if (matrix_at != 0 && opcode(matrix_at) == OpTypeMatrix) {
              const u32 column_at = def_of(operand(matrix_at, 2));
              const u32 rows = column_at != 0 ? operand(column_at, 3) : 0;
              vectors = row_major ? rows : operand(matrix_at, 3);
            }
            member_size = stride * vectors;
          } else {
            member_size = type_size(member_type, depth + 1);
          }
          u32 offset = running;
          member_decoration(type_id, m, DecOffset, &offset);
          running = offset + member_size;
          size = running > size ? running : size;
        }
        return size;
      }
      default: return 0;
    }
  }
};

}  // namespace

const ShaderEntryPoint* ShaderReflection::entry(std::string_view name) const noexcept {
  for (const ShaderEntryPoint& e : entry_points) {
    if (e.name == name) return &e;
  }
  return nullptr;
}

const ShaderBinding* ShaderReflection::binding_at(u32 set, u32 binding) const noexcept {
  for (const ShaderBinding& b : bindings) {
    if (b.set == set && b.binding == binding) return &b;
  }
  return nullptr;
}

bool reflect_spirv(std::span<const u32> words, ShaderReflection& out, std::string* error) {
  out = ShaderReflection{};
  if (words.size() < 5 || words[0] != k_spirv_magic) {
    if (error != nullptr) *error = "reflect_spirv: not a SPIR-V module";
    return false;
  }
  Module m;
  m.words = words;
  const u32 bound = words[3];
  if (bound == 0 || bound > 4u * 1024u * 1024u) {
    if (error != nullptr) *error = "reflect_spirv: implausible id bound";
    return false;
  }
  m.def.resize(bound);
  out.spirv_version = words[1];

  // Pass 1: index definitions, decorations, names, entry points.
  struct EntryRecord {
    u32 id;
    u32 model;
    std::string name;
  };
  Vector<EntryRecord> entries;
  struct ModeRecord {
    u32 entry;
    u32 mode;
    u32 at;
  };
  Vector<ModeRecord> modes;
  Vector<u32> variables;
  for (u32 at = 5; at < words.size();) {
    const u16 len = m.length(at);
    if (len == 0 || at + len > words.size()) {
      if (error != nullptr) *error = "reflect_spirv: truncated instruction";
      return false;
    }
    const u16 op = m.opcode(at);
    switch (op) {
      case OpName:
        if (len >= 3) m.names.push_back({m.operand(at, 1), at});
        break;
      case OpEntryPoint:
        if (len >= 4)
          entries.push_back(
              {m.operand(at, 2), m.operand(at, 1), literal_string(words, at + 3, at + len)});
        break;
      case OpExecutionMode:
        if (len >= 3) modes.push_back({m.operand(at, 1), m.operand(at, 2), at});
        break;
      case OpDecorate:
        if (len >= 3) {
          m.decorations.push_back(
              {m.operand(at, 1), m.operand(at, 2), len >= 4 ? m.operand(at, 3) : 0});
        }
        break;
      case OpMemberDecorate:
        if (len >= 4) {
          m.member_decorations.push_back({m.operand(at, 1), m.operand(at, 2), m.operand(at, 3),
                                          len >= 5 ? m.operand(at, 4) : 0});
        }
        break;
      case OpTypeBool:
      case OpTypeInt:
      case OpTypeFloat:
      case OpTypeVector:
      case OpTypeMatrix:
      case OpTypeImage:
      case OpTypeSampler:
      case OpTypeSampledImage:
      case OpTypeArray:
      case OpTypeRuntimeArray:
      case OpTypeStruct:
      case OpTypePointer:
      case OpTypeAccelerationStructureKHR:
        if (len >= 2 && m.operand(at, 1) < bound) m.def[m.operand(at, 1)] = at;
        break;
      case OpConstant:
      case OpVariable:
        if (len >= 3 && m.operand(at, 2) < bound) m.def[m.operand(at, 2)] = at;
        if (op == OpVariable) variables.push_back(at);
        break;
      default: break;
    }
    at += len;
  }

  // Entry points and their workgroup sizes.
  for (const EntryRecord& e : entries) {
    ShaderEntryPoint entry;
    entry.name = e.name;
    entry.stage = stage_of(e.model);
    for (const ModeRecord& mode : modes) {
      if (mode.entry != e.id) continue;
      if (mode.mode == 17 && m.length(mode.at) >= 6) {  // LocalSize
        for (u32 i = 0; i < 3; ++i)
          entry.local_size[i] = m.operand(mode.at, 3 + i);
      } else if (mode.mode == 38 && m.length(mode.at) >= 6) {  // LocalSizeId
        for (u32 i = 0; i < 3; ++i)
          entry.local_size[i] = m.constant_value(m.operand(mode.at, 3 + i));
      }
    }
    out.entry_points.push_back(std::move(entry));
  }

  // Variables: push constants and descriptor bindings.
  for (const u32 at : variables) {
    const u32 pointer_type = m.operand(at, 1);
    const u32 id = m.operand(at, 2);
    const u32 storage = m.operand(at, 3);
    const u32 pointer_at = m.def_of(pointer_type);
    if (pointer_at == 0 || m.opcode(pointer_at) != OpTypePointer) continue;
    u32 pointee = m.operand(pointer_at, 3);
    if (storage == ScPushConstant) {
      const u32 size = m.type_size(pointee);
      out.push_constant_bytes = size > out.push_constant_bytes ? size : out.push_constant_bytes;
      continue;
    }
    if (storage != ScUniformConstant && storage != ScUniform && storage != ScStorageBuffer)
      continue;

    ShaderBinding binding;
    binding.name = m.name_of(id);
    if (!m.decoration(id, DecDescriptorSet, &binding.set)) binding.set = 0;
    if (!m.decoration(id, DecBinding, &binding.binding)) continue;  // not a descriptor
    binding.count = 1;
    for (u32 guard = 0; guard < 8; ++guard) {
      const u32 type_at = m.def_of(pointee);
      if (type_at == 0) break;
      if (m.opcode(type_at) == OpTypeArray) {
        binding.count *= m.constant_value(m.operand(type_at, 3));
        pointee = m.operand(type_at, 2);
      } else if (m.opcode(type_at) == OpTypeRuntimeArray) {
        binding.count = 0;
        pointee = m.operand(type_at, 2);
      } else {
        break;
      }
    }
    const u32 type_at = m.def_of(pointee);
    if (type_at == 0) continue;
    switch (m.opcode(type_at)) {
      case OpTypeImage: {
        const u32 dim = m.operand(type_at, 3);
        const u32 sampled = m.operand(type_at, 7);
        if (dim == 5) {  // Buffer
          binding.type = sampled == 2 ? VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER
                                      : VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
        } else {
          binding.type =
              sampled == 2 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        }
        break;
      }
      case OpTypeSampler: binding.type = VK_DESCRIPTOR_TYPE_SAMPLER; break;
      case OpTypeSampledImage: binding.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; break;
      case OpTypeAccelerationStructureKHR:
        binding.type = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        break;
      case OpTypeStruct:
        if (storage == ScStorageBuffer || m.decoration(pointee, DecBufferBlock)) {
          binding.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        } else {
          binding.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        }
        break;
      default: continue;
    }
    out.bindings.push_back(std::move(binding));
  }
  std::sort(out.bindings.begin(), out.bindings.end(),
            [](const ShaderBinding& a, const ShaderBinding& b) {
              return a.set != b.set ? a.set < b.set : a.binding < b.binding;
            });
  return true;
}

// ---- library -----------------------------------------------------------------------------------

ShaderLibrary::~ShaderLibrary() { destroy(); }

bool ShaderLibrary::create(const Device* device, std::string* error) {
  ENGINE_VERIFY(!created_, "ShaderLibrary::create: already created");
  if (device != nullptr && !device->valid()) {
    if (error != nullptr) *error = "ShaderLibrary::create: the device is not valid";
    return false;
  }
  device_ = device;
  created_ = true;
  return true;
}

void ShaderLibrary::destroy() noexcept {
  if (!created_) return;
  for (Entry& e : entries_)
    release(e.shader);
  entries_.clear();
  compiler_.clear();
  device_ = nullptr;
  created_ = false;
}

void ShaderLibrary::release(Shader& shader) noexcept {
  if (shader.module != VK_NULL_HANDLE && device_ != nullptr)
    destroy_shader_module(*device_, shader.module);
  shader.module = VK_NULL_HANDLE;
}

ShaderLibrary::Entry* ShaderLibrary::find(std::string_view name) noexcept {
  for (Entry& e : entries_) {
    if (e.name == name) return &e;
  }
  return nullptr;
}

ShaderLibrary::Entry& ShaderLibrary::entry_for(std::string_view name) {
  if (Entry* existing = find(name); existing != nullptr) return *existing;
  Entry entry;
  entry.name = std::string(name);
  entry.shader.name = entry.name;
  entries_.push_back(std::move(entry));
  return entries_.back();
}

void ShaderLibrary::add_embedded(std::string_view name, const unsigned char* bytes, usize size) {
  ENGINE_VERIFY(created_, "ShaderLibrary::add_embedded: not created");
  Entry& entry = entry_for(name);
  entry.embedded = bytes;
  entry.embedded_size = size;
}

bool ShaderLibrary::load_manifest(std::string_view path, std::string* error) {
  ENGINE_VERIFY(created_, "ShaderLibrary::load_manifest: not created");
  std::string text;
  if (const io::Status status = io::read_file(path, text); status != io::Status::Ok) {
    if (error != nullptr)
      *error = std::string("cannot read shader manifest ") + std::string(path) + ": " +
               io::status_name(status);
    return false;
  }
  JsonValue root;
  if (const JsonParseResult parsed = parse_json(text, root); !parsed.ok) {
    if (error != nullptr)
      *error = std::string("shader manifest ") + std::string(path) + ": " + parsed.message;
    return false;
  }
  const JsonValue* compiler = root.find("slangc");
  const JsonValue* shaders = root.find("shaders");
  if (compiler == nullptr || !compiler->is_string() || shaders == nullptr || !shaders->is_array()) {
    if (error != nullptr) *error = "shader manifest needs \"slangc\" and \"shaders\"";
    return false;
  }
  compiler_ = std::string(compiler->as_string());
  u32 added = 0;
  for (usize i = 0; i < shaders->size(); ++i) {
    const JsonValue& s = (*shaders)[i];
    const JsonValue* name = s.find("name");
    const JsonValue* source = s.find("source");
    const JsonValue* spirv = s.find("spirv");
    const JsonValue* args = s.find("args");
    if (name == nullptr || !name->is_string() || source == nullptr || !source->is_string() ||
        spirv == nullptr || !spirv->is_string()) {
      if (error != nullptr) *error = "shader manifest entry needs name, source, and spirv";
      return false;
    }
    Entry& entry = entry_for(name->as_string());
    entry.source = std::string(source->as_string());
    entry.spirv_path = std::string(spirv->as_string());
    entry.args.clear();
    if (args != nullptr && args->is_array()) {
      for (usize a = 0; a < args->size(); ++a) {
        if ((*args)[a].is_string()) entry.args.push_back(std::string((*args)[a].as_string()));
      }
    }
    // A shader loaded from embedded bytes before the manifest arrived reloads from the file
    // on its next get().
    if (entry.loaded && !entry.shader.from_file) {
      release(entry.shader);
      entry.loaded = false;
    }
    ++added;
  }
  ENGINE_LOG_DEBUG(log_shaders, "shader manifest loaded", log::field("shaders", added),
                   log::field("path", path));
  return true;
}

bool ShaderLibrary::compile(Entry& entry, std::string* error) {
  if (compiler_.empty() || entry.source.empty()) {
    if (error != nullptr) *error = entry.name + ": no compiler or source to compile from";
    return false;
  }
  std::vector<std::string_view> argv;
  argv.push_back(compiler_);
  argv.push_back(entry.source);
  for (const std::string& a : entry.args)
    argv.push_back(a);
  argv.push_back("-o");
  argv.push_back(entry.spirv_path);
  platform::Process process;
  std::string spawn_error;
  if (!process.spawn(std::span<const std::string_view>(argv.data(), argv.size()), &spawn_error,
                     true)) {
    if (error != nullptr) *error = entry.name + ": cannot run " + compiler_ + ": " + spawn_error;
    return false;
  }
  process.close_stdin();
  std::string output;
  process.read_all(output);
  const i32 code = process.wait();
  if (code != 0) {
    while (!output.empty() && (output.back() == '\n' || output.back() == '\r'))
      output.pop_back();
    if (error != nullptr)
      *error = entry.name + ": slangc exited with " + std::to_string(code) + "\n" + output;
    ENGINE_LOG_WARN(log_shaders, "shader compile failed", log::field("shader", entry.name),
                    log::field("exit", code));
    return false;
  }
  ENGINE_LOG_INFO(log_shaders, "shader compiled", log::field("shader", entry.name),
                  log::field("source", entry.source));
  return true;
}

bool ShaderLibrary::load_bytes(Entry& entry, std::span<const u8> bytes, bool from_file,
                               std::string* error) {
  if (bytes.size() < 20 || bytes.size() % 4 != 0) {
    if (error != nullptr) *error = entry.name + ": SPIR-V size is not a multiple of four words";
    return false;
  }
  Vector<u32> words(static_cast<u32>(bytes.size() / 4));
  std::memcpy(words.data(), bytes.data(), bytes.size());
  ShaderReflection reflection;
  if (!reflect_spirv(std::span<const u32>(words.data(), words.size()), reflection, error)) {
    if (error != nullptr) error->insert(0, entry.name + ": ");
    return false;
  }
  VkShaderModule module = VK_NULL_HANDLE;
  if (device_ != nullptr) {
    module = create_shader_module(*device_, bytes.data(), bytes.size(), error);
    if (module == VK_NULL_HANDLE) {
      if (error != nullptr) error->insert(0, entry.name + ": ");
      return false;
    }
  }
  const u32 generation = entry.loaded ? entry.shader.generation + 1 : 0;
  release(entry.shader);
  entry.shader.module = module;
  entry.shader.reflection = std::move(reflection);
  entry.shader.spirv = std::move(words);
  entry.shader.hash = hash_bytes(bytes.data(), bytes.size());
  entry.shader.generation = generation;
  entry.shader.from_file = from_file;
  entry.loaded = true;
  return true;
}

bool ShaderLibrary::load(Entry& entry, std::string* error) {
  std::string local_error;
  if (!entry.spirv_path.empty()) {
    io::FileInfo source_info{};
    io::FileInfo spirv_info{};
    const bool have_source =
        !entry.source.empty() && io::stat_file(entry.source, source_info) == io::Status::Ok;
    const bool have_spirv = io::stat_file(entry.spirv_path, spirv_info) == io::Status::Ok;
    bool usable = have_spirv;
    if (have_source &&
        (!have_spirv || source_info.modified_unix_ms > spirv_info.modified_unix_ms)) {
      usable = compile(entry, &local_error);
      if (usable) io::stat_file(entry.source, source_info);
    }
    if (usable) {
      std::string bytes;
      if (io::read_file(entry.spirv_path, bytes) == io::Status::Ok &&
          load_bytes(entry,
                     std::span<const u8>(reinterpret_cast<const u8*>(bytes.data()), bytes.size()),
                     true, &local_error)) {
        entry.source_mtime = have_source ? source_info.modified_unix_ms : 0;
        return true;
      }
    }
    if (entry.embedded == nullptr) {
      if (error != nullptr) {
        *error =
            local_error.empty() ? entry.name + ": cannot load " + entry.spirv_path : local_error;
      }
      return false;
    }
    ENGINE_LOG_WARN(log_shaders, "shader file unusable, using embedded copy",
                    log::field("shader", entry.name), log::field("reason", local_error));
  }
  if (entry.embedded == nullptr) {
    if (error != nullptr) *error = entry.name + ": no source, file, or embedded bytes";
    return false;
  }
  return load_bytes(entry, std::span<const u8>(entry.embedded, entry.embedded_size), false, error);
}

const Shader* ShaderLibrary::get(std::string_view name, std::string* error) {
  ENGINE_VERIFY(created_, "ShaderLibrary::get: not created");
  Entry* entry = find(name);
  if (entry == nullptr) {
    if (error != nullptr) *error = std::string("unknown shader ") + std::string(name);
    return nullptr;
  }
  if (!entry->loaded && !load(*entry, error)) return nullptr;
  return &entry->shader;
}

u32 ShaderLibrary::poll_changes(Vector<std::string>& changed, std::string* error) {
  ENGINE_VERIFY(created_, "ShaderLibrary::poll_changes: not created");
  changed.clear();
  std::string errors;
  for (Entry& entry : entries_) {
    if (!entry.loaded || entry.source.empty()) continue;
    io::FileInfo info{};
    if (io::stat_file(entry.source, info) != io::Status::Ok ||
        info.modified_unix_ms == entry.source_mtime)
      continue;
    std::string local_error;
    if (compile(entry, &local_error)) {
      std::string bytes;
      if (io::read_file(entry.spirv_path, bytes) == io::Status::Ok &&
          load_bytes(entry,
                     std::span<const u8>(reinterpret_cast<const u8*>(bytes.data()), bytes.size()),
                     true, &local_error)) {
        entry.source_mtime = info.modified_unix_ms;
        changed.push_back(entry.name);
        ENGINE_LOG_INFO(log_shaders, "shader reloaded", log::field("shader", entry.name),
                        log::field("generation", entry.shader.generation));
        continue;
      }
    }
    // Keep the previous shader; remember this mtime so a broken save is reported once.
    entry.source_mtime = info.modified_unix_ms;
    if (!errors.empty()) errors += "\n";
    errors += local_error;
  }
  if (error != nullptr) *error = errors;
  return changed.size();
}

bool ShaderLibrary::recompile(std::string_view name, std::string* error) {
  Entry* entry = find(name);
  if (entry == nullptr) {
    if (error != nullptr) *error = std::string("unknown shader ") + std::string(name);
    return false;
  }
  if (!compile(*entry, error)) return false;
  std::string bytes;
  if (io::read_file(entry->spirv_path, bytes) != io::Status::Ok) {
    if (error != nullptr) *error = entry->name + ": cannot read " + entry->spirv_path;
    return false;
  }
  if (!load_bytes(*entry,
                  std::span<const u8>(reinterpret_cast<const u8*>(bytes.data()), bytes.size()),
                  true, error)) {
    return false;
  }
  io::FileInfo info{};
  if (io::stat_file(entry->source, info) == io::Status::Ok)
    entry->source_mtime = info.modified_unix_ms;
  return true;
}

void ShaderLibrary::describe(std::string& out) const {
  for (const Entry& e : entries_) {
    out += e.name;
    if (!e.loaded) {
      out += ": not loaded\n";
      continue;
    }
    out += e.shader.from_file ? " (file)" : " (embedded)";
    out += " entries:";
    for (const ShaderEntryPoint& entry : e.shader.reflection.entry_points) {
      out += " " + entry.name;
      if (entry.local_size[0] != 0) {
        out += "[" + std::to_string(entry.local_size[0]) + "x" +
               std::to_string(entry.local_size[1]) + "x" + std::to_string(entry.local_size[2]) +
               "]";
      }
    }
    out += " bindings:" + std::to_string(e.shader.reflection.bindings.size());
    out += " push:" + std::to_string(e.shader.reflection.push_constant_bytes) + "\n";
  }
}

}  // namespace engine::gfx
