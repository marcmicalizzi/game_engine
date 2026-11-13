#include <core/platform/process.h>
#include <domain/geometry/cluster_file.h>
#include <domain/scene_gen/derived_root.h>
#include <domain/scene_gen/scene_gen.h>
#include <foundation/io/vfs.h>

#include <filesystem>
#include <system_error>

namespace engine::scene_gen {

namespace {

// A directory made absolute against the working directory, `/`-separated; empty on failure.
std::string absolute_of(std::string_view dir) {
  std::error_code ec;
  const auto made = std::filesystem::absolute(
      dir.empty() ? std::filesystem::path(".") : std::filesystem::path(std::string(dir)), ec);
  return ec ? std::string() : io::normalize_path(made.lexically_normal().generic_string());
}

// `<root>/<sub>` made absolute; empty on failure.
std::string under(std::string_view root, std::string_view sub) {
  return absolute_of(io::join_path(root, sub));
}

}  // namespace

std::string scene_derived_root(std::string_view scene_dir) {
  const std::string from = absolute_of(scene_dir);
  if (from.empty()) return std::string();
  std::string root = geometry::find_ddc_root(from);
  return root.empty() ? io::join_path(from, "ddc") : root;
}

std::string host_derived_root(std::string_view named, std::string_view scene_dir, bool scene_file) {
  if (!named.empty()) return std::string(named);
  if (scene_file) return scene_derived_root(scene_dir);
  return geometry::find_ddc_root(platform::executable_directory());
}

std::string document_derived_root(std::string_view named, std::string_view document_dir) {
  return host_derived_root(named, document_dir, !document_dir.empty());
}

bool derived_dir_under(std::string_view root, std::string_view generator, std::string_view sub,
                       std::string& out, std::string& error) {
  out.clear();
  if (root.empty()) {
    error = std::string(generator) +
            ": no derived-data root was handed to it; a host names one (--ddc, render.load's "
            "ddc) or hands the scene's own, and a generator never guesses one from the working "
            "directory";
    return false;
  }
  out = under(root, sub);
  if (out.empty() || io::make_directories(out) != io::Status::Ok) {
    error = std::string(generator) + ": cannot make the derived-data directory " +
            (out.empty() ? io::join_path(root, sub) : out);
    out.clear();
    return false;
  }
  return true;
}

bool generator_derived_dir(const Context& context, std::string_view generator, std::string_view sub,
                           std::string& out, std::string& error) {
  if (!derived_dir_under(context.ddc, generator, sub, out, error)) {
    if (context.ddc.empty() && !context.where.empty())
      error = std::string(context.where) + ": " + error;
    return false;
  }
  return true;
}

std::string derived_elsewhere(const Context& context, std::string_view sub, std::string_view file,
                              std::string_view dir) {
  const std::string here = absolute_of(dir);
  const auto holds = [&](const std::string& root, std::string& at) {
    if (root.empty()) return false;
    at = under(root, sub);
    return !at.empty() && at != here && io::exists(io::join_path(at, file));
  };
  std::string at;
  if (holds(scene_derived_root(context.dir), at))
    return "; it is in " + at +
           ", under the scene's own root: the host's derived-data root wins over the scene's, so "
           "give the host and the engine-content command that made it the same --ddc";
  const std::string working = absolute_of({});
  std::string cwd_root = working.empty() ? std::string() : geometry::find_ddc_root(working);
  if (cwd_root.empty() && !working.empty()) cwd_root = io::join_path(working, "ddc");
  if (holds(cwd_root, at))
    return "; it is in " + at +
           ", under the working directory, which no root is taken from since T119: move it under "
           "the host's derived-data root, or make it again there with the same --ddc";
  return std::string();
}

}  // namespace engine::scene_gen
