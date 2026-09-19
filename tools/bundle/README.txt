Engine test bundle
==================

1. Unzip this whole folder somewhere with a couple of gigabytes free.
2. Double-click run-tests.cmd.  It takes a few minutes and keeps its window open
   at the end.
3. Send back three files from this folder:

       adapters.json   what your GPU can and cannot do, and why
       results.json    every test, its exit code and its timing
       results.txt     the same, readable, with the last 50 lines of anything
                       that failed

Nothing is installed and nothing outside this folder is changed.  The only thing
this machine needs is the Microsoft Visual C++ 2015-2022 x64 redistributable; if
it is missing, the script says so by name and stops before running anything.

A machine with no GPU driver is fine.  Every graphics test then reports that it
found no device and passes; that is the expected answer, not a failure.

Some tests open a window for a moment.  Let them.

Details: bundle.json lists every test, and the tests left out of the bundle with
the reason.  run-tests.ps1 -ListOnly prints both without running anything, and
-Filter <name> runs a subset.
