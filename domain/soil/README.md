# Soil capability

Static soil-bank and tree-throw recipes, closed companion meshes, and explicit
heightfield cooks. Start with [the subsystem contract](../../docs/subsystems/soil.md)
and [research](../../docs/experiments/soil-banks-tree-throw.md).

Public API is in `include/domain/soil/soil.h`, recipes in `schemas/soil.schema`,
implementation in `src/`, and source, topology, cook and determinism tests in `tests/`.

Version 2 (G105) shares curved bank profiles with the terrain cook, buries end
caps, and replaces the rock-shaped plate with a tapered, torn soil envelope and
branching finer roots. Existing recipes need recooking. Composition with a full
fallen crown and automatic support discovery remain G106.
