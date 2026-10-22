#pragma once

#include "Herta/Math/Transform.h"
#include "Herta/Physics/PhysicsWorld.h"

#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace Herta
{
inline constexpr double LevelFixedStep = 1.0 / 60.0;
inline constexpr FPhysicsWorldSettings LevelPhysicsSettings{.MaxBodies = 16384, .MaxBodyPairs = 65536, .MaxContactConstraints = 32768, .TempMemoryBytes = 64 * 1024 * 1024};

// Model-space box that a simulated object collides with. The default matches the built-in 2 m cube.
struct FLevelBodyShape
{
	FVector3 Center{};
	FVector3 HalfExtents = FVector3::One();
};

// Kinematic motion from the authored pose to Offset and back, eased at both ends.
struct FLevelSimulationMover
{
	FVector3 Offset{};
	float PeriodSeconds = 6.f;
};

struct FLevelSimulationBody
{
	std::size_t ObjectIndex = 0;
	FTransform Transform{};
	FLevelBodyShape Shape{};
	// Fitted to Shape's scaled bounds; see ELevelCollisionShape.
	EPhysicsShape Collision = EPhysicsShape::Box;
	EPhysicsMotionType MotionType = EPhysicsMotionType::Static;
	FPhysicsBodyProperties Properties{};
	// Movers simulate as kinematic bodies whatever their MotionType.
	std::optional<FLevelSimulationMover> Mover{};
	// Triggers are box sensors from Shape that report Dynamic bodies instead of colliding.
	bool bTrigger = false;
};

struct FLevelTriggerEvent
{
	std::size_t TriggerObjectIndex = 0;
	std::size_t ObjectIndex = 0;
	bool bEntered = false;
};

struct FLevelSimulationSoftBody
{
	std::size_t ObjectIndex = 0;
	// The spans only need to stay valid until Start returns.
	FPhysicsSoftBodySettings Settings{};
	// Simulates a capsule chain instead, which can carry a constrained load, and resamples it onto Settings.Vertices for rendering.
	std::optional<FPhysicsRopeSettings> Rope{};
};

struct FLevelSimulationConstraint
{
	// A Dynamic body, joined at Point, a world-space position at the start.
	std::size_t ObjectIndex = 0;
	// A rigid body, or a rope that joins by its free end; none fixes the body to the world.
	std::optional<std::size_t> TargetObjectIndex{};
	FVector3 Point{};
};

struct FLevelSimulationTransform
{
	std::size_t ObjectIndex = 0;
	FTransform Transform{};
};

class FLevelSimulation final
{
public:
	[[nodiscard]] std::expected<void, FPhysicsError> Start(std::span<const FLevelSimulationBody> Bodies, std::span<const FLevelSimulationSoftBody> SoftBodies = {}, std::span<const FLevelSimulationConstraint> Constraints = {}, const FPhysicsWorldSettings& Settings = LevelPhysicsSettings);
	void Stop() noexcept;
	// Runs once per fixed step after movers receive their motion and before bodies move, such as a player controller riding a platform.
	using FFixedStepCallback = std::function<std::expected<void, FPhysicsError>(FPhysicsWorld& World, float StepSeconds)>;
	[[nodiscard]] std::expected<void, FPhysicsError> Update(float DeltaSeconds, const FFixedStepCallback& OnFixedStep = {});

	bool IsRunning() const noexcept;
	// Null while stopped. Callers may add their own physics objects, such as a character, which stop with the world.
	FPhysicsWorld* GetPhysicsWorld() noexcept;
	// How far the frame is between the previous and latest fixed step, for interpolating anything stepped in OnFixedStep.
	float GetInterpolationAlpha() const noexcept;
	std::span<const FLevelSimulationTransform> GetTransforms() const noexcept;
	// World-space vertices of an object's soft body after the latest fixed step, or empty when the object has none.
	std::span<const FVector3> GetSoftBodyPositions(std::size_t ObjectIndex) const noexcept;
	// Advances whenever soft body positions change, so callers can skip rebuilding unchanged geometry.
	std::uint64_t GetSoftBodyRevision() const noexcept;
	bool IsTriggerOccupied(std::size_t ObjectIndex) const noexcept;
	// Enter and exit events since the previous call, in step order.
	std::vector<FLevelTriggerEvent> TakeTriggerEvents();

private:
	struct FBodyState
	{
		FPhysicsBodyId Id;
		EPhysicsMotionType MotionType;
		FTransform OriginalTransform;
		// The body's center relative to the object origin, in scaled model space.
		FVector3 Offset;
		FPhysicsBodyTransform Previous;
		FPhysicsBodyTransform Current;
		std::optional<FLevelSimulationMover> Mover;
		FPhysicsBodyTransform Start;
		bool bTrigger = false;
	};

	struct FSoftBodyState
	{
		std::size_t ObjectIndex = 0;
		FPhysicsBodyId Id{};
		std::optional<FPhysicsRope> Rope{};
		std::vector<FVector3> Positions{};
	};

	[[nodiscard]] std::expected<void, FPhysicsError> ReadSoftBodies();

	std::unique_ptr<FPhysicsWorld> World;
	std::vector<FBodyState> BodyStates;
	std::vector<FLevelSimulationTransform> Transforms;
	std::vector<FSoftBodyState> SoftBodyStates;
	std::uint64_t SoftBodyRevision = 0;
	// Scratch for rope joints before they are resampled onto rope vertices.
	std::vector<FVector3> RopePoints;

	// Physics body IDs back to BodyStates indices for sensor events.
	std::unordered_map<std::uint32_t, std::size_t> BodyIndices;
	std::unordered_map<std::size_t, std::size_t> TriggerOccupancy;
	std::vector<FLevelTriggerEvent> TriggerEvents;

	double Accumulator = 0.0;
	double Time = 0.0;
};
}
