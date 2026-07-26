# Herta

Herta is a C++23 game engine and editor for Windows and Linux. It is being built as a learning engine with the long-term goal of becoming capable of shipping production games.

The architecture takes inspiration from Unreal Engine's runtime and tooling boundaries while keeping each module small enough to understand, test, and evolve deliberately.

## Status

Herta is in its foundation design phase. The engine source, dependency bootstrap, and supported build workflow have not been implemented yet.

The current architecture and implementation roadmap are documented in [EngineDesign.md](Docs/EngineDesign.md).

## Planned foundation

- C++23 with Unreal-style naming and explicit module ownership
- Windows and Linux, including Win32, X11, and Wayland
- Right-handed Left-Up-Forward coordinates using meters, kilograms, and seconds
- Vulkan renderer behind Herta RHI and NVRHI
- Herta's GLFW fork with cross-platform custom title bars
- Dear ImGui editor and imgui-node-editor graph tooling
- Jolt Physics and ozz-animation behind Herta-owned APIs
- glTF asset pipeline with optional system Blender import and live reimport
- Premake project generation bootstrapped by pinned setup scripts
- Automated tests, static analysis, sanitizers, and reproducible CI builds

Dependencies will be introduced only when their implementation milestone requires them.

## Building

Build instructions will be added with the first executable foundation milestone. A fresh clone will eventually be bootstrapped through `Setup.bat` or `Setup.sh`, with required downloadable tools installed locally to the repository and verified against pinned checksums.

Blender remains an optional system-wide authoring tool. It will not be required to build or run Herta.

## License

Herta is available under the [MIT License](LICENSE).
