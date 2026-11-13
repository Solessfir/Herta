# simdjson

- Source: https://github.com/simdjson/simdjson
- Revision: `v5.1.0` (`ec73615105bd405fc36ddcd1b4a6afb045aeffc1`)
- License: Apache-2.0 or MIT. See `LICENSE` and `LICENSE-MIT`.

| File | Upstream path |
|---|---|
| `simdjson.h` | `singleheader/simdjson.h` |
| `simdjson.cpp` | `singleheader/simdjson.cpp` |

simdjson is only used by fastgltf and is compiled into the private `FastGltf` library. Keep the version that fastgltf's `cmake/dependencies.cmake` sets as `FASTGLTF_SIMDJSON_TARGET_VERSION`, whose SHA-256 values these copies match. The files are unmodified. To update, copy the single-header pair from the matching upstream tag, update the revision above, and run the glTF cooking tests.
