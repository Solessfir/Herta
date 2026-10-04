#include "PreviewSimulation.h"

#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <limits>
#include <numbers>

namespace Herta
{
namespace
{
constexpr FTransform DefaultFloor{{0.f, -0.25f, 0.f}, FQuaternion::Identity(), {10.f, 0.25f, 10.f}};

std::array<FPreviewSimulationBody, 2> MakeBodies(const FTransform& Object, const FTransform& Floor, const FPreviewBodyShape& Shape = {})
{
	return {{
	    {.ObjectIndex = 3, .Transform = Object, .Shape = Shape, .MotionType = EPhysicsMotionType::Dynamic},
	    {.ObjectIndex = 9, .Transform = Floor},
	}};
}
}

TEST_CASE("Preview simulation falls onto the floor and restores edits")
{
	FPreviewSimulation Simulation;
	const FTransform Original{{0.5f, 4.f, 0.25f}, FQuaternion::FromAxisAngle({0, 1, 0}, 0.4f), {1.f, 0.75f, 1.f}};
	REQUIRE(Simulation.Start(MakeBodies(Original, DefaultFloor)));
	CHECK(Simulation.IsRunning());
	CHECK(Simulation.GetTransforms()[0].Transform == Original);
	CHECK_FALSE(Simulation.Start(MakeBodies(Original, DefaultFloor)));
	for (int Frame = 0; Frame < 600; ++Frame)
	{
		REQUIRE(Simulation.Update(1.f / 120.f));
	}

	CHECK(Simulation.GetTransforms()[0].Transform.Translation.Y == doctest::Approx(0.75f).epsilon(0.02f));
	CHECK(Simulation.GetTransforms()[0].Transform.Scale3D == Original.Scale3D);
	Simulation.Stop();
	CHECK_FALSE(Simulation.IsRunning());
	CHECK(Simulation.GetTransforms()[0].Transform == Original);
	Simulation.Stop();
	CHECK(Simulation.GetTransforms()[0].Transform == Original);
	REQUIRE(Simulation.Start(MakeBodies(Original, DefaultFloor)));
	Simulation.Stop();
}

TEST_CASE("Preview simulation uses a fixed step and rejects invalid frame deltas")
{
	FPreviewSimulation Fast;
	FPreviewSimulation Slow;
	const FTransform Original{{0, 8, 0}, FQuaternion::Identity(), FVector3::One()};
	REQUIRE(Fast.Start(MakeBodies(Original, DefaultFloor)));
	REQUIRE(Slow.Start(MakeBodies(Original, DefaultFloor)));
	for (int Frame = 0; Frame < 120; ++Frame)
	{
		REQUIRE(Fast.Update(1.f / 120.f));
	}

	for (int Frame = 0; Frame < 30; ++Frame)
	{
		REQUIRE(Slow.Update(1.f / 30.f));
	}

	CHECK(Fast.GetTransforms()[0].Transform.Translation.Y == doctest::Approx(Slow.GetTransforms()[0].Transform.Translation.Y).epsilon(0.0001f));
	CHECK_FALSE(Fast.Update(-1.f));
	CHECK_FALSE(Fast.Update(std::numeric_limits<float>::quiet_NaN()));
	const float Before = Fast.GetTransforms()[0].Transform.Translation.Y;
	REQUIRE(Fast.Update(1000.f));
	CHECK(Fast.GetTransforms()[0].Transform.Translation.Y > Before - 5.f);
}

TEST_CASE("Preview simulation collision follows the edited floor transform")
{
	FPreviewSimulation Simulation;
	const FTransform Cube{{6.f, 8.f, 2.f}, FQuaternion::Identity(), {0.25f, 0.25f, 0.25f}};
	const FTransform Floor{{3.f, 2.f, 2.f}, FQuaternion::FromAxisAngle({0, 1, 0}, std::numbers::pi_v<float> * 0.5f), {-0.75f, 0.5f, 5.f}};
	REQUIRE(Simulation.Start(MakeBodies(Cube, Floor)));
	for (int Frame = 0; Frame < 600; ++Frame)
	{
		REQUIRE(Simulation.Update(1.f / 120.f));
	}

	CHECK(Simulation.GetTransforms()[0].Transform.Translation.Y == doctest::Approx(2.75f).epsilon(0.02f));
	Simulation.Stop();
	CHECK(Simulation.GetTransforms()[0].Transform == Cube);

	FTransform InvalidFloor = Floor;
	InvalidFloor.Scale3D.Y = 0.f;
	CHECK_FALSE(Simulation.Start(MakeBodies(Cube, InvalidFloor)));
	CHECK_FALSE(Simulation.IsRunning());
	CHECK(Simulation.GetTransforms()[0].Transform == Cube);
	InvalidFloor.Scale3D.Y = std::numeric_limits<float>::quiet_NaN();
	CHECK_FALSE(Simulation.Start(MakeBodies(Cube, InvalidFloor)));
	CHECK_FALSE(Simulation.IsRunning());
	CHECK(Simulation.GetTransforms()[0].Transform == Cube);
	REQUIRE(Simulation.Start(MakeBodies(Cube, Floor)));
}

TEST_CASE("Preview simulation admits every dynamic and static body and restores their transforms")
{
	FPreviewSimulation Simulation;
	const std::array<FPreviewSimulationBody, 4> Bodies{{
	    {.ObjectIndex = 7, .Transform = {{-4.f, 4.f, 0.f}, FQuaternion::Identity(), {0.5f, 0.5f, 0.5f}}, .MotionType = EPhysicsMotionType::Dynamic},
	    {.ObjectIndex = 2, .Transform = {{-4.f, -0.25f, 0.f}, FQuaternion::Identity(), {2.f, 0.25f, 2.f}}},
	    {.ObjectIndex = 12, .Transform = {{4.f, 8.f, 0.f}, FQuaternion::FromAxisAngle({0.f, 1.f, 0.f}, 0.4f), {0.5f, 0.75f, 0.5f}}, .MotionType = EPhysicsMotionType::Dynamic},
	    {.ObjectIndex = 4, .Transform = {{4.f, 2.f, 0.f}, FQuaternion::Identity(), {2.f, 0.5f, 2.f}}},
	}};

	REQUIRE(Simulation.Start(Bodies));
	REQUIRE(Simulation.GetTransforms().size() == Bodies.size());

	for (int Frame = 0; Frame < 600; ++Frame)
	{
		REQUIRE(Simulation.Update(1.f / 120.f));
	}

	const auto Transforms = Simulation.GetTransforms();
	CHECK(Transforms[0].Transform.Translation.Y == doctest::Approx(0.5f).epsilon(0.02f));
	CHECK(Transforms[2].Transform.Translation.Y == doctest::Approx(3.25f).epsilon(0.02f));
	CHECK(Transforms[1].Transform == Bodies[1].Transform);
	CHECK(Transforms[3].Transform == Bodies[3].Transform);

	for (std::size_t Index = 0; Index < Bodies.size(); ++Index)
	{
		CHECK(Transforms[Index].ObjectIndex == Bodies[Index].ObjectIndex);
		CHECK(Transforms[Index].Transform.Scale3D == Bodies[Index].Transform.Scale3D);
	}

	Simulation.Stop();
	CHECK_FALSE(Simulation.IsRunning());

	for (std::size_t Index = 0; Index < Bodies.size(); ++Index)
	{
		CHECK(Simulation.GetTransforms()[Index].Transform == Bodies[Index].Transform);
	}

	REQUIRE(Simulation.Start(Bodies));
	CHECK(Simulation.GetTransforms()[0].Transform == Bodies[0].Transform);
	Simulation.Stop();
}

TEST_CASE("Preview simulation needs neither a static floor nor a dynamic body")
{
	FPreviewSimulation Simulation;
	const std::array<FPreviewSimulationBody, 2> Falling{{
	    {.ObjectIndex = 5, .Transform = {{-3.f, 8.f, 0.f}, FQuaternion::Identity(), FVector3::One()}, .MotionType = EPhysicsMotionType::Dynamic},
	    {.ObjectIndex = 1, .Transform = {{3.f, 12.f, 0.f}, FQuaternion::Identity(), FVector3::One()}, .MotionType = EPhysicsMotionType::Dynamic},
	}};

	REQUIRE(Simulation.Start(Falling));
	const auto Before = Simulation.GetTransforms()[0].Transform;
	REQUIRE(Simulation.Update(1.f / 120.f));
	CHECK(Simulation.GetTransforms()[0].Transform == Before);
	REQUIRE(Simulation.Update(1.f / 120.f));
	const float AfterStep = Simulation.GetTransforms()[0].Transform.Translation.Y;
	REQUIRE(Simulation.Update(1.f / 120.f));
	CHECK(Simulation.GetTransforms()[0].Transform.Translation.Y < AfterStep);

	for (int Frame = 0; Frame < 120; ++Frame)
	{
		REQUIRE(Simulation.Update(1.f / 120.f));
	}

	CHECK(Simulation.GetTransforms()[0].Transform.Translation.Y < 4.f);
	CHECK(Simulation.GetTransforms()[1].Transform.Translation.Y < 8.f);
	Simulation.Stop();
	CHECK(Simulation.GetTransforms()[0].Transform == Falling[0].Transform);
	CHECK(Simulation.GetTransforms()[1].Transform == Falling[1].Transform);

	const std::array<FPreviewSimulationBody, 1> Static{{{.ObjectIndex = 8, .Transform = DefaultFloor}}};
	REQUIRE(Simulation.Start(Static));
	REQUIRE(Simulation.Update(0.25f));
	REQUIRE(Simulation.GetTransforms().size() == 1);
	CHECK(Simulation.GetTransforms()[0].ObjectIndex == 8);
	CHECK(Simulation.GetTransforms()[0].Transform == DefaultFloor);
}

TEST_CASE("Preview simulation admission failures preserve prior authored outputs and allow reuse")
{
	FPreviewSimulation Simulation;
	CHECK_FALSE(Simulation.Start({}));
	CHECK(Simulation.GetTransforms().empty());
	const FTransform Original{{0.f, 4.f, 0.f}, FQuaternion::Identity(), FVector3::One()};
	const auto Valid = MakeBodies(Original, DefaultFloor);
	REQUIRE(Simulation.Start(Valid));
	Simulation.Stop();
	CHECK_FALSE(Simulation.Start({}));

	auto Invalid = Valid;
	SUBCASE("Duplicate object")
	{
		Invalid[1].ObjectIndex = Invalid[0].ObjectIndex;
	}

	SUBCASE("Invalid transform")
	{
		Invalid[1].Transform.Translation.X = std::numeric_limits<float>::infinity();
	}

	SUBCASE("Invalid bounds")
	{
		Invalid[1].Shape.HalfExtents.X = -1.f;
	}

	SUBCASE("Nonfinite bounds")
	{
		Invalid[1].Shape.Center.X = std::numeric_limits<float>::quiet_NaN();
	}

	SUBCASE("Unsupported motion type")
	{
		Invalid[1].MotionType = static_cast<EPhysicsMotionType>(255);
	}

	CHECK_FALSE(Simulation.Start(Invalid));
	CHECK_FALSE(Simulation.IsRunning());
	REQUIRE(Simulation.GetTransforms().size() == Valid.size());
	CHECK(Simulation.GetTransforms()[0].Transform == Original);
	CHECK(Simulation.GetTransforms()[1].Transform == DefaultFloor);
	Simulation.Stop();
	REQUIRE(Simulation.Update(0.f));
	REQUIRE(Simulation.Start(Valid));
	Simulation.Stop();
}

TEST_CASE("Preview simulation body capacity failure leaves no partially running world")
{
	FPreviewSimulation Simulation;
	std::vector<FPreviewSimulationBody> Bodies(1025);

	for (std::size_t Index = 0; Index < Bodies.size(); ++Index)
	{
		Bodies[Index].ObjectIndex = Index;
		Bodies[Index].Transform.Translation.X = static_cast<float>(Index) * 3.f;
	}

	const auto Result = Simulation.Start(Bodies);
	REQUIRE_FALSE(Result);
	CHECK(Result.error().Message == "Physics world body capacity reached");
	CHECK_FALSE(Simulation.IsRunning());
	CHECK(Simulation.GetTransforms().empty());
	Bodies.resize(2);
	REQUIRE(Simulation.Start(Bodies));
	Simulation.Stop();
}

TEST_CASE("Preview simulation forwards authored body properties and restores transforms")
{
	FPreviewSimulation Simulation;
	const std::array<FPreviewSimulationBody, 2> Bodies{{
	    {.ObjectIndex = 3, .Transform = {{-3.f, 8.f, 0.f}, FQuaternion::Identity(), FVector3::One()}, .MotionType = EPhysicsMotionType::Dynamic, .Properties = {.GravityScale = 0.f}},
	    {.ObjectIndex = 7, .Transform = {{3.f, 8.f, 0.f}, FQuaternion::Identity(), FVector3::One()}, .MotionType = EPhysicsMotionType::Dynamic, .Properties = {.MassKg = 2.f, .GravityScale = 1.f}},
	}};

	REQUIRE(Simulation.Start(Bodies));

	for (int Frame = 0; Frame < 120; ++Frame)
	{
		REQUIRE(Simulation.Update(1.f / 120.f));
	}

	CHECK(Simulation.GetTransforms()[0].Transform == Bodies[0].Transform);
	CHECK(Simulation.GetTransforms()[1].Transform.Translation.Y < 4.f);
	Simulation.Stop();
	CHECK(Simulation.GetTransforms()[0].Transform == Bodies[0].Transform);
	CHECK(Simulation.GetTransforms()[1].Transform == Bodies[1].Transform);

	auto Invalid = Bodies;
	Invalid[1].Properties.Friction = std::numeric_limits<float>::quiet_NaN();
	CHECK_FALSE(Simulation.Start(Invalid));
	CHECK_FALSE(Simulation.IsRunning());
	CHECK(Simulation.GetTransforms()[0].Transform == Bodies[0].Transform);
	CHECK(Simulation.GetTransforms()[1].Transform == Bodies[1].Transform);
	REQUIRE(Simulation.Start(Bodies));
}
}

TEST_CASE("Preview simulation collides with offset mesh bounds and restores the object origin")
{
	Herta::FPreviewSimulation Simulation;
	const Herta::FTransform Cube{{0.f, 4.f, 0.f}, Herta::FQuaternion::Identity(), {2.f, 2.f, 2.f}};
	const Herta::FTransform Floor{{0.f, -0.25f, 0.f}, Herta::FQuaternion::Identity(), {10.f, 0.25f, 10.f}};
	// A mesh resting on its origin: its bounds sit entirely above Y=0 in model space.
	const Herta::FPreviewBodyShape Shape{.Center = {0.f, 0.5f, 0.f}, .HalfExtents = {0.5f, 0.5f, 0.5f}};
	REQUIRE(Simulation.Start(Herta::MakeBodies(Cube, Floor, Shape)));
	CHECK(Simulation.GetTransforms()[0].Transform.Translation.Y == doctest::Approx(4.f));
	for (int Step = 0; Step < 600; ++Step)
	{
		REQUIRE(Simulation.Update(1.f / 60.f));
	}

	// The scaled 2 m tall body rests on the floor top at Y=0, so the object origin settles at Y=0 too.
	CHECK(Simulation.GetTransforms()[0].Transform.Translation.Y == doctest::Approx(0.f).epsilon(0.02));
	Simulation.Stop();
	CHECK(Simulation.GetTransforms()[0].Transform.Translation.Y == doctest::Approx(4.f));
}
