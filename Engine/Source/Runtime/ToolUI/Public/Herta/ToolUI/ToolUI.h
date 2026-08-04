#pragma once

#include "Herta/ToolUI/Theme.h"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

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
	float X = 0.0f;
	float Y = 0.0f;

	[[nodiscard]] constexpr bool operator==(const FToolUIViewportPosition&) const noexcept = default;
};

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
	    .bFocusOnShow = !bNoFocusOnAppearing};
}

[[nodiscard]] constexpr FToolUIViewportPosition ResolveToolUIViewportPosition(const bool bProgrammaticPositionSupported, const FToolUIViewportPosition CachedPosition, const FToolUIViewportPosition PlatformPosition) noexcept
{
	return bProgrammaticPositionSupported ? PlatformPosition : CachedPosition;
}

[[nodiscard]] constexpr bool ShouldToolUIViewportCaptureTitleBar(const bool bActiveItemInViewport, const float CursorY, const float TitleBarHeight) noexcept
{
	return bActiveItemInViewport && CursorY >= 0.0f && CursorY < TitleBarHeight;
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
	std::filesystem::path LayoutPath = "Saved/Editor/ImGui.ini";
	std::filesystem::path AppearancePath = "Saved/Editor/Appearance.ini";
	FEditorAppearance Appearance;
	FToolUIRendererBridge Renderer;
	std::function<void()> RefreshRequested;
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
	void DrawWorkspace(std::string_view ApplicationTitle);
	[[nodiscard]] bool BeginPanel(std::string_view Name, bool* bOpen = nullptr);
	void EndPanel();

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
