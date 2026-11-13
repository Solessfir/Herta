# simdutf

- Source: https://github.com/simdutf/simdutf
- Revision: `v9.3.3` (`59589cd3df6823cbfdbd253cde3377e7c3e1fe9b`)
- License: Apache-2.0 or MIT. See `LICENSE-APACHE` and `LICENSE-MIT`.

| File | Upstream path |
|---|---|
| `simdutf.h` | `simdutf.h` from the v9.3.3 release assets |
| `simdutf.cpp` | `simdutf.cpp` from the v9.3.3 release assets |

simdutf is only used by fastgltf, for base64 buffers and UTF-8 handling, and is compiled into the private `FastGltf` library. Keep the version that fastgltf's `cmake/dependencies.cmake` sets as `FASTGLTF_SIMDUTF_TARGET_VERSION`; that file also lists the SHA-256 of both release assets, which these copies match. The files are unmodified. To update, download the release pair for the new version, check it against those hashes, replace both files and the licenses from the matching tag, update the revision above, and run the glTF cooking tests.
