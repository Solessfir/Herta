#pragma once

#include "Herta/Math/Transform.h"
#include "Herta/Physics/PhysicsWorld.h"

namespace Herta
{
// Model-space box that a preview object collides with. The default matches the built-in 2 m cube.
struct FPreviewBodyShape
{
	FVector3 Center{};
	FVector3 HalfExtents = FVector3::One();
};

class FPreviewSimulation final
{
public:
	[[nodiscard]] std::expected<void, FPhysicsError> Start(const FTransform& CubeTransform, const FTransform& FloorTransform, const FPreviewBodyShape& CubeShape = {}, const FPreviewBodyShape& FloorShape = {});
	void Stop() noexcept;
	[[nodiscard]] std::expected<void, FPhysicsError> Update(float DeltaSeconds);
	[[nodiscard]] bool IsRunning() const noexcept { return World != nullptr; }
	[[nodiscard]] const FTransform& GetTransform() const noexcept { return RenderTransform; }

private:
	std::unique_ptr<FPhysicsWorld> World;
	FPhysicsBodyId Cube;
	FTransform OriginalTransform;
	FTransform RenderTransform;
	// The body's center relative to the object origin, in scaled model space.
	FVector3 CubeOffset{};
	FPhysicsBodyTransform Previous;
	FPhysicsBodyTransform Current;
	double Accumulator = 0.0;
};
}
