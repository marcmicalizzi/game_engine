// Blobs and checkpoints on the wire (roadmap D13, D14; docs/subsystems/protocol.md, "Blobs and
// checkpoints"): `doc.put_blob`, `doc.blob`, `doc.collect_blobs`, `doc.checkpoint`,
// `doc.checkpoints` and `doc.rollback_to`. The reads are `read_only`; the rest are writes a role
// must name. The work is the session's (session_history.cpp) and domain/doc's.
#include "document_path.h"

#include <core/hash/base64.h>
#include <domain/doc/blob_store.h>
#include <domain/doc/checkpoint.h>
#include <domain/doc/document_store.h>
#include <domain/protocol/paths.h>
#include <domain/protocol/policy.h>
#include <domain/protocol/rpc.h>
#include <domain/protocol/session.h>
#include <foundation/io/vfs.h>

#include <filesystem>
#include <system_error>

namespace engine::protocol {

namespace {

Session* begin(Context& ctx, const std::string& session,
               const std::optional<doc::Attribution>& attribution, Caller& caller,
               RpcError& error) {
  Session* s = ctx.sessions->require(session, error);
  if (s == nullptr) return nullptr;
  if (!resolve_caller(ctx, attribution.has_value() ? &*attribution : nullptr, caller, error))
    return nullptr;
  return s;
}

// A client-named file under the document's directory, read whole: false, with nothing said of why,
// for a path the document-path rule refuses, one the roots refuse, one that leaves the document
// through a link, and one with no readable file behind it alike. The check is the one every
// method's paths pass (`admit_document_file`, paths.h), and the read is of the path it admitted.
bool read_under_document(Context& ctx, std::string_view dir, std::string_view path,
                         std::string& bytes) {
  io::Admitted admitted;
  if (!admit_document_file(ctx, dir, "path", path, admitted)) return false;
  return io::read_file(admitted.native, bytes) == io::Status::Ok;
}

bool doc_put_blob(Context& ctx, const PutBlobParams& params, BlobResult& out, RpcError& error) {
  Caller caller;
  Session* s = begin(ctx, params.session, params.attribution, caller, error);
  if (s == nullptr) return false;
  if (params.path.empty() == params.data_base64.empty()) {
    error =
        make_error(codes::k_invalid_argument,
                   "name the bytes once: a file's `path` or `data_base64`, not both or neither");
    return false;
  }
  std::string bytes;
  if (!params.path.empty()) {
    if (!read_under_document(ctx, s->dir(), params.path, bytes)) {
      error = make_error(codes::k_not_found,
                         "no readable file at that path under the document's directory: name it "
                         "by a path relative to the document, without '..', or send the bytes as "
                         "data_base64");
      return false;
    }
  } else if (!base64_decode(params.data_base64, bytes)) {
    error = make_error(codes::k_invalid_argument, "data_base64 is not base64");
    return false;
  }
  if (!s->put_blob(bytes, params.media, out, error)) return false;
  // The blob's place as the client may see it (paths.h): `<root>://<relative>`, never the host's.
  out.path = portable_path(ctx, out.path);
  return true;
}

bool doc_blob(Context& ctx, const BlobParams& params, BlobResult& out, RpcError& error) {
  Session* s = ctx.sessions->require(params.session, error);
  if (s == nullptr) return false;
  std::string bytes, message;
  if (!doc::BlobStore::read(ctx.sessions->vfs(), s->dir(), params.blob, 0, bytes, &message)) {
    error = make_error(
        doc::BlobStore::is_name(params.blob) ? codes::k_not_found : codes::k_invalid_argument,
        std::move(message));
    return false;
  }
  out.ref.blob = params.blob;
  out.ref.size = bytes.size();
  // The portable form, as every path a host answers (protocol.md, "The host's file roots"):
  // empty when no root holds the document, and then a client asks for the bytes inline.
  out.path = portable_path(ctx, doc::BlobStore::path_of(s->dir(), params.blob));
  if (params.data) out.data_base64 = base64_encode(std::string_view(bytes));
  return true;
}

bool doc_collect_blobs(Context& ctx, const CollectBlobsParams& params, CollectBlobsResult& out,
                       RpcError& error) {
  Caller caller;
  Session* s = begin(ctx, params.session, params.attribution, caller, error);
  return s != nullptr && s->collect_blobs(params, out, error);
}

bool doc_checkpoint(Context& ctx, const CheckpointParams& params, doc::CheckpointInfo& out,
                    RpcError& error) {
  Caller caller;
  Session* s = begin(ctx, params.session, params.attribution, caller, error);
  return s != nullptr && s->checkpoint(params.label, caller, out, error);
}

bool doc_checkpoints(Context& ctx, const CheckpointsParams& params, CheckpointsResult& out,
                     RpcError& error) {
  Session* s = ctx.sessions->require(params.session, error);
  if (s == nullptr) return false;
  std::string why;
  out.available = doc::Checkpoints::available(&out.git, &why);
  if (!out.available) {
    out.git = why;
    return true;
  }
  std::string message;
  if (!doc::Checkpoints::list(ctx.sessions->vfs(), s->dir(), params.limit, out.checkpoints,
                              &message)) {
    error = make_error(codes::k_io_error, std::move(message));
    return false;
  }
  return true;
}

bool doc_rollback_to(Context& ctx, const RollbackParams& params, RollbackResult& out,
                     RpcError& error) {
  Caller caller;
  Session* s = begin(ctx, params.session, params.attribution, caller, error);
  return s != nullptr && s->rollback_to(params, caller, out, error);
}

}  // namespace

void add_history_methods(Dispatcher& d) {
  d.add(method<PutBlobParams, BlobResult, &doc_put_blob>(
      "doc.put_blob",
      "Store bytes in the document's blob store, named by their SHA-256, and return the reference "
      "(engine.doc.BlobRef) a property holds instead of the bytes: from a file under the "
      "document's directory, named relative to it, or inline as base64."));
  d.add(read_only(method<BlobParams, BlobResult, &doc_blob>(
      "doc.blob",
      "A blob's path under the document, its bytes checked against its name, and with `data` the "
      "bytes themselves as base64.")));
  d.add(method<CollectBlobsParams, CollectBlobsResult, &doc_collect_blobs>(
      "doc.collect_blobs",
      "Remove the blobs nothing can bring back: no layer, no journal patch (undo history or redo "
      "tail) and no open proposal's merge base names them, and they are older than min_age_s."));
  d.add(method<CheckpointParams, doc::CheckpointInfo, &doc_checkpoint>(
      "doc.checkpoint",
      "Commit the document as saved — manifest, layers and blobs — to its directory's own git "
      "repository, with the journal since the previous checkpoint as the commit's body; 1006 "
      "without git."));
  d.add(read_only(method<CheckpointsParams, CheckpointsResult, &doc_checkpoints>(
      "doc.checkpoints", "The document's checkpoints, newest first, with who took each and why.")));
  d.add(method<RollbackParams, RollbackResult, &doc_rollback_to>(
      "doc.rollback_to",
      "Make every accepted layer what a checkpoint held, as one journaled, undoable patch per "
      "layer it changes, each checked against the caller's role and the leases before any is "
      "committed; open proposals are left as they are. dry_run reports without committing."));
}

}  // namespace engine::protocol
