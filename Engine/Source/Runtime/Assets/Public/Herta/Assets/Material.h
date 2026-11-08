#pragma once

#include "Herta/Assets/AssetRegistry.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

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
// Version 2 adds diffuse roughness and the metal edge tint, and version 3 Material Graph parameter values; older versions load with their defaults.
inline constexpr std::uint32_t MaterialAssetVersion = 3;

// Material Graph parameters share one block of float4 slots in the visual uniforms, which a graph assigns in name order.
inline constexpr std::size_t MaterialParameterSlotCount = 8;
inline constexpr std::size_t MaximumMaterialParameterValues = 32;
using FMaterialParameterSlots = std::array<std::array<float, 4>, MaterialParameterSlotCount>;

// Letters, digits, and underscores, starting with a letter and at most 32 long, so a name reads the same in graphs, material files, and logs.
[[nodiscard]] bool IsValidMaterialParameterName(std::string_view Name);

struct FMaterialParameterValue
{
	bool operator==(const FMaterialParameterValue&) const = default;

	std::string Name;
	// Scalars use the first component.
	std::array<float, 4> Value{};
};

// A parameter as a Material Graph shader reads it. A shader's parameters are in slot order, which is name order.
struct FMaterialShaderParameter
{
	bool operator==(const FMaterialShaderParameter&) const = default;

	std::string Name;
	std::array<float, 4> Default{};
	bool bVector = false;
};

// A shader's parameter table as staged beside its cooked stages: one line per parameter, "<name> <scalar|vector> x y z w".
[[nodiscard]] std::string SerializeMaterialShaderParameters(std::span<const FMaterialShaderParameter> Parameters);
[[nodiscard]] std::expected<std::vector<FMaterialShaderParameter>, FAssetError> DeserializeMaterialShaderParameters(std::string_view Text);

// Each slot takes the material's value for its parameter, or the parameter's default. Both lists are sorted by name, so this is one linear merge.
void ResolveMaterialParameters(std::span<const FMaterialShaderParameter> Parameters, std::span<const FMaterialParameterValue> Values, FMaterialParameterSlots& Slots);

struct FMaterialParameters
{
	bool operator==(const FMaterialParameters&) const = default;

	std::array<float, 4> BaseColor{1.f, 1.f, 1.f, 1.f};
	float Metallic = 0.f;
	float Roughness = 0.5f;
	// EON rough diffuse (OpenPBR base_diffuse_roughness). Zero is Lambert.
	float DiffuseRoughness = 0.f;
	// The metal's reflectance near grazing (OpenPBR specular_color), applied through the F82-tint Fresnel. White is plain Schlick.
	std::array<float, 3> SpecularColor{1.f, 1.f, 1.f};
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
	// Values for the Material Graph's parameters, sorted by name. Names the graph does not have are kept, so a parameter renamed back finds its value.
	std::vector<FMaterialParameterValue> ParameterValues{};
};

// The material's shader as a mounted path ("Engine/..." or "Game/..."); a bare path resolves in Game content unless the material is named under Engine/.
[[nodiscard]] std::string GetMountedMaterialShaderPath(const FMaterialAsset& Material, std::string_view Name);
[[nodiscard]] std::expected<void, FAssetError> ValidateMaterialParameters(const FMaterialParameters& Parameters);
[[nodiscard]] std::expected<void, FAssetError> ValidateMaterial(const FMaterialAsset& Material);
[[nodiscard]] std::expected<std::string, FAssetError> SerializeMaterial(const FMaterialAsset& Material);
[[nodiscard]] std::expected<FMaterialAsset, FAssetError> DeserializeMaterial(std::string_view Text);
}
