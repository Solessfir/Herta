#include "PreviewSimulation.h"

#include <cmath>
#include <doctest/doctest.h>
#include <limits>
#include <numbers>

namespace Herta
{
namespace
{
constexpr FTransform DefaultFloor{{0.0f, -0.25f, 0.0f}, FQuaternion::Identity(), {10.0f, 0.25f, 10.0f}};
}

TEST_CASE("Preview simulation falls onto the floor and restores edits")
{
	FPreviewSimulation Simulation;
	const FTransform Original{{0.5f, 4.0f, 0.25f}, FQuaternion::FromAxisAngle({0, 1, 0}, 0.4f), {1.0f, 0.75f, 1.0f}};
	REQUIRE(Simulation.Start(Original, DefaultFloor));
	CHECK(Simulation.IsRunning());
	CHECK(Simulation.GetTransform() == Original);
	CHECK_FALSE(Simulation.Start(Original, DefaultFloor));
	for (int Frame = 0; Frame < 600; ++Frame)
		REQUIRE(Simulation.Update(1.0f / 120.0f));
	CHECK(Simulation.GetTransform().Translation.Y == doctest::Approx(0.75f).epsilon(0.02f));
	CHECK(Simulation.GetTransform().Scale3D == Original.Scale3D);
	Simulation.Stop();
	CHECK_FALSE(Simulation.IsRunning());
	CHECK(Simulation.GetTransform() == Original);
	Simulation.Stop();
	CHECK(Simulation.GetTransform() == Original);
	REQUIRE(Simulation.Start(Original, DefaultFloor));
	Simulation.Stop();
}

TEST_CASE("Preview simulation uses a fixed step and rejects invalid frame deltas")
{
	FPreviewSimulation Fast;
	FPreviewSimulation Slow;
	const FTransform Original{{0, 8, 0}, FQuaternion::Identity(), FVector3::One()};
	REQUIRE(Fast.Start(Original, DefaultFloor));
	REQUIRE(Slow.Start(Original, DefaultFloor));
	for (int Frame = 0; Frame < 120; ++Frame)
		REQUIRE(Fast.Update(1.0f / 120.0f));
	for (int Frame = 0; Frame < 30; ++Frame)
		REQUIRE(Slow.Update(1.0f / 30.0f));
	CHECK(Fast.GetTransform().Translation.Y == doctest::Approx(Slow.GetTransform().Translation.Y).epsilon(0.0001f));
	CHECK_FALSE(Fast.Update(-1.0f));
	CHECK_FALSE(Fast.Update(std::numeric_limits<float>::quiet_NaN()));
	const float Before = Fast.GetTransform().Translation.Y;
	REQUIRE(Fast.Update(1000.0f));
	CHECK(Fast.GetTransform().Translation.Y > Before - 5.0f);
}

TEST_CASE("Preview simulation collision follows the edited floor transform")
{
	FPreviewSimulation Simulation;
	const FTransform Cube{{6.0f, 8.0f, 2.0f}, FQuaternion::Identity(), {0.25f, 0.25f, 0.25f}};
	const FTransform Floor{{3.0f, 2.0f, 2.0f}, FQuaternion::FromAxisAngle({0, 1, 0}, std::numbers::pi_v<float> * 0.5f), {-0.75f, 0.5f, 5.0f}};
	REQUIRE(Simulation.Start(Cube, Floor));
	for (int Frame = 0; Frame < 600; ++Frame)
		REQUIRE(Simulation.Update(1.0f / 120.0f));
	CHECK(Simulation.GetTransform().Translation.Y == doctest::Approx(2.75f).epsilon(0.02f));
	Simulation.Stop();
	CHECK(Simulation.GetTransform() == Cube);

	FTransform InvalidFloor = Floor;
	InvalidFloor.Scale3D.Y = 0.0f;
	CHECK_FALSE(Simulation.Start(Cube, InvalidFloor));
	CHECK_FALSE(Simulation.IsRunning());
	CHECK(Simulation.GetTransform() == Cube);
	InvalidFloor.Scale3D.Y = std::numeric_limits<float>::quiet_NaN();
	CHECK_FALSE(Simulation.Start(Cube, InvalidFloor));
	CHECK_FALSE(Simulation.IsRunning());
	CHECK(Simulation.GetTransform() == Cube);
	REQUIRE(Simulation.Start(Cube, Floor));
}
}
