#pragma once

#include "Herta/Math/Quaternion.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
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
	Dynamic
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

struct FPhysicsBoxBodySettings
{
	FVector3 HalfExtents = FVector3::One();
	FVector3 Position = FVector3::Zero();
	FQuaternion Rotation = FQuaternion::Identity();
	EPhysicsMotionType MotionType = EPhysicsMotionType::Static;
	FPhysicsBodyProperties Properties{};
};

// Position-based soft body. Ropes use explicit edges; cloth and closed shells use faces, which also generate stretch, shear, and bend edges.
struct FPhysicsSoftBodySettings
{
	// World-space rest positions.
	std::span<const FVector3> Vertices;
	// Vertices that stay where they are, such as a rope's anchor.
	std::span<const std::uint32_t> PinnedVertices;
	std::span<const std::array<std::uint32_t, 2>> StretchEdges;
	std::span<const std::array<std::uint32_t, 2>> BendEdges;
	std::span<const std::array<std::uint32_t, 3>> Faces;
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

	[[nodiscard]] std::expected<FPhysicsBodyId, FPhysicsError> CreateBoxBody(const FPhysicsBoxBodySettings& Settings);
	[[nodiscard]] std::expected<FPhysicsBodyId, FPhysicsError> CreateSoftBody(const FPhysicsSoftBodySettings& Settings);
	[[nodiscard]] std::expected<void, FPhysicsError> Step(float FixedDeltaSeconds);
	[[nodiscard]] std::expected<FPhysicsBodyTransform, FPhysicsError> GetBodyTransform(FPhysicsBodyId BodyId) const;
	// World-space vertex positions in creation order.
	[[nodiscard]] std::expected<void, FPhysicsError> GetSoftBodyVertices(FPhysicsBodyId BodyId, std::vector<FVector3>& OutPositions) const;

private:
	struct FImplementation;
	explicit FPhysicsWorld(std::unique_ptr<FImplementation> InImplementation) noexcept;

	std::unique_ptr<FImplementation> Implementation;
};
}
