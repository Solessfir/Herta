# Bend Studio Screen Space Shadows

- Source: https://www.bendstudio.com/blog/inside-bend-screen-space-shadows/, the example code from Graham Aldridge's SIGGRAPH 2023 talk, as shipped in Days Gone and other Sony titles.
- Archive: https://www.bendstudio.com/assets/cms/downloads/code_final_candidate.zip, SHA-256 `75707a8e287d485c0f71d04fb0ede245bb9a7e9569f1492b1c4d1f6ab943de83`.
- License: Apache 2.0, copyright 2023 Sony Interactive Entertainment. See `LICENSE`, the license text the file headers point to.

| File | Upstream path | Used by |
|---|---|---|
| `bend_sss_gpu.h` | `bend_sss_gpu.h` | Source of `bend_sss_gpu.slangh` |
| `bend_sss_cpu.h` | `bend_sss_cpu.h` | Included by `Engine/Source/Runtime/Renderer/Private/MeshRenderer.cpp` to build the dispatch list |
| `bend_sss_gpu.slangh` | Generated | Included by `Engine/Shaders/Compute/ContactShadows.slang` |

Both upstream files are unmodified. The archive has no other files. Slang rejects two constructs the DirectX compiler accepts, so `ConvertForSlang.py` writes `bend_sss_gpu.slangh` from `bend_sss_gpu.h`: it drops `struct` from `DispatchParameters` parameter types and marks `SetDefaults()` mutating, and keeps everything else, including the license header. To update, download the new archive, check its license, copy both headers, update the hash above, run `python ConvertForSlang.py`, and rerun `HertaEditor --renderer-test` and `--visual-test`.
