#include "TimeOfDay.h"

#include <doctest/doctest.h>

namespace Herta
{
TEST_CASE("Time of day sun rises east, peaks south at noon, and sets west")
{
	const FVector3 Sunrise = GetTimeOfDaySunDirection(6.f);
	const FVector3 Noon = GetTimeOfDaySunDirection(12.f);
	const FVector3 Sunset = GetTimeOfDaySunDirection(18.f);
	CHECK(Sunrise.X == doctest::Approx(-1.f));
	CHECK(Sunrise.Y == doctest::Approx(0.f).epsilon(1e-5));
	CHECK(Sunset.X == doctest::Approx(1.f));
	CHECK(Noon.Y == doctest::Approx(std::cos(TimeOfDayArcTilt)));
	CHECK(Noon.Z < 0.f);
	CHECK(GetTimeOfDaySunDirection(0.f).Y < 0.f);
	CHECK(Noon.Length() == doctest::Approx(1.f));
}

TEST_CASE("Time of day round-trips through the sun direction and formats as a clock")
{
	for (const float Hours : {0.f, 5.5f, 9.25f, 12.f, 17.75f, 23.5f})
	{
		CAPTURE(Hours);
		CHECK(GetTimeOfDayHours(GetTimeOfDaySunDirection(Hours)) == doctest::Approx(Hours).epsilon(1e-4));
	}

	// Off-arc directions report the closest point on the arc.
	CHECK(GetTimeOfDayHours({0.f, 1.f, 0.f}) == doctest::Approx(12.f));
	CHECK(FormatTimeOfDay(14.5f) == "14:30");
	CHECK(FormatTimeOfDay(0.f) == "00:00");
	CHECK(FormatTimeOfDay(24.f) == "00:00");
	CHECK(FormatTimeOfDay(23.999f) == "00:00");
	CHECK(ParseTimeOfDay("14:30") == 14.5f);
	CHECK(ParseTimeOfDay(" 7 ") == 7.f);
	CHECK(ParseTimeOfDay("7:05") == doctest::Approx(7.f + 5.f / 60.f));
	CHECK(ParseTimeOfDay("24:00") == 24.f);
	CHECK_FALSE(ParseTimeOfDay("24:01"));
	CHECK_FALSE(ParseTimeOfDay("12:60"));
	CHECK_FALSE(ParseTimeOfDay("12:"));
	CHECK_FALSE(ParseTimeOfDay("18.5"));
	CHECK_FALSE(ParseTimeOfDay("-5"));
	CHECK_FALSE(ParseTimeOfDay(""));
}
}
