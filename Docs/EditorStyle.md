# Herta Editor Style

Status: Milestone 1 baseline

This is the initial HertaEditor visual and interaction language. It is based on the native application shell proven in the C++ ProjectTemplate and remains the default until Herta has enough real tools to justify a deliberate redesign.

The goal is a dense, calm workspace with clear hierarchy. Neutral surfaces carry most of the interface. Color identifies actions, selection, status, and focused content instead of decorating every panel.

## Ownership

`ToolUI` owns editor style tokens, Dear ImGui style application, DPI scaling, fonts, and reusable controls. Feature panels consume named Herta tokens and helpers. They do not scatter local ImGui color or spacing literals.

Style metrics are logical pixels before DPI scaling. Each native viewport resolves its scale independently. Per-user appearance settings may override supported values later, but Herta always retains a complete default theme.

## Typography

The initial editor uses Roboto Regular and Roboto Medium with a 15 px base size. Medium is used for titles, selected tabs, compact labels, and primary actions. Regular is used for body text, property values, tables, logs, and diagnostics.

Font files live under `Engine/Content/Editor/Fonts/Roboto` with the SIL Open Font License beside them. Packaging and third-party-notice generation fail when a redistributed font is missing its license metadata. The editor exposes the license through its About or Third-party Notices surface.

Dear ImGui may retain pointers to source TTF bytes for atlas rebuilds and dynamic glyph generation. ToolUI keeps the owning font-byte storage alive until the ImGui context and every dependent atlas are destroyed.

MSDF font rendering is not part of the initial editor. Normal Dear ImGui rasterization is appropriate for native-resolution tool UI. Evaluate msdfgen only for a measured world-space, zoomable-canvas, or vector-text requirement.

## Palette

These values are the initial dark theme contract:

| Token | Value | Use |
|---|---:|---|
| `Canvas` | `#090909` | Main background, active title bars, menu bars |
| `Surface0` | `#0F0F0F` | Windows, docking background, scroll tracks |
| `Surface1` | `#141414` | Child regions, popups, inputs, default buttons |
| `Surface2` | `#1C1C1C` | Selected tabs, active frames, headers |
| `SurfaceHover` | `#222222` | Hovered controls and title-bar buttons |
| `Border` | `#262626` | Strong panel and table boundaries |
| `BorderSoft` | `#1A1A1A` | Separators and subtle boundaries |
| `TextPrimary` | `#FFFFFF` | Primary text and window-control glyphs |
| `TextSecondary` | `#B3B3B3` | Supporting text |
| `TextMuted` | `#999999` | Disabled text and inactive state |
| `Accent` | `#0099FF` | Selection, check marks, links, active emphasis |
| `AccentHover` | `#1AA3FF` | Hovered accent controls |
| `CloseHover` | `#C42B1C` | Destructive native close hover |

Project browser, onboarding, and other intentionally promotional surfaces may use the violet, magenta, and coral accent group `#6A4CF5`, `#D44DF0`, and `#FF5577`. Normal editor panels remain neutral.

Docking target previews are white at alpha `50 / 255`. This keeps docking affordances visible without making a temporary layout action look like selected content. Check marks remain accent blue on the dark selected background.

## Metrics

| Metric | Logical value |
|---|---:|
| Base font size | 15 px |
| Main title-bar height | 36 px |
| Native title-bar button width | 46 px |
| Native resize border | 6 px |
| Window padding | 12 x 12 px |
| Window and child rounding | 10 px |
| Popup rounding | 8 px |
| Frame padding | 12 x 8 px |
| Frame and tab rounding | 6 px |
| Item spacing | 8 x 8 px |
| Scrollbar width | 12 px |

Do not shrink global `FramePadding` merely to make checkboxes smaller. Dear ImGui derives checkbox size from frame height, so that also shrinks buttons, inputs, and other controls. If compact checkboxes become necessary, ToolUI provides a focused helper or a locally scoped style adjustment.

Primary call-to-action buttons use a white fill, black text, pill rounding, and no frame border. Removing the border avoids the dark aliased outline produced when a rounded white button is combined with the global frame border. Ordinary editor buttons remain neutral and rectangular with the shared frame rounding.

## Custom title bar

Title-bar drawing and native hit testing consume one shared pure layout model. Hover visuals and native behavior must never derive their rectangles independently.

- The title bar is 36 logical pixels high before platform DPI scaling.
- Win32 and X11 scale native window coordinates with the viewport content scale.
- Wayland window and cursor coordinates are already logical and must not be scaled twice.
- Resize, caption, system menu, minimize, maximize, restore, and close roles are returned through the Herta GLFW hit-test API.
- Window-control pixels are drawn with ImGui primitives, but the controls are not `ImGui::Button` widgets. The native hit-test result owns the click and window-manager action.
- The top-left application glyph returns the system-menu role and uses the same layout for hover feedback.
- Every detached ImGui platform viewport owns its own Herta window, layout, scale, focus, cursor, and capture state.

ImGui input has priority when a floating panel, popup, modal, active item, or other interactive surface overlaps the title bar. The native hit-test callback reads capture state cached by the most recent ImGui frame and returns client space for that viewport. This lets the ImGui surface receive selection and dragging instead of moving the native window.

Title-bar hit testing must be allocation-free and must not call ImGui from inside the GLFW callback. Cache only the minimal per-viewport state required by the callback.

## Application icons

The editor keeps a source SVG for its application mark. Windows builds generate or commit a multi-resolution ICO and compile a resource named `GLFW_ICON` through Premake. The Herta GLFW fork loads that resource as the default executable, taskbar, system-menu, and window-class icon.

Linux does not embed a desktop application icon into the ELF executable. Distribution installs the source SVG or generated PNG sizes with the HertaEditor `.desktop` entry. Runtime window-icon support remains a platform capability exposed through Application.

The Milestone 1 title-bar glyph may use ImGui primitives. LunaSVG and the production icon library remain deferred until an editor tool needs SVG rendering at runtime.

## Panels and backdrop blur

Milestone 1 panels are opaque. The optional translucent blurred-panel design remains planned, but it enters only after Renderer and RenderGraph can provide one shared backdrop pass per viewport. Detached viewports use an opaque or tinted fallback when they cannot sample a meaningful Herta-rendered backdrop.

Opacity, tint, blur radius, quality, and enable state are per-user settings under `Saved/Editor`. Text, controls, selection, and gizmos always render at native resolution after the backdrop pass.

## Verification

The editor shell requires focused coverage for:

- Pure `constexpr` title-bar layout, scaling, and every hit-test region.
- ImGui capture taking priority over native caption dragging.
- Focus, maximize, restore, resize, DPI changes, and system-menu behavior.
- Win32, X11, and Wayland lifecycle smoke tests.
- Per-viewport title-bar state with Dear ImGui multi-viewports enabled.
- Roboto loading and required license metadata.
- Presence of the Windows `GLFW_ICON` resource in packaged editor builds.
- Theme-token and metric defaults where a regression would change interaction geometry.

GLFW API-only tests define `GLFW_INCLUDE_NONE` before including `glfw3.h`. This prevents tests that do not use OpenGL from accidentally requiring platform OpenGL headers.
