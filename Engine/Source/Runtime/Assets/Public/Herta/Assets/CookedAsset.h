#pragma once

#include "Herta/Assets/Material.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace Herta
{
// Bump when any cooked layout or cooking rule changes; build keys include it, so stale derived data is never reused.
inline constexpr std::uint32_t CookedAssetFormatVersion = 3;

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

inline constexpr std::size_t MaximumCookedJoints = 1024;
inline constexpr std::size_t MaximumCookedClips = 256;
inline constexpr std::uint16_t CookedSkinWeightTotal = 65535;

// Up to four joints per vertex, indexed in skeleton joint order. Weights are unorm16 and sum to CookedSkinWeightTotal; unused slots have zero weight.
struct FCookedSkinInfluence
{
	std::array<std::uint16_t, 4> Joints{};
	std::array<std::uint16_t, 4> Weights{CookedSkinWeightTotal, 0, 0, 0};
};

// Joints are ordered so every parent precedes its children. Runtime is the Animation module's own archive of the same skeleton; Herta code outside Animation never reads it.
struct FCookedSkeleton
{
	std::vector<std::string> JointNames;
	std::vector<std::int16_t> JointParents;
	// Column-major model-to-joint transforms of the bind pose, one per joint.
	std::vector<std::array<float, 16>> InverseBindMatrices;
	std::vector<std::byte> Runtime;
};

// Animation and RootMotion are Animation module archives. RootMotion holds the root joint's horizontal travel, removed from Animation so clips play in place.
struct FCookedAnimationClip
{
	std::string Name;
	float DurationSeconds = 0.f;
	std::vector<std::byte> Animation;
	std::vector<std::byte> RootMotion;
};

// A mesh in model space with counter-clockwise front faces and uint32 triangle lists. A skinned mesh is in its bind pose and carries one influence per vertex.
struct FCookedModel
{
	std::vector<FCookedVertex> Vertices;
	std::vector<std::uint32_t> Indices;
	std::vector<FCookedMeshSection> Sections;
	std::vector<FCookedMaterial> Materials;
	std::vector<FCookedTexture> Textures;

	std::vector<FCookedSkinInfluence> Influences{};
	std::optional<FCookedSkeleton> Skeleton{};
	std::vector<FCookedAnimationClip> Clips{};
};

using FCookedAsset = std::variant<FCookedTexture, FCookedModel, FMaterialAsset>;

[[nodiscard]] std::expected<void, FAssetError> ValidateCookedTexture(const FCookedTexture& Texture);
[[nodiscard]] std::expected<void, FAssetError> ValidateCookedModel(const FCookedModel& Model);
// The skin, skeleton, and clip part of ValidateCookedModel. It checks Herta's own fields; the Animation module validates its runtime archives when it loads them.
[[nodiscard]] std::expected<void, FAssetError> ValidateCookedSkeletalData(const FCookedModel& Model);

[[nodiscard]] std::expected<std::vector<std::byte>, FAssetError> SerializeCookedAsset(const FCookedAsset& Asset);

// Validates every count, index, and dimension before allocating, so corrupt derived data cannot drive large allocations.
[[nodiscard]] std::expected<FCookedAsset, FAssetError> DeserializeCookedAsset(std::span<const std::byte> Bytes);
}
