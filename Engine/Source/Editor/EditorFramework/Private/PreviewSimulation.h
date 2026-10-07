#pragma once

#include "Herta/Math/Transform.h"
#include "Herta/Physics/PhysicsWorld.h"

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace Herta
{
inline constexpr FPhysicsWorldSettings PreviewPhysicsSettings{.MaxBodies = 16384, .MaxBodyPairs = 65536, .MaxContactConstraints = 32768, .TempMemoryBytes = 64 * 1024 * 1024};

// Model-space box that a preview object collides with. The default matches the built-in 2 m cube.
struct FPreviewBodyShape
{
	FVector3 Center{};
	FVector3 HalfExtents = FVector3::One();
};

struct FPreviewSimulationBody
{
	std::size_t ObjectIndex = 0;
	FTransform Transform{};
	FPreviewBodyShape Shape{};
	EPhysicsMotionType MotionType = EPhysicsMotionType::Static;
	FPhysicsBodyProperties Properties{};
};

struct FPreviewSimulationSoftBody
{
	std::size_t ObjectIndex = 0;
	// The spans only need to stay valid until Start returns.
	FPhysicsSoftBodySettings Settings{};
	// Dynamic body that Settings.Attachment refers to. Start fills in its physics ID and moves the point onto the nearest part of its box.
	std::optional<std::size_t> AttachedObjectIndex{};
};

struct FPreviewSimulationTransform
{
	std::size_t ObjectIndex = 0;
	FTransform Transform{};
};

class FPreviewSimulation final
{
public:
	[[nodiscard]] std::expected<void, FPhysicsError> Start(std::span<const FPreviewSimulationBody> Bodies, std::span<const FPreviewSimulationSoftBody> SoftBodies = {}, const FPhysicsWorldSettings& Settings = PreviewPhysicsSettings);
	void Stop() noexcept;
	[[nodiscard]] std::expected<void, FPhysicsError> Update(float DeltaSeconds);

	bool IsRunning() const noexcept;
	std::span<const FPreviewSimulationTransform> GetTransforms() const noexcept;
	// World-space vertices of an object's soft body after the latest fixed step, or empty when the object has none.
	std::span<const FVector3> GetSoftBodyPositions(std::size_t ObjectIndex) const noexcept;
	// Advances whenever soft body positions change, so callers can skip rebuilding unchanged geometry.
	std::uint64_t GetSoftBodyRevision() const noexcept;

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
	};

	struct FSoftBodyState
	{
		std::size_t ObjectIndex = 0;
		FPhysicsBodyId Id;
		std::vector<FVector3> Positions;
	};

	[[nodiscard]] std::expected<void, FPhysicsError> ReadSoftBodies();

	std::unique_ptr<FPhysicsWorld> World;
	std::vector<FBodyState> BodyStates;
	std::vector<FPreviewSimulationTransform> Transforms;
	std::vector<FSoftBodyState> SoftBodyStates;
	std::uint64_t SoftBodyRevision = 0;

	double Accumulator = 0.0;
};
}
