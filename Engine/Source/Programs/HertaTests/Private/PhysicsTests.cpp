#include "Herta/Physics/PhysicsWorld.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>
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
	const auto Body = (*World)->CreateBoxBody({});
	REQUIRE(Body);
	const auto Overflow = (*World)->CreateBoxBody({});
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
		REQUIRE((*World)->CreateBoxBody({.Position = {0.f, 4.f, 0.f}, .MotionType = EPhysicsMotionType::Dynamic}));
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
		REQUIRE((*World)->CreateBoxBody({.HalfExtents = {10.f, 0.05f, 0.05f}, .Position = {Offset, 4.f, Offset}, .Rotation = Rotation, .MotionType = EPhysicsMotionType::Dynamic, .Properties = {.GravityScale = 0.f}}));
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

TEST_CASE("Physics validates every body property for static and dynamic bodies")
{
	auto World = FPhysicsWorld::Create();
	REQUIRE(World);

	struct FPropertyRange
	{
		float FPhysicsBodyProperties::* Member;
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
				FPhysicsBoxBodySettings Box{.MotionType = Type};
				Box.Properties.*Range.Member = Value;
				CHECK_FALSE((*World)->CreateBoxBody(Box));
			}

			for (const float Value : {Range.Minimum, Range.Maximum})
			{
				FPhysicsBoxBodySettings Box{.MotionType = Type};
				Box.Properties.*Range.Member = Value;
				CHECK((*World)->CreateBoxBody(Box));
			}
		}
	}
}

TEST_CASE("Physics applies gravity scale and linear damping independently per body")
{
	auto World = FPhysicsWorld::Create();
	REQUIRE(World);
	const auto Floating = (*World)->CreateBoxBody({.Position = {-4.f, 8.f, 0.f}, .MotionType = EPhysicsMotionType::Dynamic, .Properties = {.GravityScale = 0.f}});
	const auto Falling = (*World)->CreateBoxBody({.Position = {0.f, 8.f, 0.f}, .MotionType = EPhysicsMotionType::Dynamic, .Properties = {.LinearDamping = 0.f}});
	const auto Damped = (*World)->CreateBoxBody({.Position = {4.f, 8.f, 0.f}, .MotionType = EPhysicsMotionType::Dynamic, .Properties = {.LinearDamping = 1.f}});
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
	REQUIRE((*World)->CreateBoxBody({.HalfExtents = {10.f, 0.25f, 10.f}, .Position = {0.f, -0.25f, 0.f}}));
	const auto Inelastic = (*World)->CreateBoxBody({.HalfExtents = {0.5f, 0.5f, 0.5f}, .Position = {-3.f, 3.f, 0.f}, .MotionType = EPhysicsMotionType::Dynamic, .Properties = {.Restitution = 0.f, .LinearDamping = 0.f}});
	const auto Elastic = (*World)->CreateBoxBody({.HalfExtents = {0.5f, 0.5f, 0.5f}, .Position = {3.f, 3.f, 0.f}, .MotionType = EPhysicsMotionType::Dynamic, .Properties = {.Restitution = 1.f, .LinearDamping = 0.f}});
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
		REQUIRE((*World)->CreateBoxBody({.HalfExtents = {20.f, 0.25f, 2.f}, .Rotation = Rotation, .Properties = {.Friction = Friction}}));
		const auto Box = (*World)->CreateBoxBody({.HalfExtents = {0.5f, 0.5f, 0.5f}, .Position = {0.f, 2.f, 0.f}, .Rotation = Rotation, .MotionType = EPhysicsMotionType::Dynamic, .Properties = {.Friction = Friction}});
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

TEST_CASE("Physics authored mass changes dynamic collision response")
{
	const auto Simulate = [](const float MassKg)
	{
		auto World = FPhysicsWorld::Create();
		REQUIRE(World);
		const auto Target = (*World)->CreateBoxBody({.HalfExtents = {0.5f, 0.5f, 0.5f}, .Position = {0.f, 2.f, 0.f}, .MotionType = EPhysicsMotionType::Dynamic, .Properties = {.MassKg = MassKg, .LinearDamping = 0.f, .GravityScale = 0.f}});
		REQUIRE(Target);
		REQUIRE((*World)->CreateBoxBody({.HalfExtents = {0.5f, 0.5f, 0.5f}, .Position = {0.f, 5.f, 0.f}, .MotionType = EPhysicsMotionType::Dynamic, .Properties = {.LinearDamping = 0.f}}));

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
	REQUIRE((*World)->CreateBoxBody({.HalfExtents = {5.f, 0.5f, 5.f}, .Position = {0.f, -0.5f, 0.f}}));
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
	const auto Box = (*World)->CreateBoxBody({});
	REQUIRE(Box);
	std::vector<FVector3> Positions;
	CHECK_FALSE((*World)->GetSoftBodyVertices(*Box, Positions));
}
}
