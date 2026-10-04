# Scenes

[Editor usage](EditorGuide.md#scenes) | [Engine design](EngineDesign.md)

## Runtime ownership

`Scene` owns `FWorld`, with EnTT hidden behind its implementation. `FSceneEntity` is a copied inspection/serialization snapshot, not a borrowed component reference. The initial components are name, parent, transform, optional static mesh asset ID, and optional static/dynamic rigid body with authored material and motion settings. The body component describes authored intent; it does not create a Jolt body by itself.

`FObjectId` is a stable UUID written to scene files. `FEntityId` is a transient world-scoped handle with a Herta-owned 64-bit generation. Recycled EnTT handles cannot revive stale Herta handles. Replacing a world invalidates all previous runtime handles. IDs are not row indices or raw EnTT values.

World access is single-owner for now. Create/destroy requests apply only at `FlushStructuralChanges`; a rejected batch changes nothing and drops its queued commands. `ReplaceEntities` validates before replacing the world and clears pending changes only on success. Property updates preserve existing handles. A parent may be destroyed only when no surviving child references it. Hierarchy validation rejects missing parents and cycles; world matrices compose parent-to-child `Translation * Rotation * Scale`.

`ApplyEntityChanges` is an atomic authoring barrier with explicit before/after snapshots. It rejects stale before states, duplicate IDs, invalid final scenes, and outstanding queued structural requests without consuming those requests. Updates preserve handles; deletion invalidates a handle, and undo restores the persistent UUID with a new runtime handle.

## File contract

`.hscene` is UTF-8 JSON with `format: "HertaScene"`, `formatVersion: 1`, and `engineSchemaVersion: 2`. Scene and entity UUIDs, names, parent UUIDs, TRS, mesh asset UUIDs, and rigid body properties are explicit. Positions are double-precision meters; rotations are float XYZW quaternions; scale is positive. Missing optional components are omitted.

Schema 2 rigid bodies write `type`, `massKg`, `friction`, `restitution`, `linearDamping`, `angularDamping`, and `gravityScale` in that order. Mass is finite and between 0.001 and 1,000,000 kg. Friction, restitution, and both damping values are finite and between 0 and 1. Gravity scale is finite and between 0 and 10. An entity without a rigid body must keep default settings so removing the component cannot silently preserve unsaved data.

Serialization sorts entities by UUID, keeps a fixed field order, uses round-trip numeric formatting, normalizes negative zero, and ends with LF. Reads reject unknown or duplicate fields, unsupported versions, malformed UTF-8, control characters in names, invalid IDs, non-finite or out-of-range values, degenerate transforms, and invalid hierarchy. Limits are 64 MiB per file, one million entities, and 1,024 UTF-8 bytes per name. Engine schema 1 remains readable and migrates type-only rigid bodies to the schema 2 defaults. Saving always writes schema 2.

Saving writes and flushes a unique sibling temporary file, then atomically replaces the target. A failed write or replacement preserves the previous file. Only the operation's own temporary file is removed on failure.

## Initial editor boundary

The editor opens flat scenes containing mesh and empty entities. Hierarchy is still rejected before replacing its current state, even though runtime and headless commands support it. Viewport editing uses float adapters; untouched double position axes remain unchanged when saving other properties. Current editing limits are position magnitude 10,000,000 m per axis and scale 0.001 through 1,000.

Every entity has a name and transform. Static Mesh and Rigid Body are independently optional components, with one of each type per entity. Details adds, removes, and edits them through atomic authoring patches. Adding a component to a group affects only entities missing it; removal affects only entities that have it. Removing a mesh preserves the rigid body and transform. Meshless entities have a selectable editor marker, never a placeholder render mesh. The physics preview simulates every meshed Dynamic rigid body and collides against every meshed Static rigid body, using box shapes from mesh bounds. A static floor is not required for gravity. All simulation poses remain transient and stop restores every authored transform; this is not yet the full physics milestone.

`Games/Sandbox/Scenes/Sandbox.hscene` is the default scene. Names, transforms, and mesh choices save with Ctrl+S. Simulation changes only the transient viewport pose; saving during simulation writes the authored world. Scene loading and authoring are disabled during simulation. File > Open prompts before replacing unsaved edits; explicit `scene.load` replaces them directly. Exit does not automatically save.

## Transactions and clipboard

`EditorCore` owns a bounded, domain-independent transaction history. `EditorScene` records stable-ID before/after patches and selection IDs, not cached row indices or component addresses. Numeric and gizmo gestures produce one entry for the entire selected group; canceled and unchanged edits produce none. Successful saves mark the current history state clean. Successful loads clear history and select the first object; failed operations leave the current state intact.

Undo history keeps at most 256 transactions and 64 MiB of estimated payload and entry storage. New edits discard the redo branch only after success. State tokens distinguish saved states across branching and history eviction. History is session-local and is not serialized.

Create, duplicate, delete, and paste use the same atomic patch path. Clipboard text is a canonical versioned `HertaScene` excerpt using the existing scene schema. Paste allocates new entity UUIDs and preserves referenced asset UUIDs; it does not copy asset files. Malformed text and unsupported editor entities are rejected before mutation. Selection restores by persistent ID when undoing structural edits.

There is no prefab format, generic runtime descriptor system, parallel world query API, or game runtime yet.
