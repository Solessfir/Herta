# Herta Editor Style

Status: Milestone 1 baseline

This is the initial HertaEditor visual and interaction language. It is based on the native application shell proven in the C++ ProjectTemplate, with engine-specific ownership and persistence added where the template intentionally stayed small.

The goal is a dense, calm workspace with clear hierarchy. Surfaces stay desaturated and slightly above black. The configurable background gradient carries the color, while controls use that hue only for interaction and selection.

## Ownership

`ToolUI` owns editor style tokens, Dear ImGui style application, DPI scaling, fonts, reusable controls, panel presentation, and appearance persistence. Feature panels consume named Herta tokens and helpers. They do not scatter local ImGui color, spacing, transparency, or rounding literals.

Style metrics are logical pixels before DPI scaling. Each native viewport resolves its scale independently. ToolUI always reapplies scaling from an immutable base style instead of scaling the already-scaled style.

Per-user appearance and layout settings live under `Saved/Editor`. Project content never stores a developer's theme, panel transparency, window placement, or dock layout.

## Visual model

The title bar, application toolbar, and dock canvas are one visual workspace. ToolUI draws one full-viewport background before any chrome or panels:

```text
Canvas fill
    -> low-intensity accent gradient
    -> dockspace and panels
    -> text, controls, overlays, and native window glyphs
```

Do not place unrelated solid backgrounds, overlays, or separator lines between the title bar, toolbar, and dock canvas. Pure black is not used for normal surfaces.

## Base palette

These values are the initial neutral contract:

Palette and appearance values are authored as sRGB display values. ToolUI deliberately blends Dear ImGui colors and grayscale font coverage perceptually instead of decoding vertex RGB to linear in its fragment shader. This matches the rasterizer tuning used by desktop UI and keeps small text crisp across light and dark controls.

ToolUI presentation swapchains use an 8-bit UNORM image format with `VK_COLOR_SPACE_SRGB_NONLINEAR_KHR`, so authored palette values and clears reach the compositor without a second sRGB encode. Herta rejects a surface that lacks a compatible UNORM format instead of silently changing the editor appearance through an sRGB attachment fallback. This exception belongs only to ToolUI presentation. The future scene renderer, lighting, post-processing, and HDR pipeline remain linear in offscreen targets and explicitly encode the final display image before composition.

| Token | Value | Use |
|---|---:|---|
| `Canvas` | `#121213` | Full-window background and gradient endpoint |
| `Surface0` | `#171719` | Windows, docking background, and scroll tracks |
| `Surface1` | `#1C1C1E` | Child regions, popups, inputs, and default buttons |
| `Surface2` | `#242427` | Selected tabs, active frames, and headers |
| `SurfaceHover` | `#2C2C2F` | Hovered neutral controls |
| `Border` | `#343438` | Strong panel and table boundaries |
| `BorderSoft` | `#252528` | Separators and subtle boundaries |
| `TextPrimary` | `#FFFFFF` | Primary text and window-control glyphs |
| `TextSecondary` | `#BEBEC1` | Supporting text |
| `TextMuted` | `#949498` | Disabled text and inactive state |
| `NeutralAccent` | `#CDCDD2` | Application mark and neutral active emphasis |
| `NeutralAccentHover` | `#E8E8EB` | Hovered neutral emphasis |
| `TitleBarControlHover` | white, alpha `24 / 255` | System menu, application menu, and caption controls |
| `CloseHover` | `#C42B1C` | Destructive native close hover |

Docking target previews are white at alpha `0.15`. They remain independent of the selected background hue so docking is always legible and does not look like content selection.

## Background color

The initial presets are:

| Preset | Color |
|---|---:|
| Amber | `#E88B76` |
| Rust | `#CD941E` |
| Olive | `#8FB357` |
| Grass | `#4DB17A` |
| Ocean | `#2EA9B7` |
| Sky | `#43A4D2` |
| Cobalt | `#546CE8` |
| Violet | `#9350DC` |
| Plum | `#C253B1` |

Cobalt is the default. The appearance settings also expose a custom HSV color picker and three independent controls:

- Gradient height: default `50%` of the viewport.
- Saturation: default `80%` of the selected color.
- Intensity: default `15%`.

Intensity is a real interpolation factor. At `100%`, the top-left gradient sample reaches the selected color. The top-right uses `40%` of the current intensity, and both lower samples resolve to `Canvas` at the configured height. This gives predictable controls instead of hiding a second hard-coded strength cap.

The ToolUI fragment shader applies deterministic 8 x 8 ordered dithering when vertex RGB varies across a primitive. This removes visible bands from low-intensity background gradients while leaving flat text, controls, icons, and images unchanged. The pattern is static so screenshots remain stable and inactive windows do not shimmer.

When a native viewport loses focus, its background gradient retains `90%` of the configured saturation. Intensity, height, and the shared interaction palette remain unchanged. This lightly quiets inactive workspaces without making their content look disabled. Minimized viewports do not render at all.

Interaction colors are derived from the resolved background hue with additive brightening over neutral control surfaces. Initial strengths are `0.06` subtle, `0.10` hover, `0.16` active, and `0.24` strong. Clamp the result per channel. The close button remains explicitly red.

## Panels and docking

ToolUI exposes four panel background modes:

- All panels transparent.
- Floating panels only.
- Docked panels only.
- Transparency disabled.

The initial default is all panels transparent. Child cards, inputs, tables, popups, and other content surfaces keep their own neutral fills, so the workspace color is visible without sacrificing local contrast. Transparency and backdrop blur are separate features. Milestone 1 uses transparency only.

Panel presentation is applied through a ToolUI window wrapper before `ImGui::Begin`. The wrapper tracks the previous dock state by stable window ID because Dear ImGui exposes the current dock state only after the window begins. Herta-owned panels do not reach into ImGui internals for this. Third-party panels require a narrow adapter if their `Begin` call cannot be wrapped.

The default dock layout is created only when no compatible saved layout exists. A first launch docks the primary Start or viewport surface in the center. Once the user has saved a layout, that layout owns docking and intentional floating state. Layout schema changes use explicit versioning instead of rebuilding the default every launch.

Do not patch Dear ImGui internals merely to hide dock-node corner notches or other small upstream rendering details. First use public style controls, then carry a narrow documented patch only if the defect materially affects the editor.

## Output Log

The Output Log is an EditorFramework panel docked across the bottom of the first-run layout. It uses compact colored text lines rather than a table. Its toolbar contains Clear, Copy, Filter, Options, and a full-width search field. Options expose auto-scroll, pause, and category colorization without adding permanent chrome.

Normal records use a stable readable color derived from their category and apply it to the whole line. Warnings are always yellow and errors are always red. Trace and debug may use quieter neutral colors when category colorization is disabled. The format begins with elapsed time, level, and category before the message, with enough left padding to keep timestamps clear of the panel edge.

The text area behaves as one read-only editor surface even though individual lines retain different colors:

- Hold LMB and drag to select across any number of lines.
- Shift extends the existing range.
- Ctrl+A selects all visible text.
- Ctrl+C copies the selected range.
- Copy uses the selected range, or all visible records when nothing is selected.

Do not implement each line as a separate `InputText`, because Dear ImGui selection cannot cross widget boundaries. ToolUI renders colored lines and selection rectangles through public ImGui drawing APIs, clips large histories, hit-tests UTF-8 boundaries, and supports horizontal and vertical scrolling. The selection model remains UI-independent and unit tested.

Auto-scroll follows new records only while the user is already at the bottom or after an explicit request. Scrolling up relinquishes the tail. Command submission keeps the request active until both the echoed command and resulting records have reached the visible tail, avoiding a one-message lag.

The command field sits at the bottom and Submit aligns with the panel's right content edge using standard window padding. Prefix suggestions open above the field, Up and Down change the active suggestion, and Tab completes it. When no suggestions are visible, Up and Down navigate command history. Available commands come from EditorCore so the interactive panel and `HertaEditorCmd` execute the same operations.

## Typography

The initial editor uses Roboto Regular and Roboto Medium with a 15 px base size. Medium is used for titles, selected tabs, compact labels, and primary actions. Regular is used for body text, property values, tables, logs, and diagnostics.

ToolUI uses Dear ImGui's FreeType builder with its normal hinted rasterization for crisp native-resolution text. Font scaling follows each viewport's content scale. MSDF font rendering is not part of the initial editor. Evaluate msdfgen only for a measured world-space, zoomable-canvas, or vector-text requirement.

Font files live under `Engine/Content/Editor/Fonts/Roboto` with the SIL Open Font License beside them. FreeType attribution is retained with the dependency metadata. Packaging and third-party-notice generation fail when redistributed font or library bytes are missing required license metadata. Tests validate semantic markers and non-empty content instead of brittle exact file sizes.

Dear ImGui may retain pointers to source TTF bytes for atlas rebuilds and dynamic glyph generation. ToolUI keeps the owning font-byte storage alive until the ImGui context and every dependent atlas are destroyed.

## Metrics and controls

| Metric | Logical value |
|---|---:|
| Base font size | 15 px |
| Main title-bar height | 36 px |
| Native title-bar button width | 46 px |
| Native resize border | 6 px |
| Main toolbar height | 46 px |
| Window padding | 12 x 12 px |
| Window and child rounding | 6 px |
| Popup rounding | 5 px |
| Frame padding | 12 x 8 px |
| Frame, tab, menu-item, and drag-target rounding | 4 px |
| Scrollbar width and rounding | 12 px and 5 px |
| Primary-button rounding | 8 px |
| Title-bar control rounding | 4 px |

Do not shrink global `FramePadding` merely to make checkboxes smaller. Dear ImGui derives checkbox size from frame height, so that also shrinks buttons, inputs, and other controls. If compact checkboxes become necessary, ToolUI provides a focused helper or a locally scoped style adjustment.

Primary call-to-action buttons use a white fill, dark text, 8 px rounding, and no frame border. Removing the border avoids the dark aliased outline produced when a rounded white button is combined with the global frame border. Ordinary editor buttons remain neutral.

Menu items, tabs, title-bar controls, drag targets, scrollbars, and image frames use the shared rounding tokens. Hover feedback brightens the current surface additively instead of replacing it with a fixed blue rectangle.

## Custom title bar

Title-bar drawing and native hit testing consume one shared pure layout model. Hover visuals and native behavior must never derive their rectangles independently.

- The top-left application glyph returns the native system-menu role.
- The adjacent hamburger is an ImGui client control for Herta commands.
- The application title starts after both controls and clips before the native caption buttons.
- Minimize, maximize, restore, close, caption, system-menu, and resize roles are returned through the Herta GLFW hit-test API.
- The maximize glyph changes to the overlapping restore glyph while the native window is maximized.
- Window-control pixels are drawn with ImGui primitives, but the controls are not `ImGui::Button` widgets. GLFW and the window system own matching press, release, cancellation, and action behavior.
- Win32 and X11 scale native window coordinates with the viewport content scale.
- Wayland window and cursor coordinates are already logical and must not be scaled twice.
- Every detached ImGui platform viewport owns its own Herta window, layout, scale, focus, cursor, capture, and renderer state.

ImGui input has priority when a floating panel, popup, modal, active item, or other interactive surface overlaps the title bar. The native hit-test callback reads capture state cached by the most recent ImGui frame and returns client space for that viewport. Title-bar hit testing is allocation-free and never calls ImGui from inside the GLFW callback.

## Window lifecycle

On first launch, the main editor window uses `80%` of the primary monitor work area and is centered where the platform permits. Wayland decides global placement. Later launches restore validated per-user placement, clamped so the title bar remains reachable after monitor changes.

The editor window remains hidden until its window, renderer, ToolUI context, fonts, and first frame are ready. When minimized, the main loop waits for events and skips rendering instead of polling and consuming a CPU core.

Native move and resize can enter a platform modal loop that delays the normal frame pump. The GLFW refresh callback renders through the same frame function so exposed areas and newly enlarged framebuffer regions do not remain black until mouse release. That path requires renderer-readiness and recursion guards, current framebuffer dimensions, and safe swapchain recreation. The callback is removed before renderer shutdown.

## Application icons

The editor keeps its application mark at `Engine/Content/Editor/Icons/Herta.svg`. Windows builds compile the matching multi-resolution ICO under `Engine/Source/Programs/HertaEditor/Resources/Windows` as a resource named `GLFW_ICON` through Premake. The Herta GLFW fork loads that resource as the default executable, taskbar, system-menu, and window-class icon.

Linux does not embed a desktop application icon into the ELF executable. Distribution installs the source SVG or generated PNG sizes with the HertaEditor `.desktop` entry. Runtime window-icon support remains a platform capability exposed through Application.

The Milestone 1 title-bar glyph may use ImGui primitives. LunaSVG and the production icon library remain deferred until an editor tool needs SVG rendering at runtime.

## Editor resources

Development loads editor resources loosely for immediate iteration. Packaged editor builds access embedded or external cooked resources through the same Herta-owned provider. ToolUI never branches on loose versus embedded storage.

Milestone 1 keeps this provider narrow and does not preempt the later VFS and package format. When packaging arrives, embed one cooked editor package through the normal package reader instead of generating a separate C++ array API for every asset. Asset baking runs as an explicit build dependency, never downloads data, and writes only ignored generated output.

## Backdrop blur

The optional translucent blurred-panel design remains planned for a later renderer milestone. It renders the scene beneath the main editor panel region, builds one shared downsampled blur pyramid per viewport, then draws text, controls, selection, and gizmos at native resolution. Detached viewports use an opaque or tinted fallback when they cannot sample a meaningful Herta-rendered backdrop.

Opacity, tint, blur radius, quality, and enable state are per-user settings. Transparency remains available when blur is disabled.

## Verification

The editor shell requires focused coverage for:

- Pure `constexpr` title-bar layout, scaling, and every hit-test region.
- Pure window-placement, toolbar-alignment, and panel-transparency policy.
- ImGui capture taking priority over native caption dragging.
- Focus, maximize, restore, live resize, DPI changes, and system-menu behavior.
- Minimized event waiting without continuous frame production.
- Win32, X11, and Wayland lifecycle smoke tests.
- Per-viewport title-bar and renderer state with Dear ImGui multi-viewports enabled.
- First-run default docking without overwriting a saved layout.
- Roboto loading, FreeType integration, and required license metadata.
- Presence of the Windows `GLFW_ICON` resource in packaged editor builds.
- Theme-token, gradient, interaction-strength, and metric defaults where a regression would change presentation or geometry.
- Output Log selection ordering, copy ranges, tail ownership, command completion, history navigation, filter behavior, and bounded-buffer reset or truncation.

GLFW API-only tests define `GLFW_INCLUDE_NONE` before including `glfw3.h`. This prevents tests that do not use OpenGL from accidentally requiring platform OpenGL headers.
