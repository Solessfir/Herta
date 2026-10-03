#include "PreviewSimulation.h"

#include <algorithm>
#include <cmath>

namespace Herta
{
namespace
{
[[nodiscard]] FVector3 Multiply(const FVector3& Left, const FVector3& Right)
{
	return {Left.X * Right.X, Left.Y * Right.Y, Left.Z * Right.Z};
}

[[nodiscard]] FVector3 Absolute(const FVector3& Vector)
{
	return {std::abs(Vector.X), std::abs(Vector.Y), std::abs(Vector.Z)};
}
}

std::expected<void, FPhysicsError> FPreviewSimulation::Start(const FTransform& CubeTransform, const FTransform& FloorTransform, const FPreviewBodyShape& CubeShape, const FPreviewBodyShape& FloorShape)
{
	if (IsRunning())
	{
		return std::unexpected(FPhysicsError{"Preview simulation is already running"});
	}

	auto NewWorld = FPhysicsWorld::Create();
	if (!NewWorld)
	{
		return std::unexpected(NewWorld.error());
	}

	const FVector3 FloorHalfExtents = Absolute(Multiply(FloorTransform.Scale3D, FloorShape.HalfExtents));
	const FVector3 FloorPosition = FloorTransform.Translation + FloorTransform.Rotation.RotateVector(Multiply(FloorTransform.Scale3D, FloorShape.Center));
	const auto Floor = (*NewWorld)->CreateBoxBody({.HalfExtents = FloorHalfExtents, .Position = FloorPosition, .Rotation = FloorTransform.Rotation});
	if (!Floor)
	{
		return std::unexpected(Floor.error());
	}

	const FVector3 HalfExtents = Absolute(Multiply(CubeTransform.Scale3D, CubeShape.HalfExtents));
	CubeOffset = Multiply(CubeTransform.Scale3D, CubeShape.Center);
	const FVector3 Position = CubeTransform.Translation + CubeTransform.Rotation.RotateVector(CubeOffset);
	const auto NewCube = (*NewWorld)->CreateBoxBody({.HalfExtents = HalfExtents, .Position = Position, .Rotation = CubeTransform.Rotation, .MotionType = EPhysicsMotionType::Dynamic});
	if (!NewCube)
	{
		return std::unexpected(NewCube.error());
	}

	World = std::move(*NewWorld);
	Cube = *NewCube;
	OriginalTransform = CubeTransform;
	RenderTransform = CubeTransform;
	Previous = Current = {.Position = Position, .Rotation = CubeTransform.Rotation};
	Accumulator = 0.0;
	return {};
}

void FPreviewSimulation::Stop() noexcept
{
	if (!IsRunning())
	{
		return;
	}

	World.reset();
	RenderTransform = OriginalTransform;
	Accumulator = 0.0;
}

std::expected<void, FPhysicsError> FPreviewSimulation::Update(const float DeltaSeconds)
{
	if (!std::isfinite(DeltaSeconds) || DeltaSeconds < 0.f)
	{
		return std::unexpected(FPhysicsError{"Preview frame delta must be finite and nonnegative"});
	}

	if (!IsRunning())
	{
		return {};
	}

	constexpr double FixedStep = 1.0 / 60.0;
	// Bound catch-up after a paused debugger or a stalled frame.
	Accumulator += std::min(static_cast<double>(DeltaSeconds), 0.25);
	while (Accumulator >= FixedStep)
	{
		Previous = Current;
		if (const auto Result = World->Step(static_cast<float>(FixedStep)); !Result)
		{
			return Result;
		}

		const auto Transform = World->GetBodyTransform(Cube);
		if (!Transform)
		{
			return std::unexpected(Transform.error());
		}

		Current = *Transform;
		Accumulator -= FixedStep;
	}

	const float Alpha = static_cast<float>(Accumulator / FixedStep);
	RenderTransform.Translation = Previous.Position * (1.f - Alpha) + Current.Position * Alpha;
	const auto& A = Previous.Rotation;
	const auto& B = Current.Rotation;
	const float Sign = A.X * B.X + A.Y * B.Y + A.Z * B.Z + A.W * B.W < 0.f ? -1.f : 1.f;
	RenderTransform.Rotation = FQuaternion{std::lerp(A.X, Sign * B.X, Alpha), std::lerp(A.Y, Sign * B.Y, Alpha), std::lerp(A.Z, Sign * B.Z, Alpha), std::lerp(A.W, Sign * B.W, Alpha)}.NormalizedOrIdentity();
	RenderTransform.Translation -= RenderTransform.Rotation.RotateVector(CubeOffset);
	return {};
}
}
