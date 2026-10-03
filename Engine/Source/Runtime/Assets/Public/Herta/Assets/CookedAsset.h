#pragma once

#include "Herta/Assets/AssetRegistry.h"

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
inline constexpr std::uint32_t CookedAssetFormatVersion = 1;

inline constexpr std::uint32_t MaximumCookedTextureDimension = 4096;
// Matches the per-recording GPU upload budget.
inline constexpr std::size_t MaximumCookedBufferBytes = std::size_t{64} * 1024 * 1024;

enum class ETextureColorSpace : std::uint8_t
{
	Linear,
	Srgb
};

// Tightly packed RGBA8 rows, top row first.
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
	std::vector<FCookedTextureMip> Mips;
};

struct FCookedVertex
{
	std::array<float, 3> Position{};
	std::array<float, 2> UV{};
};

struct FCookedMeshSection
{
	std::uint32_t FirstIndex = 0;
	std::uint32_t IndexCount = 0;
	std::uint32_t Material = 0;
};

struct FCookedMaterial
{
	std::string Name;
	// Base color with the source material's color factor already applied.
	std::uint32_t BaseColorTexture = 0;
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

using FCookedAsset = std::variant<FCookedTexture, FCookedModel>;

[[nodiscard]] std::expected<void, FAssetError> ValidateCookedTexture(const FCookedTexture& Texture);
[[nodiscard]] std::expected<void, FAssetError> ValidateCookedModel(const FCookedModel& Model);

[[nodiscard]] std::expected<std::vector<std::byte>, FAssetError> SerializeCookedAsset(const FCookedAsset& Asset);

// Validates every count, index, and dimension before allocating, so corrupt derived data cannot drive large allocations.
[[nodiscard]] std::expected<FCookedAsset, FAssetError> DeserializeCookedAsset(std::span<const std::byte> Bytes);
}
