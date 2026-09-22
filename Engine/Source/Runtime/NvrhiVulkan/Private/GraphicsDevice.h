#pragma once

#include "Herta/RHI/Graphics.h"

#include <nvrhi/vulkan.h>

namespace Herta
{
[[nodiscard]] std::expected<std::unique_ptr<IGraphicsDevice>, FPresentationError> CreateGraphicsDevice(nvrhi::IDevice* Device, nvrhi::vulkan::IDevice* VulkanDevice, VkDevice NativeDevice);
[[nodiscard]] nvrhi::ITexture* GetNvrhiTexture(const FTextureHandle& Texture, nvrhi::IDevice* ExpectedDevice = nullptr) noexcept;
}
