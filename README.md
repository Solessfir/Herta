# Herta

Herta is a C++23 game engine and editor for Windows and Linux. It is being built as a learning engine with the long-term goal of becoming capable of shipping production games.

The architecture takes inspiration from Unreal Engine's runtime and tooling boundaries while keeping each module small enough to understand, test, and evolve deliberately.

## Status

Milestone 0 - Foundation is complete. The repository contains Core, Math, and Platform modules, automated tests, a pinned dependency bootstrap, Premake project generation, and required Windows/Linux CI and quality gates. Milestone 1 - Application shell is next. Herta does not contain an application shell, renderer, editor, or game runtime yet.

The current architecture and implementation roadmap are documented in [EngineDesign.md](Docs/EngineDesign.md). The initial editor visual and interaction baseline is documented in [EditorStyle.md](Docs/EditorStyle.md).

## Technical direction

- C++23 with Unreal-style naming and explicit module ownership
- Windows and Linux, including Win32, X11, and Wayland
- Right-handed Left-Up-Forward coordinates using meters, kilograms, and seconds
- Vulkan renderer behind Herta RHI and NVRHI
- Herta's GLFW fork with cross-platform custom title bars
- Herta-owned structured logging with `spdlog` private behind Core
- UX-first task system with bounded CPU and IO work, cancellation, progress, and safe reload ownership
- Dear ImGui editor and imgui-node-editor graph tooling
- Desaturated graphite editor styling with Roboto and FreeType, a configurable low-intensity background gradient, transparent-panel modes, a 36 px custom title bar, and centralized ToolUI tokens
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

Setup initializes Git submodules and downloads the pinned Premake binary into ignored `External/Premake`. Normal project generation and builds do not access the network.

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
MSBuild Herta.slnx /m /t:HertaTests /p:Configuration=Development /p:Platform=x64
Binaries\windows\x86_64\Development\HertaTests.exe
```

Use `Herta.sln` when Setup selected the fallback toolchain.

### Linux

Setup validates Git, Make, `sha256sum`, `tar`, curl or wget, and a C++23 compiler and standard library providing `<expected>` and `<print>`. GCC 14 or newer is required when using libstdc++.

```bash
bash ./Setup.sh
bash ./GenerateProjectFiles.sh
make --directory=Intermediate/ProjectFiles/gmake --jobs=2 config=development HertaTests
./Binaries/linux/x86_64/Development/HertaTests
```

On Ubuntu 24.04, select GCC 14 explicitly:

```bash
export CC=gcc-14 CXX=g++-14
bash ./Setup.sh
bash ./GenerateProjectFiles.sh
```

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
