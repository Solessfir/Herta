#include "Herta/Core/Log.h"
#include "Herta/Physics/PhysicsWorld.h"
#include "Herta/Tasks/TaskSystem.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <limits>
#include <numbers>
#include <tuple>
#include <vector>

namespace Herta
{
TEST_CASE("Physics capacity validation and exhaustion are explicit")
{
	FPhysicsWorldSettings Invalid;
	Invalid.MaxBodies = 0;
	CHECK_FALSE(FPhysicsWorld::Create(Invalid));
	Invalid = {};
	Invalid.MaxBodyPairs = 0;
	CHECK_FALSE(FPhysicsWorld::Create(Invalid));
	Invalid = {};
	Invalid.MaxContactConstraints = std::numeric_limits<std::uint32_t>::max();
	CHECK_FALSE(FPhysicsWorld::Create(Invalid));
	Invalid = {};
	Invalid.TempMemoryBytes = 1;
	const auto InsufficientMemory = FPhysicsWorld::Create(Invalid);
	REQUIRE_FALSE(InsufficientMemory);
	CHECK(InsufficientMemory.error().Message.find("temporary-memory capacity") != std::string::npos);

	auto World = FPhysicsWorld::Create({.MaxBodies = 1, .MaxBodyPairs = 4, .MaxContactConstraints = 4, .TempMemoryBytes = 1024 * 1024});
	REQUIRE(World);
	const auto Body = (*World)->CreateBody({});
	REQUIRE(Body);
	const auto Overflow = (*World)->CreateBody({});
	REQUIRE_FALSE(Overflow);
	CHECK(Overflow.error().Message.find("MaxBodies=1") != std::string::npos);
	CHECK((*World)->GetBodyTransform(*Body));
	CHECK_FALSE((*World)->GetBodyTransform({Body->Value ^ (1u << 23)}));
	CHECK_FALSE((*World)->GetBodyTransform({0x80000000u}));
	REQUIRE((*World)->Step(1.f / 60.f));
}

TEST_CASE("Physics reports contact overflow and refuses to continue a truncated step")
{
	auto World = FPhysicsWorld::Create({.MaxBodies = 32, .MaxBodyPairs = 1024, .MaxContactConstraints = 4, .TempMemoryBytes = 1024 * 1024});
	REQUIRE(World);

	for (int Index = 0; Index < 32; ++Index)
	{
		REQUIRE((*World)->CreateBody({.Position = {0.f, 4.f, 0.f}, .MotionType = EPhysicsMotionType::Dynamic}));
	}

	const auto Result = (*World)->Step(1.f / 60.f);
	REQUIRE_FALSE(Result);
	CHECK(Result.error().Message.find("MaxContactConstraints=4") != std::string::npos);
	const auto Repeated = (*World)->Step(1.f / 60.f);
	REQUIRE_FALSE(Repeated);
	CHECK(Repeated.error().Message == Result.error().Message);
}

TEST_CASE("Physics reports body-pair cache overflow independently of contacts")
{
	auto World = FPhysicsWorld::Create({.MaxBodies = 64, .MaxBodyPairs = 4, .MaxContactConstraints = 4, .TempMemoryBytes = 1024 * 1024});
	REQUIRE(World);
	const FQuaternion Rotation = FQuaternion::FromAxisAngle({0.f, 1.f, 0.f}, std::numbers::pi_v<float> * 0.25f);

	for (int Index = 0; Index < 64; ++Index)
	{
		// Parallel thin boxes have overlapping broad-phase bounds but no contact manifolds.
		const float Offset = static_cast<float>(Index) * 0.25f;
		REQUIRE((*World)->CreateBody({.HalfExtents = {10.f, 0.05f, 0.05f}, .Position = {Offset, 4.f, Offset}, .Rotation = Rotation, .MotionType = EPhysicsMotionType::Dynamic, .Properties = {.GravityScale = 0.f}}));
	}

	const auto Result = (*World)->Step(1.f / 60.f);
	REQUIRE_FALSE(Result);
	CHECK(Result.error().Message.find("MaxBodyPairs=4") != std::string::npos);
	CHECK(Result.error().Message.find("MaxContactConstraints") == std::string::npos);
}

TEST_CASE("Dynamic box settles on a static floor")
{
	auto FirstWorld = FPhysicsWorld::Create();
	REQUIRE(FirstWorld.has_value());
	auto World = FPhysicsWorld::Create();
	REQUIRE(World.has_value());
	FirstWorld->reset();

	FPhysicsBodySettings Floor;
	Floor.HalfExtents = {10.f, 0.25f, 10.f};
	Floor.Position = {0.f, -0.25f, 0.f};
	const auto FloorId = (*World)->CreateBody(Floor);
	REQUIRE(FloorId.has_value());

	FPhysicsBodySettings Cube;
	Cube.HalfExtents = {0.5f, 0.5f, 0.5f};
	Cube.Position = {0.f, 3.f, 0.f};
	Cube.MotionType = EPhysicsMotionType::Dynamic;
	const auto CubeId = (*World)->CreateBody(Cube);
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

	FPhysicsBodySettings Box;
	Box.HalfExtents.X = 0.f;
	CHECK_FALSE((*World)->CreateBody(Box).has_value());
	Box.HalfExtents.X = std::numeric_limits<float>::quiet_NaN();
	CHECK_FALSE((*World)->CreateBody(Box).has_value());
	Box.HalfExtents.X = 0.5f;
	Box.Rotation = {0.f, 0.f, 0.f, 0.f};
	CHECK_FALSE((*World)->CreateBody(Box).has_value());
	Box.Rotation = {std::numeric_limits<float>::max(), 0.f, 0.f, 1.f};
	CHECK_FALSE((*World)->CreateBody(Box).has_value());
	CHECK_FALSE((*World)->Step(0.f).has_value());
	CHECK_FALSE((*World)->Step(std::numeric_limits<float>::infinity()).has_value());
	CHECK_FALSE((*World)->GetBodyTransform({}).has_value());
}

TEST_CASE("Physics validates every body property for static and dynamic bodies")
{
	auto World = FPhysicsWorld::Create();
	REQUIRE(World);

	struct FPropertyRange
	{
		float FPhysicsBodyProperties::*Member;
		float Minimum;
		float Maximum;
	};

	const std::array Ranges{
	    FPropertyRange{.Member = &FPhysicsBodyProperties::MassKg, .Minimum = 0.001f, .Maximum = 1000000.f},
	    FPropertyRange{.Member = &FPhysicsBodyProperties::Friction, .Minimum = 0.f, .Maximum = 1.f},
	    FPropertyRange{.Member = &FPhysicsBodyProperties::Restitution, .Minimum = 0.f, .Maximum = 1.f},
	    FPropertyRange{.Member = &FPhysicsBodyProperties::LinearDamping, .Minimum = 0.f, .Maximum = 1.f},
	    FPropertyRange{.Member = &FPhysicsBodyProperties::AngularDamping, .Minimum = 0.f, .Maximum = 1.f},
	    FPropertyRange{.Member = &FPhysicsBodyProperties::GravityScale, .Minimum = 0.f, .Maximum = 10.f},
	};

	for (const EPhysicsMotionType Type : {EPhysicsMotionType::Static, EPhysicsMotionType::Dynamic})
	{
		for (const FPropertyRange& Range : Ranges)
		{
			for (const float Value : {Range.Minimum - 1.f, Range.Maximum + 1.f, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()})
			{
				FPhysicsBodySettings Box{.MotionType = Type};
				Box.Properties.*Range.Member = Value;
				CHECK_FALSE((*World)->CreateBody(Box));
			}

			for (const float Value : {Range.Minimum, Range.Maximum})
			{
				FPhysicsBodySettings Box{.MotionType = Type};
				Box.Properties.*Range.Member = Value;
				CHECK((*World)->CreateBody(Box));
			}
		}
	}
}

TEST_CASE("Physics applies gravity scale and linear damping independently per body")
{
	auto World = FPhysicsWorld::Create();
	REQUIRE(World);
	const auto Floating = (*World)->CreateBody({.Position = {-4.f, 8.f, 0.f}, .MotionType = EPhysicsMotionType::Dynamic, .Properties = {.GravityScale = 0.f}});
	const auto Falling = (*World)->CreateBody({.Position = {0.f, 8.f, 0.f}, .MotionType = EPhysicsMotionType::Dynamic, .Properties = {.LinearDamping = 0.f}});
	const auto Damped = (*World)->CreateBody({.Position = {4.f, 8.f, 0.f}, .MotionType = EPhysicsMotionType::Dynamic, .Properties = {.LinearDamping = 1.f}});
	REQUIRE(Floating);
	REQUIRE(Falling);
	REQUIRE(Damped);

	for (int StepIndex = 0; StepIndex < 60; ++StepIndex)
	{
		REQUIRE((*World)->Step(1.f / 60.f));
	}

	const auto FloatingTransform = (*World)->GetBodyTransform(*Floating);
	const auto FallingTransform = (*World)->GetBodyTransform(*Falling);
	const auto DampedTransform = (*World)->GetBodyTransform(*Damped);
	REQUIRE(FloatingTransform);
	REQUIRE(FallingTransform);
	REQUIRE(DampedTransform);
	CHECK(FloatingTransform->Position.Y == 8.f);
	CHECK(FallingTransform->Position.Y < 4.f);
	CHECK(DampedTransform->Position.Y > FallingTransform->Position.Y + 0.5f);
}

TEST_CASE("Physics restitution changes the rebound from a static body")
{
	auto World = FPhysicsWorld::Create();
	REQUIRE(World);
	REQUIRE((*World)->CreateBody({.HalfExtents = {10.f, 0.25f, 10.f}, .Position = {0.f, -0.25f, 0.f}}));
	const auto Inelastic = (*World)->CreateBody({.HalfExtents = {0.5f, 0.5f, 0.5f}, .Position = {-3.f, 3.f, 0.f}, .MotionType = EPhysicsMotionType::Dynamic, .Properties = {.Restitution = 0.f, .LinearDamping = 0.f}});
	const auto Elastic = (*World)->CreateBody({.HalfExtents = {0.5f, 0.5f, 0.5f}, .Position = {3.f, 3.f, 0.f}, .MotionType = EPhysicsMotionType::Dynamic, .Properties = {.Restitution = 1.f, .LinearDamping = 0.f}});
	REQUIRE(Inelastic);
	REQUIRE(Elastic);

	for (int StepIndex = 0; StepIndex < 60; ++StepIndex)
	{
		REQUIRE((*World)->Step(1.f / 60.f));
	}

	const auto InelasticTransform = (*World)->GetBodyTransform(*Inelastic);
	const auto ElasticTransform = (*World)->GetBodyTransform(*Elastic);
	REQUIRE(InelasticTransform);
	REQUIRE(ElasticTransform);
	CHECK(InelasticTransform->Position.Y == doctest::Approx(0.5f).epsilon(0.04f));
	CHECK(ElasticTransform->Position.Y > 1.5f);
}

TEST_CASE("Physics friction opposes sliding on an inclined static body")
{
	const auto Simulate = [](const float Friction)
	{
		auto World = FPhysicsWorld::Create();
		REQUIRE(World);
		const FQuaternion Rotation = FQuaternion::FromAxisAngle({0.f, 0.f, 1.f}, 0.3f);
		REQUIRE((*World)->CreateBody({.HalfExtents = {20.f, 0.25f, 2.f}, .Rotation = Rotation, .Properties = {.Friction = Friction}}));
		const auto Box = (*World)->CreateBody({.HalfExtents = {0.5f, 0.5f, 0.5f}, .Position = {0.f, 2.f, 0.f}, .Rotation = Rotation, .MotionType = EPhysicsMotionType::Dynamic, .Properties = {.Friction = Friction}});
		REQUIRE(Box);

		for (int StepIndex = 0; StepIndex < 180; ++StepIndex)
		{
			REQUIRE((*World)->Step(1.f / 60.f));
		}

		const auto Transform = (*World)->GetBodyTransform(*Box);
		REQUIRE(Transform);
		return std::abs(Transform->Position.X);
	};

	const float SlidingDistance = Simulate(0.f);
	const float GrippingDistance = Simulate(1.f);
	CHECK(SlidingDistance > GrippingDistance + 1.f);
}

TEST_CASE("Physics spheres roll down a ramp that holds a box, and capsules rest upright")
{
	auto World = FPhysicsWorld::Create();
	REQUIRE(World);
	// A 20 degree ramp descending towards -X; friction 0.8 exceeds tan(20 degrees), so a box sticks while a sphere rolls.
	const FQuaternion Tilt = FQuaternion::FromAxisAngle({0.f, 0.f, 1.f}, 20.f * std::numbers::pi_v<float> / 180.f);
	REQUIRE((*World)->CreateBody({.HalfExtents = {6.f, 0.25f, 3.f}, .Rotation = Tilt, .Properties = {.Friction = 0.8f}}));
	const FVector3 Up = Tilt.RotateVector({0.f, 1.f, 0.f});
	const auto Box = (*World)->CreateBody({.HalfExtents = {0.25f, 0.25f, 0.25f}, .Position = Up * 0.51f + FVector3{0.f, 0.f, -1.f}, .Rotation = Tilt, .MotionType = EPhysicsMotionType::Dynamic, .Properties = {.Friction = 0.8f}});
	const auto Sphere = (*World)->CreateBody({.Shape = EPhysicsShape::Sphere, .Radius = 0.25f, .Position = Up * 0.51f + FVector3{0.f, 0.f, 1.f}, .MotionType = EPhysicsMotionType::Dynamic, .Properties = {.Friction = 0.8f}});
	const auto Capsule = (*World)->CreateBody({.Shape = EPhysicsShape::Capsule, .Radius = 0.25f, .HalfHeight = 0.5f, .Position = {20.f, 2.f, 0.f}, .MotionType = EPhysicsMotionType::Dynamic});
	REQUIRE((*World)->CreateBody({.HalfExtents = {2.f, 0.5f, 2.f}, .Position = {20.f, -0.5f, 0.f}}));
	REQUIRE(Box);
	REQUIRE(Sphere);
	REQUIRE(Capsule);
	for (int Step = 0; Step < 60; ++Step)
	{
		REQUIRE((*World)->Step(1.f / 60.f));
	}

	const auto BoxTransform = (*World)->GetBodyTransform(*Box);
	const auto SphereTransform = (*World)->GetBodyTransform(*Sphere);
	const auto CapsuleTransform = (*World)->GetBodyTransform(*Capsule);
	REQUIRE(BoxTransform);
	REQUIRE(SphereTransform);
	REQUIRE(CapsuleTransform);
	const float StartX = (Up * 0.51f).X;
	CHECK(std::abs(BoxTransform->Position.X - StartX) < 0.05f);
	CHECK(SphereTransform->Position.X < StartX - 0.5f);
	// Upright capsules rest on their lower cap: half height plus radius above the floor.
	CHECK(CapsuleTransform->Position.Y == doctest::Approx(0.75f).epsilon(0.02));

	CHECK_FALSE((*World)->CreateBody({.Shape = EPhysicsShape::Sphere, .Radius = 0.f}));
	CHECK_FALSE((*World)->CreateBody({.Shape = EPhysicsShape::Capsule, .Radius = 0.5f, .HalfHeight = -1.f}));
	CHECK_FALSE((*World)->CreateBody({.Shape = static_cast<EPhysicsShape>(7)}));
	CHECK((*World)->CreateBody({.Shape = EPhysicsShape::Capsule, .Radius = 0.5f, .HalfHeight = 0.f}));
}

TEST_CASE("Kinematic bodies carry resting bodies and sensors report enter and exit")
{
	auto World = FPhysicsWorld::Create();
	REQUIRE(World);
	const auto Platform = (*World)->CreateBody({.HalfExtents = {1.f, 0.1f, 1.f}, .Position = {0.f, 1.f, 0.f}, .MotionType = EPhysicsMotionType::Kinematic, .Properties = {.Friction = 1.f}});
	const auto Box = (*World)->CreateBody({.HalfExtents = {0.2f, 0.2f, 0.2f}, .Position = {0.f, 1.31f, 0.f}, .MotionType = EPhysicsMotionType::Dynamic, .Properties = {.Friction = 1.f}});
	const auto Sensor = (*World)->CreateBody({.HalfExtents = {0.5f, 1.f, 0.5f}, .Position = {0.f, 1.5f, 2.f}, .MotionType = EPhysicsMotionType::Kinematic, .bSensor = true});
	REQUIRE(Platform);
	REQUIRE(Box);
	REQUIRE(Sensor);
	CHECK_FALSE((*World)->MoveKinematicBody(*Box, {.Position = FVector3::Zero(), .Rotation = FQuaternion::Identity()}, 1.f / 60.f));

	// Ease 4 m along +Z over two seconds; peak acceleration stays below what friction can transmit.
	std::vector<FPhysicsSensorEvent> Events;
	for (int Step = 1; Step <= 180; ++Step)
	{
		const float Time = std::min(static_cast<float>(Step) / 60.f, 2.f);
		const FVector3 Target{0.f, 1.f, 2.f * (1.f - std::cos(std::numbers::pi_v<float> * Time / 2.f))};
		REQUIRE((*World)->MoveKinematicBody(*Platform, {.Position = Target, .Rotation = FQuaternion::Identity()}, 1.f / 60.f));
		REQUIRE((*World)->Step(1.f / 60.f));

		// Sensors also report the kinematic platform passing through; this test follows the box.
		std::ranges::copy_if((*World)->GetSensorEvents(), std::back_inserter(Events), [&](const FPhysicsSensorEvent& Event)
		{
			return Event.Body == *Box;
		});
	}

	const auto Transform = (*World)->GetBodyTransform(*Box);
	REQUIRE(Transform);
	CHECK(Transform->Position.Z == doctest::Approx(4.f).epsilon(0.03));
	CHECK(Transform->Position.Y > 1.2f);
	REQUIRE(Events.size() == 2);
	CHECK(Events[0].Sensor == *Sensor);
	CHECK(Events[0].Body == *Box);
	CHECK(Events[0].bEntered);
	CHECK(Events[1].Body == *Box);
	CHECK_FALSE(Events[1].bEntered);
}

TEST_CASE("Physics authored mass changes dynamic collision response")
{
	const auto Simulate = [](const float MassKg)
	{
		auto World = FPhysicsWorld::Create();
		REQUIRE(World);
		const auto Target = (*World)->CreateBody({.HalfExtents = {0.5f, 0.5f, 0.5f}, .Position = {0.f, 2.f, 0.f}, .MotionType = EPhysicsMotionType::Dynamic, .Properties = {.MassKg = MassKg, .LinearDamping = 0.f, .GravityScale = 0.f}});
		REQUIRE(Target);
		REQUIRE((*World)->CreateBody({.HalfExtents = {0.5f, 0.5f, 0.5f}, .Position = {0.f, 5.f, 0.f}, .MotionType = EPhysicsMotionType::Dynamic, .Properties = {.LinearDamping = 0.f}}));

		for (int StepIndex = 0; StepIndex < 120; ++StepIndex)
		{
			REQUIRE((*World)->Step(1.f / 60.f));
		}

		const auto Transform = (*World)->GetBodyTransform(*Target);
		REQUIRE(Transform);
		return Transform->Position.Y;
	};

	const float LightPosition = Simulate(0.1f);
	const float HeavyPosition = Simulate(100.f);
	CHECK(LightPosition < 0.5f);
	CHECK(HeavyPosition > 1.5f);
}

TEST_CASE("Physics simulates multiple dynamic bodies against each other and multiple statics")
{
	auto World = FPhysicsWorld::Create();
	REQUIRE(World);
	const auto LowFloor = (*World)->CreateBody({.HalfExtents = {2.f, 0.25f, 2.f}, .Position = {-4.f, -0.25f, 0.f}});
	const auto HighFloor = (*World)->CreateBody({.HalfExtents = {2.f, 0.5f, 2.f}, .Position = {4.f, 2.f, 0.f}});
	const auto Bottom = (*World)->CreateBody({.HalfExtents = {0.5f, 0.5f, 0.5f}, .Position = {-4.f, 3.f, 0.f}, .MotionType = EPhysicsMotionType::Dynamic});
	const auto Top = (*World)->CreateBody({.HalfExtents = {0.5f, 0.5f, 0.5f}, .Position = {-4.f, 5.f, 0.f}, .MotionType = EPhysicsMotionType::Dynamic});
	const auto Separate = (*World)->CreateBody({.HalfExtents = {0.5f, 0.5f, 0.5f}, .Position = {4.f, 6.f, 0.f}, .MotionType = EPhysicsMotionType::Dynamic});
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

TEST_CASE("Pinned soft body ropes hang from their anchor and keep their length")
{
	auto World = FPhysicsWorld::Create();
	REQUIRE(World);
	constexpr std::uint32_t Segments = 10;
	constexpr float Length = 2.f;
	std::vector<FVector3> Vertices;
	std::vector<std::array<std::uint32_t, 2>> Stretch;
	std::vector<std::array<std::uint32_t, 2>> Bend;
	for (std::uint32_t Index = 0; Index <= Segments; ++Index)
	{
		Vertices.push_back({static_cast<float>(Index) * Length / Segments, 5.f, 0.f});
		if (Index > 0)
		{
			Stretch.push_back({Index - 1, Index});
		}

		if (Index > 1)
		{
			Bend.push_back({Index - 2, Index});
		}
	}

	const std::array<std::uint32_t, 1> Pinned{0};
	const auto Rope = (*World)->CreateSoftBody({.Vertices = Vertices, .PinnedVertices = Pinned, .StretchEdges = Stretch, .BendEdges = Bend, .MassKg = 0.5f, .BendCompliance = 0.1f, .LinearDamping = 1.f});
	REQUIRE(Rope);
	// Released horizontally, the rope swings like a pendulum; heavy damping settles it within the test window.
	for (int Step = 0; Step < 600; ++Step)
	{
		REQUIRE((*World)->Step(1.f / 60.f));
	}

	std::vector<FVector3> Positions;
	REQUIRE((*World)->GetSoftBodyVertices(*Rope, Positions));
	REQUIRE(Positions.size() == Vertices.size());
	CHECK(Positions.front().X == doctest::Approx(0.f).epsilon(1e-3));
	CHECK(Positions.front().Y == doctest::Approx(5.f).epsilon(1e-3));
	CHECK(Positions.back().Y < 5.f - Length * 0.8f);
	float Total = 0.f;
	for (std::size_t Index = 1; Index < Positions.size(); ++Index)
	{
		Total += (Positions[Index] - Positions[Index - 1]).Length();
	}

	CHECK(Total == doctest::Approx(Length).epsilon(0.05));
}

TEST_CASE("Pressurized soft body shells land on static bodies without passing through")
{
	auto World = FPhysicsWorld::Create();
	REQUIRE(World);
	REQUIRE((*World)->CreateBody({.HalfExtents = {5.f, 0.5f, 5.f}, .Position = {0.f, -0.5f, 0.f}}));
	// Octahedron shell, wound outwards.
	const std::array<FVector3, 6> Vertices{{{0.5f, 2.f, 0.f}, {-0.5f, 2.f, 0.f}, {0.f, 2.5f, 0.f}, {0.f, 1.5f, 0.f}, {0.f, 2.f, 0.5f}, {0.f, 2.f, -0.5f}}};
	const std::array<std::array<std::uint32_t, 3>, 8> Faces{{{0, 2, 4}, {4, 2, 1}, {1, 2, 5}, {5, 2, 0}, {4, 3, 0}, {1, 3, 4}, {5, 3, 1}, {0, 3, 5}}};
	const auto Ball = (*World)->CreateSoftBody({.Vertices = Vertices, .Faces = Faces, .MassKg = 1.f, .VertexRadius = 0.02f, .Pressure = 2000.f});
	REQUIRE(Ball);
	for (int Step = 0; Step < 180; ++Step)
	{
		REQUIRE((*World)->Step(1.f / 60.f));
	}

	std::vector<FVector3> Positions;
	REQUIRE((*World)->GetSoftBodyVertices(*Ball, Positions));
	float Lowest = Positions.front().Y;
	float Highest = Lowest;
	for (const FVector3& Position : Positions)
	{
		Lowest = std::min(Lowest, Position.Y);
		Highest = std::max(Highest, Position.Y);
	}

	CHECK(Lowest > -0.05f);
	CHECK(Highest < 1.5f);
	CHECK(Highest - Lowest > 0.3f);
}

TEST_CASE("Rope chains carry a constrained heavy box two ways and keep their length")
{
	auto World = FPhysicsWorld::Create();
	REQUIRE(World);
	constexpr float Length = 2.f;
	const FVector3 Pin{0.f, 5.f, 0.f};
	const FVector3 End = Pin + FVector3{Length, 0.f, 0.f};

	// Released horizontally, a 5 kg box swings down on a 0.2 kg rope, a mass ratio that stretches a naive chain.
	const auto Rope = (*World)->CreateRope({.Start = Pin, .End = End, .Segments = 10, .MassKg = 0.2f});
	REQUIRE(Rope);
	const auto Box = (*World)->CreateBody({.HalfExtents = {0.2f, 0.2f, 0.2f}, .Position = End - FVector3{0.f, 0.2f, 0.f}, .MotionType = EPhysicsMotionType::Dynamic, .Properties = {.MassKg = 5.f, .LinearDamping = 0.5f, .AngularDamping = 0.5f}});
	REQUIRE(Box);
	REQUIRE((*World)->CreatePointConstraint({.Body = *Box, .Other = Rope->Last, .Point = End}));
	for (int Step = 0; Step < 600; ++Step)
	{
		REQUIRE((*World)->Step(1.f / 60.f));
	}

	const auto Transform = (*World)->GetBodyTransform(*Box);
	REQUIRE(Transform);
	const FVector3 Top = Transform->Position + Transform->Rotation.RotateVector({0.f, 0.2f, 0.f});
	CHECK((Top - Pin).Length() == doctest::Approx(Length).epsilon(0.02));
	CHECK(Top.Y < Pin.Y - Length * 0.9f);

	std::vector<FVector3> Points;
	REQUIRE((*World)->GetRopePoints(*Rope, Points));
	REQUIRE(Points.size() == 11);
	CHECK((Points.front() - Pin).Length() < 0.01f);
	CHECK((Points.back() - Top).Length() < 0.02f);
	// The load pulls the rope taut along the line to the pin; a rope that only followed the box would sag.
	const FVector3 Axis = (Top - Pin).Normalized();
	for (const FVector3& Point : Points)
	{
		const FVector3 Offset = Point - Pin;
		CHECK((Offset - Axis * Offset.Dot(Axis)).Length() < 0.05f);
	}
}

TEST_CASE("Point constraints hold bodies to the world or each other without self-collision")
{
	auto World = FPhysicsWorld::Create();
	REQUIRE(World);
	const FVector3 Hook{0.f, 3.f, 0.f};
	const auto Hung = (*World)->CreateBody({.HalfExtents = {0.2f, 0.2f, 0.2f}, .Position = Hook - FVector3{0.f, 0.2f, 0.f}, .MotionType = EPhysicsMotionType::Dynamic});
	REQUIRE(Hung);
	REQUIRE((*World)->CreatePointConstraint({.Body = *Hung, .Point = Hook}));

	// Half-overlapping boxes would be pushed apart by contacts if the joint did not disable them.
	const FPhysicsBodyProperties Floating{.GravityScale = 0.f};
	const auto Left = (*World)->CreateBody({.HalfExtents = {0.5f, 0.5f, 0.5f}, .Position = {10.f, 0.f, 0.f}, .MotionType = EPhysicsMotionType::Dynamic, .Properties = Floating});
	const auto Right = (*World)->CreateBody({.HalfExtents = {0.5f, 0.5f, 0.5f}, .Position = {10.5f, 0.f, 0.f}, .MotionType = EPhysicsMotionType::Dynamic, .Properties = Floating});
	REQUIRE(Left);
	REQUIRE(Right);
	REQUIRE((*World)->CreatePointConstraint({.Body = *Right, .Other = *Left, .Point = {10.25f, 0.f, 0.f}}));
	for (int Step = 0; Step < 120; ++Step)
	{
		REQUIRE((*World)->Step(1.f / 60.f));
	}

	const auto HungTransform = (*World)->GetBodyTransform(*Hung);
	REQUIRE(HungTransform);
	CHECK((HungTransform->Position + HungTransform->Rotation.RotateVector({0.f, 0.2f, 0.f}) - Hook).Length() < 0.01f);
	const auto LeftTransform = (*World)->GetBodyTransform(*Left);
	const auto RightTransform = (*World)->GetBodyTransform(*Right);
	REQUIRE(LeftTransform);
	REQUIRE(RightTransform);
	CHECK((RightTransform->Position - LeftTransform->Position).Length() == doctest::Approx(0.5f).epsilon(0.01));

	const auto Floor = (*World)->CreateBody({});
	REQUIRE(Floor);
	CHECK_FALSE((*World)->CreatePointConstraint({.Body = *Floor, .Point = Hook}));
	CHECK_FALSE((*World)->CreatePointConstraint({.Body = *Hung, .Other = *Hung, .Point = Hook}));
	CHECK_FALSE((*World)->CreatePointConstraint({.Body = *Hung, .Other = FPhysicsBodyId{}, .Point = Hook}));
	CHECK_FALSE((*World)->CreatePointConstraint({.Body = *Hung, .Point = {std::numeric_limits<float>::quiet_NaN(), 0.f, 0.f}}));
	CHECK_FALSE((*World)->CreateRope({.Segments = 0}));
	CHECK_FALSE((*World)->CreateRope({.End = FVector3::Zero()}));
	CHECK_FALSE((*World)->CreateRope({.Radius = 0.f}));
	std::vector<FVector3> Points;
	CHECK_FALSE((*World)->GetRopePoints({.Index = 7}, Points));
}

TEST_CASE("Soft body creation rejects invalid topology and parameters")
{
	auto World = FPhysicsWorld::Create();
	REQUIRE(World);
	const std::array<FVector3, 2> Vertices{{{0.f, 1.f, 0.f}, {1.f, 1.f, 0.f}}};
	const std::array<std::array<std::uint32_t, 2>, 1> Edge{{{0, 1}}};
	const std::array<std::array<std::uint32_t, 2>, 1> OutOfRange{{{0, 2}}};
	const std::array<std::array<std::uint32_t, 2>, 1> Degenerate{{{1, 1}}};
	const std::array<std::uint32_t, 2> AllPinned{0, 1};
	const std::array<std::uint32_t, 1> MissingPin{5};
	CHECK((*World)->CreateSoftBody({.Vertices = Vertices, .StretchEdges = Edge}));
	CHECK_FALSE((*World)->CreateSoftBody({.Vertices = Vertices, .StretchEdges = OutOfRange}));
	CHECK_FALSE((*World)->CreateSoftBody({.Vertices = Vertices, .StretchEdges = Degenerate}));
	CHECK_FALSE((*World)->CreateSoftBody({.Vertices = Vertices}));
	CHECK_FALSE((*World)->CreateSoftBody({.Vertices = Vertices, .PinnedVertices = AllPinned, .StretchEdges = Edge}));
	CHECK_FALSE((*World)->CreateSoftBody({.Vertices = Vertices, .PinnedVertices = MissingPin, .StretchEdges = Edge}));
	CHECK_FALSE((*World)->CreateSoftBody({.Vertices = Vertices, .StretchEdges = Edge, .MassKg = std::numeric_limits<float>::quiet_NaN()}));
	CHECK_FALSE((*World)->CreateSoftBody({.Vertices = Vertices, .StretchEdges = Edge, .Iterations = 0}));
	const auto Box = (*World)->CreateBody({});
	REQUIRE(Box);
	std::vector<FVector3> Positions;
	CHECK_FALSE((*World)->GetSoftBodyVertices(*Box, Positions));
}

namespace
{
// A floor and a pile of mixed shapes that collide while they settle, which exercises contacts, islands, and sleeping.
std::unique_ptr<FPhysicsWorld> CreateDeterminismScene(FTaskSystem* const Tasks, const bool bReportContacts = false)
{
	auto World = FPhysicsWorld::Create({.Tasks = Tasks});
	REQUIRE(World);
	REQUIRE((*World)->CreateBody({.HalfExtents = {20.f, 0.5f, 20.f}, .Position = {0.f, -0.5f, 0.f}}));
	for (int Index = 0; Index < 48; ++Index)
	{
		const float X = static_cast<float>(Index % 4) * 1.1f - 1.65f + static_cast<float>(Index / 16) * 0.13f;
		const float Z = static_cast<float>((Index / 4) % 4) * 1.1f - 1.65f;
		const float Y = 1.f + static_cast<float>(Index / 16) * 1.3f;
		FPhysicsBodySettings Body{
		    .Shape = static_cast<EPhysicsShape>(Index % 3),
		    .HalfExtents = {0.4f, 0.3f, 0.45f},
		    .Radius = 0.35f,
		    .HalfHeight = 0.25f,
		    .Position = {X, Y, Z},
		    .Rotation = FQuaternion::FromAxisAngle(FVector3{1.f, static_cast<float>(Index % 5), 0.5f}.Normalized(), static_cast<float>(Index) * 0.37f),
		    .MotionType = EPhysicsMotionType::Dynamic,
		    .Properties = {.MassKg = 1.f + static_cast<float>(Index % 7), .Friction = 0.5f, .Restitution = 0.2f},
		    .bReportContacts = bReportContacts,
		};
		REQUIRE((*World)->CreateBody(Body));
	}

	return std::move(*World);
}

void StepScene(FPhysicsWorld& World, const int Steps)
{
	for (int Step = 0; Step < Steps; ++Step)
	{
		REQUIRE(World.Step(1.f / 60.f));
	}
}
}

TEST_CASE("Physics hashes the same after equal steps on every run, thread count, and snapshot restore")
{
	const auto First = CreateDeterminismScene(nullptr);
	const std::uint64_t Initial = First->ComputeStateHash();
	StepScene(*First, 180);
	const std::uint64_t Settled = First->ComputeStateHash();
	CHECK(Settled != Initial);

	const auto Second = CreateDeterminismScene(nullptr);
	StepScene(*Second, 180);
	CHECK(Second->ComputeStateHash() == Settled);

	// Jolt's jobs on four Herta workers give the same world as stepping on one thread.
	auto Log = FLogService::Create({.bConsoleOutput = false, .bDebuggerOutput = false, .bFileOutput = false});
	REQUIRE(Log);
	auto Tasks = FTaskSystem::Create({.CpuWorkerCount = 4, .Log = Log->get()});
	REQUIRE(Tasks);
	{
		const auto Threaded = CreateDeterminismScene(Tasks->get());
		StepScene(*Threaded, 180);
		CHECK(Threaded->ComputeStateHash() == Settled);
	}

	(*Tasks)->Shutdown();

	// Rollback: restoring a snapshot and replaying the same steps reaches the same world.
	const auto Rolled = CreateDeterminismScene(nullptr);
	StepScene(*Rolled, 90);
	const std::vector<std::byte> Snapshot = Rolled->SaveState();
	StepScene(*Rolled, 90);
	CHECK(Rolled->ComputeStateHash() == Settled);
	REQUIRE(Rolled->RestoreState(Snapshot));
	const bool bRestoredExactly = Rolled->SaveState() == Snapshot;
	CHECK(bRestoredExactly);
	StepScene(*Rolled, 90);
	CHECK(Rolled->ComputeStateHash() == Settled);

	// Damaged or foreign snapshots are rejected before Jolt reads them, leaving the world as it was.
	CHECK_FALSE(Rolled->RestoreState(std::span(Snapshot).first(Snapshot.size() / 2)));
	std::vector<std::byte> Damaged = Snapshot;
	Damaged[Damaged.size() / 2] ^= std::byte{1};
	CHECK_FALSE(Rolled->RestoreState(Damaged));
	auto Empty = FPhysicsWorld::Create();
	REQUIRE(Empty);
	CHECK_FALSE((*Empty)->RestoreState(Snapshot));
	CHECK(Rolled->ComputeStateHash() == Settled);

	// Jolt's cross-platform build makes this the same value on Windows and Linux, so CI on both checks it. It changes only with the scene, Jolt, or
	// its settings; update it deliberately then.
	CHECK(Settled == 913175411509097514ull);
}

TEST_CASE("Contact events merge per body pair, carry the impact, and keep one order on any thread count")
{
	auto World = FPhysicsWorld::Create();
	REQUIRE(World);
	const auto Floor = (*World)->CreateBody({.HalfExtents = {5.f, 0.5f, 5.f}, .Position = {0.f, -0.5f, 0.f}});
	const auto Ball = (*World)->CreateBody({.Shape = EPhysicsShape::Sphere, .Radius = 0.5f, .Position = {0.f, 3.f, 0.f}, .MotionType = EPhysicsMotionType::Dynamic, .Properties = {.MassKg = 2.f, .Restitution = 0.9f, .LinearDamping = 0.f}, .bReportContacts = true});
	const auto Quiet = (*World)->CreateBody({.Position = {3.f, 2.f, 0.f}, .MotionType = EPhysicsMotionType::Dynamic, .Properties = {.Restitution = 0.f}});
	REQUIRE(Floor);
	REQUIRE(Ball);
	REQUIRE(Quiet);

	std::vector<FPhysicsContactEvent> Events;
	for (int Step = 0; Step < 90; ++Step)
	{
		REQUIRE((*World)->Step(1.f / 60.f));
		const auto Stepped = (*World)->GetContactEvents();
		Events.insert(Events.end(), Stepped.begin(), Stepped.end());
	}

	// The ball bounces, so its contact begins and ends; the quiet box reports nothing.
	REQUIRE(Events.size() >= 2);
	const FPhysicsContactEvent& Impact = Events[0];
	CHECK(Impact.Phase == EPhysicsContactPhase::Began);
	CHECK(Impact.First == *Floor);
	CHECK(Impact.Second == *Ball);
	CHECK(Impact.Normal.Y > 0.99f);
	// A 2.5 m fall closes at about 7 m/s, and stopping and returning 2 kg at restitution 0.9 takes about 1.9 times that momentum.
	CHECK(Impact.ApproachSpeed == doctest::Approx(std::sqrt(2.f * 9.80665f * 2.5f)).epsilon(0.08));
	CHECK(Impact.Impulse == doctest::Approx(2.f * Impact.ApproachSpeed * 1.9f).epsilon(0.05));
	CHECK(Events[1].Phase == EPhysicsContactPhase::Ended);
	CHECK(std::ranges::none_of(Events, [&](const FPhysicsContactEvent& Event)
	{
		return Event.First == *Quiet || Event.Second == *Quiet;
	}));

	// Every body reports in the pile; the event stream must not depend on which job threads found the contacts.
	const auto Record = [](FPhysicsWorld& Scene)
	{
		std::vector<std::tuple<std::uint32_t, std::uint32_t, EPhysicsContactPhase, float>> Stream;
		for (int Step = 0; Step < 120; ++Step)
		{
			REQUIRE(Scene.Step(1.f / 60.f));
			for (const FPhysicsContactEvent& Event : Scene.GetContactEvents())
			{
				Stream.emplace_back(Event.First.Value, Event.Second.Value, Event.Phase, Event.Impulse);
			}
		}

		return Stream;
	};

	auto Log = FLogService::Create({.bConsoleOutput = false, .bDebuggerOutput = false, .bFileOutput = false});
	REQUIRE(Log);
	auto Tasks = FTaskSystem::Create({.CpuWorkerCount = 4, .Log = Log->get()});
	REQUIRE(Tasks);
	{
		const auto Single = Record(*CreateDeterminismScene(nullptr, true));
		const auto Threaded = Record(*CreateDeterminismScene(Tasks->get(), true));
		CHECK(Single.size() > 48);
		const bool bSameStream = Single == Threaded;
		CHECK(bSameStream);
	}

	(*Tasks)->Shutdown();
}

TEST_CASE("Shifting the physics origin keeps every body where it is in the world")
{
	auto World = FPhysicsWorld::Create();
	REQUIRE(World);
	const FWorldPosition Far{250000.0, 12.0, -90000.0};
	REQUIRE((*World)->ShiftOrigin(Far));
	CHECK((*World)->GetOrigin() == Far);
	CHECK((*World)->ToPhysics(Far) == FVector3::Zero());

	const auto Floor = (*World)->CreateBody({.HalfExtents = {5.f, 0.5f, 5.f}, .Position = (*World)->ToPhysics(Far.TranslatedBy({0.0, -0.5, 0.0}))});
	const auto Box = (*World)->CreateBody({.HalfExtents = {0.5f, 0.5f, 0.5f}, .Position = (*World)->ToPhysics(Far.TranslatedBy({0.0, 2.0, 0.0})), .MotionType = EPhysicsMotionType::Dynamic});
	REQUIRE(Floor);
	REQUIRE(Box);
	for (int Step = 0; Step < 120; ++Step)
	{
		REQUIRE((*World)->Step(1.f / 60.f));
	}

	const FWorldPosition Rest = (*World)->ToWorld((*World)->GetBodyTransform(*Box)->Position);
	CHECK(Rest.Meters.Y == doctest::Approx(12.5).epsilon(0.001));

	// Moving the origin 1 km leaves the resting box in place in the world, and it keeps resting.
	REQUIRE((*World)->ShiftOrigin(Far.TranslatedBy({1000.0, 0.0, 0.0})));
	const FWorldPosition Shifted = (*World)->ToWorld((*World)->GetBodyTransform(*Box)->Position);
	CHECK(Shifted.Meters.X == doctest::Approx(Rest.Meters.X).epsilon(1e-9));
	CHECK(Shifted.Meters.Y == doctest::Approx(Rest.Meters.Y).epsilon(1e-6));
	for (int Step = 0; Step < 60; ++Step)
	{
		REQUIRE((*World)->Step(1.f / 60.f));
	}

	CHECK((*World)->ToWorld((*World)->GetBodyTransform(*Box)->Position).Meters.Y == doctest::Approx(12.5).epsilon(0.001));
	CHECK_FALSE((*World)->ShiftOrigin(FWorldPosition{std::numeric_limits<double>::infinity(), 0.0, 0.0}));
}

TEST_CASE("Collision layers keep debris off other debris and characters while it still lands on the world")
{
	FPhysicsWorldSettings Asymmetric;
	Asymmetric.LayerCollisions[PhysicsLayerDebris] = 0b1011;
	CHECK_FALSE(FPhysicsWorld::Create(Asymmetric));

	auto World = FPhysicsWorld::Create();
	REQUIRE(World);
	REQUIRE((*World)->CreateBody({.HalfExtents = {5.f, 0.5f, 5.f}, .Position = {0.f, -0.5f, 0.f}}));
	const auto Lower = (*World)->CreateBody({.HalfExtents = {0.5f, 0.5f, 0.5f}, .Position = {0.f, 0.5f, 0.f}, .MotionType = EPhysicsMotionType::Dynamic, .Layer = PhysicsLayerDebris});
	const auto Upper = (*World)->CreateBody({.HalfExtents = {0.5f, 0.5f, 0.5f}, .Position = {0.f, 2.f, 0.f}, .MotionType = EPhysicsMotionType::Dynamic, .Layer = PhysicsLayerDebris});
	const auto Crate = (*World)->CreateBody({.HalfExtents = {0.5f, 0.5f, 0.5f}, .Position = {3.f, 0.5f, 0.f}, .MotionType = EPhysicsMotionType::Dynamic});
	REQUIRE(Lower);
	REQUIRE(Upper);
	REQUIRE(Crate);
	CHECK_FALSE((*World)->CreateBody({.Position = {0.f, 5.f, 0.f}, .MotionType = EPhysicsMotionType::Dynamic, .Layer = PhysicsLayerStatic}));
	const auto Character = (*World)->CreateCharacter({.FootPosition = {-2.f, 0.f, 0.f}});
	REQUIRE(Character);
	for (int Step = 0; Step < 90; ++Step)
	{
		REQUIRE((*World)->Step(1.f / 60.f));
		// Walking right at 2 m/s passes the debris but stops at the crate.
		REQUIRE((*World)->MoveCharacter(*Character, {2.f, 0.f, 0.f}, 1.f / 60.f));
	}

	// The upper debris falls through the lower one onto the floor, and both rest on the world.
	CHECK((*World)->GetBodyTransform(*Lower)->Position.Y == doctest::Approx(0.5f).epsilon(0.02));
	CHECK((*World)->GetBodyTransform(*Upper)->Position.Y == doctest::Approx(0.5f).epsilon(0.05));
	const float CharacterX = (*World)->UpdateCharacterGround(*Character)->FootPosition.X;
	CHECK(CharacterX > 0.5f);
	CHECK(CharacterX < 2.75f);
}

TEST_CASE("Convex hull and compound bodies settle, and destroyed bodies free their slot")
{
	auto World = FPhysicsWorld::Create();
	REQUIRE(World);
	REQUIRE((*World)->CreateBody({.HalfExtents = {10.f, 0.5f, 10.f}, .Position = {0.f, -0.5f, 0.f}}));
	// A wedge whose lowest face is 0.2 m below its origin.
	const std::array<FVector3, 6> Wedge{FVector3{-0.5f, -0.2f, -0.5f}, {0.5f, -0.2f, -0.5f}, {-0.5f, -0.2f, 0.5f}, {0.5f, -0.2f, 0.5f}, {-0.5f, 0.6f, 0.f}, {0.5f, 0.6f, 0.f}};
	const auto Hull = (*World)->CreateBody({.Shape = EPhysicsShape::ConvexHull, .Position = {0.f, 1.f, 0.f}, .MotionType = EPhysicsMotionType::Dynamic, .HullPoints = Wedge});
	const std::array<FVector3, 8> Left{FVector3{-1.f, -0.25f, -0.25f}, {0.f, -0.25f, -0.25f}, {-1.f, 0.25f, -0.25f}, {0.f, 0.25f, -0.25f}, {-1.f, -0.25f, 0.25f}, {0.f, -0.25f, 0.25f}, {-1.f, 0.25f, 0.25f}, {0.f, 0.25f, 0.25f}};
	std::array<FVector3, 8> Right = Left;
	for (FVector3& Point : Right)
	{
		Point.X += 1.f;
	}

	const std::array<std::span<const FVector3>, 2> Halves{std::span<const FVector3>(Left), std::span<const FVector3>(Right)};
	const auto Compound = (*World)->CreateBody({.Shape = EPhysicsShape::Compound, .Position = {4.f, 1.f, 0.f}, .MotionType = EPhysicsMotionType::Dynamic, .CompoundHulls = Halves});
	REQUIRE(Hull);
	REQUIRE(Compound);
	CHECK_FALSE((*World)->CreateBody({.Shape = EPhysicsShape::ConvexHull, .MotionType = EPhysicsMotionType::Dynamic, .HullPoints = std::span(Wedge).first(3)}));
	CHECK((*World)->GetBodyCount() == 3);

	std::vector<FPhysicsDebugLine> Lines;
	(*World)->GetDebugLines(Lines);
	// Both of the compound's boxes draw, each as 12 triangles; only the compound sits above the floor on that side.
	const auto CompoundLines = std::ranges::count_if(Lines, [](const FPhysicsDebugLine& Line)
	{
		return Line.Start.X > 2.5f && Line.Start.Y > 0.5f && Line.End.Y > 0.5f;
	});

	CHECK(CompoundLines >= 2 * 12 * 3);
	for (int Step = 0; Step < 120; ++Step)
	{
		REQUIRE((*World)->Step(1.f / 60.f));
	}

	CHECK((*World)->GetBodyTransform(*Hull)->Position.Y == doctest::Approx(0.2f).epsilon(0.05));
	CHECK((*World)->GetBodyTransform(*Compound)->Position.Y == doctest::Approx(0.25f).epsilon(0.05));

	// An impulse wakes and moves a resting body.
	REQUIRE((*World)->AddImpulse(*Compound, {0.f, 0.f, 20.f}, (*World)->GetBodyTransform(*Compound)->Position));
	REQUIRE((*World)->Step(1.f / 60.f));
	CHECK((*World)->GetLinearVelocity(*Compound)->Z > 1.f);
	REQUIRE((*World)->SetVelocity(*Hull, {0.f, 3.f, 0.f}));
	CHECK((*World)->GetLinearVelocity(*Hull)->Y == doctest::Approx(3.f));

	REQUIRE((*World)->DestroyBody(*Hull));
	CHECK((*World)->GetBodyCount() == 2);
	CHECK_FALSE((*World)->GetBodyTransform(*Hull));
	CHECK_FALSE((*World)->DestroyBody(*Hull));
	for (int Step = 0; Step < 10; ++Step)
	{
		REQUIRE((*World)->Step(1.f / 60.f));
	}

	// A new body reuses the slot under a new identity, so the old ID stays dead.
	const auto Replacement = (*World)->CreateBody({.Position = {0.f, 2.f, 0.f}, .MotionType = EPhysicsMotionType::Dynamic});
	REQUIRE(Replacement);
	CHECK_FALSE(*Replacement == *Hull);
	CHECK_FALSE((*World)->GetBodyTransform(*Hull));
}
}
