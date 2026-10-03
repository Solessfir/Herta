# Herta

Herta is a C++23 game engine and editor for Windows and Linux. It is being built as a learning engine with the long-term goal of becoming capable of shipping production games.

The architecture takes inspiration from Unreal Engine's runtime and tooling boundaries while keeping each module small enough to understand, test, and evolve deliberately.

![Herta Editor with the Vulkan viewport, Details panel, and docked Output Log](.github/Herta.png)

## Status

Milestones 0 through 2.5 are implemented. Herta includes RHI graphics resources, submission-based frame retirement, RenderGraph, Slang shader cooking, and an interactive editor Viewport with reversed-Z, camera navigation, transform gizmos, and depth-tested debug drawing. The editor retains docking, platform viewports, Output Log, and a separate display-independent command host. Milestone 3 - Asset pipeline is implemented: stable asset IDs, metadata sidecars, the content registry, build keys, DerivedDataCache, texture, glTF, and Blender cooking in an isolated worker process, headless asset commands, and editor mesh previews with live reimport, drag-and-drop import, and fuzzy search. The game runtime has not started.

The current architecture and implementation roadmap are documented in [EngineDesign.md](Docs/EngineDesign.md). The initial editor visual and interaction baseline is documented in [EditorStyle.md](Docs/EditorStyle.md).

## Editor workspace

The workspace renders the scene across the canvas beneath blurred Outliner, Details, and Output Log overlays, with compact chrome, glass viewport controls, and inline transform editing. The camera's projection center follows the unobscured Viewport pane, so overlays do not push the subject off-center. Panels remain dockable and resizable; detached Viewports render within their own windows. The world grid is procedural on the GPU, with antialiased lines and a distance fade.

See [Rendering.md](Docs/Rendering.md) for GPU ownership, shader cooking, and renderer verification. Use **File > Reset layout** to restore the default arrangement, including the Output Log's 26% workspace height. Existing saved layouts are preserved when defaults change.

In the Viewport, hold RMB and use WASD/QE to fly, Alt+LMB to orbit, MMB to pan, and the wheel to dolly. Press F to focus the selection. F11 toggles a full-workspace viewport without changing the saved panel layout. Click the cube or floor to select it; Ctrl+click or Shift+click toggles additional objects. Click empty viewport space or press Escape in the focused viewport to deselect. During a transform drag, Escape restores the entire selection instead. The toolbar provides move/rotate/scale, local/world axes, snapping, and camera/debug settings. Transform edits apply to the selection around its active object's pivot and are not saved.

The editor preview includes a 1 m cube initially positioned at Y=4 m and a 10 m square floor whose top is at Y=0. Both use the engine cube asset `Engine/Content/Shapes/Cube.gltf`; the floor is that cube scaled to 10 x 0.5 x 10 m. Click **Simulate** or press **Alt+S** to drop the cube from its current editor transform using Jolt Physics. Camera navigation and selection remain available, but transform editing is locked. Press **Escape** or click the active Simulate button to stop and restore the original transform. Physics runs at 60 Hz with render interpolation and bounded catch-up after stalls. **Play** remains disabled because the game runtime is not implemented. This is an editor preview slice, not the full physics milestone.

Details shows the active object's editable location, rotation, and scale. With multiple objects selected, transform changes also apply to the other selected objects. Click a value to type or drag it to adjust; typed values support arithmetic such as `10/2`, applied with Enter. Shift+RMB copies an individual value or a whole transform row from its label; Shift+LMB pastes it. Row clipboard text supports UE's `X/Y/Z` location and scale format and `Pitch/Yaw/Roll` rotation format. Values stay in Herta's units and axis conventions; clipboard compatibility does not convert them. Click the camera coordinates to copy a position that can be pasted onto Location. Floor edits affect its static collider when simulation starts.

Press F2 in the Viewport, Outliner, or Details to rename the active object in Details, or double-click its Details heading. Enter or moving focus commits the name; Escape cancels. Blank names are rejected. Names are editor-only and are not saved. S toggles grid snapping in the focused viewport when no drag is active; RMB+S still flies backward. Ctrl+Q quits the editor from any panel.

The Outliner docks above Details and lists the cube and floor with label and type columns. Search filters the list. Shift+click selects a range of visible rows, Ctrl+click toggles individual rows, and Ctrl+A selects all visible objects. Ctrl+Shift+click adds a range to the selection. Selecting a row updates Details and viewport outlines; double-clicking focuses the selection. Click empty list space to deselect. Reopen it from **Window > Outliner**. Existing layouts with docked Details gain the panel above it without resetting other dock positions; detached panels are preserved.

The Output Log supports search, verbosity filtering, text selection, copying, pause, auto-scroll, and commands with completion and history. Timestamps show local clock time in `HH:mm:ss` format. Normal messages use category colors; warnings stay yellow/orange and errors red. Search and command entry share an outlined input style. Start a line with `!` to run it through your shell (`cmd.exe` on Windows, `$SHELL` on Linux), for example `!git commit -m "Fixed tabs"`. Shell commands run in the background and their output appears under the `Shell` category when they exit; they get no input, so interactive programs and prompts do not work.

Press the backtick key to open Output Log and focus its command field; pressing Enter submits and returns focus to the viewport. Enter `stat unit` to toggle timing readouts below the viewport's top-right controls, and `stat fps` to toggle FPS independently. Frame is the smoothed frame interval; CPU frame measures the editor render callback, including presentation waits, rather than CPU utilization. GPU UI uses asynchronous GPU timestamps for the main window's UI rendering, including backdrop copy and blur, but excludes the scene renderer and detached windows. It shows `--` until a sample is available. CPU and GPU times overlap and should not be added together.

Use **Appearance** in the bottom bar for all workspace styling, including panel transparency, opacity, blur, reduced motion, gradient, and accent color. Default panel opacity is 90%, with 24 px background blur. Start is a welcome panel, closed by default, and can be reopened from **Window > Start panel**.

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

In the editor, select an object and choose a model or texture from **Static Mesh** in Details, which lists `Engine/` and `Game/` content. It cooks in the background and replaces the mesh between frames; textures preview on a 1 m cube. Picking, outlines, focus, bounds, and Simulate use the loaded mesh's bounds. Choices are not saved yet. See [AssetPipeline.md](Docs/AssetPipeline.md) for the metadata format, cooking rules, cooked formats, build keys, and DerivedDataCache layout. Type in the picker to filter by name. Saved edits to shown assets, including `.blend` files, reimport automatically within about a second; a failed reimport keeps the previous mesh. Drop model or image files onto the editor, or use **File > Import...**, to import them into `Models` or `Textures`. The import dialog is the system file picker on Windows and `zenity` or `kdialog` on Linux.

## Technical direction

- C++23 with Unreal-style naming and explicit module ownership
- Windows and Linux, including Win32, X11, and Wayland
- Right-handed Left-Up-Forward coordinates using meters, kilograms, and seconds
- Vulkan renderer behind Herta RHI and NVRHI
- Herta's GLFW fork with capability-driven custom title bars across Win32, X11, and Wayland; on Linux, all editor window controls are hidden when minimization is unsupported
- Herta-owned structured logging with `spdlog` private behind Core and a docked searchable Output Log in EditorFramework
- UX-first task system with bounded CPU and IO work, cancellation, progress, and safe reload ownership
- Dear ImGui editor using the [Solessfir Herta branch](https://github.com/Solessfir/imgui/tree/herta) with physical-pixel text alignment and generic draw-channel reordering, and imgui-node-editor graph tooling
- Desaturated graphite editor styling with Roboto and FreeType at a centralized 15.625 logical-pixel size, Droid Sans Mono log entries at 90% of that size, per-monitor DPI and framebuffer-density scaling, a configurable focus-aware background gradient, transparent-panel modes, a capability-driven 36 px custom title bar, and centralized ToolUI tokens
- Headless editor commands that do not require windows, rendering, ImGui, or an audio device
- Server-authoritative multiplayer with dedicated and player-hosted listen-server compositions
- Versioned project templates and editor-only C++ game-module hot reload
- Optional statically typed gameplay scripting, with Umka gated by a focused production-readiness spike
- ECS world storage with EnTT as the preferred implementation candidate after a focused milestone spike
- Jolt Physics and ozz-animation behind Herta-owned APIs
- Recast/Detour navigation and a first-party compiled StateTree AI runtime
- miniaudio behind Herta-owned audio assets, mixing, spatialization, and thread boundaries, with Steam Audio as the preferred optional acoustics candidate
- ICU and HarfBuzz at the localization milestone, expanding the MS1 FreeType dependency behind Herta international-text APIs
- glTF asset pipeline with optional system Blender import and live reimport
- Zstandard-compressed packages, optional single-executable embedding, and data-mod mounting through one VFS
- Dynamic-only lighting with a crisp native TAA baseline, optional upscalers and capability-driven advanced GI
- Source-built user plugins with first-party Git, terminal, and MCP editor integrations planned on public extension APIs
- Premake project generation bootstrapped by pinned setup scripts
- Automated tests, static analysis, sanitizers, and reproducible CI builds

Dependencies will be introduced only when their implementation milestone requires them.

## Building

Setup initializes Git submodules and downloads pinned project-local Premake and Vulkan SDK installations under ignored `External/Premake` and `SDK`. Versions, download URLs, and SHA-256 hashes are recorded in [Dependencies.lock](Config/Dependencies.lock). Normal project generation and builds do not access the network. A Vulkan-capable driver and production loader remain platform requirements; updating the SDK does not update the GPU driver.

Building `HertaEditor` also builds `HertaShaderWorker` and cooks its shaders using the SDK's Slang compiler. Keep the generated `Shaders` directory beside the editor executable.

### Windows

Requirements are Git and Visual Studio with Desktop development with C++. Setup prefers Visual Studio 2026 with `v145`, then automatically falls back to Visual Studio 2022 with `v143`.

From a normal terminal:

```bat
Setup.bat
GenerateProjectFiles.bat
```

Project generation uses the same detected toolchain and writes `Herta.slnx` or `Herta.sln` at the repository root. Supporting Visual Studio project files remain under `Intermediate\ProjectFiles`.

Windows project generation pauses on errors when run interactively. Automation remains non-blocking when `CI` or `HERTA_NO_PAUSE` is defined.

Pass a version explicitly when fallback is not wanted:

```bat
Setup.bat -VisualStudioVersion 2026
GenerateProjectFiles.bat vs2026

Setup.bat -VisualStudioVersion 2022
GenerateProjectFiles.bat vs2022
```

From a matching Visual Studio Developer Command Prompt:

```bat
MSBuild Herta.slnx /m /t:HertaTests,HertaEditorCmd,HertaEditor /p:Configuration=Development /p:Platform=x64
Binaries\windows\x86_64\Development\HertaTests.exe
Binaries\windows\x86_64\Development\HertaEditorCmd.exe --json help
Binaries\windows\x86_64\Development\HertaEditor.exe
```

Use `Herta.sln` when Setup selected the fallback toolchain.

From PowerShell 7+ in the repository root, build and launch the editor without an IDE using the installed MSBuild path:

```powershell
& "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\MSBuild\Current\Bin\MSBuild.exe" Herta.slnx /m /t:HertaEditor /p:Configuration=Development /p:Platform=x64 && .\Binaries\windows\x86_64\Development\HertaEditor.exe
```

Adjust the MSBuild path for your Visual Studio installation. `&&` launches the editor only if the build succeeds; Windows PowerShell 5.1 does not support it. Replace `Development` with `Debug` in both paths and arguments to enable debug builds and validation.

To verify both Windows compilers locally, add **C++ Clang tools for Windows** and **MSBuild support for LLVM (clang-cl)** to the same Visual Studio installation, then run:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File Scripts/VerifyWindowsCompilers.ps1
```

The script regenerates projects, rebuilds the entire solution with ClangCL and MSVC, and runs `HertaTests` after each successful build. MSVC runs last even if Clang fails because both compilers share output directories. Build and test logs are saved under `Intermediate/CompilerVerification`. It uses Setup's detected Visual Studio toolchain without downloading anything. Pass `-Configuration Debug` or `-Configuration Shipping` to check another configuration; the default is `Development`.

### Linux

Setup validates Git, Make, `sha256sum`, `tar`, curl or wget, Linux X11 and Wayland development packages, and a C++23 compiler and standard library providing `<expected>` and `<print>`. GCC 14 or newer is required when using libstdc++.

On Ubuntu 24.04, install the required compiler and window-system packages with:

```bash
sudo apt-get update
sudo apt-get install -y gcc-14 g++-14 pkg-config xorg-dev libwayland-dev libwayland-bin libxkbcommon-dev libvulkan1 mesa-vulkan-drivers vulkan-validationlayers
```

```bash
bash ./Setup.sh
bash ./GenerateProjectFiles.sh
make --directory=Intermediate/ProjectFiles/gmake --jobs=2 config=development HertaTests HertaEditorCmd HertaEditor
./Binaries/linux/x86_64/Development/HertaTests
./Binaries/linux/x86_64/Development/HertaEditorCmd --json help
./Binaries/linux/x86_64/Development/HertaEditor
```

On Ubuntu 24.04, select GCC 14 explicitly:

```bash
export CC=gcc-14 CXX=g++-14
bash ./Setup.sh
bash ./GenerateProjectFiles.sh
```

Generated Visual Studio launch settings provide the project-local Vulkan SDK tools and validation-layer paths. Windows project generation also creates the Git-ignored `.run/HertaEditor.run.xml`; select **HertaEditor** in Rider and use the solution configuration dropdown to choose Debug, Debug-ASan, Development, or Shipping. Regeneration updates this managed profile without modifying personal Rider configurations.

Debug and Debug-ASan enable Vulkan and NVRHI validation by default. Development disables validation by default; add `--validation` to the launch arguments (including Rider EzArgs) to enable it. Shipping keeps validation disabled. Renderer regression tests require validation in non-Shipping builds. When requested validation layers are unavailable, normal launches report the downgrade and continue; renderer regression tests fail instead.

## Cleaning

`Cleanup.bat` and `Cleanup.sh` remove all Herta-managed generated state, including build output, generated projects, caches, saved data, test results, IDE state, downloaded Premake binaries, and local SDKs. Run Setup again before generating projects after a full cleanup.

```bat
Cleanup.bat
```

```bash
./Cleanup.sh
```

## License

Herta is available under the [MIT License](LICENSE).
