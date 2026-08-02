#pragma once

#include <algorithm>
#include <cstdint>

namespace Herta
{
inline constexpr int DefaultWindowWorkAreaPercent = 80;

struct FWindowPlacement
{
	int X = 0;
	int Y = 0;
	int Width = 0;
	int Height = 0;
};

[[nodiscard]] constexpr FWindowPlacement ResolveCenteredWindowPlacement(const int WorkAreaX, const int WorkAreaY, const int WorkAreaWidth, const int WorkAreaHeight, const int SizePercent = DefaultWindowWorkAreaPercent) noexcept
{
	if (WorkAreaWidth <= 0 || WorkAreaHeight <= 0)
	{
		return {WorkAreaX, WorkAreaY, 0, 0};
	}

	const std::int64_t ClampedPercent = std::clamp<std::int64_t>(SizePercent, 1, 100);
	const int Width = static_cast<int>(static_cast<std::int64_t>(WorkAreaWidth) * ClampedPercent / 100);
	const int Height = static_cast<int>(static_cast<std::int64_t>(WorkAreaHeight) * ClampedPercent / 100);
	return {
	    .X = WorkAreaX + (WorkAreaWidth - Width) / 2,
	    .Y = WorkAreaY + (WorkAreaHeight - Height) / 2,
	    .Width = Width,
	    .Height = Height};
}
}
