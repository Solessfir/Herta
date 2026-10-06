#pragma once

#include "Herta/Assets/AssetRegistry.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace Herta
{
enum class ETextureColorSpace : std::uint8_t
{
	Linear,
	Srgb
};

enum class EMaterialBlendMode : std::uint8_t
{
	Opaque,
	Masked
};

enum class EMaterialTextureSlot : std::uint8_t
{
	BaseColor,
	Metallic,
	Roughness,
	Normal,
	Occlusion,
	Emissive,
	Count
};

enum class EMaterialChannel : std::uint8_t
{
	Red,
	Green,
	Blue,
	Alpha,
	Rgb
};

inline constexpr std::size_t MaterialTextureSlotCount = static_cast<std::size_t>(EMaterialTextureSlot::Count);
inline constexpr std::uint32_t MaterialAssetVersion = 1;

struct FMaterialParameters
{
	bool operator==(const FMaterialParameters&) const = default;

	std::array<float, 4> BaseColor{1.f, 1.f, 1.f, 1.f};
	float Metallic = 0.f;
	float Roughness = 0.5f;
	float NormalStrength = 1.f;
	float OcclusionStrength = 1.f;
	std::array<float, 3> Emissive{};
	float EmissiveIntensity = 1.f;
	std::array<float, 2> UVScale{1.f, 1.f};
	std::array<float, 2> UVOffset{};
	EMaterialBlendMode BlendMode = EMaterialBlendMode::Opaque;
	float AlphaCutoff = 0.5f;
};

struct FMaterialTextureBinding
{
	bool operator==(const FMaterialTextureBinding&) const = default;

	FAssetId Texture;
	EMaterialChannel Channel = EMaterialChannel::Rgb;
	ETextureColorSpace ColorSpace = ETextureColorSpace::Linear;
};

inline constexpr std::array<FMaterialTextureBinding, MaterialTextureSlotCount> DefaultMaterialTextureBindings{
    FMaterialTextureBinding{.Texture = {}, .Channel = EMaterialChannel::Rgb, .ColorSpace = ETextureColorSpace::Srgb},
    FMaterialTextureBinding{.Texture = {}, .Channel = EMaterialChannel::Red, .ColorSpace = ETextureColorSpace::Linear},
    FMaterialTextureBinding{.Texture = {}, .Channel = EMaterialChannel::Red, .ColorSpace = ETextureColorSpace::Linear},
    FMaterialTextureBinding{.Texture = {}, .Channel = EMaterialChannel::Rgb, .ColorSpace = ETextureColorSpace::Linear},
    FMaterialTextureBinding{.Texture = {}, .Channel = EMaterialChannel::Red, .ColorSpace = ETextureColorSpace::Linear},
    FMaterialTextureBinding{.Texture = {}, .Channel = EMaterialChannel::Rgb, .ColorSpace = ETextureColorSpace::Srgb},
};

struct FMaterialAsset
{
	bool operator==(const FMaterialAsset&) const = default;

	std::string Name = "Material";
	FMaterialParameters Parameters{};
	std::array<FMaterialTextureBinding, MaterialTextureSlotCount> Textures = DefaultMaterialTextureBindings;
	// Content-root-relative compatible material shader; empty selects the engine PBR shader.
	std::string ShaderPath{};
};

[[nodiscard]] std::expected<void, FAssetError> ValidateMaterialParameters(const FMaterialParameters& Parameters);
[[nodiscard]] std::expected<void, FAssetError> ValidateMaterial(const FMaterialAsset& Material);
[[nodiscard]] std::expected<std::string, FAssetError> SerializeMaterial(const FMaterialAsset& Material);
[[nodiscard]] std::expected<FMaterialAsset, FAssetError> DeserializeMaterial(std::string_view Text);
}
