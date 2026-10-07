# Editor guide

[Back to README](../README.md) | [Build and run](GettingStarted.md)

The editor loads and saves levels with selectable entities, editable components, and imported assets. An asterisk after the level name in the title bar indicates unsaved changes; saving or undoing back to the saved state clears it. The game runtime is not implemented yet.

## Workspace

The workspace renders the level across the canvas beneath blurred Outliner, Details, Content Browser, and Output Log overlays, with compact chrome, glass viewport controls, and inline transform editing. The camera's projection center follows the unobscured Viewport pane, so overlays do not push the subject off-center. The main Viewport stays anchored to the central dock area with no tab bar; other panels remain movable, dockable, and resizable. The world grid is procedural on the GPU, with antialiased lines and a distance fade.

See [Rendering.md](Rendering.md) for GPU ownership, shader cooking, and renderer verification. The editor starts maximized. Use **File > Reset layout** to restore the default arrangement: a 440 px right column (20 to 40% of the workspace) where Outliner takes 35% and Details with Performance takes the rest, and a bottom dock taking 36% of the workspace height with Content Browser before Output Log. Existing saved layouts are preserved when defaults change.

## Viewport controls

**Ctrl+N** creates a project, **Ctrl+O** opens a project, and **Ctrl+I** imports assets, matching the File menu actions. These shortcuts leave active text editing and gestures alone. Game content appears before Engine content in the Content Browser's folder tree.

Transform readouts use fixed-point numbers with trailing zeros removed. Display rounding does not change stored values or text-entry precision.

In the Viewport, hold RMB and use WASD/QE to fly, Alt+LMB on empty space to orbit, MMB to pan, and the wheel to dolly. Alt+drag a transform gizmo to duplicate the selection and transform the copies as one undo step. Copies appear only when the transform changes; Escape cancels the drag and removes them. Press F to focus the selection. F11 toggles a full-workspace viewport without changing the saved panel layout. Click an object to select it; Ctrl+click or Shift+click toggles additional objects. Click empty viewport space or press Escape in the focused viewport to deselect. During a transform drag, Escape restores the entire selection instead. The toolbar provides move/rotate/scale, local/world axes, snapping, and camera/debug settings. Transform edits apply to the selection around its active object's pivot.

Press G in the focused Viewport to toggle Game view, or use **Overlays > Game view** in viewport settings. It hides the grid, gizmos, selection outlines, empty-entity markers, bounds, axes, viewport stats, and camera speed readout without changing individual overlay settings. Camera coordinates, off by default under **Overlays > Camera coordinates**, remain visible and clickable for copying when enabled. **Overlays > Camera speed**, also off by default, shows the fly speed above the coordinates. **Overlays > Time of day**, on by default, shows a top-left clock slider whenever the level has an enabled directional light. Dragging it moves the atmosphere's linked sun, or the first enabled directional light, along a fixed arc: sunrise in the east (-X) at 06:00, highest towards the south (-Z) at noon, and sunset in the west (+X) at 18:00. A manually rotated sun shows the nearest time on that arc. Each drag is one undo step. Panels and the viewport toolbar remain visible. This does not start Play or Simulate.

Drag LMB from empty viewport space to box-select intersecting projected mesh bounds and empty-entity markers. Shift-drag adds objects, Ctrl-drag toggles them, and Escape restores the selection from before the drag. Selection updates live; gizmo drags and Alt+LMB orbit keep their existing behavior.

## Outliner folders

Right-click the Outliner to create a folder, or press **Ctrl+Shift+N** while it is focused. **New Folder from Selection** groups selected assemblies. Creation starts an inline name edit; **F2** renames the selected folder. Drag a folder onto another to nest it, or onto empty Outliner space to move it to the root.

Drag entities onto a folder to organize them. Attached children move with their top-level assembly; folder moves never change entity transforms or transform parents. Entity-on-entity drops still parent entities, and **Unparent** explicitly removes a transform parent. Deleting a folder keeps its entities and unwraps nested folders into its parent. Folder edits support save/load and undo/redo; folders are not selectable viewport anchors or runtime components.

## Simulation

The default [Sandbox playground](Playground.md) includes bounce, gravity, friction/damping, and stacking comparisons, plus a static traversal course and hierarchy workshop. Click **Simulate** or press **Alt+S** to run its dynamic bodies and soft bodies using Jolt Physics. Camera navigation and selection remain available, but transform editing is locked. Press **Escape** or click the active Simulate button to stop and restore every authored transform. Physics runs at 60 Hz with render interpolation and bounded catch-up after stalls. **Play** remains disabled because the game runtime is not implemented. This is an editor preview slice, not the full physics milestone.

All entities with Static Mesh and Dynamic Rigid Body components simulate, including duplicates. Static bodies and other dynamics collide using mesh-bounds boxes. A floor is optional. Stopping restores every authored transform; simulated poses are never saved.

## Details and renaming

Details shows the active object's editable location, rotation, and scale. With multiple objects selected, transform changes also apply to the other selected objects. Click a value to type or drag it to adjust; typed values support arithmetic such as `10/2`, applied with Enter. Shift+RMB copies an individual value or a whole transform row from its label; Shift+LMB pastes it. Row clipboard text supports UE's `X/Y/Z` location and scale format and `Pitch/Yaw/Roll` rotation format. Values stay in Herta's units and axis conventions; clipboard compatibility does not convert them. With **Overlays > Camera coordinates** enabled, click the readout to copy a position that can be pasted onto Location. Floor edits affect its static collider when simulation starts.

Press F2 in the Viewport or Outliner to rename the active object's Outliner row. Details displays the name without an inline editor. Enter or moving focus commits the name; Escape cancels. Blank names are rejected. S toggles grid snapping in the focused viewport when no drag is active; RMB+S still flies backward. Ctrl+Q quits the editor from any panel.

## Materials and environment

Right-click a Game folder in the Content Browser to create a reusable `.hmat` material; double-click it to edit. Static Mesh exposes material slots in Details; drag a material onto a viewport mesh to override slot zero. The material editor previews unsaved edits, supports its own undo/redo, and exposes color, PBR maps, packed channels, UVs, masked opacity, and a Slang shader path. Closing a dirty material, changing projects, or exiting prompts to Save, Discard, or Cancel. See [Materials](Materials.md) for the shader contract and save/reload behavior.

Shift+A places Directional, Sky, Point, Spot, or Rect Light, Sky Atmosphere, and Height Fog as well as Empty Entity, Cube, and Rope, Cloth, or Soft Ball soft bodies. Add Component attaches the same visual components to an existing entity. Light guides show emitter shape, direction, and selected range/cones; G hides them with other editor overlays. Changes support level undo/redo, duplication, clipboard, and saving.

Viewport settings > Camera is a physical full-frame (36x24 mm) camera: focal length sets the field of view, and aperture, shutter, and ISO set exposure as photographic EV100 (default 19 mm, f/8, 1/250 s, ISO 100, about EV100 14). They do not add depth of field or motion blur. Viewport settings > Rendering controls shadow quality and native SMAA 1x, defaulting to Soft PCF shadows and SMAA High. Lights use physical units: Directional illuminance in lux, Point, Spot, and Rect luminous power in lumens, and the Sky Light as a multiplier on its calibrated environment, where 1x with full ambient strength is physically correct. **Window > Performance** is open by default as a tab beside Details, which stays the selected tab. It graphs frame, CPU, and GPU time over the last 240 frames, breaks GPU time down per pass with sparklines on a shared scale, and reports lights, draws, render-target memory, and shadow budgets. **Pause** freezes the history for inspection. Sky Light separates ambient intensity from visible sky, using a cooked HDR texture or the linked atmosphere's generated sky. Height Fog supports non-volumetric and volumetric modes plus low/medium/high integration quality. See [Rendering](Rendering.md) for budgets and current limitations.

## Levels

The default project is `Games/Sandbox/Sandbox.hertaproject`. Use **File > Open project...** or launch `HertaEditor --project=<path>` to choose another project. Dropping a `.hlevel` onto `HertaEditor.exe` (or passing it as the first plain argument) opens that level inside the nearest project whose `.hertaproject` sits in its folder or an ancestor, falling back to Sandbox; dropping a `.hertaproject` opens that project. `--project=` takes precedence over a dropped level's owning project. **File > New project...** creates a minimal C++ Game module, content folder, and empty level in a new directory. Creation runs in the background and never overwrites an existing directory. Opening another level or project prompts to Save, Discard, or Cancel when the current level is dirty. Invalid projects or starting levels leave the current document intact. See [Projects.md](Projects.md) for headless commands and generated module builds.

The editor loads `Games/Sandbox/Levels/Sandbox.hlevel` on startup. **File > Save level** or **Ctrl+S** saves names, folders, transforms, hierarchy, mesh/material asset IDs, and authored components, including rigid bodies, lights, atmosphere, and fog. Material source edits are saved separately in their material editor. Changes are not saved automatically on exit. **File > Open level...** loads another `.hlevel`; a failed load keeps the current level intact. Levels support parented mesh and empty entities. Details and gizmos edit world-space transforms; the level stores parent-local transforms.

File > Open level starts in the current level's folder, initially `Games/Sandbox/Levels`. File > Save level and Ctrl+S write directly to the current level path without opening a dialog.

Output Log commands: `level.save [path]`, `level.load <path>`, and `level.validate <path>`. Quote paths containing spaces. Saving during simulation writes authored transforms, never the transient physics pose. `level.load` requires simulation to be stopped.

The level title shows `*` while changes are unsaved. Opening another level from File prompts to Save, Discard, or Cancel. Successful loading clears undo history; explicit `level.load` replaces edits without prompting.

Headlessly, use `HertaEditorCmd level.validate <path>` or `HertaEditorCmd level.canonicalize <source> <destination>`. Level files are canonical UTF-8 JSON with stable object and asset IDs. Unsupported versions, unknown fields, malformed transforms, duplicate IDs, and broken hierarchy references are rejected. See [Levels.md](Levels.md) for the runtime and file contracts. Prefab authoring and gameplay are later slices.

## Authoring and undo

**Edit** provides Undo, Redo, Duplicate, Copy, Delete, and Paste. Ctrl+Z undoes; Ctrl+Y or Ctrl+Shift+Z redoes. A gizmo or numeric drag is one undo step for the entire selection. Names, transforms, mesh choices, reset, and structural edits are undoable. Escape cancels an active transform gesture.

Right-click the viewport without dragging to open Select All (Ctrl+A) and Add (Shift+A). Ctrl+A in the focused viewport selects every level object. Camera fly drags do not open the menu.

Shift+A while hovering Details opens Add Component for the selection. Over other UI, it opens the level's Add menu at the cursor, regardless of keyboard focus. The viewport context menu's Add also opens the level menu. Type to fuzzy-search, use arrows or Tab to focus a result, Enter to add it, and Escape to close. Empty Entity and Cube spawn at the camera pivot. Cube is an entity preset with the engine cube Static Mesh attached; an empty entity has only its name, transform, and a selectable editor marker.

Use **Add Component** in Details to attach Static Mesh, Jolt Rigid Body, Soft Body, a light, Sky Atmosphere, or Height Fog. A new Static Mesh starts with the engine cube asset; its picker selects imported assets, and its material slots override imported defaults independently. A new Rigid Body defaults to Dynamic; its Body type picker also supports Static. Click the X on a component header to remove it, including when collapsed. Components are independent: removing Static Mesh leaves Rigid Body intact. Add, remove, and property changes support undo/redo, saving, duplication, and clipboard operations. On multi-selection, adding fills missing components and removing removes existing ones; component types are not duplicated. Transform stays built-in.

Rigid Body exposes Mass (kg), Friction, Bounciness, Linear Damping and Angular Damping (1/s), and Gravity Scale. Mass is the body's inertia, not its weight. Friction controls sliding resistance; bounciness ranges from 0 (no bounce) to 1 (elastic). Damping slows motion over time. Gravity Scale 0 disables gravity, 1 uses normal gravity, and larger values accelerate the fall. Static bodies use friction and bounciness but do not use mass, damping, or gravity. Numeric property edits support multi-selection and one undo step per gesture.

The physics preview simulates all meshed Dynamic and Static bodies using scaled box colliders and their authored properties. Meshless rigid bodies persist as authored intent but have no preview collider; general collision shapes remain a later physics slice.

Ctrl+D duplicates selected objects and offsets the copies by one configured translation grid step along world X and Z, even when snapping is off. Selected parent-child links are remapped to the copies; the offset is applied only once per selected hierarchy root. Alt+drag applies no extra offset. Delete removes selected objects but preserves unselected descendants, moving surviving direct children to the level root without changing their world pose. Ctrl+C/Ctrl+V copy/paste a canonical level excerpt. Pasted objects receive new UUIDs while retaining asset references and internal parent links; copied objects whose parents are not selected become roots at their original world poses. These shortcuts work in Viewport, Outliner, and Details; text inputs retain their own editing shortcuts. Authoring and undo/redo are disabled during simulation and active gestures. Exit does not automatically save.

## Outliner

The Outliner docks above Details and displays collapsible level hierarchies with label and type columns. Search retains ancestors of matching objects and reveals matches inside collapsed branches. Shift+click selects a range of visible rows, Ctrl+click toggles individual rows, and Ctrl+A selects all visible objects. Ctrl+Shift+click adds a range to the selection. Selecting a row updates Details and viewport outlines; double-clicking focuses the selection. Clicking an object in the viewport expands its collapsed parents and folders and scrolls its row into view. Click empty list space to deselect. Reopen it from **Window > Outliner**. Existing layouts with docked Details gain the panel above it without resetting other dock positions; detached panels are preserved.

Drag objects onto another row to parent them. Drop onto empty list space or use **Move to root** in the context menu to unparent. World poses stay unchanged, and the operation is one undo step. If both a parent and its descendants are selected, only the highest selected ancestors move. Cycles and poses that require shear are rejected without changing the level; rotated non-uniform scale can cause this restriction.

## Output Log

Command entry uses shell-style shortcuts: Ctrl+A/E moves to the beginning/end, Alt+B/F moves by word, Ctrl+U/K deletes to the beginning/end, and Ctrl+W deletes the previous whitespace-delimited word. Ctrl+Shift+A selects all. Deletions are undoable with Ctrl+Z; history and completion remain available. Suggestions hide when the command is empty; Escape or clicking outside dismisses them.

The Output Log supports search, verbosity filtering, text selection, copying, pause, auto-scroll, and commands with completion and history. Timestamps show local clock time in `HH:mm:ss` format. Normal messages use category colors; warnings stay yellow/orange and errors red. Search and command entry share an outlined input style. Start a line with `!` to run it through your shell (`cmd.exe` on Windows, `$SHELL` on Linux), for example `!git status`. Shell commands run in the background and their output appears under the `Shell` category when they exit; they get no input, so interactive programs and prompts do not work.

Press the backtick key to open Output Log and focus its command field; pressing Enter submits and returns focus to the viewport. Enter `stat unit` to toggle timing readouts below the viewport's top-right controls, and `stat fps` to toggle FPS independently. Frame is the smoothed frame interval; CPU frame measures the editor render callback, including presentation waits, rather than CPU utilization. GPU UI uses asynchronous GPU timestamps for the main window's UI rendering, including backdrop copy and blur, but excludes the scene renderer and detached windows. It shows `--` until a sample is available. CPU and GPU times overlap and should not be added together.

## Appearance

Use **Appearance** in the bottom bar for all workspace styling, including panel transparency, opacity, blur, gradient, and accent color. Default panel opacity is 95%, with 24 px background blur.

## Blur profiling

Set `HERTA_PROFILE_BLUR=1` before launching the editor to log GPU timings for the main window's backdrop copy and blur passes. After 30 warm-up samples, it reports average/minimum/maximum milliseconds over 120 samples, together with framebuffer resolution and blur radius. Resizing or changing the radius restarts sampling. These timings exclude final UI compositing.

## Assets

**Ctrl+Space** hides or restores the entire bottom panel, including Content Browser and Output Log, without focusing search. It preserves panel size, open tabs, and the active tab. The individual bottom-bar buttons and Window menu entries show or hide their panels and reveal the bottom dock if necessary. The default bottom dock occupies 36% of the workspace, with Content Browser before Output Log. Double-click a folder tile to browse it; search includes nested folders and assets. Hold **Ctrl+wheel** over the browser, or drag the size slider at the right of the footer, to resize tiles; the slider shows the tile size and zooming fully out switches to a compact list. Drag the sidebar divider to resize the folder tree. Right-click for **Import...** and **New folder**. **Ctrl+Shift+N** or the context menu's **New folder** creates `New Folder` in the current Game folder and selects the new tile's name for inline editing in the main browser area. Existing names receive a numeric suffix. Enter or clicking away commits the name; Escape keeps the created folder's default name. Engine content is read-only. Drag a model tile into the viewport to create a selected Static Mesh entity at the drop position; creation is one undo step. Textures are browsable but do not create entities. Tiles and folder rows are clipped at scale, and narrow panels use a folder picker.

Right-click a folder and choose **Open in Explorer** (Windows) or **Open in file manager** (Linux) to open it in the default file manager. Right-clicking empty space opens the current content folder. **Ctrl+Shift+O** in the focused Content Browser opens the selected folder, the selected asset's containing folder, or the current folder when nothing is selected.

Drop files from your file manager onto the Content Browser or use its **Import...** button to import into the selected Game subfolder. Dropping elsewhere, or importing with Game/Engine selected, uses `Models` or `Textures`. Import runs in the background, reports failures in Output Log, and refreshes the browser after success. Project switching waits until imports finish. External multi-file `.gltf` assets must already have their relative dependencies inside content; use `.glb` for a self-contained file drop.

Model tiles show textured previews of their cooked meshes. Visible tile previews load asynchronously, share scene mesh uploads, and refresh after reimport. The preview cache retains up to 64 visible assets; list view uses compact type icons.

Hover an asset for its type, mounted source path, and stable ID. Loaded previews also show mesh vertex/triangle/material counts and bounds in meters, or texture dimensions, format, color space, and mip count. Hovering never loads an asset; uncached previews show their loading status instead.

Game content comes from the loaded project's content root, initially `Games/Sandbox/Content`. Each source file is registered with a `<source>.hmeta` sidecar that holds its stable ID, importer, and import settings; commit both. PNG/JPEG/HDR textures, `.hmat` materials, and glTF 2.0 `.gltf`/`.glb` models cook in the isolated `HertaAssetWorker` process into the project's ignored `DerivedDataCache` folder. HDR radiance remains floating-point Linear data. Manage content headlessly:

```bat
HertaEditorCmd asset.import path\to\Crate.glb --destination Models
HertaEditorCmd asset.reimport Models/Crate.glb --force
HertaEditorCmd asset.list
HertaEditorCmd asset.validate
```

Every asset command accepts `--content-root <path>`. Import a `.gltf` with separate files by copying its folder into content first, then running `asset.import` on the copied file. `.blend` files import the same way when Blender is installed; set `HERTA_BLENDER` to its executable if it is not found automatically. Keep textures a `.blend` references inside the content root.

In the editor, select an object and choose a model or texture from **Static Mesh** in Details, which lists `Engine/` and `Game/` content. It cooks in the background and replaces the mesh between frames; textures preview on a 1 m cube. Picking, outlines, focus, bounds, and Simulate use the loaded mesh's bounds. Mesh choices are saved with the level. See [AssetPipeline.md](AssetPipeline.md) for the metadata format, cooking rules, cooked formats, build keys, and DerivedDataCache layout. Type in the picker to filter by name. Saved edits to shown assets, including `.blend` files, reimport automatically within about a second; a failed reimport keeps the previous mesh. **File > Import...** uses the Content Browser's selected Game subfolder, or `Models` and `Textures` when a mount root is selected. The import dialog is the system file picker on Windows and `zenity` or `kdialog` on Linux.
