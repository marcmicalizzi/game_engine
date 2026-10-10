-- A role that may not write, through the console (apps/engine_cli/tests/console_tests.cpp): run
-- under `engine-host --roles content/roles/roles.json --role qa`, the write is refused with 1008
-- and the refusal reaches pcall as a table; the reads around it are never restricted. With a second
-- argument "uncaught" the same write is made unprotected at the end, and the run fails there.
--
--   engine-cli --roles content/roles/roles.json --role qa \
--              --console content/console/forbidden_write.luau -- <document directory> [uncaught]

local dir, mode = ...
assert(dir, "usage: forbidden_write.luau <document directory> [uncaught]")

local session = engine.session.open({ path = dir, create = true }).session
local write = {
  session = session,
  commands = {
    { kind = "CreateObject", id = "00000000000000100000000000000001", type = "engine.content.AssetProvenance" },
  },
  attribution = { rationale = "a write the qa role may not make" },
}

local ok, err = pcall(engine.doc.apply, write)
assert(not ok, "a role that may not write wrote")
assert(err.code == 1008 and err.method == "doc.apply", tostring(err))
assert(err.data.reason == "method", engine.json(err.data))

assert(engine.doc.objects({ session = session }).total == 0)
print("refused", err.code, err.data.reason)

if mode == "uncaught" then
  engine.doc.apply(write)
  print("unreachable")
end
