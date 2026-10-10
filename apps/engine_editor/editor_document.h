#pragma once

// engine-editor's document side (docs/subsystems/apps.md, "engine-editor"): the session it holds,
// the selection, and the models its panels are drawn from — with no window and no GPU, so the
// tests build every panel's rows exactly as the window does.
//
// **The editor has no private path into the engine** (06 §6.13). It opens the document through
// `protocol::Session`, the object engine-host's `session.open` makes; every edit is a command list
// through `Session::apply`'s checked form with the editor's `protocol::Caller`, the same call
// `doc.apply` makes; undo and redo are the session's (`doc.undo`, `doc.redo`); the journal panel is
// the session's journal (`doc.journal`); the property panel is the schema registry's descriptor
// (`schema.describe`). In process, because there is no transport yet that a second process could
// hold a session through (roadmap A2): when there is, these calls become requests and nothing above
// them changes.

#include <core/base/macros.h>
#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/ids/id128.h>
#include <core/schema/type_info.h>
#include <domain/doc/document.h>
#include <domain/protocol/policy.h>
#include <domain/protocol/session.h>
#include <foundation/io/vfs.h>

#include <memory>
#include <span>
#include <string>

namespace engine::editor {

// One outliner row: a live record, parents before children and siblings by id, `depth` its number
// of ancestors.
struct OutlinerRow {
  Id128 id;
  std::string type;
  std::string name;  // the record's `name` property, empty when it has none
  u32 depth = 0;
  u32 children = 0;
};

// One property-panel row: a field of the record's schema type, in the type's order, its kind as the
// schema language spells it, and the record's value as JSON — or, when the record does not set it,
// the type's default and `set` false. A property the record holds that the type does not declare
// comes after the fields with the kind "undeclared", so nothing a record holds is hidden.
struct PropertyRow {
  std::string name;
  std::string kind;
  std::string value;
  bool set = false;
};

// One journal-panel row: a committed transaction, newest first.
struct JournalRow {
  u32 index = 0;  // in the journal
  std::string actor;
  std::string role;
  std::string task;
  std::string rationale;
  i64 timestamp_unix_ms = 0;
  u32 commands = 0;
  bool undone = false;  // past the undo position: the redo tail
};

std::string hex(Id128 id);
// A field's kind as the schema language spells it: "f32", "worldpos", "quat", "vec3?", "Mesh[]",
// "map<string, json>" (schemas/README.md).
std::string kind_name(const schema::TypeRef& type);

class EditorDocument {
 public:
  EditorDocument() = default;
  ENGINE_NON_COPYABLE(EditorDocument);

  // Opens the document at `dir` (creating it with one base layer when `create` and the directory
  // holds none) as `actor`, who edits as the director (06 §6.10: the human is the director, the one
  // role that writes layers directly). The session runs with no role policy, as engine-host without
  // `--roles` does.
  bool open(const std::string& dir, bool create, const std::string& actor, std::string& error);
  bool valid() const noexcept { return session_ != nullptr; }

  const doc::Document& document() const;
  protocol::Session& session() { return *session_; }
  const std::string& directory() const noexcept { return dir_; }
  const protocol::Caller& caller() const noexcept { return caller_; }

  void outliner(Vector<OutlinerRow>& out) const;
  // False when `id` names no live record.
  bool properties(Id128 id, Vector<PropertyRow>& out) const;
  void journal(Vector<JournalRow>& out, u32 limit) const;

  Id128 selection() const noexcept { return selection_; }
  void select(Id128 id) noexcept { selection_ = id; }

  // One transaction through the checked write path, attributed to the editor's caller with
  // `rationale` (a default naming the edit when empty). False, with `error`, when the session
  // refuses it or a command fails (atomic: nothing is committed then).
  bool apply(std::span<const doc::Command> commands, const std::string& rationale,
             std::string& error);
  bool undo(std::string& error);
  bool redo(std::string& error);

 private:
  io::Vfs vfs_;
  std::unique_ptr<protocol::SessionManager> sessions_;
  protocol::Session* session_ = nullptr;
  protocol::Caller caller_;
  std::string dir_;
  Id128 selection_;
};

}  // namespace engine::editor
