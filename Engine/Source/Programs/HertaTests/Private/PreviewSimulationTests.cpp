#include "PreviewSimulation.h"

#include <doctest/doctest.h>

#include <cmath>
#include <limits>
#include <numbers>

namespace Herta
{
namespace
{
constexpr FTransform DefaultFloor{{0.f, -0.25f, 0.f}, FQuaternion::Identity(), {10.f, 0.25f, 10.f}};
}

TEST_CASE("Preview simulation falls onto the floor and restores edits")
{
	FPreviewSimulation Simulation;
	const FTransform Original{{0.5f, 4.f, 0.25f}, FQuaternion::FromAxisAngle({0, 1, 0}, 0.4f), {1.f, 0.75f, 1.f}};
	REQUIRE(Simulation.Start(Original, DefaultFloor));
	CHECK(Simulation.IsRunning());
	CHECK(Simulation.GetTransform() == Original);
	CHECK_FALSE(Simulation.Start(Original, DefaultFloor));
	for (int Frame = 0; Frame < 600; ++Frame)
	{
		REQUIRE(Simulation.Update(1.f / 120.f));
	}

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
	{
		REQUIRE(Fast.Update(1.f / 120.f));
	}

	for (int Frame = 0; Frame < 30; ++Frame)
	{
		REQUIRE(Slow.Update(1.f / 30.f));
	}

	CHECK(Fast.GetTransform().Translation.Y == doctest::Approx(Slow.GetTransform().Translation.Y).epsilon(0.0001f));
	CHECK_FALSE(Fast.Update(-1.f));
	CHECK_FALSE(Fast.Update(std::numeric_limits<float>::quiet_NaN()));
	const float Before = Fast.GetTransform().Translation.Y;
	REQUIRE(Fast.Update(1000.f));
	CHECK(Fast.GetTransform().Translation.Y > Before - 5.f);
}

TEST_CASE("Preview simulation collision follows the edited floor transform")
{
	FPreviewSimulation Simulation;
	const FTransform Cube{{6.f, 8.f, 2.f}, FQuaternion::Identity(), {0.25f, 0.25f, 0.25f}};
	const FTransform Floor{{3.f, 2.f, 2.f}, FQuaternion::FromAxisAngle({0, 1, 0}, std::numbers::pi_v<float> * 0.5f), {-0.75f, 0.5f, 5.f}};
	REQUIRE(Simulation.Start(Cube, Floor));
	for (int Frame = 0; Frame < 600; ++Frame)
	{
		REQUIRE(Simulation.Update(1.f / 120.f));
	}

	CHECK(Simulation.GetTransform().Translation.Y == doctest::Approx(2.75f).epsilon(0.02f));
	Simulation.Stop();
	CHECK(Simulation.GetTransform() == Cube);

	FTransform InvalidFloor = Floor;
	InvalidFloor.Scale3D.Y = 0.f;
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

TEST_CASE("Preview simulation collides with offset mesh bounds and restores the object origin")
{
	Herta::FPreviewSimulation Simulation;
	const Herta::FTransform Cube{{0.f, 4.f, 0.f}, Herta::FQuaternion::Identity(), {2.f, 2.f, 2.f}};
	const Herta::FTransform Floor{{0.f, -0.25f, 0.f}, Herta::FQuaternion::Identity(), {10.f, 0.25f, 10.f}};
	// A mesh resting on its origin: its bounds sit entirely above Y=0 in model space.
	const Herta::FPreviewBodyShape Shape{.Center = {0.f, 0.5f, 0.f}, .HalfExtents = {0.5f, 0.5f, 0.5f}};
	REQUIRE(Simulation.Start(Cube, Floor, Shape));
	CHECK(Simulation.GetTransform().Translation.Y == doctest::Approx(4.f));
	for (int Step = 0; Step < 600; ++Step)
	{
		REQUIRE(Simulation.Update(1.f / 60.f));
	}

	// The scaled 2 m tall body rests on the floor top at Y=0, so the object origin settles at Y=0 too.
	CHECK(Simulation.GetTransform().Translation.Y == doctest::Approx(0.f).epsilon(0.02));
	Simulation.Stop();
	CHECK(Simulation.GetTransform().Translation.Y == doctest::Approx(4.f));
}
