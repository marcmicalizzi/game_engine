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

#include <span>
#include <string>
#include <string_view>

namespace engine::protocol {

class Session;
struct DocumentGuard;

// The guard every host hands SessionManager::open (session.h): `roots.protect` when a session is
// made and `roots.release` when it closes, so an open document's own files are written through
// doc.* alone, and only while it is open. `roots` outlives every close the guard sees.
DocumentGuard document_guard(io::Roots& roots) noexcept;

// True with `out` the admitted path. False with `error` set and the record `path_not_found` logs
// (the same for a missing file), or for a root the host lacks the root's name.
bool admit_client_path(Context& ctx, std::string_view field, std::string_view path,
                       std::string_view base, io::Access access, io::Admitted& out,
                       RpcError& error);

// `admit_client_path` for reading a file that must be there, which then also answers a missing
// one, at the same point of the call and with the same error (`path_not_found`): a method that
// found the file missing later, after validating its other parameters or after asking for a
// device, told "under a root, not there" from "refused" by the answer it gave.
bool admit_existing_path(Context& ctx, std::string_view field, std::string_view path,
                         std::string_view base, io::Admitted& out, RpcError& error);

// A native path and what a client is told in its place (`shown_message`): a file and the client's
// or the record's own spelling of it, or a directory ending in a separator and the spelling of that
// directory (empty, to leave a file's name alone).
struct ShownPath {
  std::string native;
  std::string spelled;
};

// **The one shown form** of a sentence a reader, a loader or an importer wrote about the files it
// opened, before it is answered to a client (T174, the owner's rule: no client is told an absolute
// path of this machine): every occurrence of each `named` native path replaced by its spelling, in
// order, so a file goes before its directory; then, with `roots`, every root's directory by its
// `<root>://` form (`out://later.gltf`), longest first, so a file the roots admitted is named
// portably wherever the sentence names it. The comparison takes either separator, and on Windows
// either case, as the roots compare. The host's own log keeps the native path in typed fields.
std::string shown_message(std::string message, std::span<const ShownPath> named,
                          const io::Roots* roots);
// The same for one file, without roots: the client's own spelling for the admitted native path.
std::string shown_message(std::string message, std::string_view native, std::string_view spelled);

// One plain file name a writer puts in a directory it admitted: no separator, not `.` or `..`,
// and the text rules of a path (no device name, stream, trailing dot or space, reserved character).
bool is_plain_file_name(std::string_view name) noexcept;

// The file `name` in `dir` (admitted by `admit_client_path` for make_directory, and made), itself
// admitted for writing: the last component may not be a link, a junction or a directory (roots.h,
// rule 10), so a link planted at that name is refused rather than written through. The caller
// writes it with `io::write_file_atomic`, which replaces a hard link there instead of writing
// through it. `spelled` is the directory as the client named it, for the refusal's sentence, which
// names `<spelled>/<name>` and never the native path.
bool admit_output_file(Context& ctx, std::string_view field, std::string_view spelled,
                       const io::Admitted& dir, std::string_view name, io::Admitted& out,
                       RpcError& error);

// A file a client names inside a session's document, which a record or the blob store keeps
// relative to it (`doc.place_asset`'s asset, `doc.put_blob`'s path): relative and without `..`
// (domain/protocol's document-path rule), admitted for reading by the roots from the document's
// directory, and after links are resolved still inside that directory. False when any of the three
// refuses, with nothing told; the caller answers its one sentence, the same as for a missing file.
bool admit_document_file(Context& ctx, std::string_view document_dir, std::string_view field,
                         std::string_view path, io::Admitted& out);

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
  // The host's derived-data folder (`--ddc`, else the repository's `ddc/` as AGENTS.md finds it):
  // the root "ddc", made at start if missing, **read** for clients (a client may name a built
  // container in it) and written by the host alone, by content address. Empty: no such root, and
  // a session's derived data goes to its document's `out/ddc` (`derived_data_root`).
  std::string ddc_dir;
};

// Where the host's own work for `session` writes derived data (a generated ground's tiles, the
// generators' caches, a cached build): the host's "ddc" root, else `<the session's document>/out/
// ddc` when its directory may be written, else empty (the generator then refuses, naming why).
// Never `<doc>/ddc` or anywhere else in a document outside its `out/`, which the roots keep from
// every client write (protocol.md, "Where derived data goes").
std::string derived_data_root(const Context& ctx, const Session* session);
// Fills `roots` and logs one line per root (its name, its access and whether it was a link, never
// its directory). False with `error` naming the root that cannot be one: a malformed spec, a name
// taken twice, a read root that is not there, a writable root over the shell.
bool configure_host_roots(io::Roots& roots, const HostRootsConfig& config, std::string& error);
// The lines a host prints once on its own stderr at start, one per root and served directory:
// `<program>: root <name> = <canonical> (read[ and write][, followed from <spelled>])`. The
// operator's, and the one place a root's directory is written down.
std::string describe_host_roots(const io::Roots& roots, std::string_view program);

}  // namespace engine::protocol
