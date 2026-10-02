# xxHash

- Source: https://github.com/Cyan4973/xxHash
- Revision: `v0.8.3` (`e626a72bc2321cd320e953a0ccf1584cad60f363`)
- License: BSD-2-Clause. See `LICENSE`.

| File | Upstream path |
|---|---|
| `xxhash.h` | `xxhash.h` |

Core includes it with `XXH_INLINE_ALL` to implement `HashBytes`. XXH3-128 output is frozen upstream, so updating must not change Herta build keys. The file is unmodified. To update, copy `xxhash.h` from the new upstream tag, update the revision above, and run the Core hash tests, which pin a known XXH3-128 value.
