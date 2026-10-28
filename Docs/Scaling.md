# Scaling checkpoint

The early Milestone 4 checkpoint uses deterministic levels with 1,000, 5,000, and 10,000 cubes plus one floor, sharing the engine cube asset. Rendering fixtures have no rigid bodies. Dynamic fixtures have ten-cube stacks against a static floor. IDs and canonical level text repeat across runs.

## Generate and inspect

Use `HertaEditorCmd` or the editor console:

```text
level.generate-scaling rendering 10000 TestResults/Rendering10000.hlevel
level.generate-scaling dynamic 10000 TestResults/Dynamic10000.hlevel
```

The destination directory must already exist. The command appears in console completion. Generation writes atomically and does not replace the current editor level. Use `level.load <path>` to inspect the fixture, then exercise search, scrolling, Shift/Ctrl selection, batch component edits, undo/redo, and Simulate/Stop. Outliner and asset-picker widgets clip off-screen rows; Details submits one aggregated component inspector rather than one inspector per selected entity.

## Repeatable capture

Build `HertaEditor`, `HertaEditorCmd`, and `HertaTests` in Shipping first. Run from the repository:

```powershell
& Scripts/Windows/MeasureScaling.ps1
```

```sh
./Scripts/Linux/MeasureScaling.sh
```

The scripts generate all six fixtures under a unique `TestResults/Scaling` directory and run the native editor sequentially. Windows accepts `-Counts 10000 -Workloads dynamic` for a focused capture; Linux uses GNU `time` for peak resident memory. Run without another benchmark, compiler, or game consuming the CPU/GPU.

Individual captures also work:

```text
HertaEditor --scaling-test=TestResults/Rendering10000.hlevel
HertaEditor --scaling-test=TestResults/Dynamic10000.hlevel --scaling-simulate
```

Captures use an isolated default workspace, disable VSync, wait for shared assets to load, and frame the entire fixture. Each phase warms up for 60 frames and samples 240 presented frames. Rendering and select-all phases always run. Dynamic captures additionally simulate at a fixed 60 Hz for one step per frame, stop, and verify every model matrix against its authored pose before sampling the restored level. Early close, rendering errors, physics failures, or the five-minute budget produce failure, not a partial success.

Logs report configuration, viewport resolution, entity/selection count, scene draw count, and median/p95/p99 milliseconds for CPU frame, inspector submission, float model extraction, simulation plus synchronization, and render submission. CPU frame includes presentation waits; render submission is CPU recording, not GPU execution. GPU UI timing is explicitly UI-only. No scene GPU timing is claimed.

`HertaTests --test-case="*Scaling*,*scaling*"` covers fixture determinism, canonical save/load, invalid admission, large selections, batch transforms/components, duplication/deletion, history, stable IDs, repeated simulation, pose restoration, and capacity failures. Authoring tests separately report EnTT world snapshot extraction timings. Timing values are diagnostics, not machine-dependent pass/fail thresholds.

## Bounded preview physics

`FPhysicsWorldSettings` centralizes body, body-pair cache, contact, and temporary-memory budgets. Runtime adapter defaults remain small. Editor preview defaults support the checkpoint with 16,384 bodies, 65,536 body-pair entries, 32,768 contact constraints, and 64 MiB temporary storage.

Startup validates the whole configured scratch budget against the pinned Jolt box-body path before allocation and stages bodies locally. Failed admission leaves existing preview state untouched. Jolt reports pair-cache/contact exhaustion during stepping; the error latches, no partial preview transforms publish, and the editor stops and restores authored poses. Pair capacity sizes Jolt's cache, not a promise of a strict observed pair-count cutoff. Dense overlapping levels can exceed contact budgets even below the body limit.

The scratch bound accounts for discrete rigid boxes, one single-threaded update step, island/body arrays, pair queue, and contact constraints with alignment margin. It must be revisited when adding CCD, joints, soft bodies, or a parallel job adapter. Large-island splitting is disabled under the existing single-threaded preview job system.

## Reference measurements

Windows capture on 2026-10-05: Core i9-14900HX, RTX 4090 Laptop GPU, MSVC Shipping, Vulkan validation disabled, VSync off, 2557 x 1423 viewport. These are end-to-end CPU frame times, not GPU timings or isolated renderer benchmarks.

Rendering fixtures, no selection:

| Cubes | Before instancing median (ms) | Instanced median / p95 / p99 (ms) | Draws before / after |
| --- | ---: | --- | ---: |
| 1,000 | 3.277 | 0.854 / 1.059 / 1.255 | 1,002 / 2 |
| 5,000 | 16.417 | 2.831 / 3.231 / 3.506 | 5,002 / 2 |
| 10,000 | 34.324 | 5.518 / 5.894 / 6.174 | 10,002 / 2 |

Draw counts include the world grid and exclude ToolUI. All-selected rendering uses three draws; at 10,000 cubes its CPU median is 15.555 ms and inspector submission is 0.977 ms. Selection overlay expansion remains a significant cost despite clipped Outliner rows and aggregated Details.

Dynamic fixtures, all selected, one fixed physics step per frame:

| Dynamic cubes | CPU median / p95 / p99 (ms) | Physics plus synchronization median / p95 / p99 (ms) | Peak working set (MiB) |
| --- | --- | --- | ---: |
| 1,000 | 2.253 / 2.558 / 2.771 | 0.551 / 0.945 / 0.989 | 157.68 |
| 5,000 | 8.079 / 9.708 / 10.154 | 2.225 / 4.068 / 4.297 | 193.01 |
| 10,000 | 17.345 / 21.015 / 23.142 | 5.817 / 9.451 / 10.385 | 242.24 |

Every dynamic capture completed selection, simulation, and exact authored-pose restoration. The 10,000-body capture used 365.95 MiB before instancing. Working-set peaks cover the whole process and are sampled every 250 ms, not allocation totals.

Before-instancing logs are under `TestResults/Scaling/20261005-002519-950`, `20261005-002541-360`, and `20261005-002750-897`. All six final fixtures and logs are under `TestResults/Scaling/20261005-004755-135`. These local artifacts are ignored by Git; retain them when comparing changes.

Windows capture on 2026-10-11, same machine and settings, after per-view culling, volatile draw uniforms, and per-recording framebuffer and binding set reuse:

| Fixture | Phase | CPU median / p95 / p99 (ms) |
| --- | --- | --- |
| 1,000 rendering | rendering | 1.182 / 1.963 / 2.033 |
| 10,000 rendering | rendering | 2.635 / 2.858 / 3.057 |
| 10,000 rendering | all selected | 8.573 / 9.475 / 9.820 |
| 10,000 dynamic | simulation | 15.155 / 18.643 / 19.420 |

The all-selected phase had been failing since Milestone 4.5 grew the visual uniforms. It drew one selection outline per object and exceeded the 4 MiB per-frame uniform budget. The outline now draws the visible selection as one instanced draw per batch. Logs are under `TestResults/Scaling/20261011-192302-555`.

Windows capture on 2026-10-29, same machine and settings, after the Milestone 6 renderer work (GTAO, clustered lights, compute SMAA, fog, and exposure, quality tiers, and the shadow changes):

| Fixture | Phase | CPU median / p95 / p99 (ms) | Render submission median (ms) |
| --- | --- | --- | ---: |
| 1,000 rendering | rendering | 1.369 / 1.769 / 1.800 | |
| 10,000 rendering | rendering | 2.906 / 3.201 / 3.501 | 1.701 |
| 10,000 rendering | all selected | 10.007 / 10.715 / 11.084 | 2.903 |
| 10,000 dynamic | simulation | 15.967 / 19.724 / 20.399 | 2.846 |

Every capture completed, including exact authored-pose restoration after simulation, and wrote empty error logs. Logs are under `TestResults/Scaling/20261029-000059-686`. Against 2026-10-11, inspector and simulation times are unchanged within noise, but CPU render submission grew from 1.351 to 1.701 ms when rendering and from 1.983 to 2.903 ms with everything selected. These fixtures have no shadow-casting lights, so the shadow caches do not run in them; the growth is CPU recording of the passes Milestone 6 added. Timing each part of a 10,000-object rendering frame put 1.12 ms in the setup before the render graph (culling and instance batching), 0.61 ms in graph passes, of which uploading instances took 0.43 ms, and 0.27 ms in command replay. Batching then looked each model's batch up through a key that copied its material list, a heap allocation per model per frame; a transparent lookup now allocates only for new batches, and the per-float finiteness check on instance uploads became a branch-free exponent test that vectorizes. Together they took setup to 1.06 ms and the instance upload to 0.41 ms, and 0.69 ms from 0.81 ms with everything selected. Most of what remains is instance size: each instance grew from 128 to 192 bytes when the previous frame's transform was added for motion vectors, and every frame uploads it even with motion vectors off, the default. Moving that transform into its own vertex stream, bound only for the velocity prepass, would cut the upload back by a third.

Native readback compares instanced and single-draw rendering across two frames with multiple meshes, nonzero instance offsets, rotation, nonuniform and mirrored scale. Both frames matched exactly, with zero differing pixels.

Windows ClangCL and MSVC Shipping rebuilds passed with zero warnings. Each passed 385 test cases and 189,959 assertions, with one skipped case. Cppcheck passed for the changed modules, and Windows/Linux package tests include the instanced shader. A native early-close probe returned failure as required.

Linux GCC and Clang syntax checks and packaging tests passed through WSL. Full Linux builds, tests, and native measurements remain unverified: the Linux SDK/host tools are not installed in this checkout, and the available Vulkan ICD is software-only. The native capture is tracked in [LinuxTodo.md](LinuxTodo.md).
