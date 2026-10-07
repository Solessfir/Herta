# Levels

[Editor usage](EditorGuide.md#levels) | [Engine design](EngineDesign.md)

A level is the editable `.hlevel` document. A world is the live ECS instance created from it.

## Runtime ownership

`Level` owns `FWorld`, with EnTT hidden behind its implementation. `FLevelEntity` is a copied inspection/serialization snapshot, not a borrowed component reference. The initial components are name, parent, transform, optional static mesh asset ID, and optional static/dynamic rigid body with authored material and motion settings. The body component describes authored intent; it does not create a Jolt body by itself.

`FObjectId` is a stable UUID written to level files. `FEntityId` is a transient world-scoped handle with a Herta-owned 64-bit generation. Recycled EnTT handles cannot revive stale Herta handles. Replacing a world invalidates all previous runtime handles. IDs are not row indices or raw EnTT values.

World access is single-owner for now. Create/destroy requests apply only at `FlushStructuralChanges`; a rejected batch changes nothing and drops its queued commands. `ReplaceEntities` validates before replacing the world and clears pending changes only on success. Property updates preserve existing handles. A parent may be destroyed only when no surviving child references it. Hierarchy validation rejects missing parents and cycles; world matrices compose parent-to-child `Translation * Rotation * Scale`.

`ApplyEntityChanges` is an atomic authoring barrier with explicit before/after snapshots. It rejects stale before states, duplicate IDs, invalid final levels, and outstanding queued structural requests without consuming those requests. Updates preserve handles; deletion invalidates a handle, and undo restores the persistent UUID with a new runtime handle.

## Runtime descriptors

`LevelDescriptors.h` exposes an immutable, explicitly registered catalog through `GetLevelComponentDescriptors`, `GetLevelComponentDescriptor`, and keyed component/property lookups. Stable type IDs are `Herta.Level.Transform`, `Herta.Level.StaticMesh`, and `Herta.Level.RigidBody`; their serialized component keys remain `transform`, `staticMesh`, and `body`. A property's identity is its owning type ID plus its serialized key, not its C++ member name or inspector label.

Properties describe Herta-owned value types, typed defaults, physical units, and optional scalar ranges. Positions use meters, mass kilograms, and damping inverse seconds; quaternion storage remains XYZW and editor Euler angles remain degrees. Defaults come from the existing component value types. Adding a rigid body defaults to Dynamic; absent rigid bodies remain `None`.

Serialization reads and writes descriptor keys. Details consumes the same component/property labels, rigid-body ranges, and reset defaults; viewport-specific transform limits and widget behavior remain editor-owned. Descriptors do not replace validation, generate serializers, discover C++ declarations, or provide dynamic plugin registration.

## Gameplay-system contracts

`FLevelSystemScheduler` is a single-owner serial executor over an existing `FWorld`; the world must outlive it. Callers explicitly invoke `FixedUpdate`, `Update`, and `Extract` with finite, nonnegative delta seconds. This is not a game loop or automatic fixed-step accumulator.

Each `FLevelSystemDescriptor` declares component read/write masks, same-phase `After` dependencies, structural mutation permission, and consumed/produced event types. Ready systems run in name order. Missing or cross-phase dependencies and cycles reject the phase before callbacks run. Registration changes and recursive execution are rejected during execution; pending external structural requests must be flushed first.

Callbacks use `FLevelSystemContext`, not captured world APIs. Write access also permits reading that component. Queries require declared access for both their projection and required/excluded component masks, and return owned UUID-sorted copies with only requested fields populated. `ReadEntity` enforces access and handle validity; `GetWorldMatrix` requires Transform and Hierarchy reads. `UpdateEntity` validates each update; create/destroy, parenting, and optional-component presence changes also require structural permission. Creating or destroying entities requires write access to every level component.

Ordinary property writes are visible immediately to later systems unless their entity already has a staged structural change, in which case subsequent updates remain staged too. Structural changes publish atomically at the phase barrier; queued entities and pending component-presence changes are not visible to queries before it. A failed callback, invalid access, or rejected barrier discards deferred changes and emitted events, but does not roll back earlier validated property writes. Context failures are latched, so ignoring a returned error cannot turn the phase into success. Callback exceptions are fatal boundary violations.

Events are copied, buffered by target phase, and visible only on a later invocation of that phase, never to later systems in the publishing invocation. Successful execution consumes that phase's input batch; failure retains it. Capacity is 65,536 buffered events with at most 65,536 payload bytes per event. This implementation uses sorted snapshots and serial scheduling; parallel views and execution remain unimplemented, and the editor does not run gameplay systems yet.

## File contract

`.hlevel` is UTF-8 JSON with `format: "HertaLevel"`, `formatVersion: 2`, and `engineSchemaVersion: 5`. Level and entity UUIDs, names, parent UUIDs, TRS, mesh asset UUIDs, and rigid body properties are explicit. Positions are double-precision meters; rotations are float XYZW quaternions; scale is positive. Missing optional components are omitted.

Rigid bodies write `type`, `massKg`, `friction`, `restitution`, `linearDamping`, `angularDamping`, and `gravityScale` in that order. Mass is finite and between 0.001 and 1,000,000 kg. Friction, restitution, and both damping values are finite and between 0 and 1. Gravity scale is finite and between 0 and 10. An entity without a rigid body must keep default settings so removing the component cannot silently preserve unsaved data.

Schema 3 adds a required `folders` array of authoring metadata: stable folder `id`, `name`, nullable folder `parent`, and assigned entity UUIDs in `entities`. Folder and entity identities cannot collide; unknown references, duplicate memberships, and folder cycles are rejected. Folders have no transform or ECS presence. Canonical output sorts folders and their member UUIDs. The editor displays transform hierarchies within the folder of their root entity; moving an attached entity into a folder organizes its whole assembly without modifying entity parenting or poses. Folder deletion unwraps its contents without destroying entities.

Serialization sorts entities by UUID, keeps a fixed field order, uses round-trip numeric formatting, normalizes negative zero, and ends with LF. Reads reject unknown or duplicate fields, unsupported versions, malformed UTF-8, control characters in names, invalid IDs, non-finite or out-of-range values, degenerate transforms, and invalid hierarchy. Limits are 64 MiB per file, one million entities, one million folders/memberships, and 1,024 UTF-8 bytes per name. Schemas 1-4 remain readable; schema 1 migrates type-only rigid bodies to defaults, schemas before 3 have no folders, and schemas before 4 have no material overrides or visual components, and schemas before 5 have no soft bodies. Saving writes schema 5.

Legacy `.hscene` documents with `format: "HertaScene"` and `formatVersion: 1` remain readable with engine schema 1 or 2. Marker/version pairs cannot be mixed. `SerializeLevel`, `SaveLevel`, and `level.canonicalize <source> <destination>` always write the current `HertaLevel` format. Loading does not rewrite or rename source files; use canonicalization with an explicit `.hlevel` destination to migrate them. Public APIs and commands use only Level names, with no legacy aliases.

Saving writes and flushes a unique sibling temporary file, then atomically replaces the target. A failed write or replacement preserves the previous file. Only the operation's own temporary file is removed on failure.

`ValidateLevelDocument` shares header, name, entity, and hierarchy validation between serialization, parsing, and in-memory editor loading. `FEditorLevel::LoadDocument` validates before replacing the world, path, selection, or history; a failed project/level transition preserves the current editor state.

## Initial editor boundary

Schema 4 adds independently optional Light, Sky Atmosphere, and Height Fog components and Static Mesh material-slot asset IDs. Sun references are soft entity UUIDs: duplication and paste remap a sun when it is included in the copied group. An absent linked sun falls back to the first enabled directional light. Visual components and overrides use the same canonical persistence, descriptors, validation, and atomic authoring history as existing components.

Schema 5 adds an optional `softBody` component: `shape` (`rope`, `cloth`, or `ball`), `length` (rope length, cloth width, or ball diameter), cloth `height`, render and collision `thickness`, total `massKg`, `stiffness` from 0 to 1, ball `pressure`, `friction`, `pinned`, and a `material` asset ID. The editor generates its geometry at 10 cm vertex spacing, so a Soft Body needs no Static Mesh. Pinned ropes hold their first vertex at the entity origin and pinned cloth holds its two top corners. Simulate runs soft bodies through Jolt against the same static and dynamic boxes as rigid bodies; a Rigid Body on the same entity is ignored while simulating. Simulated vertices stay transient like rigid poses.

The editor opens hierarchies containing mesh and empty entities. Level transforms are parent-local; viewport gizmos and Details edit world-space TRS. Moving a parent updates unselected descendants without changing their local transforms. Viewport editing uses float adapters; untouched double position axes remain unchanged when saving other properties. Current editing limits are world position magnitude 10,000,000 m per axis and world scale 0.001 through 1,000.

Parenting and unparenting preserve world poses and commit atomically through transaction history. When ancestors and descendants are selected together, reparenting moves only the highest selected ancestors. Cycles, missing parents, and nonrepresentable poses are rejected before mutation. Rotated, non-uniformly scaled hierarchies can produce shear; the current TRS-only viewport rejects these levels and operations rather than approximating their transforms.

Every entity has a name and transform. Static Mesh and Rigid Body are independently optional components, with one of each type per entity. Details adds, removes, and edits them through atomic authoring patches. Adding a component to a group affects only entities missing it; removal affects only entities that have it. Removing a mesh preserves the rigid body and transform. Meshless entities have a selectable editor marker, never a placeholder render mesh. The physics preview simulates every meshed Dynamic rigid body and collides against every meshed Static rigid body, using box shapes from mesh bounds. A static floor is not required for gravity. Simulated bodies have independent world poses; descendants without a simulated body follow their parent through authored local transforms. All simulation poses remain transient and stop restores every authored transform; this is not yet the full physics milestone.

The current project's `startingLevel` is opened initially; `Games/Sandbox/Levels/Sandbox.hlevel` is the default [Sandbox playground](Playground.md). Names, transforms, and mesh choices save with Ctrl+S. Simulation changes only the transient viewport pose; saving during simulation writes the authored world. Level loading and authoring are disabled during simulation. File > Open prompts before replacing unsaved edits; explicit `level.load` replaces them directly. Exit does not automatically save. See [Projects](Projects.md) for descriptor and native-module contracts.

## Transactions and clipboard

`EditorCore` owns a bounded, domain-independent transaction history. `EditorLevel` records stable-ID before/after patches and selection IDs, not cached row indices or component addresses. Numeric and gizmo gestures produce one entry for the entire selected group; canceled and unchanged edits produce none. Successful saves mark the current history state clean. Successful loads clear history and select the first object; failed operations leave the current state intact.

Undo history keeps at most 256 transactions and 64 MiB of estimated payload and entry storage. New edits discard the redo branch only after success. State tokens distinguish saved states across branching and history eviction. History is session-local and is not serialized.

Create, duplicate, delete, reparent, and paste use the same atomic patch path. Deleting a parent does not delete unselected descendants: surviving direct children become level roots with their world poses preserved. Duplication remaps links between selected entities, retains unselected external parents, and applies a world-space offset once per selected hierarchy root.

Clipboard text is a canonical versioned `HertaLevel` excerpt using the existing level schema. Copy preserves links within the selection and detaches fragment roots from unselected parents while retaining their world poses. Paste allocates new entity UUIDs, remaps internal parent links, and preserves referenced asset UUIDs; it does not copy asset files or folder organization. Pasted entities begin at the Outliner root; duplication within a level retains folder membership. Malformed text and unsupported editor entities are rejected before mutation. Selection restores by persistent ID when undoing structural edits.

There is no prefab format, generic reflection framework, parallel world query API, or game runtime yet.
