#include <domain/doc/checkpoint.h>

#include <core/json/json.h>
#include <core/platform/process.h>
#include <core/schema/json_reflect.h>
#include <core/time/time.h>
#include <domain/doc/blob_store.h>
#include <domain/doc/document_store.h>
#include <domain/doc/partition.h>

#include <cstdlib>
#include <string>

namespace engine::doc {

namespace {

bool fail(std::string* error, std::string message) {
  if (error != nullptr) *error = std::move(message);
  return false;
}

// The repository is the engine's, written as the engine writes it: none of the user's hooks,
// signing, line-ending conversion or automatic maintenance, and a fixed committer. Every call names
// the repository and the work tree outright, so a GIT_DIR in the environment cannot redirect it.
void base_args(const std::string& native, Vector<std::string>& args) {
  args.push_back(Checkpoints::git_executable());
  args.push_back("--git-dir=" + native + "/.git");
  args.push_back("--work-tree=" + native);
  for (const char* c : {"core.autocrlf=false", "core.safecrlf=false", "core.fsmonitor=false",
                        "commit.gpgsign=false", "gc.auto=0", "maintenance.auto=false",
                        "core.quotepath=off", "user.name=engine", "user.email=engine@localhost"}) {
    args.push_back("-c");
    args.push_back(c);
  }
  args.push_back("-c");
  args.push_back("core.hooksPath=" + native + "/.git/engine-no-hooks");
}

// Runs git with `args`, feeds it `input`, and collects its standard output (and its standard error
// with `merge_stderr`). False with a sentence when it cannot start or exits other than zero.
bool run_git(const Vector<std::string>& args, std::string_view input, bool merge_stderr,
             std::string& out, std::string* error) {
  Vector<std::string_view> argv;
  for (const std::string& a : args)
    argv.push_back(a);
  platform::Process process;
  std::string why;
  if (!process.spawn(std::span<const std::string_view>(argv.data(), argv.size()), &why,
                     merge_stderr, true))
    return fail(error, "git could not be started (" + why +
                           "): checkpoints need git on PATH, or ENGINE_GIT naming it");
  if (!input.empty()) (void)process.write(input);
  process.close_stdin();
  out.clear();
  (void)process.read_all(out);
  const i32 code = process.wait();
  if (code != 0) {
    std::string message = "git exited with " + std::to_string(code);
    if (merge_stderr && !out.empty()) {
      std::string tail = out.substr(0, 400);
      while (!tail.empty() && (tail.back() == '\n' || tail.back() == '\r'))
        tail.pop_back();
      message += ": " + tail;
    }
    return fail(error, std::move(message));
  }
  return true;
}

bool native_dir(const io::Vfs& vfs, std::string_view dir, bool for_write, std::string& native,
                std::string* error) {
  if (const io::Status s = vfs.resolve(dir, native, for_write); s != io::Status::Ok)
    return fail(error, std::string(dir) + ": " + io::status_name(s));
  native = io::normalize_path(native);
  return true;
}

// One line: a trailer's value, or a commit's subject.
std::string one_line(std::string_view text) {
  std::string out;
  for (const char c : text)
    out.push_back(c == '\n' || c == '\r' ? ' ' : c);
  while (!out.empty() && out.back() == ' ')
    out.pop_back();
  return out;
}

std::string author_of(const Attribution& who) {
  std::string name;
  for (const char c : one_line(who.actor.empty() ? std::string("engine") : who.actor))
    if (c != '<' && c != '>') name.push_back(c);
  if (name.empty()) name = "engine";
  return name + " <" + "engine@localhost" + ">";
}

constexpr std::string_view k_exclude =
    "# The engine's checkpoints hold the document's own files and nothing else\n"
    "# (docs/subsystems/doc.md, \"Checkpoints\").\n"
    "/*\n"
    "!/manifest.json\n"
    "!/layers/\n"
    "!/blobs/\n";

bool ensure_repository(const std::string& native, std::string* error) {
  if (io::exists(native + "/.git/HEAD")) return true;
  Vector<std::string> args;
  args.push_back(Checkpoints::git_executable());
  args.push_back("-c");
  args.push_back("init.defaultBranch=checkpoints");
  args.push_back("init");
  args.push_back("-q");
  // No template directory: none of the user's (or git's) sample hooks.
  args.push_back("--template=");
  args.push_back(native);
  std::string out;
  if (!run_git(args, {}, true, out, error)) return false;
  if (io::make_directories(native + "/.git/info") != io::Status::Ok ||
      io::write_file_atomic(native + "/.git/info/exclude", k_exclude) != io::Status::Ok)
    return fail(error, native + "/.git/info/exclude could not be written");
  return true;
}

// ---- reading a commit's log entry ---------------------------------------------------------------

constexpr char k_field = '\x1f';
constexpr char k_record = '\x1e';

// One commit's id, parent, subject and engine trailers, as `log_format` writes them.
bool parse_log(std::string_view text, Vector<CheckpointInfo>& out) {
  while (!text.empty()) {
    usize end = text.find(k_record);
    std::string_view record = text.substr(0, end);
    text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
    while (!record.empty() && (record.front() == '\n' || record.front() == '\r'))
      record.remove_prefix(1);
    if (record.empty()) continue;
    std::string_view fields[4];
    for (u32 i = 0; i < 4; ++i) {
      const usize at = i < 3 ? record.find(k_field) : std::string_view::npos;
      fields[i] = record.substr(0, at);
      record = at == std::string_view::npos ? std::string_view{} : record.substr(at + 1);
    }
    CheckpointInfo info;
    info.id = std::string(fields[0]);
    std::string_view parents = fields[1];
    info.parent = std::string(parents.substr(0, parents.find(' ')));
    info.label = std::string(fields[2]);
    std::string_view trailers = fields[3];
    while (!trailers.empty()) {
      const usize nl = trailers.find('\n');
      std::string_view line = trailers.substr(0, nl);
      trailers = nl == std::string_view::npos ? std::string_view{} : trailers.substr(nl + 1);
      const usize colon = line.find(": ");
      if (colon == std::string_view::npos) continue;
      const std::string_view key = line.substr(0, colon);
      const std::string value(line.substr(colon + 2));
      const auto number = [&]() { return std::strtoll(value.c_str(), nullptr, 10); };
      if (key == "Engine-Actor") info.attribution.actor = value;
      else if (key == "Engine-Role") info.attribution.role = value;
      else if (key == "Engine-Task") info.attribution.task = value;
      else if (key == "Engine-Rationale") info.attribution.rationale = value;
      else if (key == "Engine-Time") info.attribution.timestamp_unix_ms = number();
      else if (key == "Engine-Journal-Length") info.journal_length = static_cast<u32>(number());
      else if (key == "Engine-Undo-Position") info.undo_position = static_cast<u32>(number());
      else if (key == "Engine-Journal-From") info.journal_from = static_cast<u32>(number());
    }
    out.push_back(std::move(info));
  }
  return true;
}

void log_args(const std::string& native, Vector<std::string>& args) {
  base_args(native, args);
  args.push_back("log");
  args.push_back("--format=%H%x1f%P%x1f%s%x1f%(trailers:only=true,unfold=true)%x1e");
}

// ---- a commit's files, through `git archive` ---------------------------------------------------

u64 tar_number(const char* p, usize n) {
  // A size past what eleven octal digits hold is written base-256 with the high bit set.
  if ((static_cast<u8>(p[0]) & 0x80u) != 0) {
    u64 v = static_cast<u8>(p[0]) & 0x7fu;
    for (usize i = 1; i < n; ++i)
      v = (v << 8) | static_cast<u8>(p[i]);
    return v;
  }
  u64 v = 0;
  for (usize i = 0; i < n && p[i] != 0; ++i) {
    if (p[i] == ' ') continue;
    if (p[i] < '0' || p[i] > '7') break;
    v = v * 8 + static_cast<u64>(p[i] - '0');
  }
  return v;
}

std::string tar_string(const char* p, usize n) {
  usize len = 0;
  while (len < n && p[len] != 0)
    ++len;
  return std::string(p, len);
}

// The regular files of a tar stream (ustar names, and a pax header's `path` when it has one).
bool parse_tar(std::string_view tar, Vector<std::pair<std::string, std::string>>& files,
               std::string* error) {
  usize at = 0;
  std::string pax_path;
  while (at + 512 <= tar.size()) {
    const char* h = tar.data() + at;
    bool zero = true;
    for (usize i = 0; i < 512 && zero; ++i)
      zero = h[i] == 0;
    if (zero) break;
    const u64 size = tar_number(h + 124, 12);
    const char type = h[156];
    std::string name = tar_string(h, 100);
    if (std::string_view(h + 257, 5) == "ustar") {
      const std::string prefix = tar_string(h + 345, 155);
      if (!prefix.empty()) name = prefix + "/" + name;
    }
    at += 512;
    if (at + size > tar.size()) return fail(error, "git archive: a truncated archive");
    const std::string_view body = tar.substr(at, size);
    at += (size + 511) / 512 * 512;
    if (type == 'x') {
      // "<len> path=<value>\n" records; the next entry's name.
      std::string_view rest = body;
      while (!rest.empty()) {
        const usize sp = rest.find(' ');
        if (sp == std::string_view::npos) break;
        const u64 len = std::strtoull(std::string(rest.substr(0, sp)).c_str(), nullptr, 10);
        if (len == 0 || len > rest.size()) break;
        std::string_view rec = rest.substr(sp + 1, len - sp - 1);
        if (!rec.empty() && rec.back() == '\n') rec.remove_suffix(1);
        if (rec.substr(0, 5) == "path=") pax_path = std::string(rec.substr(5));
        rest = rest.substr(len);
      }
      continue;
    }
    if (type == 'g') continue;
    if (type != '0' && type != 0) {
      pax_path.clear();
      continue;
    }
    if (!pax_path.empty()) name = std::move(pax_path);
    pax_path.clear();
    files.push_back({std::move(name), std::string(body)});
  }
  return true;
}

bool archive(const std::string& native, std::string_view id, std::span<const std::string> paths,
             Vector<std::pair<std::string, std::string>>& files, std::string* error) {
  Vector<std::string> args;
  base_args(native, args);
  args.push_back("archive");
  args.push_back("--format=tar");
  args.push_back(std::string(id));
  args.push_back("--");
  for (const std::string& p : paths)
    args.push_back(p);
  std::string tar;
  // Standard error stays apart: the archive is binary.
  if (!run_git(args, {}, false, tar, error)) return false;
  return parse_tar(tar, files, error);
}

const std::string* file_in(const Vector<std::pair<std::string, std::string>>& files,
                           std::string_view path) {
  for (const auto& f : files)
    if (f.first == path) return &f.second;
  return nullptr;
}

template <class T>
bool parse_file(const std::string& text, std::string_view path, T& out, std::string* error) {
  JsonValue json;
  if (const JsonParseResult r = parse_json(text, json); !r.ok)
    return fail(error, std::string(path) + ": " + r.message);
  schema::ReadContext ctx;
  if (!schema::from_json(out, json, ctx)) {
    std::string message = std::string(path) + ": invalid";
    if (!ctx.diagnostics.empty()) message += ": " + ctx.diagnostics[0].message;
    return fail(error, std::move(message));
  }
  return true;
}

bool parse_layer_text(const std::string& text, std::string_view path, Layer& out,
                      std::string* error) {
  schema::ReadContext ctx;
  if (!Layer::from_json_text(text, out, ctx)) {
    std::string message = std::string(path) + ": invalid";
    if (!ctx.diagnostics.empty()) message += ": " + ctx.diagnostics[0].message;
    return fail(error, std::move(message));
  }
  return true;
}

}  // namespace

// Read at every call, not once: a test names a git that is not there for one case
// (tests/support/test_environment.h), and a checkpoint is a host's, never a loop's.
std::string Checkpoints::git_executable() {
  std::string named;
#if defined(_MSC_VER)
  char* value = nullptr;
  size_t size = 0;
  if (_dupenv_s(&value, &size, "ENGINE_GIT") == 0 && value != nullptr) {
    named = value;
    std::free(value);
  }
#else
  if (const char* value = std::getenv("ENGINE_GIT"); value != nullptr) named = value;
#endif
  return named.empty() ? std::string("git") : named;
}

bool Checkpoints::available(std::string* version, std::string* error) {
  Vector<std::string> args;
  args.push_back(git_executable());
  args.push_back("--version");
  std::string out;
  if (!run_git(args, {}, true, out, error)) return false;
  if (version != nullptr) *version = one_line(out);
  return true;
}

bool Checkpoints::initialized(const io::Vfs& vfs, std::string_view dir) noexcept {
  return vfs.exists(DocumentStore::path_of(DocumentStore::path_of(dir, k_git_dir), "HEAD"));
}

bool Checkpoints::take(const io::Vfs& vfs, std::string_view dir, const Document& doc,
                       const DocumentManifest& manifest, std::string_view label,
                       const Attribution& attribution, CheckpointInfo& out, std::string* error) {
  std::string native;
  if (!native_dir(vfs, dir, true, native, error)) return false;
  if (doc.dirty())
    return fail(error, "the document has changes that are not saved: save it before a checkpoint");
  if (vfs.exists(DocumentStore::path_of(dir, DocumentStore::k_save_log)))
    return fail(error, "a save of this document did not finish (save.pending): save it first");
  if (!ensure_repository(native, error)) return false;

  // The previous checkpoint, for the parent and the start of the journal's segment.
  Vector<CheckpointInfo> previous;
  if (!list(vfs, dir, 1, previous, error)) return false;

  out = CheckpointInfo{};
  out.label = one_line(label.empty() ? std::string_view("checkpoint") : label);
  out.attribution = attribution;
  if (out.attribution.timestamp_unix_ms == 0) out.attribution.timestamp_unix_ms = time::wall_unix_ms();
  out.journal_length = doc.journal().size();
  out.undo_position = manifest.undo_position;
  // The patches since the previous checkpoint, when the journal still holds them in order; after
  // an undo below the previous checkpoint's position the whole applied journal is the segment.
  out.journal_from = 0;
  if (!previous.empty()) {
    out.parent = previous[0].id;
    if (previous[0].undo_position <= out.undo_position) out.journal_from = previous[0].undo_position;
  }

  std::string message = out.label + "\n\n";
  if (!attribution.rationale.empty()) message += attribution.rationale + "\n\n";
  std::string segment;
  for (u32 i = out.journal_from; i < out.undo_position && i < doc.journal().size(); ++i) {
    write_json(schema::to_json(doc.journal()[i]), segment, JsonWriteOptions{.pretty = false});
    segment.push_back('\n');
  }
  if (!segment.empty()) message += segment + "\n";
  // The last paragraph is git trailers, which `list` reads back with %(trailers); one with no
  // value is left out rather than written empty.
  const auto trailer = [&](std::string_view key, const std::string& value) {
    if (!value.empty()) message += std::string(key) + ": " + value + "\n";
  };
  trailer("Engine-Checkpoint", "1");
  trailer("Engine-Actor", one_line(out.attribution.actor));
  trailer("Engine-Role", one_line(out.attribution.role));
  trailer("Engine-Task", one_line(out.attribution.task));
  trailer("Engine-Rationale", one_line(out.attribution.rationale));
  trailer("Engine-Time", std::to_string(out.attribution.timestamp_unix_ms));
  trailer("Engine-Journal-Length", std::to_string(out.journal_length));
  trailer("Engine-Undo-Position", std::to_string(out.undo_position));
  trailer("Engine-Journal-From", std::to_string(out.journal_from));

  Vector<std::string> args;
  std::string text;
  base_args(native, args);
  args.push_back("add");
  args.push_back("-A");
  if (!run_git(args, {}, true, text, error)) return false;

  args.clear();
  base_args(native, args);
  args.push_back("commit");
  args.push_back("-q");
  args.push_back("--allow-empty");
  args.push_back("--cleanup=verbatim");
  args.push_back("--author=" + author_of(out.attribution));
  args.push_back("-F");
  args.push_back("-");
  if (!run_git(args, message, true, text, error)) return false;

  args.clear();
  base_args(native, args);
  args.push_back("rev-parse");
  args.push_back("HEAD");
  if (!run_git(args, {}, true, text, error)) return false;
  out.id = one_line(text);
  return true;
}

bool Checkpoints::list(const io::Vfs& vfs, std::string_view dir, u32 limit,
                       Vector<CheckpointInfo>& out, std::string* error) {
  out.clear();
  if (!initialized(vfs, dir)) return true;
  std::string native;
  if (!native_dir(vfs, dir, false, native, error)) return false;
  Vector<std::string> args;
  std::string text;
  // A repository with no commit yet has no HEAD to log.
  base_args(native, args);
  args.push_back("rev-parse");
  args.push_back("-q");
  args.push_back("--verify");
  args.push_back("HEAD");
  if (!run_git(args, {}, true, text, nullptr)) return true;
  args.clear();
  log_args(native, args);
  if (limit > 0) args.push_back("-n" + std::to_string(limit));
  if (!run_git(args, {}, true, text, error)) return false;
  return parse_log(text, out);
}

bool Checkpoints::find(const io::Vfs& vfs, std::string_view dir, std::string_view id,
                       CheckpointInfo& out, std::string* error) {
  bool hex = !id.empty() && id.size() <= 64;
  for (const char c : id)
    hex = hex && ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'));
  if (!hex || id.size() < 4)
    return fail(error, "'" + std::string(id) + "' is not a checkpoint id (a commit name, at least "
                       "four hexadecimal characters)");
  if (!initialized(vfs, dir)) return fail(error, "this document has no checkpoints");
  std::string native;
  if (!native_dir(vfs, dir, false, native, error)) return false;
  Vector<std::string> args;
  std::string text;
  base_args(native, args);
  args.push_back("rev-parse");
  args.push_back("-q");
  args.push_back("--verify");
  args.push_back(std::string(id) + "^{commit}");
  if (!run_git(args, {}, false, text, nullptr))
    return fail(error, "no checkpoint " + std::string(id) + " in this document's repository");
  const std::string full = one_line(text);
  // A checkpoint is a commit of this document's history, not any object git can name.
  args.clear();
  base_args(native, args);
  args.push_back("merge-base");
  args.push_back("--is-ancestor");
  args.push_back(full);
  args.push_back("HEAD");
  if (!run_git(args, {}, false, text, nullptr))
    return fail(error, "no checkpoint " + std::string(id) + " in this document's history");
  args.clear();
  log_args(native, args);
  args.push_back("-n1");
  args.push_back(full);
  Vector<CheckpointInfo> found;
  if (!run_git(args, {}, true, text, error) || !parse_log(text, found)) return false;
  if (found.empty()) return fail(error, "no checkpoint " + std::string(id));
  out = std::move(found[0]);
  return true;
}

bool Checkpoints::read(const io::Vfs& vfs, std::string_view dir, std::string_view id,
                       CheckpointState& out, std::string* error) {
  std::string native;
  if (!native_dir(vfs, dir, false, native, error)) return false;
  Vector<std::pair<std::string, std::string>> files;
  const std::string paths[] = {std::string(DocumentStore::k_manifest_file),
                               std::string(DocumentStore::k_layers_dir)};
  if (!archive(native, id, paths, files, error)) return false;
  const std::string* manifest = file_in(files, DocumentStore::k_manifest_file);
  if (manifest == nullptr) return fail(error, "checkpoint " + std::string(id) + " has no manifest");
  out = CheckpointState{};
  if (!parse_file(*manifest, "manifest.json", out.manifest, error)) return false;

  for (const LayerRef& ref : out.manifest.layers) {
    const std::string path = std::string(DocumentStore::k_layers_dir) + "/" + ref.file;
    if (!ref.partition.has_value()) {
      const std::string* text = file_in(files, path);
      if (text == nullptr) return fail(error, "checkpoint " + std::string(id) + " lacks " + path);
      Layer layer(ref.name, ref.role);
      if (!parse_layer_text(*text, path, layer, error)) return false;
      out.layers.push_back(std::move(layer));
      continue;
    }
    // A partitioned layer: the index, every tile it lists, and the untiled records.
    const std::string* index_text = file_in(files, path);
    if (index_text == nullptr) return fail(error, "checkpoint " + std::string(id) + " lacks " + path);
    LayerIndex index;
    if (!parse_file(*index_text, path, index, error)) return false;
    const std::string layer_dir(io::parent_path(path));
    Layer layer(ref.name, ref.role);
    layer.set_partition(*ref.partition);
    const auto take_records = [&](const std::string& file_path, const TileCoord* tile) {
      const std::string* text = file_in(files, file_path);
      if (text == nullptr)
        return fail(error, "checkpoint " + std::string(id) + " lacks " + file_path);
      Layer part(ref.name, ref.role);
      if (!parse_layer_text(*text, file_path, part, error)) return false;
      for (const auto& entry : part.records()) {
        TileCoord at;
        const bool tiled = tile_of(entry.second, *ref.partition, at);
        if (tile != nullptr ? (!tiled || at != *tile) : tiled)
          return fail(error, file_path + ": a record filed where its position does not stand");
        layer.set(entry.second);
      }
      return true;
    };
    for (const TileRef& t : index.tiles) {
      const TileCoord tile{t.x, t.y};
      if (!take_records(layer_dir + "/" + std::string(DocumentStore::k_tiles_dir) + "/" + t.file,
                        &tile))
        return false;
    }
    if (!index.untiled.empty() &&
        !take_records(layer_dir + "/" + std::string(DocumentStore::k_untiled_file), nullptr))
      return false;
    out.layers.push_back(std::move(layer));
  }
  return true;
}

bool Checkpoints::read_blobs(const io::Vfs& vfs, std::string_view dir, std::string_view id,
                             std::span<const std::string> names,
                             Vector<std::pair<std::string, std::string>>& out, std::string* error) {
  out.clear();
  if (names.empty()) return true;
  std::string native;
  if (!native_dir(vfs, dir, false, native, error)) return false;
  // Only the names the checkpoint's tree holds can be archived: ask which first.
  Vector<std::string> args;
  base_args(native, args);
  args.push_back("ls-tree");
  args.push_back("-r");
  args.push_back("--name-only");
  args.push_back(std::string(id));
  args.push_back("--");
  args.push_back(std::string(BlobStore::k_blobs_dir));
  std::string listing;
  if (!run_git(args, {}, true, listing, error)) return false;
  Vector<std::string> paths;
  for (const std::string& name : names) {
    if (!BlobStore::is_name(name)) continue;
    std::string path = BlobStore::path_of("", name);  // "blobs/<aa>/<rest>", relative
    if (listing.find(path) != std::string::npos) paths.push_back(std::move(path));
  }
  if (paths.empty()) return true;
  Vector<std::pair<std::string, std::string>> files;
  if (!archive(native, id, std::span<const std::string>(paths.data(), paths.size()), files, error))
    return false;
  for (auto& f : files) {
    // "blobs/aa/rest" back to the name.
    const std::string_view p = f.first;
    if (p.size() < 6 + 64 + 1) continue;
    std::string name = std::string(p.substr(6, 2)) + std::string(p.substr(9));
    if (!BlobStore::is_name(name)) continue;
    out.push_back({std::move(name), std::move(f.second)});
  }
  return true;
}

}  // namespace engine::doc
