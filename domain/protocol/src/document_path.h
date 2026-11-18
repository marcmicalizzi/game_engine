#pragma once

// Where a client may name a file on the host's disk: under the document's directory, and nowhere
// else (protocol.md, "Blobs and checkpoints"). A host serves clients of several roles, and a path
// that is absolute or climbs out would let any of them learn whether a file exists anywhere on the
// machine, and, through `doc.put_blob` and the read-only `doc.blob`, read every byte of it.
//
// `stays_under_document` is the lexical rule `doc.place_asset` holds its `asset` to (b68-A12's
// placement.cpp, the same behaviour under the same name): relative, no drive, no stream (`:`), no
// `..` component, no empty name (`a//b`, a trailing separator), no control character. A caller that
// reads the file also checks that the path, links followed, still lands under the directory
// (`doc.put_blob`'s `resolves_under`, history_methods.cpp), and answers a refused path and an
// unreadable one with the same code and the same sentence, so the answer says nothing of the disk
// outside.

#include <core/base/types.h>

#include <string_view>

namespace engine::protocol {

inline bool stays_under_document(std::string_view path) noexcept {
  if (path.empty() || path[0] == '/' || path[0] == '\\') return false;
  usize start = 0;
  for (usize i = 0; i <= path.size(); ++i) {
    const char c = i < path.size() ? path[i] : '/';
    if (static_cast<unsigned char>(c) < 0x20 || c == ':') return false;
    if (c != '/' && c != '\\') continue;
    const std::string_view name = path.substr(start, i - start);
    if (name.empty() || name == "..") return false;
    start = i + 1;
  }
  return true;
}

}  // namespace engine::protocol
