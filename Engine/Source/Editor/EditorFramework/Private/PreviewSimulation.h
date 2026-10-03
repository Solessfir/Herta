#pragma once

#include "Herta/Math/Transform.h"
#include "Herta/Physics/PhysicsWorld.h"

#include <cstddef>
#include <span>
#include <vector>

namespace Herta
{
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
};

struct FPreviewSimulationTransform
{
	std::size_t ObjectIndex = 0;
	FTransform Transform{};
};

class FPreviewSimulation final
{
public:
	[[nodiscard]] std::expected<void, FPhysicsError> Start(std::span<const FPreviewSimulationBody> Bodies);
	void Stop() noexcept;
	[[nodiscard]] std::expected<void, FPhysicsError> Update(float DeltaSeconds);

	bool IsRunning() const noexcept;
	std::span<const FPreviewSimulationTransform> GetTransforms() const noexcept;

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

	std::unique_ptr<FPhysicsWorld> World;
	std::vector<FBodyState> BodyStates;
	std::vector<FPreviewSimulationTransform> Transforms;

	double Accumulator = 0.0;
};
}
