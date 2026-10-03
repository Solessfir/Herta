#include "ViewportStats.h"

#include <doctest/doctest.h>

#include <initializer_list>

namespace Herta
{
TEST_CASE("Stat command toggles viewport timing overlays independently")
{
	FEditorCommandRegistry Registry;
	auto Stats = std::make_shared<FViewportStats>();
	REQUIRE(RegisterViewportStatsCommand(Registry, Stats).has_value());

	CHECK(Registry.Execute("stat unit").has_value());
	CHECK(Stats->bUnitVisible);
	CHECK_FALSE(Stats->bFpsVisible);
	CHECK(Registry.Execute("stat fps").has_value());
	CHECK(Stats->bUnitVisible);
	CHECK(Stats->bFpsVisible);
	CHECK(Registry.Execute("stat unit").has_value());
	CHECK_FALSE(Stats->bUnitVisible);
	CHECK(Stats->bFpsVisible);
	CHECK(Registry.Execute("stat fps").has_value());
	CHECK_FALSE(Stats->bFpsVisible);
}

TEST_CASE("Stat command rejects invalid arguments without changing visibility")
{
	FEditorCommandRegistry Registry;
	auto Stats = std::make_shared<FViewportStats>();
	REQUIRE(RegisterViewportStatsCommand(Registry, Stats).has_value());

	for (const char* const Command : {"stat", "stat gpu", "stat fps extra", "stat unit fps"})
	{
		const auto Result = Registry.Execute(Command);
		REQUIRE_FALSE(Result.has_value());
		CHECK(Result.error().Code == EEditorCommandErrorCode::ExecutionFailed);
		CHECK(Result.error().Message.find("Usage: stat") != std::string::npos);
		CHECK_FALSE(Stats->bUnitVisible);
		CHECK_FALSE(Stats->bFpsVisible);
	}

	CHECK_FALSE(RegisterViewportStatsCommand(Registry, {}).has_value());
}
}
