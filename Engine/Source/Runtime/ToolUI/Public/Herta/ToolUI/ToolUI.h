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
#include <string>
#include <string_view>
#include <vector>

namespace Herta
{
class FApplication;
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
	std::filesystem::path LayoutPath = "Saved/Editor/ImGui.ini";
	std::filesystem::path AppearancePath = "Saved/Editor/Appearance.ini";
	FEditorAppearance Appearance;
	FToolUIRendererBridge Renderer;
	bool bVSync = true;
	std::function<void(bool)> VSyncChanged;
	std::function<void()> RefreshRequested;
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
	Delete
};

bool ToolUIMenuItem(std::string_view Label, EToolUIMenuIcon Icon, bool* bSelected = nullptr, const char* Shortcut = nullptr);
// Switch for boolean settings, right-aligned within the next item width.
bool ToolUIToggle(const char* Id, bool* bValue);

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
	[[nodiscard]] bool BeginPanel(std::string_view Name, bool* bOpen = nullptr, bool bViewport = false);
	void EndPanel();
	void SetViewportImmersive(bool bImmersive) noexcept;
	[[nodiscard]] bool IsViewportImmersive() const noexcept;
	bool IsPanelFocused(std::string_view Name) const noexcept;
	// Files dropped onto any editor window since the last call, in drop order.
	[[nodiscard]] std::vector<std::filesystem::path> TakeDroppedFiles();
	void PushLogFont();
	void PopLogFont();
	[[nodiscard]] std::optional<FToolUICanvasBounds> GetWorkspaceCanvasForCurrentPanel() const noexcept;
	void DrawGlassSurface(float X, float Y, float Width, float Height, float Radius) const;
	bool DrawSearchField(const char* Label, const char* Hint, char* Buffer, std::size_t BufferSize) const;

	void SetAppearance(FEditorAppearance Appearance) noexcept;
	[[nodiscard]] const FEditorAppearance& GetAppearance() const noexcept;
	[[nodiscard]] const FToolUIThemeMetrics& GetMetrics() const noexcept;
	[[nodiscard]] bool WantsTextInput() const noexcept;
	[[nodiscard]] bool WantsMouseInput() const noexcept;

private:
	explicit FToolUIContext(std::unique_ptr<FImplementation> Implementation) noexcept;

	std::unique_ptr<FImplementation> Implementation;
};
}
