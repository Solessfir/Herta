#pragma once

#include "Herta/Math/Vector.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>
#include <string>

namespace Herta
{
// The editor sun follows one fixed mid-latitude arc: sunrise in the east (-X) at 06:00, highest towards the south (-Z) at noon, sunset in the west (+X) at 18:00.
inline constexpr float TimeOfDayArcTilt = 35.f * std::numbers::pi_v<float> / 180.f;

[[nodiscard]] FVector3 GetTimeOfDaySunDirection(float Hours);
[[nodiscard]] float GetTimeOfDayHours(const FVector3& SunDirection);
[[nodiscard]] std::string FormatTimeOfDay(float Hours);

namespace TimeOfDayDetail
{
[[nodiscard]] inline FVector3 GetArcNoon()
{
	return FVector3{0.f, std::cos(TimeOfDayArcTilt), -std::sin(TimeOfDayArcTilt)};
}

inline constexpr FVector3 East{-1.f, 0.f, 0.f};
}

// Unit vector pointing towards the sun.
inline FVector3 GetTimeOfDaySunDirection(const float Hours)
{
	const float Angle = (Hours - 6.f) / 12.f * std::numbers::pi_v<float>;
	return TimeOfDayDetail::East * std::cos(Angle) + TimeOfDayDetail::GetArcNoon() * std::sin(Angle);
}

// Projects any sun direction onto the arc, so manually rotated suns still report a nearby time.
inline float GetTimeOfDayHours(const FVector3& SunDirection)
{
	const float Angle = std::atan2(SunDirection.Dot(TimeOfDayDetail::GetArcNoon()), SunDirection.Dot(TimeOfDayDetail::East));
	const float Hours = 6.f + Angle / std::numbers::pi_v<float> * 12.f;
	return std::fmod(Hours + 24.f, 24.f);
}

inline std::string FormatTimeOfDay(const float Hours)
{
	const int Minutes = static_cast<int>(std::round(std::clamp(Hours, 0.f, 24.f) * 60.f)) % (24 * 60);
	return std::format("{:02}:{:02}", Minutes / 60, Minutes % 60);
}
}
