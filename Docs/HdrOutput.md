# HDR Output

Status: implemented in the editor. `HertaGame` will present through the same swapchain and renderer path in Milestone 5. Rendering was already HDR internally; this covers presenting it to HDR10 displays.

## Pipeline

GT7 is designed so HDR is the same curve with a higher peak, so there is one tone mapper whose parameters and output encoding change.

- **Exposure is unchanged.** Exposed 1.0 is paper white, so the same scene at the same EV looks equally bright in SDR and HDR, and only highlights gain range.
- **Tone mapping** (`Engine/Shaders/ToneMap.slang`): SDR places paper white and the peak at GT7's 250 cd/m2 and writes the `Rgba8Srgb` display target, exactly as before. With `FVisualSettings::HdrDisplay` set, exposed values are scaled to the display paper white, mapped with the display peak as GT7's peak, and written to an `Rgba16Float` display target as linear Rec.709 relative to paper white. A highlight therefore reads at most `PeakLuminance / PaperWhite`.
- **Overlays:** the grid, selection outline, and debug primitives write that same target with float pipeline variants. Their colors mean the same thing in both modes, since 1.0 is paper white. The HDR variants are built the first time HDR output is requested.
- **SMAA** stays after tone mapping and needs no separate path. Edge detection already sRGB-encodes its input; above 1.0 the encoding continues logarithmically with the curve's slope at white, so SDR-range edges behave exactly as before and highlights still produce sensible luma contrast. Neighborhood blending runs in linear light, as it does through the sRGB view in SDR.
- **Final encode:** ToolUI is the only pass that writes the swapchain. `ToolUI.frag` encodes per draw from a push constant: sRGB UI content is decoded and shown at paper white, the HDR scene target is scaled by paper white, and the blurred backdrop, already in the target's encoding, passes through. On HDR10 targets the result is converted to Rec.2020 and PQ encoded. ImGui blends in display encoding in both modes, PQ values in HDR and sRGB values in SDR, so text and antialiasing keep their weight.
- **Swapchain:** `VK_EXT_swapchain_colorspace` is enabled when present. With HDR requested, a surface that offers `A2B10G10R10_UNORM_PACK32` with `VK_COLOR_SPACE_HDR10_ST2084_EXT` gets HDR10; any other surface keeps 8-bit sRGB. Secondary ImGui windows choose per surface. `VK_EXT_hdr_metadata`, when available, reports Rec.2020 primaries, the peak as maximum and content light level, and paper white as frame-average level.
- **Monitor changes:** the main surface's HDR10 support is re-queried at most once per second, and the swapchain is recreated when HDR should switch on or off, for example after dragging the window between an HDR and an SDR monitor.

Thumbnails and other SDR images are shown at paper white. scRGB (`R16G16B16A16_SFLOAT` with extended sRGB linear) is not implemented: HDR10 is what displays consume natively and is offered everywhere Herta has been tested. HDR output clips GT7's result to Rec.709 primaries; a Rec.2020 display target would keep the wider gamut.

## Settings

The **Appearance** popup in the editor's bottom bar holds per-user display settings, saved with the rest of the appearance in `Saved/Editor/Appearance.ini` (format version 4; older files load with HDR off). **Reset** keeps them, because they describe the monitor rather than the look.

- **HDR output** is off by default and only offered while the main window's display reports HDR10.
- **Peak brightness** (250 to 10,000 nits) and **Paper white** (80 to 500 nits) follow the operating system until changed, shown as "(system)". On Windows, the peak comes from `DXGI_OUTPUT_DESC1::MaxLuminance` for the monitor under the window, and paper white from the "SDR content brightness" setting (`DISPLAYCONFIG_SDR_WHITE_LEVEL`). Elsewhere, or when nothing is reported, they default to 1000 and 200 nits. The peak never drops below paper white. **Use system values** returns to following the system.
- **Calibrate peak** replaces the viewport with an outer square at the configured peak around an inner square at 10,000 nits on black. Raise Peak brightness until the inner square disappears into the outer one; that is where the display clips. The pattern turns off when the popup closes.

Visual-test and scaling captures and smoke runs always use SDR, because they read back 8-bit images.

## Platforms

- **Windows:** turn on Use HDR in Windows display settings. Verified on an NVIDIA RTX 4090 Laptop GPU with a mini-LED panel.
- **Linux, Wayland:** works when the compositor implements the color management protocol and the Vulkan driver reports HDR10 for the surface (recent Mesa; NVIDIA support depends on the driver version). Otherwise the option stays unavailable. The system luminance query is Windows-only, so Linux starts from the 1000/200 nit defaults; calibrate the peak. Not yet verified on Hyprland.
- **Linux, X11:** no HDR path.

## Verification

- `HertaEditor --renderer-test` renders the 18% grey card in HDR at 250 nit paper white and requires it to match the SDR grey (0.146 against 0.147 of paper white), requires an 8-stop overexposed highlight to land on the 1000 nit peak (4.0), and checks the calibration pattern values. Where the display offers HDR10, it also recreates the swapchain as HDR10 and back under validation.
- Unit tests cover the ST 2084 encoding against reference values, neutral HDR10 white, appearance-file round-trips and older versions, luminance resolution and clamping, and HDR uniform validation.
- The SDR grey card still reads 107.
- Visual review on an HDR display remains manual: sun disc and specular highlights should exceed UI white, UI text should match the desktop's SDR brightness, and toggling HDR should not change midtone brightness.
