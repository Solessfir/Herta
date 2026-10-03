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
}
