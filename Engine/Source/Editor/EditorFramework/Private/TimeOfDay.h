#pragma once

#include "Herta/Math/Vector.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>

namespace Herta
{
// The editor sun follows one fixed mid-latitude arc: sunrise in the east (-X) at 06:00, highest towards the south (-Z) at noon, sunset in the west (+X) at 18:00.
inline constexpr float TimeOfDayArcTilt = 35.f * std::numbers::pi_v<float> / 180.f;

[[nodiscard]] FVector3 GetTimeOfDaySunDirection(float Hours);
[[nodiscard]] float GetTimeOfDayHours(const FVector3& SunDirection);
[[nodiscard]] std::string FormatTimeOfDay(float Hours);
// Reads a clock time such as "7", "07:30", or "19:05", from 00:00 to 24:00.
[[nodiscard]] std::optional<float> ParseTimeOfDay(std::string_view Text);

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

inline std::optional<float> ParseTimeOfDay(std::string_view Text)
{
	const auto ParseNumber = [](const std::string_view Digits) -> std::optional<int>
	{
		int Number = 0;
		const auto [End, Error] = std::from_chars(Digits.data(), Digits.data() + Digits.size(), Number);
		if (Digits.empty() || Digits.size() > 2 || Error != std::errc{} || End != Digits.data() + Digits.size() || Number < 0)
		{
			return std::nullopt;
		}

		return Number;
	};

	const std::size_t First = Text.find_first_not_of(' ');
	const std::size_t Last = Text.find_last_not_of(' ');
	if (First == std::string_view::npos)
	{
		return std::nullopt;
	}

	Text = Text.substr(First, Last - First + 1);
	const std::size_t Colon = Text.find(':');
	const std::optional<int> Hours = ParseNumber(Text.substr(0, Colon));
	const std::optional<int> Minutes = Colon == std::string_view::npos ? std::optional<int>{0} : ParseNumber(Text.substr(Colon + 1));
	if (!Hours || !Minutes || *Hours > 24 || *Minutes > 59 || (*Hours == 24 && *Minutes > 0))
	{
		return std::nullopt;
	}

	return static_cast<float>(*Hours) + static_cast<float>(*Minutes) / 60.f;
}
}
