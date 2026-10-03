# Scenes

[Editor usage](EditorGuide.md#scenes) | [Engine design](EngineDesign.md)

## Runtime ownership

`Scene` owns `FWorld`, with EnTT hidden behind its implementation. `FSceneEntity` is a copied inspection/serialization snapshot, not a borrowed component reference. The initial components are name, parent, transform, optional static mesh asset ID, and optional static/dynamic body motion. The body component describes authored intent; it does not create a Jolt body by itself.

`FObjectId` is a stable UUID written to scene files. `FEntityId` is a transient world-scoped handle with a Herta-owned 64-bit generation. Recycled EnTT handles cannot revive stale Herta handles. Replacing a world invalidates all previous runtime handles. IDs are not row indices or raw EnTT values.

World access is single-owner for now. Create/destroy requests apply only at `FlushStructuralChanges`; a rejected batch changes nothing and drops its queued commands. `ReplaceEntities` validates before replacing the world and clears pending changes only on success. Property updates preserve existing handles. A parent may be destroyed only when no surviving child references it. Hierarchy validation rejects missing parents and cycles; world matrices compose parent-to-child `Translation * Rotation * Scale`.

## File contract

`.hscene` is UTF-8 JSON with `magic: "HertaScene"`, `formatVersion: 1`, and `engineSchemaVersion: 1`. Scene and entity UUIDs, names, parent UUIDs, TRS, mesh asset UUIDs, and body motion are explicit. Positions are double-precision meters; rotations are float XYZW quaternions; scale is positive. Missing optional components are omitted.

Serialization sorts entities by UUID, keeps a fixed field order, uses round-trip numeric formatting, normalizes negative zero, and ends with LF. Reads reject unknown or duplicate fields, unsupported versions, malformed UTF-8, control characters in names, invalid IDs, non-finite/degenerate transforms, and invalid hierarchy. Limits are 64 MiB per file, one million entities, and 1,024 UTF-8 bytes per name. Version 1 has no predecessor to migrate; future changes require explicit version handling.

Saving writes and flushes a unique sibling temporary file, then atomically replaces the target. A failed write or replacement preserves the previous file. Only the operation's own temporary file is removed on failure.

## Initial editor boundary

The editor currently opens flat static-mesh scenes only. It rejects hierarchy and non-mesh entities before replacing its current state, even though the runtime and headless commands accept them. Viewport editing uses float adapters; untouched double position axes remain unchanged when saving other properties. Current editing limits are position magnitude 10,000,000 m per axis and scale 0.001 through 1,000.

`Games/Sandbox/Scenes/Sandbox.hscene` is the default scene. Names, transforms, and mesh choices save with Ctrl+S. Simulation changes only the transient viewport pose; saving during simulation writes the authored world. Scene loading is disabled during simulation. Opening a valid scene replaces unsaved edits, and exit does not automatically save.

Selection still uses frame-local editor indices and resets on scene replacement. There is no undo/redo, prefab format, generic runtime descriptor system, parallel world query API, or game runtime yet.
