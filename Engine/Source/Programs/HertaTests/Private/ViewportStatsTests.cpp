#include "PerformancePanel.h"
#include "ViewportStats.h"

#include <doctest/doctest.h>

#include <array>
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

TEST_CASE("Performance history keeps the newest samples in order and averages a trailing window")
{
	FMetricHistory History;
	CHECK(History.GetCount() == 0);
	CHECK(History.GetLatest() == 0.f);
	CHECK(History.GetAverage(10) == 0.f);
	for (std::size_t Index = 0; Index < FMetricHistory::Capacity + 5; ++Index)
	{
		History.Push(static_cast<float>(Index));
	}

	CHECK(History.GetCount() == FMetricHistory::Capacity);
	CHECK(History.At(0) == 5.f);
	CHECK(History.GetLatest() == static_cast<float>(FMetricHistory::Capacity + 4));
	CHECK(History.GetMaximum() == History.GetLatest());
	CHECK(History.GetAverage(2) == doctest::Approx(History.GetLatest() - 0.5f));
}

TEST_CASE("Performance samples keep per-pass histories aligned when passes come and go")
{
	FPerformancePanelState State;
	const std::array First{FGpuPassTiming{.Name = "Shadows", .Milliseconds = 2.}, FGpuPassTiming{.Name = "Fog", .Milliseconds = 0.5}};
	RecordPerformanceSample(State, {.FrameMilliseconds = 5., .CpuMilliseconds = 3., .GpuUIMilliseconds = 0.25, .Passes = First});
	const std::array Second{FGpuPassTiming{.Name = "Shadows", .Milliseconds = 1.}};
	RecordPerformanceSample(State, {.FrameMilliseconds = 4., .Passes = Second});
	REQUIRE(State.Passes.size() == 2);
	CHECK(State.Passes[0].Name == "Shadows");
	CHECK(State.Passes[1].Milliseconds.GetCount() == 2);
	CHECK(State.Passes[1].Milliseconds.GetLatest() == 0.f);
	CHECK(State.Gpu.At(0) == doctest::Approx(2.75f));
	CHECK(State.Frame.GetLatest() == 4.f);

	State.bPaused = true;
	RecordPerformanceSample(State, {.FrameMilliseconds = 9., .Passes = Second});
	CHECK(State.Frame.GetCount() == 2);
}
}
