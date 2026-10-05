# Editor guide

[Back to README](../README.md) | [Build and run](GettingStarted.md)

The editor loads and saves scenes with selectable entities, editable components, and imported assets. An asterisk after the scene name in the title bar indicates unsaved changes; saving or undoing back to the saved state clears it. The game runtime is not implemented yet.

## Workspace

The workspace renders the scene across the canvas beneath blurred Outliner, Details, and Output Log overlays, with compact chrome, glass viewport controls, and inline transform editing. The camera's projection center follows the unobscured Viewport pane, so overlays do not push the subject off-center. The main Viewport stays anchored to the central dock area with no tab bar; other panels remain movable, dockable, and resizable. The world grid is procedural on the GPU, with antialiased lines and a distance fade.

See [Rendering.md](Rendering.md) for GPU ownership, shader cooking, and renderer verification. Use **File > Reset layout** to restore the default arrangement: Outliner takes 35% of the right column, Details takes the rest, and Output Log takes 26% of the workspace height. Existing saved layouts are preserved when defaults change.

## Viewport controls

Transform readouts use fixed-point numbers with trailing zeros removed. Display rounding does not change stored values or text-entry precision.

In the Viewport, hold RMB and use WASD/QE to fly, Alt+LMB on empty space to orbit, MMB to pan, and the wheel to dolly. Alt+drag a transform gizmo to duplicate the selection and transform the copies as one undo step. Copies appear only when the transform changes; Escape cancels the drag and removes them. Press F to focus the selection. F11 toggles a full-workspace viewport without changing the saved panel layout. Click an object to select it; Ctrl+click or Shift+click toggles additional objects. Click empty viewport space or press Escape in the focused viewport to deselect. During a transform drag, Escape restores the entire selection instead. The toolbar provides move/rotate/scale, local/world axes, snapping, and camera/debug settings. Transform edits apply to the selection around its active object's pivot.

Press G in the focused Viewport to toggle Game view, or use **Overlays > Game view** in viewport settings. It hides the grid, gizmos, selection outlines, empty-entity markers, bounds, axes, viewport stats, and fly-speed HUD without changing individual overlay settings. Camera coordinates remain visible and clickable for copying. Panels and the viewport toolbar remain visible. This does not start Play or Simulate.

Drag LMB from empty viewport space to box-select intersecting projected mesh bounds and empty-entity markers. Shift-drag adds objects, Ctrl-drag toggles them, and Escape restores the selection from before the drag. Selection updates live; gizmo drags and Alt+LMB orbit keep their existing behavior.

## Simulation

The editor preview includes a 1 m cube initially positioned at Y=4 m and a 10 m square floor whose top is at Y=0. Both use the engine cube asset `Engine/Content/Shapes/Cube.gltf`; the floor is that cube scaled to 10 x 0.5 x 10 m. Click **Simulate** or press **Alt+S** to drop the cube from its current editor transform using Jolt Physics. Camera navigation and selection remain available, but transform editing is locked. Press **Escape** or click the active Simulate button to stop and restore the original transform. Physics runs at 60 Hz with render interpolation and bounded catch-up after stalls. **Play** remains disabled because the game runtime is not implemented. This is an editor preview slice, not the full physics milestone.

All entities with Static Mesh and Dynamic Rigid Body components simulate, including duplicates. Static bodies and other dynamics collide using mesh-bounds boxes. A floor is optional. Stopping restores every authored transform; simulated poses are never saved.

## Details and renaming

Details shows the active object's editable location, rotation, and scale. With multiple objects selected, transform changes also apply to the other selected objects. Click a value to type or drag it to adjust; typed values support arithmetic such as `10/2`, applied with Enter. Shift+RMB copies an individual value or a whole transform row from its label; Shift+LMB pastes it. Row clipboard text supports UE's `X/Y/Z` location and scale format and `Pitch/Yaw/Roll` rotation format. Values stay in Herta's units and axis conventions; clipboard compatibility does not convert them. Click the camera coordinates to copy a position that can be pasted onto Location. Floor edits affect its static collider when simulation starts.

Press F2 in the Viewport or Outliner to rename the active object's Outliner row. Details displays the name without an inline editor. Enter or moving focus commits the name; Escape cancels. Blank names are rejected. S toggles grid snapping in the focused viewport when no drag is active; RMB+S still flies backward. Ctrl+Q quits the editor from any panel.

## Scenes

The editor loads `Games/Sandbox/Scenes/Sandbox.hscene` on startup. **File > Save scene** or **Ctrl+S** saves names, transforms, hierarchy, mesh asset IDs, and rigid body components. Changes are not saved automatically on exit. **File > Open scene...** loads another `.hscene`; a failed load keeps the current scene intact. Scenes support parented mesh and empty entities. Details and gizmos edit world-space transforms; the scene stores parent-local transforms.

File > Open scene starts in the current scene's folder, initially `Games/Sandbox/Scenes`. File > Save scene and Ctrl+S write directly to the current scene path without opening a dialog.

Output Log commands: `scene.save [path]`, `scene.load <path>`, and `scene.validate <path>`. Quote paths containing spaces. Saving during simulation writes authored transforms, never the transient physics pose. `scene.load` requires simulation to be stopped.

The scene title shows `*` while changes are unsaved. Opening another scene from File prompts to Save, Discard, or Cancel. Successful loading clears undo history; explicit `scene.load` replaces edits without prompting.

Headlessly, use `HertaEditorCmd scene.validate <path>` or `HertaEditorCmd scene.canonicalize <source> <destination>`. Scene files are canonical UTF-8 JSON with stable object and asset IDs. Unsupported versions, unknown fields, malformed transforms, duplicate IDs, and broken hierarchy references are rejected. See [Scenes.md](Scenes.md) for the runtime and file contracts. Prefab authoring and gameplay are later slices.

## Authoring and undo

**Edit** provides Undo, Redo, Duplicate, Copy, Delete, and Paste. Ctrl+Z undoes; Ctrl+Y or Ctrl+Shift+Z redoes. A gizmo or numeric drag is one undo step for the entire selection. Names, transforms, mesh choices, reset, and structural edits are undoable. Escape cancels an active transform gesture.

Right-click the viewport without dragging to open Select All (Ctrl+A) and Add (Shift+A). Ctrl+A in the focused viewport selects every scene object. Camera fly drags do not open the menu.

Shift+A while hovering Details opens Add Component for the selection. Over other UI, it opens the scene's Add menu at the cursor, regardless of keyboard focus. The viewport context menu's Add also opens the scene menu. Type to fuzzy-search, use arrows or Tab to focus a result, Enter to add it, and Escape to close. Empty Entity and Cube spawn at the camera pivot. Cube is an entity preset with the engine cube Static Mesh attached; an empty entity has only its name, transform, and a selectable editor marker.

Use **Add Component** in Details to attach Static Mesh or Jolt Rigid Body. A new Static Mesh starts with the engine cube asset; its picker selects imported assets. A new Rigid Body defaults to Dynamic; its Body type picker also supports Static. Click the X on a component header to remove it, including when collapsed. Components are independent: removing Static Mesh leaves Rigid Body intact. Add, remove, and property changes support undo/redo, saving, duplication, and clipboard operations. On multi-selection, adding fills missing components and removing removes existing ones; component types are not duplicated. Transform stays built-in.

Rigid Body exposes Mass (kg), Friction, Bounciness, Linear Damping and Angular Damping (1/s), and Gravity Scale. Mass is the body's inertia, not its weight. Friction controls sliding resistance; bounciness ranges from 0 (no bounce) to 1 (elastic). Damping slows motion over time. Gravity Scale 0 disables gravity, 1 uses normal gravity, and larger values accelerate the fall. Static bodies use friction and bounciness but do not use mass, damping, or gravity. Numeric property edits support multi-selection and one undo step per gesture.

The physics preview simulates all meshed Dynamic and Static bodies using scaled box colliders and their authored properties. Meshless rigid bodies persist as authored intent but have no preview collider; general collision shapes remain a later physics slice.

Ctrl+D duplicates selected objects and offsets the copies by one configured translation grid step along world X and Z, even when snapping is off. Selected parent-child links are remapped to the copies; the offset is applied only once per selected hierarchy root. Alt+drag applies no extra offset. Delete removes selected objects but preserves unselected descendants, moving surviving direct children to the scene root without changing their world pose. Ctrl+C/Ctrl+V copy/paste a canonical scene excerpt. Pasted objects receive new UUIDs while retaining asset references and internal parent links; copied objects whose parents are not selected become roots at their original world poses. These shortcuts work in Viewport, Outliner, and Details; text inputs retain their own editing shortcuts. Authoring and undo/redo are disabled during simulation and active gestures. Exit does not automatically save.

## Outliner

The Outliner docks above Details and displays collapsible scene hierarchies with label and type columns. Search retains ancestors of matching objects and reveals matches inside collapsed branches. Shift+click selects a range of visible rows, Ctrl+click toggles individual rows, and Ctrl+A selects all visible objects. Ctrl+Shift+click adds a range to the selection. Selecting a row updates Details and viewport outlines; double-clicking focuses the selection. Click empty list space to deselect. Reopen it from **Window > Outliner**. Existing layouts with docked Details gain the panel above it without resetting other dock positions; detached panels are preserved.

Drag objects onto another row to parent them. Drop onto empty list space or use **Move to root** in the context menu to unparent. World poses stay unchanged, and the operation is one undo step. If both a parent and its descendants are selected, only the highest selected ancestors move. Cycles and poses that require shear are rejected without changing the scene; rotated non-uniform scale can cause this restriction.

## Output Log

Command entry uses shell-style shortcuts: Ctrl+A/E moves to the beginning/end, Alt+B/F moves by word, Ctrl+U/K deletes to the beginning/end, and Ctrl+W deletes the previous whitespace-delimited word. Ctrl+Shift+A selects all. Deletions are undoable with Ctrl+Z; history and completion remain available. Suggestions hide when the command is empty; Escape or clicking outside dismisses them.

The Output Log supports search, verbosity filtering, text selection, copying, pause, auto-scroll, and commands with completion and history. Timestamps show local clock time in `HH:mm:ss` format. Normal messages use category colors; warnings stay yellow/orange and errors red. Search and command entry share an outlined input style. Start a line with `!` to run it through your shell (`cmd.exe` on Windows, `$SHELL` on Linux), for example `!git status`. Shell commands run in the background and their output appears under the `Shell` category when they exit; they get no input, so interactive programs and prompts do not work.

Press the backtick key to open Output Log and focus its command field; pressing Enter submits and returns focus to the viewport. Enter `stat unit` to toggle timing readouts below the viewport's top-right controls, and `stat fps` to toggle FPS independently. Frame is the smoothed frame interval; CPU frame measures the editor render callback, including presentation waits, rather than CPU utilization. GPU UI uses asynchronous GPU timestamps for the main window's UI rendering, including backdrop copy and blur, but excludes the scene renderer and detached windows. It shows `--` until a sample is available. CPU and GPU times overlap and should not be added together.

## Appearance

Use **Appearance** in the bottom bar for all workspace styling, including panel transparency, opacity, blur, reduced motion, gradient, and accent color. Default panel opacity is 90%, with 24 px background blur. Start is a welcome panel, closed by default, and can be reopened from **Window > Start panel**.

## Blur profiling

Set `HERTA_PROFILE_BLUR=1` before launching the editor to log GPU timings for the main window's backdrop copy and blur passes. After 30 warm-up samples, it reports average/minimum/maximum milliseconds over 120 samples, together with framebuffer resolution and blur radius. Resizing or changing the radius restarts sampling. These timings exclude final UI compositing.

## Assets

Content lives in `Games/Sandbox/Content` until project loading exists. Each source file is registered with a `<source>.hmeta` sidecar that holds its stable ID, importer, and import settings; commit both. PNG and JPEG textures and glTF 2.0 `.gltf` and `.glb` models cook in the isolated `HertaAssetWorker` process into the ignored `DerivedDataCache` folder. Manage content headlessly:

```bat
HertaEditorCmd asset.import path\to\Crate.glb --destination Models
HertaEditorCmd asset.reimport Models/Crate.glb --force
HertaEditorCmd asset.list
HertaEditorCmd asset.validate
```

Every asset command accepts `--content-root <path>`. Import a `.gltf` with separate files by copying its folder into content first, then running `asset.import` on the copied file. `.blend` files import the same way when Blender is installed; set `HERTA_BLENDER` to its executable if it is not found automatically. Keep textures a `.blend` references inside the content root.

In the editor, select an object and choose a model or texture from **Static Mesh** in Details, which lists `Engine/` and `Game/` content. It cooks in the background and replaces the mesh between frames; textures preview on a 1 m cube. Picking, outlines, focus, bounds, and Simulate use the loaded mesh's bounds. Mesh choices are saved with the scene. See [AssetPipeline.md](AssetPipeline.md) for the metadata format, cooking rules, cooked formats, build keys, and DerivedDataCache layout. Type in the picker to filter by name. Saved edits to shown assets, including `.blend` files, reimport automatically within about a second; a failed reimport keeps the previous mesh. Drop model or image files onto the editor, or use **File > Import...**, to import them into `Models` or `Textures`. The import dialog is the system file picker on Windows and `zenity` or `kdialog` on Linux.
