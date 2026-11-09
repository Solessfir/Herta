#pragma once

#include "Herta/Assets/CookedAsset.h"

#include <fastgltf/types.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <vector>

namespace Herta
{
struct FGltfSkeleton
{
	FCookedSkeleton Skeleton;
	// Skeleton joint index for each of the skin's joint slots, which JOINTS_0 vertex attributes reference.
	std::vector<std::uint16_t> SkinJointToSkeleton;
	std::vector<FCookedAnimationClip> Clips;
};

// Builds the runtime skeleton of one glTF skin and every animation that moves its joints. glTF and Herta share axes and units, so joints keep their transforms.
[[nodiscard]] std::expected<FGltfSkeleton, FAssetError> CookGltfSkeleton(const fastgltf::Asset& Asset, std::size_t SkinIndex, std::vector<std::string>& Warnings);
}
