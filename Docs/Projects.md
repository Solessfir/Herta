# Projects

[Editor usage](EditorGuide.md) | [Level contracts](Levels.md)

## Descriptor and runtime API

`Project` owns `.hertaproject` parsing, canonical serialization, path resolution, and transactional template creation. Its public API is `ParseProject`, `SerializeProject`, `LoadProject`, and `CreateProject`, returning `std::expected` with `FProjectError`. `FLoadedProject` contains the validated descriptor and resolved project, content, and starting-level paths; it does not load native code or instantiate a world.

Descriptors use UTF-8 JSON with `format: "HertaProject"`, `formatVersion: 1`, and `schemaVersion: 2`. They record a UUID, project name, engine association, native modules, editor targets, one `Game` content root, an empty feature list, and `startingLevel`. Loading currently requires `engineAssociation: "Herta"`. Unknown or duplicate fields, unsupported versions/features/dependencies, unsafe relative paths, and paths escaping the project through symbolic links are rejected. The descriptor limit is 1 MiB.

Legacy schema 1 descriptors remain readable with `startingScene`; their `Scene` module dependency migrates in memory to `Level`. Serialization always writes schema 2 with `startingLevel`. Mixed or cross-version starting-document fields are rejected. Parsing does not rewrite descriptors or rename referenced files, so an existing `.hscene` path stays unchanged until explicitly migrated. See [Level contracts](Levels.md#file-contract) for document canonicalization.

Modules live at `Source/<ModuleName>`, with `Public` headers and `Private` implementation files. Supported dependencies are `Core`, `Math`, `Assets`, and `Level`; names cannot collide with engine modules. Targets currently describe editor composition, not standalone game executables. Paths use forward slashes and remain relative to the descriptor directory.

`LoadProject` checks the content, level file, and module directories exist. Starting-level JSON is validated when the editor loads it; project creation validates the template level before publishing. `HertaEditor --project=<descriptor>` opens the project; without this flag it opens `Games/Sandbox/Sandbox.hertaproject`. A plain `.hlevel` argument, such as a file dropped on the executable, opens that level in the project `FindOwningProject` locates, and a plain `.hertaproject` argument opens that project.

## Creation and headless commands

`project.create <name> <module> <destination>` uses `Templates/Projects/Game` from the associated source checkout. The destination must not exist and its parent directory must already exist. Names use portable C++-style identifiers. Creation expands only whitelisted manifest substitutions in a sibling staging directory, validates the result, and atomically publishes without replacing existing data. Cancellation or failure removes only that staging directory. The C++ request exposes a `std::stop_token`.

The template supplies a descriptor, empty starting level, content directory, and one native module with a deferred entity-creation example. It does not generate a standalone game application. `project.validate <descriptor>` checks the same project-loading contract; `--json` must precede the command when machine-readable diagnostics are wanted.

Windows example, after building `HertaEditorCmd`:

```powershell
$EnginePath = "C:\Git\Herta"
$ProjectRoot = "C:\Git\MyGame"
& "$EnginePath\Binaries\windows\x86_64\Development\HertaEditorCmd.exe" project.create MyGame MyGameplay $ProjectRoot
& "$EnginePath\Binaries\windows\x86_64\Development\HertaEditorCmd.exe" --json project.validate "$ProjectRoot\MyGame.hertaproject"
& "$EnginePath\Binaries\windows\x86_64\Development\HertaEditor.exe" "--project=$ProjectRoot\MyGame.hertaproject"
```

Linux uses `Binaries/linux/x86_64/Development/HertaEditorCmd` with the same positional command arguments, and `HertaEditor --project=/path/to/MyGame/MyGame.hertaproject`.

## Building the generated native module

Run the associated engine checkout's Setup first. Use its installed, pinned Premake and explicitly select both the engine script and project descriptor. Project descriptors and templates are data, not executable build scripts.

```powershell
$env:HERTA_VULKAN_SDK = "$EnginePath\SDK\Windows\Vulkan\1.4.363.0"
& "$EnginePath\External\Premake\Windows\5.0.0-beta8\premake5.exe" "--file=$EnginePath\premake5.lua" "--project=$ProjectRoot\MyGame.hertaproject" vs2026
& "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\MSBuild\Current\Bin\MSBuild.exe" "$ProjectRoot\Intermediate\ProjectFiles\vs2026\MyGameplay\MyGameplay.vcxproj" /m /p:Configuration=Development /p:Platform=x64
```

Use the MSBuild executable from the installed Visual Studio. `vs2022` selects the matching older workspace instead. Windows solutions are placed at the project root; module projects are under `Intermediate/ProjectFiles/<action>/<ModuleName>`.

```sh
env HERTA_VULKAN_SDK=/path/to/Herta/SDK/Linux/Vulkan/1.4.363.0 /path/to/Herta/External/Premake/Linux/5.0.0-beta8/premake5 --file=/path/to/Herta/premake5.lua --project=/path/to/MyGame/MyGame.hertaproject gmake
make --directory=/path/to/MyGame/Intermediate/ProjectFiles/gmake --jobs=2 config=development MyGameplay
```

The module is a static library: `Binaries/windows/x86_64/Development/MyGameplay.lib` or `Binaries/linux/x86_64/Development/libMyGameplay.a` under the project root. Its objects are under project `Intermediate/Build`; engine outputs remain under the engine checkout. Generation records the associated engine revision in project `Intermediate/HertaEngineBuild.json`. Rebuild after switching engine revisions; no dynamic native-module loader or standalone `HertaGame` target exists yet.
