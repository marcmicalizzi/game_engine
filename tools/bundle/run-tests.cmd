@echo off
rem Double-click entry point for the engine test bundle: runs run-tests.ps1 under
rem Windows PowerShell 5.1, which every Windows 10 and 11 machine already has, and
rem keeps the window open afterwards so a double-click does not flash and vanish.
rem
rem -ExecutionPolicy Bypass applies to this one invocation only and changes nothing
rem on the machine; the script is unsigned because it came out of a zip.
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0run-tests.ps1" %*
set ENGINE_EXIT=%ERRORLEVEL%
echo.
echo ----------------------------------------------------------------------
echo Exit code: %ENGINE_EXIT%   (0 = everything passed, 1 = something failed,
echo                             2 = the bundle could not run at all)
echo.
echo Send back, from this folder:  results.json  results.txt  adapters.json
echo ----------------------------------------------------------------------
pause
exit /b %ENGINE_EXIT%
