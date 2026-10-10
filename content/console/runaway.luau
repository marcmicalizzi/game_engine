-- A script that never ends (apps/engine_cli/tests/console_tests.cpp): the step budget stops it,
-- the run exits 1 and stderr names this file, the line and the budget. The inner pcall is the
-- point: catching the budget's error only reaches the next safepoint, which raises it again, so a
-- script cannot catch its way past the limit.
--
--   engine-cli --step-budget 100000 --console content/console/runaway.luau

print("looping")
while true do
  pcall(function()
    while true do
    end
  end)
end
