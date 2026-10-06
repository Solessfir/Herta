#pragma once

#include "Herta/Assets/Material.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace Herta
{
// Bump when any cooked layout or cooking rule changes; build keys include it, so stale derived data is never reused.
inline constexpr std::uint32_t CookedAssetFormatVersion = 2;

inline constexpr std::uint32_t MaximumCookedTextureDimension = 4096;
// Matches the per-recording GPU upload budget.
inline constexpr std::size_t MaximumCookedBufferBytes = std::size_t{64} * 1024 * 1024;

enum class ETexturePixelFormat : std::uint8_t
{
	Rgba8,
	Rgba32Float
};

// Tightly packed rows, top row first. RGBA32F retains linear HDR radiance.
struct FCookedTextureMip
{
	std::uint32_t Width = 0;
	std::uint32_t Height = 0;
	std::vector<std::byte> Pixels;
};

// Mips halve each dimension, rounding down and clamping to one, until both reach one.
struct FCookedTexture
{
	ETextureColorSpace ColorSpace = ETextureColorSpace::Srgb;
	ETexturePixelFormat PixelFormat = ETexturePixelFormat::Rgba8;
	std::vector<FCookedTextureMip> Mips;
};

struct FCookedVertex
{
	std::array<float, 3> Position{};
	std::array<float, 2> UV{};
	std::array<float, 3> Normal{0.f, 1.f, 0.f};
	std::array<float, 4> Tangent{1.f, 0.f, 0.f, 1.f};
};

struct FCookedMeshSection
{
	std::uint32_t FirstIndex = 0;
	std::uint32_t IndexCount = 0;
	std::uint32_t Material = 0;
};

inline constexpr std::uint32_t NoCookedTexture = UINT32_MAX;

struct FCookedMaterial
{
	std::string Name;
	FMaterialParameters Parameters{};
	std::array<std::uint32_t, MaterialTextureSlotCount> Textures{NoCookedTexture, NoCookedTexture, NoCookedTexture, NoCookedTexture, NoCookedTexture, NoCookedTexture};
	std::array<EMaterialChannel, MaterialTextureSlotCount> Channels{EMaterialChannel::Rgb, EMaterialChannel::Red, EMaterialChannel::Red, EMaterialChannel::Rgb, EMaterialChannel::Red, EMaterialChannel::Rgb};
};

// A static mesh in model space with counter-clockwise front faces and uint32 triangle lists.
struct FCookedModel
{
	std::vector<FCookedVertex> Vertices;
	std::vector<std::uint32_t> Indices;
	std::vector<FCookedMeshSection> Sections;
	std::vector<FCookedMaterial> Materials;
	std::vector<FCookedTexture> Textures;
};

using FCookedAsset = std::variant<FCookedTexture, FCookedModel, FMaterialAsset>;

[[nodiscard]] std::expected<void, FAssetError> ValidateCookedTexture(const FCookedTexture& Texture);
[[nodiscard]] std::expected<void, FAssetError> ValidateCookedModel(const FCookedModel& Model);

[[nodiscard]] std::expected<std::vector<std::byte>, FAssetError> SerializeCookedAsset(const FCookedAsset& Asset);

// Validates every count, index, and dimension before allocating, so corrupt derived data cannot drive large allocations.
[[nodiscard]] std::expected<FCookedAsset, FAssetError> DeserializeCookedAsset(std::span<const std::byte> Bytes);
}
