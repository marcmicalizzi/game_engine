// The protocol's door to the host's disk (paths.h; protocol.md, "The host's file roots").
#include <core/log/log.h>
#include <domain/protocol/paths.h>
#include <domain/protocol/session.h>

namespace engine::protocol {

namespace {
ENGINE_LOG_CATEGORY_DEFINE(log_paths, "protocol.paths");

// The name a root-qualified path gives, for the one refusal that says something more than "no
// file": the root is missing on this host (a stored path from another machine, or a worktree that
// is gone). It names only what the client sent.
std::string_view named_root(std::string_view path) noexcept {
  const usize end = path.find("://");
  if (end == std::string_view::npos) return {};
  return path.substr(0, end);
}

// Whether a client's spelling may go into the operator's log as it is: a root-qualified or a
// relative path says nothing of the disk; an absolute one names the host's own directories, and
// `log.tail` is a read every role may make (protocol.md, "What is logged for the operator").
bool loggable_spelling(std::string_view path) noexcept {
  if (path.find("://") != std::string_view::npos) return true;
  if (path.empty() || path.front() == '/' || path.front() == '\\') return false;
  return !(path.size() >= 2 && path[1] == ':');
}
}  // namespace

RpcError path_not_found(std::string_view field, std::string_view path, io::Access access) {
  // One record for a refused path and a missing one, as the answer is one sentence for both: the
  // field, the access and the client's own spelling when it names no directory of the host. The
  // refusal's reason and the canonical form are never logged, because the ring is the client's to
  // read and they are an existence oracle for the operator's disk.
  ENGINE_LOG_WARN(
      log_paths, "a client path was not admitted or is not there", log::field("field", field),
      log::field("path", loggable_spelling(path) ? path : std::string_view("(absolute)")),
      log::field("access", io::access_name(access)));
  JsonValue data = JsonValue::object();
  data.set("reason", "path");
  data.set("field", std::string(field));
  const std::string what = access == io::Access::read ? ": no file this host may read at '"
                                                      : ": no place this host may write at '";
  return make_error(codes::k_not_found, std::string(field) + what + std::string(path) + "'",
                    std::move(data));
}

bool admit_client_path(Context& ctx, std::string_view field, std::string_view path,
                       std::string_view base, io::Access access, io::Admitted& out,
                       RpcError& error) {
  io::Refusal why = io::Refusal::outside;
  if (ctx.roots != nullptr && ctx.roots->admit(path, base, access, out, why)) return true;
  if (why == io::Refusal::unknown_root) {
    // The one refusal the answer names, so the log may name it too: the root, nothing of the disk.
    ENGINE_LOG_WARN(log_paths, "a client path names a root this host lacks",
                    log::field("field", field), log::field("root", named_root(path)),
                    log::field("access", io::access_name(access)));
    JsonValue data = JsonValue::object();
    data.set("reason", "root");
    data.set("field", std::string(field));
    data.set("root", std::string(named_root(path)));
    error = make_error(codes::k_not_found,
                       std::string(field) + ": root " + std::string(named_root(path)) +
                           " is not present on this host",
                       std::move(data));
    return false;
  }
  error = path_not_found(field, path, access);
  return false;
}

bool is_plain_file_name(std::string_view name) noexcept {
  if (name.empty() || name == "." || name == "..") return false;
  if (name.find_first_of("/\\") != std::string_view::npos) return false;
  return io::check_path_text(name) == io::Refusal::none;
}

bool admit_output_file(Context& ctx, std::string_view field, std::string_view spelled,
                       const io::Admitted& dir, std::string_view name, io::Admitted& out,
                       RpcError& error) {
  io::Refusal why = io::Refusal::outside;
  if (is_plain_file_name(name) && ctx.roots != nullptr &&
      ctx.roots->admit(io::join_path(dir.native, name), {}, io::Access::write, out, why))
    return true;
  std::string shown(spelled);
  if (!shown.empty() && shown.back() != '/' && shown.back() != '\\') shown += '/';
  error = path_not_found(field, shown + std::string(name), io::Access::write);
  return false;
}

std::string client_base(Context& ctx, std::string_view session) {
  if (!session.empty() && ctx.sessions != nullptr) {
    if (const Session* s = ctx.sessions->find(session); s != nullptr) {
      // A session's directory is the canonical one it was opened at (session.open admits it).
      return s->dir();
    }
  }
  if (ctx.roots != nullptr) {
    if (const io::Root* doc = ctx.roots->find("doc"); doc != nullptr) return doc->canonical;
  }
  return {};
}

std::string portable_path(const Context& ctx, std::string_view native) {
  std::string out;
  if (ctx.roots == nullptr || !ctx.roots->portable(native, out)) return {};
  return out;
}

namespace {
const char* status_text(io::Status s, bool writable) {
  switch (s) {
    case io::Status::NotFound: return "it is not there (a read root must exist)";
    case io::Status::InvalidArgument:
      return writable ? "its name is malformed or taken, or it holds or lies under the shell's "
                        "directory, which is never written"
                      : "its name is malformed or taken";
    case io::Status::InvalidPath: return "the system cannot say what it is";
    default: return io::status_name(s);
  }
}

bool add_root(io::Roots& roots, std::string_view name, std::string_view dir, bool writable,
              bool make, std::string& error) {
  const io::Status s = roots.add(name, dir, writable, make);
  if (s != io::Status::Ok) {
    error = "root " + std::string(name) + "=" + std::string(dir) + (writable ? ":rw" : "") + ": " +
            status_text(s, writable);
    return false;
  }
  const io::Root& r = roots.roots().back();
  // The name and the access only: the directory is the operator's to read, on the host's stderr
  // at start (`describe_host_roots`), never in the ring `log.tail` serves.
  ENGINE_LOG_INFO(log_paths, "a file root", log::field("name", r.name),
                  log::field("writable", r.writable), log::field("linked", r.linked));
  return true;
}
}  // namespace

std::string describe_host_roots(const io::Roots& roots, std::string_view program) {
  std::string out;
  for (const io::Root& r : roots.roots()) {
    out += std::string(program) + ": root " + r.name + " = " + r.canonical +
           (r.writable ? " (read and write" : " (read");
    if (r.linked) out += ", followed from " + r.spelled;
    out += ")\n";
  }
  for (const std::string& s : roots.served())
    out += std::string(program) + ": served, never written: " + s + "\n";
  return out;
}

bool configure_host_roots(io::Roots& roots, const HostRootsConfig& config, std::string& error) {
  // The shell first, so a writable root over it is refused by name.
  if (!config.shell_dir.empty() && roots.serve(config.shell_dir) != io::Status::Ok) {
    error = "the shell's directory " + config.shell_dir + " cannot be served: a root holds it";
    return false;
  }
  if (!config.doc_dir.empty()) {
    if (!add_root(roots, "doc", config.doc_dir, false, false, error)) return false;
    // Made by the first write under it, not at start: a host that writes nothing leaves the
    // document as it found it.
    if (!add_root(roots, "out", io::join_path(config.doc_dir, "out"), true, false, error))
      return false;
  }
  if (config.mounts != nullptr) {
    for (const io::Mount& m : config.mounts->mounts()) {
      if (!add_root(roots, m.scheme, m.root, m.writable, true, error)) return false;
    }
  }
  for (const std::string& spec : config.root_specs) {
    const usize eq = spec.find('=');
    if (eq == std::string::npos || eq == 0 || eq + 1 == spec.size()) {
      error = "--root " + spec + ": expected <name>=<dir>[:rw]";
      return false;
    }
    std::string_view dir = std::string_view(spec).substr(eq + 1);
    bool writable = false;
    if (dir.ends_with(":rw")) {
      writable = true;
      dir.remove_suffix(3);
    }
    if (!add_root(roots, std::string_view(spec).substr(0, eq), dir, writable, true, error))
      return false;
  }
  return true;
}

}  // namespace engine::protocol
