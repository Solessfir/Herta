# Linux Todo

Checks that need a native Linux machine with a hardware Vulkan driver. Earlier Linux runs used WSL with Mesa lavapipe, which is a software renderer, so they prove function and validation but not performance. Remove an item once it passes, and record the results in the linked document.

## Build and tests

```sh
./Setup.sh
./GenerateProjectFiles.sh
make --directory=Intermediate/ProjectFiles/gmake --jobs=$(nproc) config=development HertaTests HertaEditorCmd HertaEditor
./Binaries/linux/x86_64/Development/HertaTests
```

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

- [ ] Every image in `Saved/VisualTest` matches the matching Windows capture: same materials, lighting bays, rect light shapes, sky, fog and exposure.
- [ ] Record the per-bookmark GPU times in [Rendering.md](Rendering.md), next to the Windows numbers.

## Physics

- [ ] In the Sandbox soft body bay, Simulate (Alt+S) swings the rope with the box held at its free end, and Escape restores the authored transforms.

## Scaling

```sh
./Scripts/Linux/MeasureScaling.sh
```

- [ ] Complete a Shipping capture of the 1k/5k/10k fixtures on hardware, and record it in [Scaling.md](Scaling.md).

## Game runtime

```sh
make --directory=Intermediate/ProjectFiles/gmake --jobs=$(nproc) config=shipping HertaGame HertaEditorCmd HertaAssetWorker HertaShaders
./Scripts/Linux/PackageGame.sh --verify
./Binaries/linux/x86_64/Development/HertaEditor --play-test --validation
```

- [ ] Play the Sandbox course by hand: pointer capture and relative look (Wayland uses the pointer-constraints path in GLFW), F11 fullscreen on the right monitor, focus loss pausing, and the HUD text size.
- [ ] Settings land in `$XDG_CONFIG_HOME/Herta/Sandbox` or `~/.config/Herta/Sandbox`. Logs already land there; the scripted run writes no settings file, so change a setting by hand.

## HDR

- [ ] HDR10 output on Hyprland: the option is offered, and peak and paper-white calibration look right. See [HdrOutput.md](HdrOutput.md).
