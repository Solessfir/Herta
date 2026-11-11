#pragma once

#include "Herta/ToolUI/Theme.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Herta
{
class FApplication;
class FLocalization;
class FWindow;

struct FToolUIError
{
	std::string Message;
};

enum class EToolUIViewportFrameStatus : std::uint8_t
{
	Ready,
	Skipped,
	NeedsResize
};

struct FToolUIViewportPosition
{
	float X = 0.f;
	float Y = 0.f;

	[[nodiscard]] constexpr bool operator==(const FToolUIViewportPosition&) const noexcept = default;
};

struct FToolUICanvasBounds
{
	float X = 0.f;
	float Y = 0.f;
	float Width = 0.f;
	float Height = 0.f;

	[[nodiscard]] constexpr bool operator==(const FToolUICanvasBounds&) const noexcept = default;
};

[[nodiscard]] constexpr FToolUICanvasBounds ResolveToolUIWorkspaceCanvas(const FToolUICanvasBounds Window, const float TitleBarHeight, const float StatusBarHeight) noexcept
{
	const float Top = std::clamp(TitleBarHeight, 0.f, std::max(0.f, Window.Height));
	const float Bottom = std::clamp(StatusBarHeight, 0.f, std::max(0.f, Window.Height - Top));
	return {Window.X, Window.Y + Top, std::max(0.f, Window.Width), std::max(0.f, Window.Height - Top - Bottom)};
}

struct FToolUIViewportWindowPolicy
{
	bool bShowInTaskbar = true;
	bool bTopMost = false;
	bool bFocusOnShow = true;

	[[nodiscard]] constexpr bool operator==(const FToolUIViewportWindowPolicy&) const noexcept = default;
};

[[nodiscard]] constexpr FToolUIViewportWindowPolicy ResolveToolUIViewportWindowPolicy(const bool bNoTaskBarIcon, const bool bTopMost, const bool bNoFocusOnAppearing) noexcept
{
	return {
	    .bShowInTaskbar = !bNoTaskBarIcon,
	    .bTopMost = bTopMost,
	    .bFocusOnShow = !bNoFocusOnAppearing,
	};
}

[[nodiscard]] constexpr FToolUIViewportPosition ResolveToolUIViewportPosition(const bool bProgrammaticPositionSupported, const FToolUIViewportPosition CachedPosition, const FToolUIViewportPosition PlatformPosition) noexcept
{
	return bProgrammaticPositionSupported ? PlatformPosition : CachedPosition;
}

struct FToolUIRendererBridge
{
	std::move_only_function<std::expected<void, FToolUIError>()> Initialize;
	std::move_only_function<std::expected<void, FToolUIError>(const void*)> Render;
	std::move_only_function<std::expected<std::uint64_t, FToolUIError>(void*, std::uint32_t, std::uint32_t)> CreateViewport;
	std::move_only_function<std::expected<void, FToolUIError>(std::uint64_t)> DestroyViewport;
	std::move_only_function<std::expected<void, FToolUIError>(std::uint64_t, std::uint32_t, std::uint32_t)> ResizeViewport;
	std::move_only_function<std::expected<EToolUIViewportFrameStatus, FToolUIError>(std::uint64_t)> BeginViewportFrame;
	std::move_only_function<std::expected<void, FToolUIError>(std::uint64_t, const void*)> RenderViewport;
	std::move_only_function<std::expected<EToolUIViewportFrameStatus, FToolUIError>(std::uint64_t)> PresentViewport;
	std::move_only_function<void()> Shutdown;
};

struct FToolUIDescriptor
{
	FApplication* Application = nullptr;
	FWindow* Window = nullptr;
	std::filesystem::path RegularFontPath = "Engine/Content/Editor/Fonts/Roboto/Roboto-Regular.ttf";
	std::filesystem::path MediumFontPath = "Engine/Content/Editor/Fonts/Roboto/Roboto-Medium.ttf";
	std::filesystem::path LogFontPath = "Engine/Content/Editor/Fonts/DroidSansMono/DroidSansMono.ttf";
	// Merged into every editor font for the emoji they lack; optional.
	std::filesystem::path EmojiFontPath = "Engine/Content/Editor/Fonts/NotoEmoji/NotoEmoji.ttf";
	std::filesystem::path LayoutPath = "Saved/Editor/ImGui.ini";
	std::filesystem::path AppearancePath = "Saved/Editor/Appearance.ini";
	FEditorAppearance Appearance;
	FToolUIRendererBridge Renderer;
	bool bVSync = true;
	std::function<void(bool)> VSyncChanged;
	std::function<void()> RefreshRequested;
};

enum class EToolUIPanelKind : std::uint8_t
{
	// Docks wherever the user or the default layout put it.
	Tool,
	// The level viewport: always the central node's first tab, with no close button.
	Viewport,
	// An asset editor: a title-bar tab beside the level that fills the workspace while in front, as Unreal's major tabs do. "Label###Id" names the tab.
	Document,
	// A transient overlay across the bottom of the workspace, such as the content drawer. It closes when focus leaves it and never docks or saves a layout.
	Drawer,
};

enum class EToolUIMenuIcon : std::uint8_t
{
	Panel,
	Outliner,
	Details,
	Log,
	Layout,
	Sync,
	Exit,
	Import,
	Open,
	Save,
	Undo,
	Redo,
	Add,
	Copy,
	Paste,
	Duplicate,
	Delete,
	Cube,
	Entity,
	Physics,
	Light,
	SkyAtmosphere,
	Fog,
	Material,
	SelectAll,
	ContentBrowser,
	Performance,
	Sound,
};

bool ToolUIMenuItem(std::string_view Label, EToolUIMenuIcon Icon, bool* bSelected = nullptr, const char* Shortcut = nullptr);
bool ToolUIButton(std::string_view Label, EToolUIMenuIcon Icon, float Height = 0.f);
void ToolUIIcon(EToolUIMenuIcon Icon, float CenterX, float CenterY, float Scale = 1.f);
// Returns nothing for a missing, older-than-supported, or out-of-range file, which leaves the defaults in place.
[[nodiscard]] std::optional<FEditorAppearance> LoadEditorAppearance(const std::filesystem::path& Path);
void SaveEditorAppearance(const std::filesystem::path& Path, const FEditorAppearance& Appearance) noexcept;
// Switch for boolean settings, right-aligned within the next item width.
bool ToolUIToggle(const char* Id, bool* bValue);
// A single-line text field that edits whole grapheme clusters, so Backspace removes an emoji with its skin tone and the cursor never lands
// inside a flag. Width zero fills the available width. Returns true when the text changed.
bool ToolUIInputText(const char* Id, std::string& Text, const char* Hint = nullptr, float Width = 0.f);

// An item ImGui laid out, as automation sees it: the label without its "##id" part, its window, and its screen bounds.
struct FToolUIItem
{
	std::uint32_t Id = 0;
	std::string Label{};
	std::string Window{};
	FToolUICanvasBounds Bounds{};
	bool bDisabled = false;
	bool bCheckable = false;
	bool bChecked = false;
	bool bOpenable = false;
	bool bOpened = false;
	bool bInput = false;
};

// What the last frame asked the GPU to draw for the editor's UI, over every native window.
struct FToolUIFrameStats
{
	std::uint32_t Windows = 0;
	std::uint32_t PlatformWindows = 0;
	std::uint32_t DrawLists = 0;
	std::uint32_t DrawCommands = 0;
	std::uint32_t Vertices = 0;
	std::uint32_t Indices = 0;
};

class FToolUIContext final
{
public:
	struct FImplementation;

	[[nodiscard]] static std::expected<std::unique_ptr<FToolUIContext>, FToolUIError> Create(FToolUIDescriptor Descriptor);

	~FToolUIContext();

	FToolUIContext(const FToolUIContext&) = delete;
	FToolUIContext& operator=(const FToolUIContext&) = delete;
	FToolUIContext(FToolUIContext&&) = delete;
	FToolUIContext& operator=(FToolUIContext&&) = delete;

	void BeginFrame();
	[[nodiscard]] std::expected<void, FToolUIError> EndFrame(bool bRenderMainViewport = true);
	[[nodiscard]] std::expected<void, FToolUIError> RenderPlatformWindows();
	// DrawFileMenuItems adds items at the top of the File menu, above the workspace section.
	void DrawWorkspace(std::string_view ApplicationTitle, const std::function<void()>& DrawWindowMenuItems = {}, const std::function<void()>& DrawStatusItems = {}, const std::function<void()>& DrawFileMenuItems = {}, const std::function<void()>& DrawEditMenuItems = {});
	[[nodiscard]] bool BeginPanel(std::string_view Name, bool* bOpen = nullptr, EToolUIPanelKind Kind = EToolUIPanelKind::Tool);
	void EndPanel();
	void SetViewportImmersive(bool bImmersive) noexcept;
	[[nodiscard]] bool IsViewportImmersive() const noexcept;
	// Undocks a panel to screen-space bounds the next time it begins. Bounds outside the main window give it its own native window.
	void FloatPanel(std::string_view Name, FToolUICanvasBounds Bounds);
	// Rebuilds the default layout next frame, docking every panel back into the main window.
	void ResetLayout() noexcept;
	// Native windows ImGui currently renders, including the main one.
	[[nodiscard]] std::size_t GetPlatformWindowCount() const noexcept;
	// The main window in the screen coordinates FloatPanel takes.
	[[nodiscard]] FToolUICanvasBounds GetMainWindowBounds() const noexcept;
	// Hides the pointer for play in the viewport. While captured, ImGui sees the pointer resting at the viewport's centre, so clicks always land
	// on the viewport, and its motion is GetCapturedMouseMotion.
	void SetCursorCaptured(bool bCaptured);
	// How far the captured pointer moved over the last frame, in pixels.
	[[nodiscard]] FToolUIViewportPosition GetCapturedMouseMotion() const noexcept;
	bool IsPanelFocused(std::string_view Name) const noexcept;
	bool IsPanelHovered(std::string_view Name) const noexcept;
	// True while an asset editor tab is in front, hiding the level panels.
	bool IsDocumentInFront() const noexcept;
	// Brings the level tab to the front and focuses its viewport.
	void FocusViewport() noexcept;
	void SetLevelTabLabel(std::string_view Label);
	// Files dropped onto any editor window since the last call, in drop order.
	[[nodiscard]] std::vector<std::filesystem::path> TakeDroppedFiles();
	void PushLogFont();
	void PopLogFont();
	[[nodiscard]] std::optional<FToolUICanvasBounds> GetWorkspaceCanvasForCurrentPanel() const noexcept;
	void DrawGlassSurface(float X, float Y, float Width, float Height, float Radius) const;
	bool DrawSearchField(const char* Label, const char* Hint, char* Buffer, std::size_t BufferSize) const;

	// The editor's text, which outlives the context; without it the workspace shows English.
	void SetText(const FLocalization* Text) noexcept;
	void SetAppearance(FEditorAppearance Appearance) noexcept;
	[[nodiscard]] const FEditorAppearance& GetAppearance() const noexcept;
	[[nodiscard]] const FToolUIThemeMetrics& GetMetrics() const noexcept;
	[[nodiscard]] bool WantsTextInput() const noexcept;
	[[nodiscard]] bool WantsMouseInput() const noexcept;

	[[nodiscard]] const FToolUIFrameStats& GetFrameStats() const noexcept;
	// Records the items each frame lays out, for automation; off by default.
	void SetItemRecording(bool bEnabled) noexcept;
	// The last complete frame's items, in the order they were laid out.
	[[nodiscard]] std::span<const FToolUIItem> GetRecordedItems() const noexcept;
	// Queues input as if the user gave it, in screen coordinates; it takes effect over the next frames.
	void QueueClick(float X, float Y, int Button, bool bDouble);
	void QueueText(std::string_view Text);
	// A key with optional modifiers, such as "Enter" or "Ctrl+S"; false for a name it does not know.
	[[nodiscard]] bool QueueKey(std::string_view Chord);

private:
	friend struct FToolUITestAccess;
	static void MigrateContentDockHeight(std::uint32_t DockspaceId);
	static bool OrderContentDockTabs();
	explicit FToolUIContext(std::unique_ptr<FImplementation> Implementation) noexcept;

	std::unique_ptr<FImplementation> Implementation;
};
}
