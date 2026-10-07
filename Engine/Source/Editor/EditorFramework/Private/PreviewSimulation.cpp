#include "PreviewSimulation.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>
#include <unordered_set>
#include <utility>

namespace Herta
{
namespace
{
[[nodiscard]] FVector3 Absolute(const FVector3& Vector)
{
	return {std::abs(Vector.X), std::abs(Vector.Y), std::abs(Vector.Z)};
}

[[nodiscard]] FVector3 GetMoverPosition(const FVector3& Start, const FPreviewMover& Mover, const double Time)
{
	const double Phase = 2.0 * std::numbers::pi * Time / Mover.PeriodSeconds;
	return Start + Mover.Offset * static_cast<float>(0.5 - 0.5 * std::cos(Phase));
}
}

std::expected<void, FPhysicsError> FPreviewSimulation::Start(const std::span<const FPreviewSimulationBody> Bodies, const std::span<const FPreviewSimulationSoftBody> SoftBodies, const FPhysicsWorldSettings& Settings)
{
	if (IsRunning())
	{
		return std::unexpected(FPhysicsError{"Preview simulation is already running"});
	}

	if (Bodies.empty() && SoftBodies.empty())
	{
		return std::unexpected(FPhysicsError{"Preview simulation requires at least one body"});
	}

	if (Bodies.size() + SoftBodies.size() > Settings.MaxBodies)
	{
		return std::unexpected(FPhysicsError{std::format("Preview requires {} bodies but MaxBodies={}", Bodies.size() + SoftBodies.size(), Settings.MaxBodies)});
	}

	auto NewWorld = FPhysicsWorld::Create(Settings);
	if (!NewWorld)
	{
		return std::unexpected(NewWorld.error());
	}

	std::vector<FBodyState> NewBodyStates;
	std::vector<FPreviewSimulationTransform> NewTransforms;
	std::vector<FVector3> NewHalfExtents;
	std::unordered_map<std::uint32_t, std::size_t> NewBodyIndices;
	std::unordered_set<std::size_t> ObjectIndices;
	NewBodyStates.reserve(Bodies.size());
	NewHalfExtents.reserve(Bodies.size());
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
		if (Body.Mover && (!std::isfinite(Body.Mover->PeriodSeconds) || Body.Mover->PeriodSeconds <= 0.f))
		{
			return std::unexpected(FPhysicsError{"Preview mover periods must be positive"});
		}

		const float Radius = Body.Collision == EPhysicsShape::Capsule ? std::max(HalfExtents.X, HalfExtents.Z) : std::max({HalfExtents.X, HalfExtents.Y, HalfExtents.Z});
		const EPhysicsMotionType MotionType = Body.Mover || Body.bTrigger ? EPhysicsMotionType::Kinematic : Body.MotionType;
		const EPhysicsShape Collision = Body.bTrigger ? EPhysicsShape::Box : Body.Collision;
		const auto Id = (*NewWorld)->CreateBody({.Shape = Collision, .HalfExtents = HalfExtents, .Radius = Radius, .HalfHeight = std::max(HalfExtents.Y - Radius, 0.f), .Position = Position, .Rotation = Body.Transform.Rotation, .MotionType = MotionType, .Properties = Body.Properties, .bSensor = Body.bTrigger});
		if (!Id)
		{
			return std::unexpected(Id.error());
		}

		const FPhysicsBodyTransform Transform{.Position = Position, .Rotation = Body.Transform.Rotation.NormalizedOrIdentity()};
		NewBodyStates.push_back({.Id = *Id, .MotionType = MotionType, .OriginalTransform = Body.Transform, .Offset = Offset, .Previous = Transform, .Current = Transform, .Mover = Body.Mover, .Start = Transform, .bTrigger = Body.bTrigger});
		NewBodyIndices.emplace(Id->Value, NewBodyStates.size() - 1);
		NewTransforms.push_back({.ObjectIndex = Body.ObjectIndex, .Transform = Body.Transform});
		NewHalfExtents.push_back(HalfExtents);
	}

	std::vector<FSoftBodyState> NewSoftBodyStates;
	NewSoftBodyStates.reserve(SoftBodies.size());
	for (const FPreviewSimulationSoftBody& SoftBody : SoftBodies)
	{
		if (!ObjectIndices.insert(SoftBody.ObjectIndex).second)
		{
			return std::unexpected(FPhysicsError{"Preview simulation contains a duplicate object"});
		}

		FPhysicsSoftBodySettings Physics = SoftBody.Settings;
		if (Physics.Attachment)
		{
			const auto Attached = SoftBody.AttachedObjectIndex ? std::ranges::find(NewTransforms, *SoftBody.AttachedObjectIndex, &FPreviewSimulationTransform::ObjectIndex) : NewTransforms.end();
			const auto BodyIndex = static_cast<std::size_t>(Attached - NewTransforms.begin());
			if (Attached == NewTransforms.end() || NewBodyStates[BodyIndex].MotionType != EPhysicsMotionType::Dynamic)
			{
				return std::unexpected(FPhysicsError{"Soft body attachments must reference a Dynamic body in the simulation"});
			}

			// Clamping into the box puts a rope authored just above it on the box's top face.
			const FPhysicsBodyTransform& Pose = NewBodyStates[BodyIndex].Current;
			const FVector3& HalfExtents = NewHalfExtents[BodyIndex];
			const FVector3 Local = Pose.Rotation.Conjugated().RotateVector(Physics.Attachment->Point - Pose.Position);
			const FVector3 Clamped{std::clamp(Local.X, -HalfExtents.X, HalfExtents.X), std::clamp(Local.Y, -HalfExtents.Y, HalfExtents.Y), std::clamp(Local.Z, -HalfExtents.Z, HalfExtents.Z)};
			Physics.Attachment->Body = NewBodyStates[BodyIndex].Id;
			Physics.Attachment->Point = Pose.Position + Pose.Rotation.RotateVector(Clamped);
		}

		const auto Id = (*NewWorld)->CreateSoftBody(Physics);
		if (!Id)
		{
			return std::unexpected(Id.error());
		}

		NewSoftBodyStates.push_back({.ObjectIndex = SoftBody.ObjectIndex, .Id = *Id, .Positions = {}});
	}

	World = std::move(*NewWorld);
	BodyStates = std::move(NewBodyStates);
	Transforms = std::move(NewTransforms);
	SoftBodyStates = std::move(NewSoftBodyStates);
	BodyIndices = std::move(NewBodyIndices);
	TriggerOccupancy.clear();
	TriggerEvents.clear();
	Accumulator = 0.0;
	Time = 0.0;
	return ReadSoftBodies();
}

std::expected<void, FPhysicsError> FPreviewSimulation::ReadSoftBodies()
{
	for (FSoftBodyState& SoftBody : SoftBodyStates)
	{
		if (const auto Result = World->GetSoftBodyVertices(SoftBody.Id, SoftBody.Positions); !Result)
		{
			return Result;
		}
	}

	++SoftBodyRevision;
	return {};
}

std::span<const FVector3> FPreviewSimulation::GetSoftBodyPositions(const std::size_t ObjectIndex) const noexcept
{
	const auto Found = std::ranges::find(SoftBodyStates, ObjectIndex, &FSoftBodyState::ObjectIndex);
	return Found == SoftBodyStates.end() ? std::span<const FVector3>{} : std::span<const FVector3>{Found->Positions};
}

std::uint64_t FPreviewSimulation::GetSoftBodyRevision() const noexcept
{
	return SoftBodyRevision;
}

bool FPreviewSimulation::IsTriggerOccupied(const std::size_t ObjectIndex) const noexcept
{
	const auto Found = TriggerOccupancy.find(ObjectIndex);
	return Found != TriggerOccupancy.end() && Found->second > 0;
}

std::vector<FPreviewTriggerEvent> FPreviewSimulation::TakeTriggerEvents()
{
	return std::exchange(TriggerEvents, {});
}

void FPreviewSimulation::Stop() noexcept
{
	if (!IsRunning())
	{
		return;
	}

	World.reset();
	SoftBodyStates.clear();
	BodyIndices.clear();
	TriggerOccupancy.clear();
	TriggerEvents.clear();
	++SoftBodyRevision;

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
	const bool bStepping = Accumulator >= FixedStep;
	while (Accumulator >= FixedStep)
	{
		Time += FixedStep;
		for (FBodyState& Body : BodyStates)
		{
			Body.Previous = Body.Current;
			if (Body.Mover)
			{
				const FPhysicsBodyTransform Target{.Position = GetMoverPosition(Body.Start.Position, *Body.Mover, Time), .Rotation = Body.Start.Rotation};
				if (const auto Result = World->MoveKinematicBody(Body.Id, Target, static_cast<float>(FixedStep)); !Result)
				{
					return Result;
				}
			}
		}

		if (const auto Result = World->Step(static_cast<float>(FixedStep)); !Result)
		{
			return Result;
		}

		for (const FPhysicsSensorEvent& Event : World->GetSensorEvents())
		{
			// Jolt sensors also see kinematic bodies, including movers; triggers only care about bodies physics moves.
			const auto Trigger = BodyIndices.find(Event.Sensor.Value);
			const auto Object = BodyIndices.find(Event.Body.Value);
			if (Trigger == BodyIndices.end() || Object == BodyIndices.end() || BodyStates[Object->second].MotionType != EPhysicsMotionType::Dynamic)
			{
				continue;
			}

			const std::size_t TriggerObject = Transforms[Trigger->second].ObjectIndex;
			std::size_t& Occupancy = TriggerOccupancy[TriggerObject];
			Occupancy = Event.bEntered ? Occupancy + 1 : Occupancy - std::min<std::size_t>(Occupancy, 1);
			TriggerEvents.push_back({.TriggerObjectIndex = TriggerObject, .ObjectIndex = Transforms[Object->second].ObjectIndex, .bEntered = Event.bEntered});
		}

		for (FBodyState& Body : BodyStates)
		{
			if (Body.MotionType == EPhysicsMotionType::Static || Body.bTrigger)
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

	// Soft bodies show the latest step without interpolation; their vertices change shape, not just pose.
	if (bStepping && !SoftBodyStates.empty())
	{
		if (const auto Result = ReadSoftBodies(); !Result)
		{
			return Result;
		}
	}

	const float Alpha = static_cast<float>(Accumulator / FixedStep);

	for (std::size_t Index = 0; Index < BodyStates.size(); ++Index)
	{
		const FBodyState& Body = BodyStates[Index];
		if (Body.MotionType == EPhysicsMotionType::Static || Body.bTrigger)
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
