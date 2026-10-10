-- The console's editing round trip (apps/engine_cli/tests/console_tests.cpp; docs/subsystems/apps.md,
-- "The console"): create a document, apply two transactions with a rationale, read the journal back,
-- undo one, and list what is left. Every check is an assert, so a wrong answer ends the run at the
-- line that noticed it with exit code 1, and the test reads the exit code and the printed summary.
--
--   engine-cli --console content/console/edit_session.luau -- <document directory>

local dir = ...
assert(dir, "usage: edit_session.luau <document directory>")

local TYPE = "engine.content.AssetProvenance"
local A = "00000000000000100000000000000001"
local B = "00000000000000100000000000000002"

local session = engine.session.open({ path = dir, create = true, name = "Console" }).session

-- A script keeps state between calls: one attribution, a rationale per transaction.
local attribution = { actor = "console-test", role = "environment", task = "a31" }
local function apply(commands, rationale)
  attribution.rationale = rationale
  return engine.doc.apply({ session = session, commands = commands, attribution = attribution })
end

local first = apply({
  { kind = "CreateObject", id = A, type = TYPE },
  { kind = "SetProperty", id = A, name = "generator", value = "dune-gen" },
}, "the first object, named by its generator")
assert(first.committed and first.patch_index == 0, engine.json(first))

local second = apply({ { kind = "CreateObject", id = B, type = TYPE, parent = A } }, "a child under it")
assert(second.committed and second.patch_index == 1, engine.json(second))

local journal = engine.doc.journal({ session = session })
assert(journal.total == 2 and #journal.patches == 2, engine.json(journal))
assert(journal.patches[1].attribution.rationale == "the first object, named by its generator")
assert(journal.patches[2].attribution.rationale == "a child under it")
assert(journal.patches[2].attribution.actor == "console-test")

local undone = engine.doc.undo({ session = session })
assert(undone.position == 1 and undone.stepped == 1, engine.json(undone))

local listed = engine.doc.objects({ session = session })
assert(listed.total == 1 and #listed.objects == 1, engine.json(listed))
assert(listed.objects[1].id == A, engine.json(listed.objects[1]))
assert(listed.objects[1].properties.generator == "dune-gen")

-- The same thing by name, and a method that does not exist, caught: the error is a table.
local info = engine.call("session.info", { session = session })
assert(info.name == "Console", engine.json(info))
local ok, err = pcall(engine.call, "no.such_method", {})
assert(not ok and err.code == -32601 and err.method == "no.such_method", tostring(err))

print("objects", listed.total)
print("journal", journal.total, "position", undone.position)
