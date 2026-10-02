# Herta Asset Pipeline

Status: Milestone 3 in progress - asset identity, metadata, registry, build keys, DerivedDataCache, and headless commands are implemented. Importers and cooking start in the next slice.

This document records the contracts that later importers, cookers, and editor jobs build on. The roadmap lives in [EngineDesign.md](EngineDesign.md).

## Modules

| Module | Kind | Owns |
|---|---|---|
| `Assets` | Runtime | `FAssetId`, portable asset paths, and the immutable `FAssetRegistry` snapshot. |
| `AssetPipeline` | Developer | `.hmeta` metadata, content scanning and import, build keys, DerivedDataCache, and the `asset.*` editor commands. |

Core provides `FHash128` and `HashBytes`, a private XXH3-128 implementation from the pinned `External/xxHash` submodule. It is a content identity, not a cryptographic hash.

## Content root

Until Milestone 4 introduces `.hertaproject` loading, content lives in `Games/Sandbox/Content`. `HertaEditorCmd` finds it by walking up from the working directory, then from the executable, until it reaches a directory containing `Engine/Content`. Every asset command accepts `--content-root <path>` to override it.

Scanning ignores dot-prefixed files and directories. Symbolic links are reported as errors and never followed.

## Asset IDs

`FAssetId` is a 128-bit identifier serialized as a lowercase `8-4-4-4-12` UUID. New IDs are random version 4 UUIDs. Parsing accepts only the canonical form, so every ID has exactly one textual representation. The nil UUID is invalid.

Parsing does not require version 4 because importers will derive stable sub-asset IDs, such as individual glTF meshes, from their parent ID.

## Asset paths

Registry paths are relative to the content root, UTF-8, and `/`-separated. To stay portable between Windows and Linux they reject empty, `.`, and `..` segments, backslashes, drive colons, `*?"<>|`, control characters, and segments ending in a period or space. Two paths that differ only by ASCII case are rejected because they collide on case-insensitive file systems.

## Metadata sidecars

Each registered source has a `<source>.hmeta` sidecar beside it:

```text
Format = HertaAssetMetadata
Version = 1
Id = 123e4567-e89b-42d3-a456-426614174000
Importer = Texture
Setting.Srgb = true
```

- `Format` and `Version` are always the first two lines. Unknown versions are rejected.
- `Id` and `Importer` are required. Importer and setting names are `[A-Za-z0-9_]+`.
- Setting values are single-line UTF-8 without control characters or surrounding spaces.
- Herta writes LF line endings and settings in byte-wise name order. Parsing accepts any field order and CRLF checkouts, so rewriting a hand-edited file produces the canonical form.
- Unknown keys are errors in version 1.

Importers are selected by extension: `.gltf` and `.glb` use `Gltf`, `.png`, `.jpg`, and `.jpeg` use `Texture`, and `.blend` uses `Blender`.

## Registry

`FAssetRegistry` is an immutable value sorted by source path, with ID lookup through a secondary index. Publishers replace the whole snapshot instead of mutating it. `ScanContentRoot` builds one from the sidecars and reports per-asset errors without failing the whole scan:

- Metadata without a source file.
- Malformed metadata.
- An ID shared by several files. Every file sharing it is excluded, because choosing one would silently retarget references.

Source files without a sidecar are reported as unregistered.

## Build keys

`ComputeAssetBuildKey` hashes a length-prefixed, little-endian encoding of:

- The key schema (`HertaAssetBuildKey/1`).
- The source content hash and content-relative path.
- The importer name and version.
- Import settings in name order.
- Dependency paths and content hashes, sorted so discovery order does not matter.
- The target platform and cooked format version.

The asset ID is deliberately excluded so identical sources share derived data. Renderer feature level joins the key when the renderer exposes one.

## DerivedDataCache

`FDerivedDataCache` is a local content-addressed store rooted at `DerivedDataCache/<platform>`. An entry for key `K` lives at `<root>/<first two hex digits of K>/<K>.hddc`:

| Field | Size |
|---|---|
| Magic `HDDC` | 4 |
| Format version (1) | 4 |
| Key | 16 |
| Payload size | 8 |
| Payload | variable |
| XXH3-128 of all preceding bytes | 16 |

Writes go to a uniquely named temporary file beside the entry, then rename over it. Readers never observe a partial entry. Because a key fixes its content, a failed replacement is a success when the existing entry is valid and identical. This happens on Windows while another process is reading it. A corrupt or mismatched entry is returned as an error so the caller can report it before rebuilding.

## Commands

The commands are registered by `RegisterAssetCommands` and exposed through `HertaEditorCmd`:

```text
HertaEditorCmd asset.validate [--content-root <path>]
HertaEditorCmd asset.list [--content-root <path>]
HertaEditorCmd asset.import <source> [--destination <content-directory>] [--content-root <path>]
```

`asset.validate` exits with code 1 when any error is found. `asset.import` registers a source already inside the content root in place. Otherwise, it copies the source atomically into `--destination` (the content root by default). It never overwrites an existing file or sidecar.

The interactive editor does not register these commands yet. They perform blocking file IO, and the editor will expose import through asynchronous jobs instead.
