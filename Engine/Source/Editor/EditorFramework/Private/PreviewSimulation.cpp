#include "PreviewSimulation.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <unordered_set>

namespace Herta
{
namespace
{
[[nodiscard]] FVector3 Absolute(const FVector3& Vector)
{
	return {std::abs(Vector.X), std::abs(Vector.Y), std::abs(Vector.Z)};
}
}

std::expected<void, FPhysicsError> FPreviewSimulation::Start(const std::span<const FPreviewSimulationBody> Bodies, const FPhysicsWorldSettings& Settings)
{
	if (IsRunning())
	{
		return std::unexpected(FPhysicsError{"Preview simulation is already running"});
	}

	if (Bodies.empty())
	{
		return std::unexpected(FPhysicsError{"Preview simulation requires at least one body"});
	}

	if (Bodies.size() > Settings.MaxBodies)
	{
		return std::unexpected(FPhysicsError{std::format("Preview requires {} bodies but MaxBodies={}", Bodies.size(), Settings.MaxBodies)});
	}

	auto NewWorld = FPhysicsWorld::Create(Settings);
	if (!NewWorld)
	{
		return std::unexpected(NewWorld.error());
	}

	std::vector<FBodyState> NewBodyStates;
	std::vector<FPreviewSimulationTransform> NewTransforms;
	std::unordered_set<std::size_t> ObjectIndices;
	NewBodyStates.reserve(Bodies.size());
	NewTransforms.reserve(Bodies.size());
	ObjectIndices.reserve(Bodies.size());

	for (const FPreviewSimulationBody& Body : Bodies)
	{
		if (!ObjectIndices.insert(Body.ObjectIndex).second)
		{
			return std::unexpected(FPhysicsError{"Preview simulation contains a duplicate object"});
		}

		if (Body.Shape.HalfExtents.X <= 0.f || Body.Shape.HalfExtents.Y <= 0.f || Body.Shape.HalfExtents.Z <= 0.f)
		{
			return std::unexpected(FPhysicsError{"Preview box half extents must be positive"});
		}

		const FVector3 HalfExtents = Absolute(Body.Transform.Scale3D.ComponentMultiply(Body.Shape.HalfExtents));
		const FVector3 Offset = Body.Transform.Scale3D.ComponentMultiply(Body.Shape.Center);
		const FVector3 Position = Body.Transform.Translation + Body.Transform.Rotation.RotateVector(Offset);
		const auto Id = (*NewWorld)->CreateBoxBody({.HalfExtents = HalfExtents, .Position = Position, .Rotation = Body.Transform.Rotation, .MotionType = Body.MotionType, .Properties = Body.Properties});
		if (!Id)
		{
			return std::unexpected(Id.error());
		}

		const FPhysicsBodyTransform Transform{.Position = Position, .Rotation = Body.Transform.Rotation.NormalizedOrIdentity()};
		NewBodyStates.push_back({.Id = *Id, .MotionType = Body.MotionType, .OriginalTransform = Body.Transform, .Offset = Offset, .Previous = Transform, .Current = Transform});
		NewTransforms.push_back({.ObjectIndex = Body.ObjectIndex, .Transform = Body.Transform});
	}

	World = std::move(*NewWorld);
	BodyStates = std::move(NewBodyStates);
	Transforms = std::move(NewTransforms);
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

	for (std::size_t Index = 0; Index < BodyStates.size(); ++Index)
	{
		Transforms[Index].Transform = BodyStates[Index].OriginalTransform;
	}

	Accumulator = 0.0;
}

bool FPreviewSimulation::IsRunning() const noexcept
{
	return World != nullptr;
}

std::span<const FPreviewSimulationTransform> FPreviewSimulation::GetTransforms() const noexcept
{
	return Transforms;
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
		for (FBodyState& Body : BodyStates)
		{
			Body.Previous = Body.Current;
		}

		if (const auto Result = World->Step(static_cast<float>(FixedStep)); !Result)
		{
			return Result;
		}

		for (FBodyState& Body : BodyStates)
		{
			if (Body.MotionType != EPhysicsMotionType::Dynamic)
			{
				continue;
			}

			const auto Transform = World->GetBodyTransform(Body.Id);
			if (!Transform)
			{
				return std::unexpected(Transform.error());
			}

			Body.Current = *Transform;
		}

		Accumulator -= FixedStep;
	}

	const float Alpha = static_cast<float>(Accumulator / FixedStep);

	for (std::size_t Index = 0; Index < BodyStates.size(); ++Index)
	{
		const FBodyState& Body = BodyStates[Index];
		if (Body.MotionType != EPhysicsMotionType::Dynamic)
		{
			continue;
		}

		FTransform& Transform = Transforms[Index].Transform;
		Transform.Translation = Body.Previous.Position * (1.f - Alpha) + Body.Current.Position * Alpha;
		const auto& A = Body.Previous.Rotation;
		const auto& B = Body.Current.Rotation;
		const float Sign = A.X * B.X + A.Y * B.Y + A.Z * B.Z + A.W * B.W < 0.f ? -1.f : 1.f;
		Transform.Rotation = FQuaternion{std::lerp(A.X, Sign * B.X, Alpha), std::lerp(A.Y, Sign * B.Y, Alpha), std::lerp(A.Z, Sign * B.Z, Alpha), std::lerp(A.W, Sign * B.W, Alpha)}.NormalizedOrIdentity();
		Transform.Translation -= Transform.Rotation.RotateVector(Body.Offset);
	}

	return {};
}
}
