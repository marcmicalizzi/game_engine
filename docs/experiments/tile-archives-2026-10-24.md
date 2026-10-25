# A tile set read as archives with deep queues (2026-10-24)

Roadmap W76 and W54. The startup trace ([W69](startup-trace-forest-2026-10-24.md)) found 46 of the forest's 72 OS-cold seconds were neither CPU nor a lock: 7,428 small tile files read one at a time, at queue depth one, to stand the region's instances on the ground (`scene expand`) and to build the terrain's first fields (`ground start`). This page measures the format that replaced them — one archive a level with a table of contents ([tileset](../subsystems/tileset.md#the-archive)) — and the reads that replaced one-at-a-time: a batch of range reads with up to `depth` in flight ([reading a level](../subsystems/tileset.md#reading-a-level), [io](../subsystems/io.md#queued-reads-a-deep-queue-from-one-thread-roadmap-w54)).

## What is measured

The measurement is taken with `engine-content terrain-read` ([apps](../subsystems/apps.md#engine-content-terrain-synth-and-terrain-tiles-an-authored-terrain)) over read-only copies of the forest POC's v7 tile set and masks; the table follows in this page's next revision.
