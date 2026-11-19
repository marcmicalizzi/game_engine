#pragma once

// The host's file roots (roadmap T174, ADR-0071; docs/subsystems/io.md, "Roots").
//
// A protocol host serves clients that name paths: a scene to load, a directory to capture into, a
// document to open. The host then reads or writes there with its own rights, which are the
// operator's. `Roots` is the one function every such path goes through before the host touches it:
// a set of directories the operator named, each read or read-and-write, and `admit`, which answers
// whether a client's string lands under one of them and, when it does, the canonical native path
// the caller opens. The caller opens that string and never the client's, so what was checked is
// what is opened.
//
// The rules are the same on Windows and Linux wherever the systems allow (a path refused on one is
// refused on the other, so a document written on Linux opens on Windows): lexical refusals first
// (text, Windows' reserved characters, streams, namespace prefixes, UNC, device names, components
// ending in a dot or a space), then `..` collapsed against the base, then the longest prefix that
// exists made canonical by the system (links, junctions, mount points, 8.3 short names and case
// resolved), then compared with each root by whole components. Everything is refused by default:
// an empty set of roots admits nothing.
//
// What `admit` cannot close, and nothing here pretends to: between the check and the open the tree
// can change. No protocol method makes a link, so only a process with the operator's rights can
// race it, and such a process could write the file itself.

#include <core/base/macros.h>
#include <core/base/types.h>
#include <core/containers/vector.h>
#include <foundation/io/vfs.h>

#include <string>
#include <string_view>

namespace engine::io {

enum class Access : u8 {
  read,            // open an existing file or directory for reading, or ask whether it exists
  write,           // create or replace a file (the caller replaces by rename: write_file_atomic)
  make_directory,  // create a directory and its missing parents
};
const char* access_name(Access access) noexcept;

// Why a path was refused. **For the operator's log, never for the client**: a client is told one
// sentence for a refusal and a missing file alike (domain/protocol/paths.h), so it learns nothing
// of the disk outside its roots.
enum class Refusal : u8 {
  none,
  empty,               // the empty string
  too_long,            // over 32,767 bytes on Windows, PATH_MAX on Linux
  not_utf8,            // not valid UTF-8
  control,             // a byte below 0x20, or 0x7F
  reserved_character,  // one of < > " | ? *
  stream,              // a ':' that is not a drive's: an alternate data stream, or "C:x"
  drive_relative,      // Windows: "/x", the root of whichever drive is current
  namespace_prefix,    // \\?\, \\.\ or \??\ in any mix of separators
  unc,     // //server/share, or a drive the system calls remote: no root may be on a network share
  device,  // CON, PRN, AUX, NUL, COM0-9, LPT0-9, COM¹²³, LPT¹²³, CONIN$, CONOUT$, CLOCK$
  trailing_dot_or_space,  // a component other than . and .. that ends in '.' or ' '
  no_base,                // a relative path where the caller has no directory to start from
  above_root,             // a ".." that climbs above the volume's root
  cannot_resolve,         // the system refused to say what the existing prefix is
  no_dos_name,            // Windows: a prefix reached only through a folder mount
  dangling_link,          // the first missing component exists as a link that leads nowhere
  outside,                // under no root
  read_only_root,         // a write under a root that is read-only
  document,               // a write inside an open document's directory but outside its out/
  served,                 // a write inside a directory the host serves to a browser
  final_link,             // a write whose last component is a link, a junction or a reparse point
  not_regular,            // a write onto a directory, or make_directory onto a file
  unknown_root,       // `<name>://` naming a root this host does not have, or whose folder is gone
  absolute_relative,  // `<name>://` followed by an absolute path or a drive
};
const char* refusal_name(Refusal refusal) noexcept;

struct Root {
  std::string name;       // [a-z0-9_]{1,32}: what a path names it by, `<name>://<relative>`
  std::string spelled;    // the directory as the operator named it
  std::string canonical;  // what the system says it is, links followed once, at add()
  bool writable = false;
  bool linked = false;  // `canonical` is not `spelled` made absolute: a link, a short name, a case
  bool made_on_write = false;  // writable and not made at add(): its directory may not exist yet
};

// **A path crosses the protocol and is stored as a root's name and a path under it** —
// `out://captures/a.png` — **never as an absolute path** (the owner, 2026-11-18: an absolute path
// of the operator's machine should not end up in a committed document, and may name a worktree
// that is gone by the time anyone reads it). An absolute path exists only on the operator's command
// line and inside the host process. `native` is what the caller opens; `portable()` is what it
// answers, logs for a client, and stores.
struct Admitted {
  std::string native;    // canonical, to open: never sent to a client, never stored
  std::string root;      // the root's name
  std::string relative;  // under it, '/'-separated; empty for the root itself
  std::string portable() const { return root + "://" + relative; }
};

// `[a-z0-9_]{1,32}`.
bool valid_root_name(std::string_view name) noexcept;

// Steps 1 to 6 alone: the lexical rules, which need no disk. `Refusal::none` when the text passes.
// UNC is not refused here (whether it is allowed depends on the roots).
Refusal check_path_text(std::string_view path) noexcept;

// Steps 7 and 8: `path` joined to `base` when relative (`base` itself made absolute against the
// process's directory when it is relative), `..` collapsed, and the longest existing prefix made
// canonical by the system, the rest appended as written. Forward slashes; a drive letter as the
// system spells it. Does not apply the lexical rules: `admit` runs them first.
Refusal canonical_path(std::string_view path, std::string_view base, std::string& out);

class Roots {
 public:
  Roots() = default;
  ENGINE_NON_COPYABLE(Roots);
  Roots(Roots&&) noexcept = default;
  Roots& operator=(Roots&&) noexcept = default;

  // At start, from the operator's command line. A read root must exist. A writable root is made
  // when `make` is set and it is missing (a `--root <name>=<dir>:rw`), or left to be made by the
  // first write under it (a document's `out/`). `InvalidArgument` for a name that is not
  // `valid_root_name` or is taken, and for a writable root that holds or lies under a served
  // directory; `NotFound` for a read root that is not there; `InvalidPath` for a directory the
  // system cannot resolve. **A directory on a network share is refused** (`InvalidPath`, with
  // `*refused` set to `Refusal::unc`): a UNC spelling, a drive the system calls remote, or a
  // directory whose canonical form is UNC (a mapped drive, a link to a share). With a root on a
  // share, a client's `//server/share` would have to be resolved to be compared, and resolving it
  // makes the host connect to the server the client named with the operator's credentials; so no
  // root is one until someone needs it and designs it (protocol.md, "Roots"). The spelling is
  // checked before anything is made or opened. `*refused`, when given, is the step that refused
  // an `InvalidPath`, for the operator's message.
  Status add(std::string_view name, std::string_view dir, bool writable, bool make = true,
             Refusal* refused = nullptr);
  // A writable root made by its first write at `child`, one plain name directly inside the root
  // `parent` — a document's `out/` — added only when nothing is there or a plain directory is: not
  // a link, a junction or another reparse point, not a file, and canonically `<parent>/<child>`
  // itself. The operator named the document; where a link its author planted leads is not theirs.
  // `NotDirectory` when something else is there (the caller goes without the root, and a path
  // that names it is `unknown_root`); `InvalidArgument` for a `parent` this set lacks or a
  // `child` that is not one plain name; `add`'s answers otherwise.
  Status add_inside(std::string_view name, std::string_view parent, std::string_view child);
  const Root* find(std::string_view name) const noexcept;
  // A directory the host serves to a browser: never written, whatever the roots say. Refused
  // (`InvalidArgument`) when a writable root holds it or lies under it.
  Status serve(std::string_view dir);
  // An open document's directory: no write lands in it but under its `out/`. Held once per
  // directory, compared canonically, with a count of its holders: each `protect` adds one and each
  // `release` takes one off, and the directory is free again when none is left. One holder a
  // session (protocol::SessionManager begins and ends it, protocol.md, "The host's file roots"),
  // since two sessions can name one directory by two spellings.
  Status protect(std::string_view document_dir);
  void release(std::string_view document_dir);

  const Vector<Root>& roots() const noexcept { return roots_; }
  const Vector<std::string>& served() const noexcept { return served_; }
  const Vector<std::string>& protected_documents() const noexcept { return documents_; }

  // `path` is `<root>://<relative>` (the form every answer and record carries), a path relative to
  // `base`, or an absolute path (an operator typing one into engine-cli); all three are checked
  // alike. True with `out` filled: the canonical path to open and the root it lies under, the most
  // specific one that allows the access. False with `why`; `resolved` (when given) is the canonical
  // form when step 8 was reached, for the operator's log only. `<root>://` naming a root this host
  // does not have, or one whose directory is gone, is `Refusal::unknown_root`: never resolved any
  // other way.
  bool admit(std::string_view path, std::string_view base, Access access, Admitted& out,
             Refusal& why, std::string* resolved = nullptr) const;
  // A path this host made or opened, as `<root>://<relative>`; false when no root holds it.
  bool portable(std::string_view native, std::string& out) const;

 private:
  Vector<Root> roots_;
  Vector<std::string> served_;     // canonical
  Vector<std::string> documents_;  // canonical, each once
  Vector<u32> document_holds_;     // beside documents_: how many holders each has
};

// Whether `path` is `dir` or continues it by whole components, both canonical (case-insensitive,
// ordinal, on Windows; bytes on Linux).
bool path_is_under(std::string_view path, std::string_view dir) noexcept;

}  // namespace engine::io
