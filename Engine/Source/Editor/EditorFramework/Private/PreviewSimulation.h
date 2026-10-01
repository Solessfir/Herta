#pragma once

#include "Herta/Math/Transform.h"
#include "Herta/Physics/PhysicsWorld.h"

namespace Herta
{
class FPreviewSimulation final
{
public:
	[[nodiscard]] std::expected<void, FPhysicsError> Start(const FTransform& CubeTransform, const FTransform& FloorTransform);
	void Stop() noexcept;
	[[nodiscard]] std::expected<void, FPhysicsError> Update(float DeltaSeconds);
	[[nodiscard]] bool IsRunning() const noexcept { return World != nullptr; }
	[[nodiscard]] const FTransform& GetTransform() const noexcept { return RenderTransform; }

private:
	std::unique_ptr<FPhysicsWorld> World;
	FPhysicsBodyId Cube;
	FTransform OriginalTransform;
	FTransform RenderTransform;
	FPhysicsBodyTransform Previous;
	FPhysicsBodyTransform Current;
	double Accumulator = 0.0;
};
}
