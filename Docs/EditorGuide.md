# Editor guide

[Back to README](../README.md) | [Build and run](GettingStarted.md)

The editor currently previews selectable objects and imported assets. Scene transforms, names, and mesh choices are not saved, and the game runtime is not implemented yet.

## Workspace

The workspace renders the scene across the canvas beneath blurred Outliner, Details, and Output Log overlays, with compact chrome, glass viewport controls, and inline transform editing. The camera's projection center follows the unobscured Viewport pane, so overlays do not push the subject off-center. The main Viewport stays anchored to the central dock area with no tab bar; other panels remain movable, dockable, and resizable. The world grid is procedural on the GPU, with antialiased lines and a distance fade.

See [Rendering.md](Rendering.md) for GPU ownership, shader cooking, and renderer verification. Use **File > Reset layout** to restore the default arrangement, including the Output Log's 26% workspace height. Existing saved layouts are preserved when defaults change.

## Viewport controls

In the Viewport, hold RMB and use WASD/QE to fly, Alt+LMB to orbit, MMB to pan, and the wheel to dolly. Press F to focus the selection. F11 toggles a full-workspace viewport without changing the saved panel layout. Click the cube or floor to select it; Ctrl+click or Shift+click toggles additional objects. Click empty viewport space or press Escape in the focused viewport to deselect. During a transform drag, Escape restores the entire selection instead. The toolbar provides move/rotate/scale, local/world axes, snapping, and camera/debug settings. Transform edits apply to the selection around its active object's pivot and are not saved.

## Simulation

The editor preview includes a 1 m cube initially positioned at Y=4 m and a 10 m square floor whose top is at Y=0. Both use the engine cube asset `Engine/Content/Shapes/Cube.gltf`; the floor is that cube scaled to 10 x 0.5 x 10 m. Click **Simulate** or press **Alt+S** to drop the cube from its current editor transform using Jolt Physics. Camera navigation and selection remain available, but transform editing is locked. Press **Escape** or click the active Simulate button to stop and restore the original transform. Physics runs at 60 Hz with render interpolation and bounded catch-up after stalls. **Play** remains disabled because the game runtime is not implemented. This is an editor preview slice, not the full physics milestone.

## Details and renaming

Details shows the active object's editable location, rotation, and scale. With multiple objects selected, transform changes also apply to the other selected objects. Click a value to type or drag it to adjust; typed values support arithmetic such as `10/2`, applied with Enter. Shift+RMB copies an individual value or a whole transform row from its label; Shift+LMB pastes it. Row clipboard text supports UE's `X/Y/Z` location and scale format and `Pitch/Yaw/Roll` rotation format. Values stay in Herta's units and axis conventions; clipboard compatibility does not convert them. Click the camera coordinates to copy a position that can be pasted onto Location. Floor edits affect its static collider when simulation starts.

Press F2 in the Viewport, Outliner, or Details to rename the active object in Details, or double-click its Details heading. Enter or moving focus commits the name; Escape cancels. Blank names are rejected. Names are editor-only and are not saved. S toggles grid snapping in the focused viewport when no drag is active; RMB+S still flies backward. Ctrl+Q quits the editor from any panel.

## Outliner

The Outliner docks above Details and lists the cube and floor with label and type columns. Search filters the list. Shift+click selects a range of visible rows, Ctrl+click toggles individual rows, and Ctrl+A selects all visible objects. Ctrl+Shift+click adds a range to the selection. Selecting a row updates Details and viewport outlines; double-clicking focuses the selection. Click empty list space to deselect. Reopen it from **Window > Outliner**. Existing layouts with docked Details gain the panel above it without resetting other dock positions; detached panels are preserved.

## Output Log

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

In the editor, select an object and choose a model or texture from **Static Mesh** in Details, which lists `Engine/` and `Game/` content. It cooks in the background and replaces the mesh between frames; textures preview on a 1 m cube. Picking, outlines, focus, bounds, and Simulate use the loaded mesh's bounds. Choices are not saved yet. See [AssetPipeline.md](AssetPipeline.md) for the metadata format, cooking rules, cooked formats, build keys, and DerivedDataCache layout. Type in the picker to filter by name. Saved edits to shown assets, including `.blend` files, reimport automatically within about a second; a failed reimport keeps the previous mesh. Drop model or image files onto the editor, or use **File > Import...**, to import them into `Models` or `Textures`. The import dialog is the system file picker on Windows and `zenity` or `kdialog` on Linux.
