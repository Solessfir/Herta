#include "Herta/Physics/PhysicsWorld.h"

#include <doctest/doctest.h>

#include <cmath>
#include <limits>

namespace Herta
{
TEST_CASE("Dynamic box settles on a static floor")
{
	auto FirstWorld = FPhysicsWorld::Create();
	REQUIRE(FirstWorld.has_value());
	auto World = FPhysicsWorld::Create();
	REQUIRE(World.has_value());
	FirstWorld->reset();

	FPhysicsBoxBodySettings Floor;
	Floor.HalfExtents = {10.f, 0.25f, 10.f};
	Floor.Position = {0.f, -0.25f, 0.f};
	const auto FloorId = (*World)->CreateBoxBody(Floor);
	REQUIRE(FloorId.has_value());

	FPhysicsBoxBodySettings Cube;
	Cube.HalfExtents = {0.5f, 0.5f, 0.5f};
	Cube.Position = {0.f, 3.f, 0.f};
	Cube.MotionType = EPhysicsMotionType::Dynamic;
	const auto CubeId = (*World)->CreateBoxBody(Cube);
	REQUIRE(CubeId.has_value());

	for (int StepIndex = 0; StepIndex < 240; ++StepIndex)
	{
		REQUIRE((*World)->Step(1.f / 60.f).has_value());
	}

	const auto CubeTransform = (*World)->GetBodyTransform(*CubeId);
	REQUIRE(CubeTransform.has_value());
	CHECK(CubeTransform->Position.Y >= 0.475f);
	CHECK(CubeTransform->Position.Y <= 0.53f);
	CHECK(std::abs(CubeTransform->Position.X) < 0.01f);
	CHECK(std::abs(CubeTransform->Position.Z) < 0.01f);
	CHECK(CubeTransform->Rotation.IsNearlyEqual(FQuaternion::Identity(), 0.01f));
	CHECK((*World)->GetBodyTransform(*FloorId)->Position == Floor.Position);
}

TEST_CASE("Physics rejects invalid input")
{
	auto World = FPhysicsWorld::Create();
	REQUIRE(World.has_value());

	FPhysicsBoxBodySettings Box;
	Box.HalfExtents.X = 0.f;
	CHECK_FALSE((*World)->CreateBoxBody(Box).has_value());
	Box.HalfExtents.X = std::numeric_limits<float>::quiet_NaN();
	CHECK_FALSE((*World)->CreateBoxBody(Box).has_value());
	Box.HalfExtents.X = 0.5f;
	Box.Rotation = {0.f, 0.f, 0.f, 0.f};
	CHECK_FALSE((*World)->CreateBoxBody(Box).has_value());
	Box.Rotation = {std::numeric_limits<float>::max(), 0.f, 0.f, 1.f};
	CHECK_FALSE((*World)->CreateBoxBody(Box).has_value());
	CHECK_FALSE((*World)->Step(0.f).has_value());
	CHECK_FALSE((*World)->Step(std::numeric_limits<float>::infinity()).has_value());
	CHECK_FALSE((*World)->GetBodyTransform({}).has_value());
}

TEST_CASE("Physics simulates multiple dynamic bodies against each other and multiple statics")
{
	auto World = FPhysicsWorld::Create();
	REQUIRE(World);
	const auto LowFloor = (*World)->CreateBoxBody({.HalfExtents = {2.f, 0.25f, 2.f}, .Position = {-4.f, -0.25f, 0.f}});
	const auto HighFloor = (*World)->CreateBoxBody({.HalfExtents = {2.f, 0.5f, 2.f}, .Position = {4.f, 2.f, 0.f}});
	const auto Bottom = (*World)->CreateBoxBody({.HalfExtents = {0.5f, 0.5f, 0.5f}, .Position = {-4.f, 3.f, 0.f}, .MotionType = EPhysicsMotionType::Dynamic});
	const auto Top = (*World)->CreateBoxBody({.HalfExtents = {0.5f, 0.5f, 0.5f}, .Position = {-4.f, 5.f, 0.f}, .MotionType = EPhysicsMotionType::Dynamic});
	const auto Separate = (*World)->CreateBoxBody({.HalfExtents = {0.5f, 0.5f, 0.5f}, .Position = {4.f, 6.f, 0.f}, .MotionType = EPhysicsMotionType::Dynamic});
	REQUIRE(LowFloor);
	REQUIRE(HighFloor);
	REQUIRE(Bottom);
	REQUIRE(Top);
	REQUIRE(Separate);

	for (int StepIndex = 0; StepIndex < 600; ++StepIndex)
	{
		REQUIRE((*World)->Step(1.f / 60.f));
	}

	const auto BottomTransform = (*World)->GetBodyTransform(*Bottom);
	const auto TopTransform = (*World)->GetBodyTransform(*Top);
	const auto SeparateTransform = (*World)->GetBodyTransform(*Separate);
	REQUIRE(BottomTransform);
	REQUIRE(TopTransform);
	REQUIRE(SeparateTransform);
	CHECK(BottomTransform->Position.Y == doctest::Approx(0.5f).epsilon(0.04f));
	CHECK(TopTransform->Position.Y == doctest::Approx(1.5f).epsilon(0.04f));
	CHECK(SeparateTransform->Position.Y == doctest::Approx(3.f).epsilon(0.02f));
	CHECK((*World)->GetBodyTransform(*LowFloor)->Position == FVector3{-4.f, -0.25f, 0.f});
	CHECK((*World)->GetBodyTransform(*HighFloor)->Position == FVector3{4.f, 2.f, 0.f});
}
}
