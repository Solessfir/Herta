# Getting started

[Back to README](../README.md)

## Clone and setup

```sh
git clone https://github.com/Solessfir/Herta.git
```

Setup initializes Git submodules and downloads pinned project-local Premake and Vulkan SDK installations under ignored `External/Premake` and `SDK`. Versions, download URLs, and SHA-256 hashes are recorded in [Dependencies.lock](../Config/Dependencies.lock). Normal project generation and builds do not access the network. A Vulkan-capable driver and production loader remain platform requirements; updating the SDK does not update the GPU driver.

Building `HertaEditor` also builds its workers and cooks its shaders using the SDK's Slang compiler. Keep the generated `Shaders` directory beside the editor executable.

## Windows

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

## Linux

Setup validates Git, Make, `sha256sum`, `tar`, curl or wget, Linux X11 and Wayland development packages, and a C++23 compiler and standard library providing `<expected>` and `<print>`. GCC 14 or newer is required when using libstdc++.

On Ubuntu 24.04, install the required compiler and window-system packages with:

```bash
sudo apt-get update
sudo apt-get install -y git make coreutils tar curl gcc-14 g++-14 pkg-config xorg-dev libwayland-dev libwayland-bin libxkbcommon-dev libvulkan1 mesa-vulkan-drivers vulkan-validationlayers
```

On Ubuntu 24.04, select GCC 14 before running Setup and generating projects:

```bash
export CC=gcc-14 CXX=g++-14
```

```bash
./Setup.sh
./GenerateProjectFiles.sh
make --directory=Intermediate/ProjectFiles/gmake --jobs=2 config=development HertaTests HertaEditorCmd HertaEditor
./Binaries/linux/x86_64/Development/HertaTests
./Binaries/linux/x86_64/Development/HertaEditorCmd --json help
./Binaries/linux/x86_64/Development/HertaEditor
```

## Configurations and validation

Generated Visual Studio launch settings provide the project-local Vulkan SDK tools and validation-layer paths. Windows project generation also creates the Git-ignored `.run/HertaEditor.run.xml`; select **HertaEditor** in Rider and use the solution configuration dropdown to choose Debug, Debug-ASan, Development, or Shipping. Regeneration updates this managed profile without modifying personal Rider configurations.

Debug and Debug-ASan enable Vulkan and NVRHI validation by default. Development disables validation by default; add `--validation` to the launch arguments (including Rider EzArgs) to enable it. Shipping keeps validation disabled. Renderer regression tests require validation in non-Shipping builds. When requested validation layers are unavailable, normal launches report the downgrade and continue; renderer regression tests fail instead.

## Cleanup

`Cleanup.bat` and `Cleanup.sh` remove all Herta-managed generated state, including build output, generated projects, caches, saved data, test results, IDE state, downloaded Premake binaries, and local SDKs. Run Setup again before generating projects after a full cleanup.

```bat
Cleanup.bat
```

```bash
./Cleanup.sh
```
