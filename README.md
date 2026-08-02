# Herta

Herta is a C++23 game engine and editor for Windows and Linux. It is being built as a learning engine with the long-term goal of becoming capable of shipping production games.

The architecture takes inspiration from Unreal Engine's runtime and tooling boundaries while keeping each module small enough to understand, test, and evolve deliberately.

## Status

Milestone 0 - Foundation and Milestone 1 - Application shell are complete. Herta now contains structured logging, task scheduling, GLFW window and input ownership, the minimum NVRHI Vulkan presentation path, Dear ImGui ToolUI with docking and platform viewports, the initial editor shell and Output Log, a configured application icon, and a display-independent editor command host. Windows, X11, and Wayland renderer smoke paths are covered. The scene renderer, asset pipeline, and game runtime have not started.

The current architecture and implementation roadmap are documented in [EngineDesign.md](Docs/EngineDesign.md). The initial editor visual and interaction baseline is documented in [EditorStyle.md](Docs/EditorStyle.md).

## Technical direction

- C++23 with Unreal-style naming and explicit module ownership
- Windows and Linux, including Win32, X11, and Wayland
- Right-handed Left-Up-Forward coordinates using meters, kilograms, and seconds
- Vulkan renderer behind Herta RHI and NVRHI
- Herta's GLFW fork with cross-platform custom title bars
- Herta-owned structured logging with `spdlog` private behind Core and a docked searchable Output Log in EditorFramework
- UX-first task system with bounded CPU and IO work, cancellation, progress, and safe reload ownership
- Dear ImGui editor and imgui-node-editor graph tooling
- Desaturated graphite editor styling with Roboto and FreeType, a configurable focus-aware background gradient, transparent-panel modes, a 36 px custom title bar, and centralized ToolUI tokens
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

Setup initializes Git submodules and downloads pinned project-local Premake and Vulkan SDK installations under ignored `External/Premake` and `SDK`. Normal project generation and builds do not access the network. A Vulkan-capable driver and production loader remain platform requirements.

### Windows

Requirements are Git and Visual Studio with Desktop development with C++. Setup prefers Visual Studio 2026 with `v145`, then automatically falls back to Visual Studio 2022 with `v143`.

From a normal terminal:

```bat
Setup.bat
GenerateProjectFiles.bat
```

Project generation uses the same detected toolchain and writes `Herta.slnx` or `Herta.sln` at the repository root. Supporting Visual Studio project files remain under `Intermediate\ProjectFiles`.

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

Generated IDE launch settings provide the project-local Vulkan SDK tools and validation-layer paths for Debug and Development. Direct launches continue without validation when the platform loader cannot discover the SDK layer, and report that downgrade through Herta logging.

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
