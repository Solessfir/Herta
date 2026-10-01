#include "Herta/Physics/PhysicsWorld.h"

#include <cmath>
#include <doctest/doctest.h>
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
	Floor.HalfExtents = {10.0f, 0.25f, 10.0f};
	Floor.Position = {0.0f, -0.25f, 0.0f};
	const auto FloorId = (*World)->CreateBoxBody(Floor);
	REQUIRE(FloorId.has_value());

	FPhysicsBoxBodySettings Cube;
	Cube.HalfExtents = {0.5f, 0.5f, 0.5f};
	Cube.Position = {0.0f, 3.0f, 0.0f};
	Cube.MotionType = EPhysicsMotionType::Dynamic;
	const auto CubeId = (*World)->CreateBoxBody(Cube);
	REQUIRE(CubeId.has_value());

	for (int StepIndex = 0; StepIndex < 240; ++StepIndex)
	{
		REQUIRE((*World)->Step(1.0f / 60.0f).has_value());
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
	Box.HalfExtents.X = 0.0f;
	CHECK_FALSE((*World)->CreateBoxBody(Box).has_value());
	Box.HalfExtents.X = std::numeric_limits<float>::quiet_NaN();
	CHECK_FALSE((*World)->CreateBoxBody(Box).has_value());
	Box.HalfExtents.X = 0.5f;
	Box.Rotation = {0.0f, 0.0f, 0.0f, 0.0f};
	CHECK_FALSE((*World)->CreateBoxBody(Box).has_value());
	Box.Rotation = {std::numeric_limits<float>::max(), 0.0f, 0.0f, 1.0f};
	CHECK_FALSE((*World)->CreateBoxBody(Box).has_value());
	CHECK_FALSE((*World)->Step(0.0f).has_value());
	CHECK_FALSE((*World)->Step(std::numeric_limits<float>::infinity()).has_value());
	CHECK_FALSE((*World)->GetBodyTransform({}).has_value());
}
}
