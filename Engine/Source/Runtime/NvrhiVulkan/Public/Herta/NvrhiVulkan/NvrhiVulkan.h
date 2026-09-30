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

protected:
	INvrhiVulkanPresentation() = default;
};

[[nodiscard]] std::expected<std::unique_ptr<INvrhiVulkanPresentation>, FPresentationError> CreateNvrhiVulkanPresentation(FNvrhiVulkanPresentationDescriptor Descriptor);
}
