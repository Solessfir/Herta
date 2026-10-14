#pragma once

#include "Herta/Math/Quaternion.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Herta
{
struct FPhysicsError
{
	std::string Message;
};

struct FPhysicsBodyId
{
	std::uint32_t Value = 0xffffffffu;

	[[nodiscard]] constexpr bool operator==(const FPhysicsBodyId&) const = default;
};

enum class EPhysicsMotionType : std::uint8_t
{
	Static,
	Dynamic,
	// Moved by MoveKinematicBody; pushes dynamic bodies but is never pushed back.
	Kinematic,
};

struct FPhysicsBodyProperties
{
	float MassKg = 1.f;
	float Friction = 0.2f;
	float Restitution = 0.f;
	float LinearDamping = 0.05f;
	float AngularDamping = 0.05f;
	float GravityScale = 1.f;
};

enum class EPhysicsShape : std::uint8_t
{
	Box,
	Sphere,
	// Upright along the body's local +Y axis.
	Capsule,
};

struct FPhysicsBodySettings
{
	EPhysicsShape Shape = EPhysicsShape::Box;
	FVector3 HalfExtents = FVector3::One();
	// Sphere and capsule radius.
	float Radius = 0.5f;
	// Half the length of a capsule's cylinder, excluding its caps.
	float HalfHeight = 0.5f;
	FVector3 Position = FVector3::Zero();
	FQuaternion Rotation = FQuaternion::Identity();
	EPhysicsMotionType MotionType = EPhysicsMotionType::Static;
	FPhysicsBodyProperties Properties{};
	// Sensors report overlaps with dynamic and kinematic bodies through GetSensorEvents instead of colliding.
	bool bSensor = false;
};

struct FPhysicsSensorEvent
{
	FPhysicsBodyId Sensor{};
	FPhysicsBodyId Body{};
	bool bEntered = false;
};

// Position-based soft body. Ropes use explicit edges; cloth and closed shells use faces, which also generate stretch, shear, and bend edges.
struct FPhysicsSoftBodySettings
{
	// World-space rest positions.
	std::span<const FVector3> Vertices{};
	// Vertices that stay where they are, such as a rope's anchor.
	std::span<const std::uint32_t> PinnedVertices{};
	std::span<const std::array<std::uint32_t, 2>> StretchEdges{};
	std::span<const std::array<std::uint32_t, 2>> BendEdges{};
	std::span<const std::array<std::uint32_t, 3>> Faces{};
	// Spread evenly over the free vertices.
	float MassKg = 1.f;
	// Compliance is the inverse of stiffness in m/N; zero is rigid.
	float StretchCompliance = 0.f;
	float BendCompliance = 0.001f;
	// Collision radius around each vertex, in meters.
	float VertexRadius = 0.f;
	// n * R * T in Pa * m^3, which inflates closed shells; zero disables pressure.
	float Pressure = 0.f;
	float Friction = 0.4f;
	float Restitution = 0.f;
	float LinearDamping = 0.1f;
	float GravityScale = 1.f;
	std::uint32_t Iterations = 8;
};

// Jolt constraints only join rigid bodies, so a rope that carries a load is a chain of capsules rather than a soft body.
struct FPhysicsRopeSettings
{
	// World-space ends; the rope rests straight between them.
	FVector3 Start = FVector3::Zero();
	FVector3 End = FVector3::Down();
	std::uint32_t Segments = 8;
	float Radius = 0.03f;
	// Spread evenly over the segments.
	float MassKg = 1.f;
	float Friction = 0.4f;
	// Damps bending at the joints; a jointed chain has no stable bending stiffness.
	float AngularDamping = 0.5f;
	// Fixes Start to the world, and keeps every joint within its rest length of it so a heavy load cannot stretch the chain.
	bool bPinned = true;
};

struct FPhysicsRope
{
	std::uint32_t Index = 0;
	// Segments carrying Start and End, for constraints that attach to either end.
	FPhysicsBodyId First{};
	FPhysicsBodyId Last{};
};

// Ball-and-socket joint at a world-space point. Joined bodies stop colliding with each other.
struct FPhysicsPointConstraintSettings
{
	FPhysicsBodyId Body{};
	// None fixes Body to the world.
	std::optional<FPhysicsBodyId> Other{};
	FVector3 Point = FVector3::Zero();
};

struct FPhysicsBodyTransform
{
	FVector3 Position;
	FQuaternion Rotation;
};

struct FPhysicsWorldSettings
{
	std::uint32_t MaxBodies = 1024;
	std::uint32_t MaxBodyPairs = 4096;
	std::uint32_t MaxContactConstraints = 1024;
	// Create rejects budgets below the scratch bound for the configured capacities.
	std::size_t TempMemoryBytes = 4 * 1024 * 1024;
};

class FPhysicsWorld final
{
public:
	[[nodiscard]] static std::expected<std::unique_ptr<FPhysicsWorld>, FPhysicsError> Create(const FPhysicsWorldSettings& Settings = {});

	~FPhysicsWorld();

	FPhysicsWorld(const FPhysicsWorld&) = delete;
	FPhysicsWorld& operator=(const FPhysicsWorld&) = delete;
	FPhysicsWorld(FPhysicsWorld&&) = delete;
	FPhysicsWorld& operator=(FPhysicsWorld&&) = delete;

	[[nodiscard]] std::expected<FPhysicsBodyId, FPhysicsError> CreateBody(const FPhysicsBodySettings& Settings);
	[[nodiscard]] std::expected<FPhysicsBodyId, FPhysicsError> CreateSoftBody(const FPhysicsSoftBodySettings& Settings);
	[[nodiscard]] std::expected<FPhysicsRope, FPhysicsError> CreateRope(const FPhysicsRopeSettings& Settings);
	[[nodiscard]] std::expected<void, FPhysicsError> CreatePointConstraint(const FPhysicsPointConstraintSettings& Settings);
	// Sets velocities that carry a kinematic body to Target over the next step, so bodies resting on it ride along.
	[[nodiscard]] std::expected<void, FPhysicsError> MoveKinematicBody(FPhysicsBodyId BodyId, const FPhysicsBodyTransform& Target, float DeltaSeconds);
	[[nodiscard]] std::expected<void, FPhysicsError> Step(float FixedDeltaSeconds);
	// Sensor overlap changes from the latest Step, in the order Jolt reported them.
	std::span<const FPhysicsSensorEvent> GetSensorEvents() const;
	[[nodiscard]] std::expected<FPhysicsBodyTransform, FPhysicsError> GetBodyTransform(FPhysicsBodyId BodyId) const;
	// World-space vertex positions in creation order.
	[[nodiscard]] std::expected<void, FPhysicsError> GetSoftBodyVertices(FPhysicsBodyId BodyId, std::vector<FVector3>& OutPositions) const;
	// World-space joints from Start to End, one more than the segment count.
	[[nodiscard]] std::expected<void, FPhysicsError> GetRopePoints(const FPhysicsRope& Rope, std::vector<FVector3>& OutPoints) const;

private:
	struct FImplementation;
	explicit FPhysicsWorld(std::unique_ptr<FImplementation> InImplementation) noexcept;

	std::unique_ptr<FImplementation> Implementation;
};
}
