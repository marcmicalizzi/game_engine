-- A frame through the console (apps/engine_cli/tests/console_tests.cpp): load the procedural grid,
-- capture it into a directory, and say where the picture went. A machine that cannot render says
-- so in one line and ends cleanly, which the test turns into a skip.
--
--   engine-cli --console content/console/capture.luau -- <output directory>

local out_dir = ...
assert(out_dir, "usage: capture.luau <output directory>")

local ok, loaded = pcall(engine.render.load, { grid = 33 })
if not ok then
  if type(loaded) == "table" and loaded.code == 1007 then
    print("render unavailable", loaded.message)
    return
  end
  error(loaded)
end

local shot = engine.render.capture({
  scene = loaded.scene,
  width = 320,
  height = 180,
  out_dir = out_dir,
  name = "console",
})
assert(shot.width == 320 and shot.height == 180, engine.json(shot))
assert(shot.files.color ~= nil, engine.json(shot.files))
engine.render.unload({ scene = loaded.scene })
print("color", shot.files.color)
