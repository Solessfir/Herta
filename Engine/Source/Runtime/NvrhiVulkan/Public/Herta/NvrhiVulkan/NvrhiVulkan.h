#pragma once

#include "Herta/RHI/Graphics.h"
#include "Herta/RHI/Presentation.h"

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Herta
{
class FLogService;

struct FNvrhiVulkanPresentationDescriptor
{
	std::string ApplicationName = "Herta";
	std::uint32_t ApplicationVersion = 1;
	void* WindowBackendHandle = nullptr;
	std::vector<std::string> RequiredInstanceExtensions;
	FExtent2D InitialExtent;
	std::uint32_t DesiredImageCount = 3;
	bool bVSync = true;
	bool bEnableValidation = false;
	bool bRequireValidation = false;
	FLogService* Log = nullptr;
};

// HDR10 output: PQ-encoded Rec.2020 on surfaces that offer it. Other surfaces keep sRGB.
struct FHdrOutputSettings
{
	bool bEnabled = false;
	// Luminance of SDR white, the editor UI, and the scene's paper white, in cd/m^2.
	float PaperWhite = 200.f;
	// Peak sent to the display as HDR metadata, in cd/m^2.
	float PeakLuminance = 1000.f;
};

class INvrhiVulkanPresentation : public IPresentationDevice
{
public:
	[[nodiscard]] virtual IGraphicsDevice& GetGraphicsDevice() noexcept = 0;
	[[nodiscard]] virtual std::expected<std::uint64_t, FPresentationError> RegisterToolUITexture(const FTextureHandle& Texture, bool bBackdropSource = false) = 0;
	virtual void UnregisterToolUITexture(std::uint64_t TextureId) noexcept = 0;
	[[nodiscard]] virtual std::expected<void, FPresentationError> InitializeToolUIRenderer() = 0;
	[[nodiscard]] virtual std::expected<void, FPresentationError> RenderToolUIDrawData(const void* DrawData) = 0;
	virtual void SetToolUIGpuTimingEnabled(bool bEnabled) noexcept = 0;
	[[nodiscard]] virtual std::optional<double> GetToolUIGpuMilliseconds() const noexcept = 0;
	virtual void ShutdownToolUIRenderer() noexcept = 0;
	[[nodiscard]] virtual std::expected<FPresentationViewportHandle, FPresentationError> CreateViewport(void* WindowBackendHandle, FExtent2D Extent) = 0;
	[[nodiscard]] virtual std::expected<void, FPresentationError> DestroyViewport(FPresentationViewportHandle Viewport) = 0;
	[[nodiscard]] virtual std::expected<void, FPresentationError> ResizeViewport(FPresentationViewportHandle Viewport, FExtent2D Extent) = 0;
	[[nodiscard]] virtual std::expected<EPresentationStatus, FPresentationError> BeginViewportFrame(FPresentationViewportHandle Viewport) = 0;
	[[nodiscard]] virtual std::expected<void, FPresentationError> RenderViewportToolUIDrawData(FPresentationViewportHandle Viewport, const void* DrawData) = 0;
	[[nodiscard]] virtual std::expected<EPresentationStatus, FPresentationError> PresentViewport(FPresentationViewportHandle Viewport) = 0;
	[[nodiscard]] virtual bool HasValidationErrors() const noexcept = 0;
	// Whether the main window's surface currently offers HDR10. Moving the window can change monitors, so the answer is refreshed about once per second.
	[[nodiscard]] virtual bool IsHdrOutputSupported() = 0;
	[[nodiscard]] virtual bool IsHdrOutputActive() const noexcept = 0;
	// Recreates swapchains whose HDR state should change. Call it outside a frame; it is cheap when nothing changes.
	[[nodiscard]] virtual std::expected<void, FPresentationError> SetHdrOutput(const FHdrOutputSettings& Settings) = 0;

protected:
	INvrhiVulkanPresentation() = default;
};

[[nodiscard]] std::expected<std::unique_ptr<INvrhiVulkanPresentation>, FPresentationError> CreateNvrhiVulkanPresentation(FNvrhiVulkanPresentationDescriptor Descriptor);
}
