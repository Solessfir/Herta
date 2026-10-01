#include "PreviewSimulation.h"

#include <algorithm>
#include <cmath>

namespace Herta
{
std::expected<void, FPhysicsError> FPreviewSimulation::Start(const FTransform& CubeTransform, const FTransform& FloorTransform)
{
	if (IsRunning())
		return std::unexpected(FPhysicsError{"Preview simulation is already running"});
	auto NewWorld = FPhysicsWorld::Create();
	if (!NewWorld)
		return std::unexpected(NewWorld.error());
	const FVector3 FloorHalfExtents{std::abs(FloorTransform.Scale3D.X), std::abs(FloorTransform.Scale3D.Y), std::abs(FloorTransform.Scale3D.Z)};
	const auto Floor = (*NewWorld)->CreateBoxBody({FloorHalfExtents, FloorTransform.Translation, FloorTransform.Rotation});
	if (!Floor)
		return std::unexpected(Floor.error());
	const FVector3 HalfExtents{std::abs(CubeTransform.Scale3D.X), std::abs(CubeTransform.Scale3D.Y), std::abs(CubeTransform.Scale3D.Z)};
	const FVector3 Position = CubeTransform.Translation;
	const auto NewCube = (*NewWorld)->CreateBoxBody({HalfExtents, Position, CubeTransform.Rotation, EPhysicsMotionType::Dynamic});
	if (!NewCube)
		return std::unexpected(NewCube.error());
	World = std::move(*NewWorld);
	Cube = *NewCube;
	OriginalTransform = CubeTransform;
	RenderTransform = CubeTransform;
	RenderTransform.Translation = Position;
	Previous = Current = {Position, CubeTransform.Rotation};
	Accumulator = 0.0;
	return {};
}

void FPreviewSimulation::Stop() noexcept
{
	if (!IsRunning())
		return;
	World.reset();
	RenderTransform = OriginalTransform;
	Accumulator = 0.0;
}

std::expected<void, FPhysicsError> FPreviewSimulation::Update(const float DeltaSeconds)
{
	if (!std::isfinite(DeltaSeconds) || DeltaSeconds < 0.0f)
		return std::unexpected(FPhysicsError{"Preview frame delta must be finite and nonnegative"});
	if (!IsRunning())
		return {};
	constexpr double FixedStep = 1.0 / 60.0;
	// Bound catch-up after a paused debugger or a stalled frame.
	Accumulator += std::min(static_cast<double>(DeltaSeconds), 0.25);
	while (Accumulator >= FixedStep)
	{
		Previous = Current;
		if (const auto Result = World->Step(static_cast<float>(FixedStep)); !Result)
			return Result;
		const auto Transform = World->GetBodyTransform(Cube);
		if (!Transform)
			return std::unexpected(Transform.error());
		Current = *Transform;
		Accumulator -= FixedStep;
	}
	const float Alpha = static_cast<float>(Accumulator / FixedStep);
	RenderTransform.Translation = Previous.Position * (1.0f - Alpha) + Current.Position * Alpha;
	const auto& A = Previous.Rotation;
	const auto& B = Current.Rotation;
	const float Sign = A.X * B.X + A.Y * B.Y + A.Z * B.Z + A.W * B.W < 0.0f ? -1.0f : 1.0f;
	RenderTransform.Rotation = FQuaternion{std::lerp(A.X, Sign * B.X, Alpha), std::lerp(A.Y, Sign * B.Y, Alpha), std::lerp(A.Z, Sign * B.Z, Alpha), std::lerp(A.W, Sign * B.W, Alpha)}.NormalizedOrIdentity();
	return {};
}
}
