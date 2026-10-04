#pragma once

#include "Herta/Scene/SceneSerialization.h"

#include <cstddef>
#include <cstdint>
#include <expected>

namespace Herta
{
enum class EScalingSceneWorkload : std::uint8_t
{
	Rendering,
	DynamicBodies,
};

struct FScalingSceneOptions
{
	std::size_t CubeCount = 1000;
	EScalingSceneWorkload Workload = EScalingSceneWorkload::Rendering;
};

[[nodiscard]] std::expected<FSceneDocument, FSceneError> GenerateScalingScene(FScalingSceneOptions Options);
}
