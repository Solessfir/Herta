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

// Jolt cannot constrain soft bodies, so the attached vertex becomes kinematic and is driven to a point on a dynamic rigid body every step.
struct FPhysicsSoftBodyAttachment
{
	FPhysicsBodyId Body{};
	std::uint32_t Vertex = 0;
	// World-space point at creation; it then moves with the body.
	FVector3 Point = FVector3::Zero();
	// A pinned vertex that holds the body up like a rope, keeping Point within TetherLength of it (or its initial distance, if longer). None lets the body move freely.
	std::optional<std::uint32_t> TetherVertex{};
	float TetherLength = 0.f;
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
	std::optional<FPhysicsSoftBodyAttachment> Attachment{};
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
	// Sets velocities that carry a kinematic body to Target over the next step, so bodies resting on it ride along.
	[[nodiscard]] std::expected<void, FPhysicsError> MoveKinematicBody(FPhysicsBodyId BodyId, const FPhysicsBodyTransform& Target, float DeltaSeconds);
	[[nodiscard]] std::expected<void, FPhysicsError> Step(float FixedDeltaSeconds);
	// Sensor overlap changes from the latest Step, in the order Jolt reported them.
	std::span<const FPhysicsSensorEvent> GetSensorEvents() const;
	[[nodiscard]] std::expected<FPhysicsBodyTransform, FPhysicsError> GetBodyTransform(FPhysicsBodyId BodyId) const;
	// World-space vertex positions in creation order.
	[[nodiscard]] std::expected<void, FPhysicsError> GetSoftBodyVertices(FPhysicsBodyId BodyId, std::vector<FVector3>& OutPositions) const;

private:
	struct FImplementation;
	explicit FPhysicsWorld(std::unique_ptr<FImplementation> InImplementation) noexcept;

	std::unique_ptr<FImplementation> Implementation;
};
}
