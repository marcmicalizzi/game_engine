#pragma once

// The protocol's one door to the host's disk (docs/subsystems/protocol.md, "The host's file roots";
// ADR-0071). Every handler that opens, writes or makes a directory at a path a client named calls
// `admit_client_path` first and opens what it answers, never the client's string. The roots are
// the operator's (`Context::roots`, filled from `--root` and the document at start); a context
// with no roots refuses every client path, so a host that forgot to give it some fails closed.
//
// **What a client is told**: one code and one sentence for a refusal and a missing file alike —
// 1003, `data.reason` "path", `data.field` the field — repeating only the client's own string. The
// log says no more: `log.tail` is a read every role may make, so a refusal and a missing file leave
// one record (the field, the access, the spelling when it is not absolute), never the reason or the
// canonical form, and a root's directory is printed on the operator's stderr at start
// (`describe_host_roots`), never logged. A path the host answers with or stores is
// `<root>://<relative>` (`portable_path`): no absolute path of the operator's machine crosses the
// protocol or lands in a document.

#include <core/containers/vector.h>
#include <domain/protocol/rpc.h>
#include <foundation/io/roots.h>
#include <foundation/io/vfs.h>

#include <string>
#include <string_view>

namespace engine::protocol {

// True with `out` the admitted path. False with `error` set and the record `path_not_found` logs
// (the same for a missing file), or for a root the host lacks the root's name.
bool admit_client_path(Context& ctx, std::string_view field, std::string_view path,
                       std::string_view base, io::Access access, io::Admitted& out,
                       RpcError& error);

// Where a relative path starts: the document directory of the session `session` names, else the
// host's own document (the root named "doc"), else nothing (a relative path is then refused).
std::string client_base(Context& ctx, std::string_view session);

// The error a method answers when a file it admitted is not there or does not read, so a
// refused path and a missing one cannot be told apart.
RpcError path_not_found(std::string_view field, std::string_view path, io::Access access);

// A path this host made or opened, as the client may see it and a record may store it:
// `<root>://<relative>`, or empty when no root holds it (then it is not answered at all).
std::string portable_path(const Context& ctx, std::string_view native);

// ---- a host's roots at start (engine-host, engine-editor; protocol.md, "The host's file roots")

struct HostRootsConfig {
  std::string doc_dir;              // --doc: the root "doc" (read) and "<doc>/out" as "out" (rw)
  Vector<std::string> root_specs;   // each --root <name>=<dir>[:rw]
  const io::Vfs* mounts = nullptr;  // every --mount is a root of its scheme's name
  std::string shell_dir;            // the listener's directory: served, never written
};
// Fills `roots` and logs one line per root (its name, its access and whether it was a link, never
// its directory). False with `error` naming the root that cannot be one: a malformed spec, a name
// taken twice, a read root that is not there, a writable root over the shell.
bool configure_host_roots(io::Roots& roots, const HostRootsConfig& config, std::string& error);
// The lines a host prints once on its own stderr at start, one per root and served directory:
// `<program>: root <name> = <canonical> (read[ and write][, followed from <spelled>])`. The
// operator's, and the one place a root's directory is written down.
std::string describe_host_roots(const io::Roots& roots, std::string_view program);

}  // namespace engine::protocol
