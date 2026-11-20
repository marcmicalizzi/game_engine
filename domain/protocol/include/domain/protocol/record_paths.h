#pragma once

// The paths a document's records hold (docs/subsystems/protocol.md, "Paths a document's records
// hold"; roadmap T174, ADR-0071). A record a writing role committed is a client's path stored for
// every later reader: a placement's `mesh`, an authored ground's `source` and its materials'
// images are opened by every `render.load {session}` and every editor viewport, across restarts
// and on other machines. So a record that names a file is admitted **when it lands** — `doc.apply`,
// a transaction's commit, a merge's result, a promotion, a rollback, an undo or a redo that brings
// a path back, all through `Session::commit_commands` and `Session::step` — and **again when it is
// read** (`systems/doc_scene`'s reader, handed the host's roots), because a document can arrive
// from a checkout or an archive with paths no host of this machine admitted.
//
// **What a record stores** is the portable form: `<root>://<relative>` or a path relative to the
// document's directory (`io::admit_stored_path`), never an absolute path, because the document is
// committed and an absolute path of the machine that wrote it names nothing anywhere else.

#include <core/containers/vector.h>
#include <domain/doc/document.h>
#include <domain/protocol/rpc.h>
#include <foundation/io/roots.h>

#include <span>
#include <string>
#include <string_view>

namespace engine::protocol {

struct StoredPath {
  std::string field;  // in the record: "mesh", "source", "materials.layers[2].normal"
  std::string path;   // as the record spells it
};

// Whether records of `type` hold a path a host opens: `engine.scene.MeshPlacement` (`mesh`) and
// `engine.terrain.Heightfield` (`source`, and each ground material layer's `base_color`, `normal`,
// `roughness`, `mask` and `height`) — the two record types `systems/doc_scene` opens files from
// (doc_scene.h). No other `@kind(record)` type names a file today; a reader that opens a path from
// another adds it here and to its own admission in the same commit.
bool holds_stored_paths(std::string_view type) noexcept;
// Every path `object` holds, in field order; nothing for a deleted record or another type. An
// empty string is no path (the reader leaves a placement with an empty `mesh` out and counts it).
void stored_paths(const doc::ResolvedObject& object, Vector<StoredPath>& out);

// The rule at the write: every path the records `ids` hold in `document` — as it is composed now,
// and as layer `layer`, the one the commands wrote, holds them, since a stronger layer's value
// hides that layer's from the composed record but not from its file, its journal or its checkpoint
// — passes `io::admit_stored_path` from `document_dir` against `roots` — or, for a session no host
// gave roots (the engine's own writers, a test of the document alone), the stored form alone
// (`io::check_stored_path_form`). False with `error` the answer every refused path gets
// (`path_not_found`: 1003, `data.reason` "path", `data.field` the record's field) and
// `data.object` the record's id; a deleted record and a record of another type pass.
bool check_stored_paths(const io::Roots* roots, const doc::Document& document, u32 layer,
                        std::span<const doc::ObjectId> ids, std::string_view document_dir,
                        RpcError& error);

}  // namespace engine::protocol
