# Monocypher

- Source: https://github.com/LoupVaillant/Monocypher
- Revision: tag `4.0.2`, commit `0d85f98c9d9b0227e42cf795cb527dff372b40a4`
- License: BSD 2-Clause or CC0 1.0, at the user's choice. See `LICENCE.md`.

| File | Upstream path | Used by |
|---|---|---|
| `monocypher.c`, `monocypher.h` | `src/` | Assets and PackageBuilder, which hash package chunks with BLAKE2b |
| `monocypher-ed25519.c`, `monocypher-ed25519.h` | `src/optional/` | Assets and PackageBuilder, which verify and sign package indexes with standard Ed25519 (SHA-512) |

The files are unmodified; the version line in `monocypher.h` reads `__git__` because the release tarball, not the tag, substitutes the version there. Herta builds them as the `Monocypher` static library. To update, copy the same files from the new upstream tag, update the revision above, and run the package tests.
