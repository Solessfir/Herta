#pragma once

#include "Herta/Assets/CookedAsset.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

namespace Herta
{
// Builds the full mip chain with a linear-light box filter, then drops top mips above MaximumCookedTextureDimension.
// Factor multiplies linear color and alpha, matching glTF's base color factor.
[[nodiscard]] std::expected<FCookedTexture, FAssetError> CookTexture(std::uint32_t Width, std::uint32_t Height, std::span<const std::byte> RgbaPixels, ETextureColorSpace ColorSpace, const std::array<float, 4>& Factor = {1.f, 1.f, 1.f, 1.f});
// Decodes PNG or JPEG. Untrusted image data must only reach this inside HertaAssetWorker.
[[nodiscard]] std::expected<FCookedTexture, FAssetError> CookEncodedTexture(std::span<const std::byte> EncodedImage, ETextureColorSpace ColorSpace, const std::array<float, 4>& Factor = {1.f, 1.f, 1.f, 1.f});
}
