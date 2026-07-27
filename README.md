# Herta

Herta is a C++23 game engine and editor for Windows and Linux. It is being built as a learning engine with the long-term goal of becoming capable of shipping production games.

The architecture takes inspiration from Unreal Engine's runtime and tooling boundaries while keeping each module small enough to understand, test, and evolve deliberately.

## Status

Milestone 0 - Foundation is complete. The repository contains Core, Math, and Platform modules, automated tests, a pinned dependency bootstrap, Premake project generation, and required Windows/Linux CI and quality gates. Milestone 1 - Application shell is next. Herta does not contain an application shell, renderer, editor, or game runtime yet.

The current architecture and implementation roadmap are documented in [EngineDesign.md](Docs/EngineDesign.md).

## Technical direction

- C++23 with Unreal-style naming and explicit module ownership
- Windows and Linux, including Win32, X11, and Wayland
- Right-handed Left-Up-Forward coordinates using meters, kilograms, and seconds
- Vulkan renderer behind Herta RHI and NVRHI
- Herta's GLFW fork with cross-platform custom title bars
- Dear ImGui editor and imgui-node-editor graph tooling
- Headless editor commands that do not require windows, rendering, ImGui, or an audio device
- ECS world storage with EnTT as the preferred implementation candidate after a focused milestone spike
- Jolt Physics and ozz-animation behind Herta-owned APIs
- Recast/Detour navigation and a first-party compiled StateTree AI runtime
- miniaudio behind Herta-owned audio assets, mixing, spatialization, and thread boundaries
- glTF asset pipeline with optional system Blender import and live reimport
- Zstandard-compressed packages, optional single-executable embedding, and data-mod mounting through one VFS
- Premake project generation bootstrapped by pinned setup scripts
- Automated tests, static analysis, sanitizers, and reproducible CI builds

Dependencies will be introduced only when their implementation milestone requires them.

## Building

Setup initializes Git submodules and downloads the pinned Premake binary into the ignored `SDK` directory. Normal project generation and builds do not access the network.

### Windows

Requirements are Git and Visual Studio 2026 with Desktop development with C++. Visual Studio 2022 remains a supported fallback.

From a normal terminal:

```bat
Setup.bat
GenerateProjectFiles.bat
```

The default action generates `Intermediate\ProjectFiles\vs2026\Herta.slnx`.

To use the Visual Studio 2022 fallback, override Setup and project generation together:

```bat
Setup.bat -VisualStudioVersion 2022
GenerateProjectFiles.bat vs2022
```

From a matching Visual Studio Developer Command Prompt:

```bat
MSBuild Intermediate\ProjectFiles\vs2026\Herta.slnx /m /t:HertaTests /p:Configuration=Development /p:Platform=x64
Binaries\windows\x86_64\Development\HertaTests.exe
```

### Linux

Setup validates Git, a C++23-capable GCC or Clang compiler, Make, `sha256sum`, `tar`, and either curl or wget.

```bash
bash ./Setup.sh
bash ./GenerateProjectFiles.sh
make --directory=Intermediate/ProjectFiles/gmake --jobs=2 config=development HertaTests
./Binaries/linux/x86_64/Development/HertaTests
```

## Cleaning

`Cleanup.bat` and `Cleanup.sh` remove all Herta-managed generated state, including build output, generated projects, caches, saved data, test results, IDE state, and the local `SDK` directory. Run Setup again before generating projects after a full cleanup.

```bat
Cleanup.bat
```

```bash
./Cleanup.sh
```

## License

Herta is available under the [MIT License](LICENSE).
