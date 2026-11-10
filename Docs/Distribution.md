# Distribution

A game ships as its executables and its cooked content. Content can sit beside the executables as loose files, as content packages, or inside the executables themselves; the game and the dedicated server read all three through one virtual file system, so nothing else changes between them.

## Content packages

`Assets` reads `.hpak` content packages (`ContentPackage.h`); the developer `PackageBuilder` module writes them. A package is a fixed 144-byte header, the files' chunks aligned to 16 bytes, and an index at the end sorted by path. Each index entry holds a relative `/`-separated path, the codec, the chunk's offset, stored size, and size, and the BLAKE2b-256 hash of its stored bytes. Chunks are one Zstandard frame each, single-threaded at level 19 with the content size and no checksum, or the bytes themselves when compression would not shrink them, such as fonts. The header holds the index's hash and, for a signed package, an Ed25519 signature over the header with the signature zeroed and the id of the key that made it. The signature covers the index hash, which covers every chunk's hash, so it authenticates every byte.

Opening a package checks the header, the signature when the package names a trusted key, and the index against its hash, then validates every entry: sorted unique safe paths, known codecs, sizes within the 1 GiB a cooked asset may take, and chunks that follow each other between the header and the index. Reading a file checks its chunk's hash before decompressing, and refuses a frame whose declared size differs from the index's, so corrupt or tampered data never reaches the decompressor or allocates more than the index allows. A fuzz test flips and truncates packages without crashing the reader.

The same files give the same bytes on every run and in any input order, and Ed25519 signatures are deterministic, so signed packages rebuild byte for byte too. zstd and Monocypher are the only dependencies; zstd builds without its assembly, threads, or legacy formats, and Monocypher's core and Ed25519 module are vendored unmodified.

## Virtual file system

`FVirtualFileSystem` reads files as if they were staged under its root, from directories and packages mounted in order: the first mount holding a path wins, a directory mirrors the root's layout, and a package names files relative to it. Paths outside the root read the disk, so a developer's `--project` elsewhere still works. `FCookedContent`, levels and their regions, navigation, material shaders and their parameter tables, the engine shaders, the HUD font, and the project itself are read through it.

`MountGameContent` (GameFramework) mounts a game's content. A package embedded in the executable comes first, then every `.hpak` beside the executable in name order; without packages, the loose staged folder there. A developer's `--project=<path>` mounts that staged folder instead. Data mods mount in front of everything.

## Signing

`HertaCooker keygen --id=1 --output=<file>` writes a private signing key and prints its public half, `1:<64 hex digits>`. Keep the private key out of the checkout and out of builds. Public keys go one per line in `Config/PackagePublicKeys.txt`, which project generation compiles into GameFramework; a Shipping game built with any key plays only packages signed by one of them, and never falls back to loose files that could replace signed content. Development builds, and builds without keys, open signed and unsigned packages alike and say which in the log. Key ids let a publisher rotate keys: a game trusts every key it ships with.

Packages are not encrypted. Encryption cannot keep data from whoever runs the game, and it waits for a project with a concrete threat model.

## Cooking

`HertaCooker` cooks content for distribution:

- `cook --project=<p.hertaproject> --output=<directory> --mode=loose|package|embedded` stages the starting level and every asset it reaches. `loose` writes the staged files; `package` writes `<name>.hpak` with the engine shaders from `--shaders=<directory>` under `Shaders/`; `embedded` builds the same package into each `--embed-into=<executable>,...`. `--signing-key=<file>` signs the package.
- `package`, `embed`, `keygen`, and `verify` are the pieces on their own; `verify --package=<file> --public-keys=<file>` checks every chunk and the signature.
- `mod` cooks a data mod.

Embedding adds the package as an `RCDATA` resource named `HERTAPAK` on Windows, which must happen before the executable is code-signed, and as a non-loaded `.hertapak` ELF section with `objcopy` on Linux. The game finds it with `FindEmbeddedData`: the resource is mapped with the image, and the section is read from the executable's own file. It is the same package reader either way.

`Scripts/Windows/PackageGame.ps1 -ContentMode Loose|Package|Embedded` and `Scripts/Linux/PackageGame.sh --content-mode loose|package|embedded` package the game and server with each; `Package` is the default. `-SigningKey`/`--signing-key` signs the content. Verification runs the relocated game, server, and a hosted game, and in package mode cooks the content again and requires the same package bytes.

Measured on the Sandbox: 122 files, 14.0 MiB staged with the engine shaders, package 7.6 MiB, embedded executables grow by the same.

## Data mods

A data mod adds and replaces files; it never runs code. Mods are folders in `Mods` beside the content, each holding `Mod.hmod` and either `<Namespace>.hpak` or a `Files` folder laid out like the staged content:

```json
{
  "format": "HertaMod",
  "formatVersion": 1,
  "namespace": "NeonCoral",
  "version": "1.0.0",
  "cookedFormat": 7,
  "dependencies": [{"namespace": "SomeMod", "minimumVersion": "1.0.0"}],
  "loadAfter": [],
  "loadBefore": [],
  "overrides": ["Sandbox/Content/a4c06002-2738-4e25-9b9e-816ae7c00001.hasset"]
}
```

`ResolveMods` (Assets) orders mods after their dependencies and `loadAfter` names and before their `loadBefore` names, breaking ties by namespace, so the order never depends on how folders were found. It refuses a mod, with a reason in the log, for a namespace another mod shares, a `cookedFormat` other than the engine's, a missing or older dependency, a load cycle, or a file that replaces the game's or an earlier mod's without being listed in `overrides`; mods depending on a refused mod are refused too. Two mods replacing one file is allowed, the later winning, with a warning, as is an override of a file nothing provides. Loaded mods mount in front of the base content, the last loaded first. The base package is never changed.

Mods are untrusted: their packages get the same checks as any other, and their files the same bounds. A mod package signed with a trusted key is reported as the publisher's; others as unsigned, installed by the user. `bLoadMods = false` plays without them.

`HertaCooker mod --source=<mod folder> --project=<p.hertaproject> --output=<Mods/Namespace> [--package]` cooks a mod's `Content` folder of source assets through the asset worker into the game's staged layout, so an asset with one of the game's ids replaces it. Textures are staged in both color spaces; materials with their own shaders are refused until mods can carry compiled shaders. `Games/Sandbox/Mods/NeonCoral` turns the Sandbox's Coral material glowing cyan.

Script behavior mods wait for Milestone 16's sandbox; native binary mods stay disabled.

## Release hygiene

Packages carry `LICENSE` and every third-party license and notice file under `ThirdPartyLicenses`, with the README naming FreeType. Game and server crash reports name the build: see [Game runtime](Game.md#crash-reports). Shipping builds compile and link deterministically with source paths mapped to the checkout root, and Linux binaries carry a GNU build id that symbol files match.

Protected release tags, clean double-build verification of binaries, SBOMs, artifact attestations, and immutable GitHub releases belong to publishing a release, not to local packaging, and are not set up yet.
