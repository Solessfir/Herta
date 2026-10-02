# Herta Asset Pipeline

Status: Milestone 3 in progress - asset identity, metadata, registry, build keys, DerivedDataCache, texture and glTF cooking in an isolated worker, headless commands, and editor mesh previews are implemented. Blender import, file watching, live reimport, and search are next.

This document records the contracts that later importers, cookers, and editor jobs build on. The roadmap lives in [EngineDesign.md](EngineDesign.md).

## Modules

| Module | Kind | Owns |
|---|---|---|
| `Assets` | Runtime | `FAssetId`, portable asset paths, the immutable `FAssetRegistry` snapshot, and the cooked texture and model formats. |
| `AssetPipeline` | Developer | `.hmeta` metadata, content scanning and import, build keys, DerivedDataCache, the texture and glTF cookers, the worker client, and the `asset.*` editor commands. |
| `HertaAssetWorker` | Program | Runs one cook per process so untrusted source parsing cannot crash or hang its caller. |

Core provides `FHash128` and `HashBytes`, a private XXH3-128 implementation from the vendored `External/xxHash/xxhash.h`. It is a content identity, not a cryptographic hash. Core's `FBinaryWriter` and `FBinaryReader` encode every cooked format in little-endian order. Platform's `RunProcess` launches the worker with UTF-8 arguments, captured output, a timeout, and cancellation. It kills the whole process tree through a job object on Windows and a process group on Linux.

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
Setting.ColorSpace = Linear
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

Importable source files without a sidecar are reported as unregistered. Other files, such as fonts and licenses, are not assets and are ignored.

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

## Cooking

`CookAsset` reads the sidecar and source, collects dependencies, computes the build key, and returns early on a valid DerivedDataCache entry. Otherwise it cooks, serializes, and stores the result. A corrupt cache entry is reported as a warning and cooked again. `--force` and `FAssetCookRequest::bForce` skip the cache lookup.

Importers parse untrusted files, so only `HertaAssetWorker` and tests call `CookAsset` in-process. Editors and commands call `CookAssetInWorker`, which runs:

```text
HertaAssetWorker cook --content-root <path> --derived-data <path> --platform <name> [--force] <content-path>
```

On success the worker exits with 0 and prints `HertaAssetCook/1`, `Key <hex>`, `Cache hit` or `Cache miss`, and one `Warning <text>` line per warning. A failed cook exits with 1 and writes the reason to standard error. The default timeout is 10 minutes.

| Importer | Version | Settings | Output |
|---|---|---|---|
| `Texture` | 1 | `ColorSpace = Srgb` (default) or `Linear` | Cooked texture |
| `Gltf` | 1 | None | Cooked model |
| `Blender` | - | - | Not available yet |

Unknown settings are errors, so a typo never silently cooks with defaults. Bump an importer's version whenever its output changes for the same input, and bump `CookedAssetFormatVersion` when any cooked layout changes. Both are part of the build key.

### Textures

PNG and JPEG sources are decoded with stb_image inside the worker, up to 8192 pixels per side. For sRGB textures, the cooker converts color channels to 16-bit linear light through a fixed table, builds the full mip chain with a 2x2 box filter, and encodes back to sRGB. Alpha and linear textures are filtered as stored. Mip sizes round down and clamp to one, matching Vulkan. Mips larger than 4096 pixels are dropped, so every cooked texture uploads within the 64 MiB recording budget.

Integer filtering and fixed tables make cooked bytes identical across compilers and C runtimes.

### glTF

glTF 2.0 `.gltf` and `.glb` sources are parsed with fastgltf and validated before use. Herta and glTF share axes, units, and counter-clockwise winding, so positions need no conversion.

- The default scene is flattened into one static model. Node transforms are baked into positions, and nodes that reuse a mesh produce separate copies.
- Primitives are grouped into one section per material, so a model draws once per material.
- Each section's base color texture is cooked with the material's `baseColorFactor` baked in. Materials without a texture get a 1x1 texture of the factor.
- A transform with a negative determinant mirrors geometry, so its triangles are reversed once to keep counter-clockwise front faces.
- Vertices are deduplicated and reordered for the GPU vertex cache with meshoptimizer.
- Only positions and the texture coordinate set used by the base color texture are kept. Normals and tangents arrive with lighting.
- Non-triangle primitives are skipped with a warning. An image that cannot be decoded is replaced by its factor with a warning.
- `KHR_mesh_quantization` is supported. Any other required extension, such as Draco compression, fails the cook instead of producing wrong data.

External buffer and image URIs must be relative and stay inside the content root. They are resolved before the loader reads any file, so a crafted glTF cannot read arbitrary files. Each one becomes a build-key dependency, so editing a `.bin` or texture invalidates the cooked model.

### Cooked formats

Cooked assets start with the magic `HCAS`, `CookedAssetFormatVersion`, and a type tag, followed by little-endian fields:

- A texture stores its color space and RGBA8 mips from largest to 1x1.
- A model stores position and UV vertices, uint32 triangle indices, sections, materials, and the textures they reference.

`DeserializeCookedAsset` validates every count, index, range, and mip dimension before allocating. Valid models stay within 64 MiB per vertex or index buffer.

## Commands

The commands are registered by `RegisterAssetCommands` and exposed through `HertaEditorCmd`:

```text
HertaEditorCmd asset.validate [--content-root <path>]
HertaEditorCmd asset.list [--content-root <path>]
HertaEditorCmd asset.import <source> [--destination <content-directory>] [--content-root <path>]
HertaEditorCmd asset.reimport <content-path|asset-id> [--force] [--content-root <path>]
```

`asset.validate` exits with code 1 when any error is found. `asset.import` registers a source already inside the content root in place. Otherwise, it copies the source atomically into `--destination` (the content root by default). It then cooks the asset. It never overwrites an existing file or sidecar. A failed cook leaves the source registered so it can be fixed and reimported.

A `.gltf` usually references separate buffers and images, so it is only imported in place. Copy it into content with its files, or import a self-contained `.glb`.

`HertaEditorCmd` passes UTF-8 arguments on every platform, so content paths may contain any Unicode characters.

## Editor previews

The editor mounts two content roots: `Engine/Content` as `Engine` and `Games/Sandbox/Content` as `Game`. Asset IDs are global, so a reference does not depend on its mount; the picker shows mount-prefixed paths such as `Engine/Shapes/Cube.gltf`. Engine content holds the 1 m `Shapes/Cube.gltf`, which the preview cube and the scaled floor load by default. Its ID is fixed in `PreviewScene.h`, and a test cooks it to verify its size, winding, and unmirrored UVs.

The editor scans content in the background at startup and again whenever the Static Mesh picker in Details opens. Choosing a model or texture cooks it in `HertaAssetWorker` on a blocking-IO task. A main-thread continuation uploads the result between frames, so the GPU upload never overlaps a frame recording. The newest choice for an object wins, and older results are discarded when they finish. Closing the editor cancels in-flight work and kills running workers.

Requests made before the first scan finishes wait for it. Objects showing the same asset share one GPU mesh. The previous mesh stays visible while a replacement cooks; a failed load clears it so the viewport matches the selection and the error.

Textures preview on a 1 m cube generated by `CreateTexturedCubeModel`. Picking, selection outlines, focus, bounds, and the physics preview use the loaded mesh's bounds.

The editor does not register the `asset.*` commands, because they block on file IO and worker processes.
