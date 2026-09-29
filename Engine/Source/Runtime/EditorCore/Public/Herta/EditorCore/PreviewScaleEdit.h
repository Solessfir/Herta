#pragma once

#include <array>
#include <cmath>
#include <cstddef>

namespace Herta
{
inline constexpr float MinimumPreviewScale = 0.001f;
inline constexpr float MaximumPreviewScale = 1000.0f;

[[nodiscard]] inline bool TrySetProportionalPreviewScale(std::array<float, 3>& Scale, const std::size_t Axis, const float Value) noexcept
{
	if (Axis >= Scale.size() || !std::isfinite(Value) || Value < MinimumPreviewScale || Value > MaximumPreviewScale)
	{
		return false;
	}
	for (const float Component : Scale)
	{
		if (!std::isfinite(Component) || Component < MinimumPreviewScale || Component > MaximumPreviewScale)
		{
			return false;
		}
	}
	const float Factor = Value / Scale[Axis];
	std::array<float, 3> Candidate = Scale;
	for (float& Component : Candidate)
	{
		Component *= Factor;
		if (!std::isfinite(Component) || Component < MinimumPreviewScale || Component > MaximumPreviewScale)
		{
			return false;
		}
	}
	Candidate[Axis] = Value;
	Scale = Candidate;
	return true;
}
}
