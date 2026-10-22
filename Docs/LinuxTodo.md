# Linux Todo

Checks that need a native Linux machine with a hardware Vulkan driver. Earlier Linux runs used WSL with Mesa lavapipe, which is a software renderer, so they prove function and validation but not performance. Remove an item once it passes, and record the results in the linked document.

## Build and tests

```sh
./Setup.sh
./GenerateProjectFiles.sh
make --directory=Intermediate/ProjectFiles/gmake --jobs=$(nproc) config=development HertaTests HertaEditorCmd HertaEditor
./Binaries/linux/x86_64/Development/HertaTests
```

- [ ] Clean GCC and Clang Development builds with zero warnings, and all tests pass.
- [ ] Cppcheck on the modules touched since Milestone 4.5, using `--project=compile_commands.json`.

## Renderer changes made after Milestone 4.5

These were verified only on Windows:

- Depth prepass with depth-equal PBR shading, alpha-tested cutouts in the prepass, and custom materials writing their own depth.
- LTC rect lights.
- Vulkan debug labels for each pass, with `VK_EXT_debug_utils` enabled whenever the loader offers it.
- GPU exposure histogram.
- Tone mapping through a 64³ GT7 LUT, with the fog composite merged into it.
- GatherRed-based shadow PCF.

```sh
./Binaries/linux/x86_64/Development/HertaEditor --renderer-test
./Binaries/linux/x86_64/Development/HertaEditor --visual-test --validation
```

- [ ] Both commands report no Vulkan warnings or errors on X11 and on Wayland. On Wayland, check that the validation layer is actually loaded; without it the run is silently clean.
- [ ] Every image in `Saved/VisualTest` matches the matching Windows capture: same materials, lighting bays, rect light shapes, sky, fog and exposure.
- [ ] Record the per-bookmark GPU times in [Rendering.md](Rendering.md), next to the Windows numbers.

## Physics

- [ ] In the Sandbox soft body bay, Simulate (Alt+S) swings the rope with the box held at its free end, and Escape restores the authored transforms.

## Scaling

```sh
./Scripts/Linux/MeasureScaling.sh
```

- [ ] Complete a Shipping capture of the 1k/5k/10k fixtures on hardware, and record it in [Scaling.md](Scaling.md).

## HDR

- [ ] HDR10 output on Hyprland: the option is offered, and peak and paper-white calibration look right. See [HdrOutput.md](HdrOutput.md).
