#include "editor_import.h"

#include <core/json/json.h>
#include <core/json/json_value.h>
#include <domain/assets/gltf.h>
#include <domain/geometry/cluster_file.h>
#include <foundation/io/vfs.h>

#include <cmath>
#include <initializer_list>
#include <string_view>

namespace engine::editor {

namespace {

std::string lower(std::string_view s) {
  std::string out(s);
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return out;
}

// The directory part of `path` (both separators), with no trailing separator; "." for none.
std::string directory_of(const std::string& path) {
  const usize slash = path.find_last_of("/\\");
  return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
}

std::string file_of(const std::string& path) {
  const usize slash = path.find_last_of("/\\");
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

// A glTF URI's percent-escapes decoded ("my%20mesh.bin" is "my mesh.bin" on disk).
std::string decode_uri(std::string_view uri) {
  std::string out;
  for (usize i = 0; i < uri.size(); ++i) {
    const auto hex = [](char c) -> int {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      if (c >= 'A' && c <= 'F') return c - 'A' + 10;
      return -1;
    };
    if (uri[i] == '%' && i + 2 < uri.size() && hex(uri[i + 1]) >= 0 && hex(uri[i + 2]) >= 0) {
      out.push_back(static_cast<char>(hex(uri[i + 1]) * 16 + hex(uri[i + 2])));
      i += 2;
    } else {
      out.push_back(uri[i]);
    }
  }
  return out;
}

// A relative path that stays inside its directory: no root, no drive, no "..".
bool stays_inside(const std::string& rel) {
  if (rel.empty() || rel[0] == '/' || rel[0] == '\\' || rel.find(':') != std::string::npos)
    return false;
  usize start = 0;
  while (start <= rel.size()) {
    usize end = rel.find_first_of("/\\", start);
    if (end == std::string::npos) end = rel.size();
    if (rel.compare(start, end - start, "..") == 0) return false;
    start = end + 1;
  }
  return true;
}

// The external files a .gltf names (buffers and images; embedded data: URIs are not files).
bool gltf_dependencies(const std::string& gltf_path, Vector<std::string>& out, std::string& error) {
  std::string text;
  if (io::read_file(gltf_path, text) != io::Status::Ok) {
    error = "cannot read " + gltf_path;
    return false;
  }
  JsonValue root;
  if (!parse_json(text, root).ok || !root.is_object()) {
    error = gltf_path + " is not JSON";
    return false;
  }
  for (const char* key : {"buffers", "images"}) {
    const JsonValue* list = root.find(key);
    if (list == nullptr || !list->is_array()) continue;
    for (u32 i = 0; i < list->size(); ++i) {
      const JsonValue* uri = (*list)[i].find("uri");
      if (uri == nullptr || !uri->is_string()) continue;
      const std::string_view u = uri->as_string();
      if (u.substr(0, 5) == "data:") continue;
      const std::string rel = decode_uri(u);
      if (!stays_inside(rel)) {
        error = file_of(gltf_path) + " names " + rel +
                ", outside its own directory: copy the model's files into one directory first";
        return false;
      }
      bool seen = false;
      for (const std::string& s : out)
        seen = seen || s == rel;
      if (!seen) out.push_back(rel);
    }
  }
  return true;
}

}  // namespace

bool import_mesh_files(const std::string& source, const std::string& doc_dir, ImportedFiles& out,
                       std::string& error) {
  out = ImportedFiles{};
  const std::string name = file_of(source);
  const usize dot = name.find_last_of('.');
  const std::string ext = dot == std::string::npos ? std::string() : lower(name.substr(dot + 1));
  if (ext != "gltf" && ext != "glb" && ext != "clusters") {
    error = name + ": a mesh is a .gltf, .glb or .clusters file";
    return false;
  }
  if (!io::exists(source)) {
    error = "no file at " + source;
    return false;
  }
  // The loader's own verdict, before anything is copied.
  if (ext == "clusters") {
    geometry::ClusterFileData data;
    std::string why;
    if (!geometry::read_cluster_file(source, data, &why)) {
      error = name + ": " + why;
      return false;
    }
  } else {
    assets::MeshData mesh;
    std::string why;
    if (!assets::load_gltf(source, mesh, &why)) {
      error = name + ": " + why;
      return false;
    }
  }
  out.stem = name.substr(0, dot);
  const std::string rel_dir = "assets/" + out.stem;
  out.mesh = rel_dir + "/" + name;
  // Every file and where it goes, relative to the source's directory and to rel_dir.
  Vector<std::string> files;
  files.push_back(name);
  if (ext == "gltf" && !gltf_dependencies(source, files, error)) return false;
  const std::string from_dir = directory_of(source);
  Vector<std::string> bytes;
  Vector<u8> write;  // 1: a fresh copy, 0: the same bytes already there
  for (const std::string& rel : files) {
    std::string data;
    const std::string from = from_dir + "/" + rel;
    if (io::read_file(from, data) != io::Status::Ok) {
      error = name + " names " + rel + ", which is not there (" + from + ")";
      return false;
    }
    const std::string to = doc_dir + "/" + rel_dir + "/" + rel;
    std::string existing;
    bool fresh = true;
    if (io::exists(to)) {
      if (io::read_file(to, existing) != io::Status::Ok || existing != data) {
        error = rel_dir + "/" + rel +
                " is already in the document with other bytes: rename the file to import it "
                "beside that one";
        return false;
      }
      fresh = false;
    }
    bytes.push_back(std::move(data));
    write.push_back(fresh ? u8{1} : u8{0});
  }
  for (u32 i = 0; i < files.size(); ++i) {
    const std::string rel = rel_dir + "/" + files[i];
    if (write[i] == 0) {
      out.reused.push_back(rel);
      continue;
    }
    const std::string to = doc_dir + "/" + rel;
    if (io::make_directories(directory_of(to)) != io::Status::Ok ||
        io::write_file(to, bytes[i]) != io::Status::Ok) {
      error = "cannot write " + to;
      remove_imported(doc_dir, out);
      return false;
    }
    out.copied.push_back(rel);
  }
  return true;
}

void remove_imported(const std::string& doc_dir, const ImportedFiles& files) noexcept {
  for (const std::string& rel : files.copied)
    (void)io::remove_file(doc_dir + "/" + rel);
}

WorldPos default_import_position(WorldPos eye, WorldPos target) noexcept {
  DVec3 d = target - eye;
  const f64 len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
  d = len > 1.0e-9 ? d / len : DVec3{0.0, 0.0, -1.0};
  if (d.y < -1.0e-3) {
    const f64 t = -eye.y / d.y;
    if (t > 0.0 && t <= 50.0) {
      WorldPos at = eye + d * t;
      at.y = 0.0;
      return at;
    }
  }
  return eye + d * 10.0;
}

}  // namespace engine::editor
