#pragma once

#include "Herta/Assets/CookedAsset.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

namespace Herta
{
bool IsEncodedHdrTexture(std::span<const std::byte> EncodedImage);

// Builds the full mip chain with a linear-light box filter, then drops top mips above MaximumCookedTextureDimension.
// Factor multiplies linear color and alpha, matching glTF's base color factor.
[[nodiscard]] std::expected<FCookedTexture, FAssetError> CookTexture(std::uint32_t Width, std::uint32_t Height, std::span<const std::byte> RgbaPixels, ETextureColorSpace ColorSpace, const std::array<float, 4>& Factor = {1.f, 1.f, 1.f, 1.f});
// Linear RGBA32F environment data, with a full mip chain capped at 1024 pixels per edge to bound upload memory.
[[nodiscard]] std::expected<FCookedTexture, FAssetError> CookHdrTexture(std::uint32_t Width, std::uint32_t Height, std::span<const float> RgbaPixels);
// Decodes PNG, JPEG, or HDR. HDR always retains linear floating-point radiance.
[[nodiscard]] std::expected<FCookedTexture, FAssetError> CookEncodedTexture(std::span<const std::byte> EncodedImage, ETextureColorSpace ColorSpace, const std::array<float, 4>& Factor = {1.f, 1.f, 1.f, 1.f});
}
