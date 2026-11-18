#include "shader_cache.h"

#include "sha256.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <system_error>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace shader_meter {
namespace {

// Bumped whenever what a key covers or how an entry is written changes: an old store is then
// simply never read (it lives under its own directory, v<N>, and ages out of nothing; delete it).
constexpr const char* k_format = "engine-shader-cache 1";
constexpr const char* k_layout = "v1";
// A manifest keeps this many compiles of one source and flags, newest last: enough for every
// branch a batch has open on one shader, few enough that a lookup hashes a handful of files.
constexpr std::size_t k_max_entries = 16;
// How often one build walks the store to hold it under its limit, and how old a temporary file
// left by a build that died has to be before the walk removes it.
constexpr auto k_trim_period = std::chrono::minutes(10);
constexpr auto k_temp_age = std::chrono::hours(1);

fs::path layout_root(const fs::path& store) { return store / k_layout; }

std::string to_utf8(const fs::path& p) {
  const std::u8string s = p.generic_u8string();
  return std::string(reinterpret_cast<const char*>(s.data()), s.size());
}

fs::path from_utf8(const std::string& s) {
  return fs::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

// One spelling per file: normal form, forward slashes, and on Windows lower case, so the depfile's
// `D:\x\y.slang` and CMake's `D:/x/y.slang` are the same file.
std::string identity(const fs::path& p) {
  std::string s = to_utf8(p.lexically_normal());
#ifdef _WIN32
  for (char& c : s) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
#endif
  return s;
}

// A path written into a key or a manifest: relative to the source's directory, so the same tree
// in another checkout has the same text; absolute only when no relative path exists (another
// drive).
std::string relative_to(const fs::path& p, const fs::path& base) {
  const fs::path rel = p.lexically_normal().lexically_relative(base.lexically_normal());
  if (rel.empty()) {
    return to_utf8(p.lexically_normal());
  }
  return to_utf8(rel);
}

fs::path resolve(const std::string& stored, const fs::path& base) {
  const fs::path p = from_utf8(stored);
  if (p.is_absolute()) {
    return p.lexically_normal();
  }
  return (base / p).lexically_normal();
}

fs::path absolute_of(const std::string& arg) {
  std::error_code ec;
  fs::path p = fs::absolute(from_utf8(arg), ec);
  if (ec) {
    p = from_utf8(arg);
  }
  return p.lexically_normal();
}

bool write_whole_file(const fs::path& path, const std::string& data) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    return false;
  }
  out.write(data.data(), static_cast<std::streamsize>(data.size()));
  out.close();
  return static_cast<bool>(out);
}

// Into place, replacing what is there (a manifest). A reader holds a manifest open only for the
// moment it reads it, and opens it sharing delete on Windows, so a refusal is brief: a few tries,
// then the entry is dropped, which costs a later miss.
bool rename_replace(const fs::path& from, const fs::path& to) {
#ifdef _WIN32
  for (int attempt = 0; attempt < 20; ++attempt) {
    if (MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING) != 0) {
      return true;
    }
    Sleep(10);
  }
#else
  if (std::rename(from.c_str(), to.c_str()) == 0) {
    return true;
  }
#endif
  std::error_code ec;
  fs::remove(from, ec);
  return false;
}

// Into place only when nothing is there (a result: written once, never replaced, so a build
// copying it out can never read half of a newer one). Another build that stored the same result
// first is success: its bytes are these bytes.
bool rename_once(const fs::path& from, const fs::path& to) {
  std::error_code ec;
#ifdef _WIN32
  if (MoveFileExW(from.c_str(), to.c_str(), 0) != 0) {
    return true;
  }
#else
  if (::link(from.c_str(), to.c_str()) == 0) {
    fs::remove(from, ec);
    return true;
  }
  if (errno != EEXIST && std::rename(from.c_str(), to.c_str()) == 0) {
    return true;  // a file system without hard links
  }
#endif
  fs::remove(from, ec);
  return fs::exists(to, ec);
}

void touch(const fs::path& p) {
  std::error_code ec;
  fs::last_write_time(p, fs::file_time_type::clock::now(), ec);
}

bool hash_file(const fs::path& p, std::string& hex, std::string* content) {
  std::string data;
  if (!read_whole_file(p, data)) {
    return false;
  }
  hex = sha256_hex(data);
  if (content != nullptr) {
    *content = std::move(data);
  }
  return true;
}

struct Entry {
  std::string result;
  std::vector<std::pair<std::string, std::string>> deps;  // (relative path, sha256)
  std::vector<std::string> absent;                        // relative paths
};

// A manifest is lines: `entry <result>`, then `dep <sha256> <path>` and `absent <path>`, then
// `end`. A path is the rest of its line. An entry with no `end` (a torn write, which the rename
// rules out, or a hand edit) is dropped.
std::vector<Entry> parse_manifest(const std::string& text) {
  std::vector<Entry> entries;
  Entry current;
  bool open = false;
  std::size_t at = 0;
  while (at < text.size()) {
    std::size_t end = text.find('\n', at);
    if (end == std::string::npos) {
      end = text.size();
    }
    std::string line = text.substr(at, end - at);
    at = end + 1;
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.rfind("entry ", 0) == 0) {
      current = Entry{};
      current.result = line.substr(6);
      open = current.result.size() == 64;
    } else if (open && line.rfind("dep ", 0) == 0 && line.size() > 4 + 64 + 1) {
      current.deps.emplace_back(line.substr(4 + 64 + 1), line.substr(4, 64));
    } else if (open && line.rfind("absent ", 0) == 0) {
      current.absent.push_back(line.substr(7));
    } else if (open && line == "end") {
      entries.push_back(std::move(current));
      current = Entry{};
      open = false;
    }
  }
  return entries;
}

std::string format_entry(const Entry& e) {
  std::string out = "entry " + e.result + "\n";
  for (const auto& [path, hash] : e.deps) {
    out += "dep " + hash + " " + path + "\n";
  }
  for (const auto& path : e.absent) {
    out += "absent " + path + "\n";
  }
  out += "end\n";
  return out;
}

fs::path manifest_path(const fs::path& store, const std::string& key) {
  return layout_root(store) / "m" / key.substr(0, 2) / key;
}

fs::path result_path(const fs::path& store, const std::string& result, const char* extension) {
  return layout_root(store) / "r" / result.substr(0, 2) / (result + extension);
}

// The -I directories of the compile, absolute.
std::vector<fs::path> include_dirs(const std::vector<std::string>& args) {
  std::vector<fs::path> dirs;
  for (std::size_t i = 0; i < args.size(); ++i) {
    if (args[i] == "-I" && i + 1 < args.size()) {
      dirs.push_back(absolute_of(args[++i]));
    } else if (args[i].size() > 2 && args[i].rfind("-I", 0) == 0) {
      dirs.push_back(absolute_of(args[i].substr(2)));
    }
  }
  return dirs;
}

// The text of every `#include "..."` in a file, as cmake/EngineShaders.cmake's scan reads them.
// A directive inside a comment or a dead #if is read too, which only adds shadows to check.
std::vector<std::string> include_directives(const std::string& text) {
  std::vector<std::string> found;
  std::size_t at = 0;
  while (at < text.size()) {
    std::size_t end = text.find('\n', at);
    if (end == std::string::npos) {
      end = text.size();
    }
    std::size_t i = at;
    at = end + 1;
    while (i < end && (text[i] == ' ' || text[i] == '\t')) {
      ++i;
    }
    if (i >= end || text[i] != '#') {
      continue;
    }
    ++i;
    while (i < end && (text[i] == ' ' || text[i] == '\t')) {
      ++i;
    }
    if (text.compare(i, 7, "include") != 0) {
      continue;
    }
    i += 7;
    while (i < end && (text[i] == ' ' || text[i] == '\t')) {
      ++i;
    }
    if (i >= end || text[i] != '"') {
      continue;
    }
    const std::size_t close = text.find('"', i + 1);
    if (close == std::string::npos || close > end) {
      continue;
    }
    found.push_back(text.substr(i + 1, close - i - 1));
  }
  return found;
}

// Whether the bytes name the directory they were compiled from, either way round and in either
// case: such an output differs between checkouts and is never shared.
bool names_directory(const std::string& bytes, const fs::path& dir) {
  std::string generic = to_utf8(dir.lexically_normal());
  while (!generic.empty() && generic.back() == '/') {
    generic.pop_back();
  }
  if (generic.size() < 4) {
    return false;
  }
  std::string native = generic;
  std::replace(native.begin(), native.end(), '/', '\\');
  auto lower = [](std::string s) {
    for (char& c : s) {
      if (c >= 'A' && c <= 'Z') {
        c = static_cast<char>(c - 'A' + 'a');
      }
    }
    return s;
  };
  const std::string haystack = lower(bytes);
  return haystack.find(lower(generic)) != std::string::npos ||
         haystack.find(lower(native)) != std::string::npos;
}

// Holds the store under its limit, at most once every k_trim_period for the whole machine (the
// stamp's time), least recently used first: a hit touches what it served.
void maybe_trim(const fs::path& store, std::uint64_t max_bytes) {
  const fs::path root = layout_root(store);
  const fs::path stamp = root / "trim-stamp";
  const auto now = fs::file_time_type::clock::now();
  std::error_code ec;
  const auto last = fs::last_write_time(stamp, ec);
  if (!ec && now - last < k_trim_period && max_bytes > 0) {
    return;
  }
  if (!write_whole_file(stamp, "")) {
    return;
  }
  struct File {
    fs::path path;
    std::uintmax_t size;
    fs::file_time_type time;
  };
  std::vector<File> files;
  std::uint64_t total = 0;
  for (const char* sub : {"r", "m", "tmp"}) {
    const bool temporary = std::string(sub) == "tmp";
    fs::recursive_directory_iterator it(root / sub, fs::directory_options::skip_permission_denied,
                                        ec);
    if (ec) {
      ec.clear();
      continue;
    }
    for (; it != fs::recursive_directory_iterator(); it.increment(ec)) {
      if (ec) {
        break;
      }
      std::error_code fec;
      if (!it->is_regular_file(fec)) {
        continue;
      }
      const auto size = it->file_size(fec);
      const auto time = it->last_write_time(fec);
      if (fec) {
        continue;
      }
      if (temporary) {
        if (now - time > k_temp_age) {
          fs::remove(it->path(), fec);
        }
        continue;
      }
      files.push_back(File{it->path(), size, time});
      total += size;
    }
    ec.clear();
  }
  if (total <= max_bytes) {
    return;
  }
  std::sort(files.begin(), files.end(),
            [](const File& a, const File& b) { return a.time < b.time; });
  // Down to nine tenths, so the next store does not walk the store again at once.
  const std::uint64_t target = max_bytes - max_bytes / 10;
  for (const File& f : files) {
    if (total <= target) {
      break;
    }
    std::error_code rec;
    if (fs::remove(f.path, rec)) {
      total -= f.size;
    }
  }
}

}  // namespace

bool read_whole_file(const fs::path& path, std::string& out) {
  out.clear();
#ifdef _WIN32
  // Sharing delete, so a manifest can be renamed over while this reads it.
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return false;
  }
  LARGE_INTEGER size{};
  bool ok = GetFileSizeEx(file, &size) != 0 && size.QuadPart >= 0;
  if (ok) {
    out.resize(static_cast<std::size_t>(size.QuadPart));
    std::size_t done = 0;
    while (ok && done < out.size()) {
      const DWORD want = static_cast<DWORD>(std::min<std::size_t>(out.size() - done, 1u << 30));
      DWORD got = 0;
      ok = ReadFile(file, out.data() + done, want, &got, nullptr) != 0 && got > 0;
      done += got;
    }
  }
  CloseHandle(file);
  return ok;
#else
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return false;
  }
  in.seekg(0, std::ios::end);
  const std::streamoff size = in.tellg();
  if (size < 0) {
    return false;
  }
  in.seekg(0, std::ios::beg);
  out.resize(static_cast<std::size_t>(size));
  in.read(out.data(), size);
  return static_cast<bool>(in) || in.gcount() == size;
#endif
}

fs::path cache_temp_path(const fs::path& store, const char* suffix) {
  static std::atomic<unsigned> counter{0};
#ifdef _WIN32
  const unsigned long pid = GetCurrentProcessId();
#else
  const unsigned long pid = static_cast<unsigned long>(getpid());
#endif
  const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
  const fs::path dir = layout_root(store) / "tmp";
  std::error_code ec;
  fs::create_directories(dir, ec);
  return dir / (std::to_string(pid) + "-" + std::to_string(counter++) + "-" +
                std::to_string(ticks) + suffix);
}

std::vector<std::string> parse_depfile(const std::string& text) {
  std::vector<std::string> out;
  std::string current;
  bool after_target = false;
  const std::size_t n = text.size();
  auto flush = [&] {
    if (after_target && !current.empty()) {
      out.push_back(current);
    }
    current.clear();
  };
  for (std::size_t i = 0; i < n; ++i) {
    const char c = text[i];
    if (c == '\\' && i + 1 < n) {
      const char next = text[i + 1];
      if (next == '\n') {
        flush();
        ++i;
      } else if (next == '\r' && i + 2 < n && text[i + 2] == '\n') {
        flush();
        i += 2;
      } else {
        current.push_back(next);
        ++i;
      }
      continue;
    }
    if (c == '$' && i + 1 < n && text[i + 1] == '$') {
      current.push_back('$');
      ++i;
      continue;
    }
    if (!after_target && c == ':' &&
        (i + 1 == n || text[i + 1] == ' ' || text[i + 1] == '\t' || text[i + 1] == '\r' ||
         text[i + 1] == '\n')) {
      after_target = true;
      current.clear();
      continue;
    }
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
      flush();
      continue;
    }
    current.push_back(c);
  }
  flush();
  return out;
}

std::string cache_manifest_key(const CacheRequest& request) {
  std::string source;
  if (!read_whole_file(request.source, source)) {
    return {};
  }
  const fs::path base = request.source.parent_path();
  const std::string source_id = identity(request.source);
  const std::string output_id = identity(request.output);
  Sha256 h;
  auto line = [&h](std::string_view what, std::string_view value) {
    h.update(what);
    h.update(" ");
    h.update(value);
    h.update("\n");
  };
  line("format", k_format);
  line("compiler", request.compiler);
  line("name", to_utf8(request.source.filename()));
  line("source", sha256_hex(source));
  const auto& args = request.args;
  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string& a = args[i];
    if (a == "-I" && i + 1 < args.size()) {
      line("arg", "-I");
      line("include-dir", relative_to(absolute_of(args[++i]), base));
    } else if (a.size() > 2 && a.rfind("-I", 0) == 0) {
      line("include-dir", relative_to(absolute_of(a.substr(2)), base));
    } else if (!a.empty() && a[0] != '-' && identity(absolute_of(a)) == source_id) {
      line("arg", "<source>");
    } else if (!a.empty() && a[0] != '-' && identity(absolute_of(a)) == output_id) {
      line("arg", "<output>");
    } else {
      line("arg", a);
    }
  }
  return h.hex();
}

bool cache_lookup(const CacheRequest& request, const std::string& manifest_key, std::string& log) {
  const fs::path manifest = manifest_path(request.store, manifest_key);
  std::string text;
  if (!read_whole_file(manifest, text)) {
    return false;
  }
  const fs::path base = request.source.parent_path();
  const std::vector<Entry> entries = parse_manifest(text);
  std::map<std::string, std::string> hashed;  // identity -> sha256, "" when unreadable
  for (auto e = entries.rbegin(); e != entries.rend(); ++e) {
    bool holds = true;
    for (const auto& [path, hash] : e->deps) {
      const fs::path p = resolve(path, base);
      const std::string id = identity(p);
      auto found = hashed.find(id);
      if (found == hashed.end()) {
        std::string hex;
        if (!hash_file(p, hex, nullptr)) {
          hex.clear();
        }
        found = hashed.emplace(id, hex).first;
      }
      if (found->second != hash) {
        holds = false;
        break;
      }
    }
    for (std::size_t i = 0; holds && i < e->absent.size(); ++i) {
      std::error_code ec;
      const fs::file_status status = fs::symlink_status(resolve(e->absent[i], base), ec);
      // Present, or not known to be absent: the shadow may be read now.
      if (ec ? ec != std::errc::no_such_file_or_directory : fs::exists(status)) {
        holds = false;
      }
    }
    if (!holds) {
      continue;
    }
    const fs::path spv = result_path(request.store, e->result, ".spv");
    std::string cost;
    std::string printed;
    std::error_code ec;
    if (!fs::exists(spv, ec) ||
        !read_whole_file(result_path(request.store, e->result, ".cost"), cost) ||
        !read_whole_file(result_path(request.store, e->result, ".log"), printed)) {
      continue;
    }
    if (!fs::copy_file(spv, request.output, fs::copy_options::overwrite_existing, ec) || ec) {
      return false;
    }
    if (!write_whole_file(request.cost, cost + "cache hit\n")) {
      return false;
    }
    touch(spv);
    touch(result_path(request.store, e->result, ".cost"));
    touch(result_path(request.store, e->result, ".log"));
    touch(manifest);
    log = std::move(printed);
    return true;
  }
  return false;
}

std::string cache_store(const CacheRequest& request, const std::string& manifest_key,
                        const fs::path& depfile, const std::string& log,
                        fs::file_time_type started) {
  std::string text;
  if (!read_whole_file(depfile, text)) {
    return "cache not stored: slangc wrote no depfile";
  }
  const std::vector<std::string> listed = parse_depfile(text);
  if (listed.empty()) {
    return "cache not stored: the depfile lists no source";
  }
  // The key was taken from the source as it was before the compile; it must still be that.
  if (cache_manifest_key(request) != manifest_key) {
    return "cache not stored: the source changed while it compiled";
  }
  const fs::path base = request.source.parent_path();
  std::map<std::string, fs::path> files;  // identity -> path, the source and everything slangc read
  files.emplace(identity(request.source), request.source.lexically_normal());
  for (const std::string& d : listed) {
    const fs::path p = absolute_of(d);
    files.emplace(identity(p), p);
  }
  Entry entry;
  std::set<std::string> absent;
  const std::vector<fs::path> dirs = include_dirs(request.args);
  std::error_code ec;
  const fs::path cwd = fs::current_path(ec);
  for (const auto& [id, path] : files) {
    std::string content;
    std::string hash;
    if (!hash_file(path, hash, &content)) {
      return "cache not stored: could not read " + to_utf8(path);
    }
    const auto written = fs::last_write_time(path, ec);
    if (ec || written >= started) {
      return "cache not stored: " + to_utf8(path.filename()) + " was written while it compiled";
    }
    entry.deps.emplace_back(relative_to(path, base), hash);
    // Every place one of this file's includes could have been found: beside it, in each -I
    // directory, beside the shader, in the compile's working directory. The one slangc read is
    // among the files; every other that does not exist now must still not exist at a lookup.
    std::vector<fs::path> roots = {path.parent_path()};
    roots.insert(roots.end(), dirs.begin(), dirs.end());
    roots.push_back(base);
    if (!cwd.empty()) {
      roots.push_back(cwd);
    }
    for (const std::string& directive : include_directives(content)) {
      for (const fs::path& root : roots) {
        const fs::path candidate = (root / from_utf8(directive)).lexically_normal();
        if (files.count(identity(candidate)) != 0) {
          continue;
        }
        std::error_code sec;
        const fs::file_status status = fs::symlink_status(candidate, sec);
        if (!fs::exists(status)) {
          absent.insert(relative_to(candidate, base));
        }
      }
    }
  }
  entry.absent.assign(absent.begin(), absent.end());

  std::string spirv;
  if (!read_whole_file(request.output, spirv)) {
    return "cache not stored: no SPIR-V at the output";
  }
  if (names_directory(spirv, base)) {
    return "cache not stored: the SPIR-V names the directory it was compiled in";
  }
  std::string cost;
  if (!read_whole_file(request.cost, cost)) {
    return "cache not stored: no cost file";
  }

  Sha256 h;
  h.update(manifest_key);
  h.update("\n");
  const std::string body = format_entry(entry);
  h.update(body);
  entry.result = h.hex();

  const fs::path spv = result_path(request.store, entry.result, ".spv");
  fs::create_directories(spv.parent_path(), ec);
  // A result is whole when its three files are; a lookup needs all three. A trim removes files one
  // by one, least recently used first, so it can take a result's log or cost and leave its SPIR-V:
  // the result is then stored again, a part already there kept (rename_once) and the missing ones
  // written, or it would miss for ever (review B8, batch 68).
  const bool whole = fs::exists(spv, ec) &&
                     fs::exists(result_path(request.store, entry.result, ".log"), ec) &&
                     fs::exists(result_path(request.store, entry.result, ".cost"), ec);
  if (!whole) {
    // The log and the cost first, the SPIR-V last, so a reader never finds a SPIR-V before them.
    const std::pair<const char*, const std::string*> parts[] = {{".log", &log}, {".cost", &cost}};
    for (const auto& [extension, data] : parts) {
      const fs::path temp = cache_temp_path(request.store, extension);
      if (!write_whole_file(temp, *data) ||
          !rename_once(temp, result_path(request.store, entry.result, extension))) {
        fs::remove(temp, ec);
        return "cache not stored: could not write the result";
      }
    }
    const fs::path temp = cache_temp_path(request.store, ".spv");
    if (!write_whole_file(temp, spirv) || !rename_once(temp, spv)) {
      fs::remove(temp, ec);
      return "cache not stored: could not write the result";
    }
  }

  const fs::path manifest = manifest_path(request.store, manifest_key);
  fs::create_directories(manifest.parent_path(), ec);
  std::string old;
  std::vector<Entry> entries;
  if (read_whole_file(manifest, old)) {
    entries = parse_manifest(old);
  }
  entries.erase(std::remove_if(entries.begin(), entries.end(),
                               [&](const Entry& e) { return e.result == entry.result; }),
                entries.end());
  entries.push_back(entry);
  if (entries.size() > k_max_entries) {
    entries.erase(entries.begin(), entries.end() - static_cast<std::ptrdiff_t>(k_max_entries));
  }
  std::string out;
  for (const Entry& e : entries) {
    out += format_entry(e);
  }
  const fs::path temp = cache_temp_path(request.store, ".manifest");
  if (!write_whole_file(temp, out) || !rename_replace(temp, manifest)) {
    fs::remove(temp, ec);
    return "cache not stored: could not write the manifest";
  }
  maybe_trim(request.store, request.max_bytes);
  return "cache stored";
}

}  // namespace shader_meter
