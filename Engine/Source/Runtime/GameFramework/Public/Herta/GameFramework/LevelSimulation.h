#pragma once

#include "Herta/GameFramework/Fracture.h"
#include "Herta/Math/Transform.h"
#include "Herta/Physics/PhysicsWorld.h"

#include <cstddef>
#include <functional>
#include <map>
#include <memory>
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

// Static bodies added after Start, such as a streamed region's scenery, removed together.
struct FLevelStaticGroup
{
	auto operator<=>(const FLevelStaticGroup&) const = default;

	std::uint32_t Value = 0;
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

// A pre-fractured object. It starts as one body; impacts break bonds between chunks, and the chunks still bonded together move as one body.
struct FLevelSimulationDestructible
{
	std::size_t ObjectIndex = 0;
	// The fractured box's center and rotation in the world; the fracture is in world units around it.
	FVector3 Position = FVector3::Zero();
	FQuaternion Rotation = FQuaternion::Identity();
	// The object's origin relative to the box center in the box's frame, and its scale, so the intact object draws where its body is.
	FVector3 Offset = FVector3::Zero();
	FVector3 Scale = FVector3::One();
	// Shared with the renderer, which draws each chunk.
	std::shared_ptr<const FFracture> Fracture{};
	// The impulse in N*s a contact must carry to break bonds. Stronger hits break bonds further from the impact.
	float Strength = 20.f;
	// Chunks on the bottom face stay fixed, like a wall on its footing; otherwise the object is one Dynamic body until it breaks.
	bool bAnchored = true;
	// MassKg is the whole object's, shared among chunks by volume.
	FPhysicsBodyProperties Properties{};
};

// Debris is every single loose chunk. It despawns after a while or, oldest first, when there is too much of it.
struct FLevelDestructionSettings
{
	std::uint32_t MaxDebris = 256;
	float DebrisLifetimeSeconds = 12.f;
};

// A body added after Start. Handles of despawned bodies stay invalid even when their slot is reused.
struct FLevelSimulationHandle
{
	std::uint32_t Index = 0;
	std::uint32_t Generation = 0;

	[[nodiscard]] constexpr bool operator==(const FLevelSimulationHandle&) const = default;
};

struct FLevelSimulationSpawn
{
	FLevelSimulationHandle Handle{};
	FPhysicsBodyTransform Transform{};
};

// One chunk of a broken destructible, drawn with its island's transform.
struct FLevelSimulationPiece
{
	std::size_t ObjectIndex = 0;
	std::uint32_t Chunk = 0;
	FPhysicsBodyTransform Transform{};
};

// A contact that began with a moving body, with the impulse stopping it took, for impact sounds and effects.
struct FLevelImpactEvent
{
	FVector3 Point = FVector3::Zero();
	// From the first body into the second, so either way it is the surface the impact struck.
	FVector3 Normal = FVector3::Up();
	float Impulse = 0.f;
};

struct FLevelBreakEvent
{
	std::size_t ObjectIndex = 0;
	FVector3 Point = FVector3::Zero();
	std::uint32_t BrokenBonds = 0;
};

class FLevelSimulation final
{
public:
	[[nodiscard]] std::expected<void, FPhysicsError> Start(std::span<const FLevelSimulationBody> Bodies, std::span<const FLevelSimulationSoftBody> SoftBodies = {}, std::span<const FLevelSimulationConstraint> Constraints = {}, const FPhysicsWorldSettings& Settings = LevelPhysicsSettings, std::span<const FLevelSimulationDestructible> Destructibles = {}, const FLevelDestructionSettings& Destruction = {});
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

	// Adds a moving body, in physics space, that despawns with the simulation if not before.
	[[nodiscard]] std::expected<FLevelSimulationHandle, FPhysicsError> Spawn(const FPhysicsBodySettings& Body);
	[[nodiscard]] std::expected<void, FPhysicsError> Despawn(FLevelSimulationHandle Handle);
	[[nodiscard]] std::optional<FPhysicsBodyId> FindSpawnedBody(FLevelSimulationHandle Handle) const noexcept;
	// Interpolated transforms of every spawned body, in spawn order.
	std::span<const FLevelSimulationSpawn> GetSpawned() const noexcept;
	// Whether a destructible is still in one piece; broken ones draw their pieces instead.
	bool IsIntact(std::size_t ObjectIndex) const noexcept;
	// Interpolated chunks of every broken destructible that have not despawned.
	std::span<const FLevelSimulationPiece> GetPieces() const noexcept;
	std::size_t GetDebrisCount() const noexcept;
	// Breaks since the previous call, in step order.
	std::vector<FLevelBreakEvent> TakeBreakEvents();
	// Adds static scenery to the running world. Only static bodies belong in a group; movers, triggers, and moving bodies are refused.
	[[nodiscard]] std::expected<FLevelStaticGroup, FPhysicsError> AddStaticBodies(std::span<const FLevelSimulationBody> Bodies);
	[[nodiscard]] std::expected<void, FPhysicsError> RemoveStaticBodies(FLevelStaticGroup Group);
	[[nodiscard]] std::size_t GetStaticGroupBodyCount() const noexcept;
	// The bonds still holding a destructible together, for saving; empty for an object that is not a running destructible.
	std::vector<bool> GetIntactBonds(std::size_t ObjectIndex) const;
	// Breaks the bonds a save recorded as broken, so the destructible splits as it was saved; freed pieces fall from where they stood.
	[[nodiscard]] std::expected<void, FPhysicsError> RestoreBonds(std::size_t ObjectIndex, const std::vector<bool>& Intact);
	// Impacts since the previous call, in step order; contacts report for Dynamic level bodies, spawned bodies that ask, and destructibles.
	std::vector<FLevelImpactEvent> TakeImpactEvents();

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

	struct FSpawnState
	{
		FPhysicsBodyId Id{};
		std::uint32_t Generation = 1;
		bool bAlive = false;
		FPhysicsBodyTransform Previous{};
		FPhysicsBodyTransform Current{};
	};

	// Chunks bonded together, moving as one body.
	struct FIslandState
	{
		FPhysicsBodyId Id{};
		std::vector<std::uint32_t> Chunks{};
		EPhysicsMotionType MotionType = EPhysicsMotionType::Static;
		FPhysicsBodyTransform Previous{};
		FPhysicsBodyTransform Current{};
		// When it became a single loose chunk; debris despawns from here.
		std::optional<double> DebrisSince{};
		bool bAlive = true;
	};

	struct FDestructibleState
	{
		FLevelSimulationDestructible Settings{};
		std::size_t TransformIndex = 0;
		std::vector<bool> BondIntact{};
		std::vector<FIslandState> Islands{};
		float Density = 0.f;
		float ChunkSize = 0.f;
		// The latest breaking impact, which freed pieces fly away from.
		FVector3 Impact = FVector3::Zero();
		bool bIntact = true;
		bool bDirty = false;
	};

	[[nodiscard]] std::expected<void, FPhysicsError> Damage(std::size_t Destructible, std::size_t Island, const FVector3& Point, float Impulse);
	[[nodiscard]] std::expected<void, FPhysicsError> SplitIslands(std::size_t Destructible, const FVector3& Impact, float Impulse);
	[[nodiscard]] std::expected<FPhysicsBodyId, FPhysicsError> CreateIsland(const FDestructibleState& State, const std::vector<std::uint32_t>& Chunks, EPhysicsMotionType MotionType, const FPhysicsBodyTransform& Transform);
	[[nodiscard]] std::expected<void, FPhysicsError> ExpireDebris();

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

	std::vector<FSpawnState> SpawnStates;
	std::vector<std::uint32_t> FreeSpawnSlots;
	std::vector<FLevelSimulationSpawn> Spawned;
	std::vector<FDestructibleState> Destructibles;
	// Island bodies back to their destructible and island, for contact events.
	std::unordered_map<std::uint32_t, std::pair<std::size_t, std::size_t>> IslandBodies;
	std::vector<FLevelSimulationPiece> Pieces;
	std::vector<FLevelBreakEvent> BreakEvents;
	std::vector<FLevelImpactEvent> ImpactEvents;
	std::map<std::uint32_t, std::vector<FPhysicsBodyId>> StaticGroups;
	std::uint32_t NextStaticGroup = 0;
	FLevelDestructionSettings DestructionSettings{};
	std::size_t DebrisCount = 0;

	double Accumulator = 0.0;
	double Time = 0.0;
};
}
