# simdjson

- Source: https://github.com/simdjson/simdjson
- Revision: `v4.6.11` (`f5de14f09256982933af2849beb43778bd421ca7`)
- License: Apache-2.0 or MIT. See `LICENSE` and `LICENSE-MIT`.

| File | Upstream path |
|---|---|
| `simdjson.h` | `singleheader/simdjson.h` |
| `simdjson.cpp` | `singleheader/simdjson.cpp` |

simdjson is only used by fastgltf and is compiled into the private `FastGltf` library. Keep the version that fastgltf's `cmake/dependencies.cmake` sets as `SIMDJSON_TARGET_VERSION`. The files are unmodified. To update, copy the single-header pair from the matching upstream tag, update the revision above, and run the glTF cooking tests.
