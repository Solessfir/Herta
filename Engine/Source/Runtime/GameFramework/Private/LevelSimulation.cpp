#include "Herta/GameFramework/LevelSimulation.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>
#include <numeric>
#include <tuple>
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

[[nodiscard]] FVector3 GetMoverPosition(const FVector3& Start, const FLevelSimulationMover& Mover, const double Time)
{
	const double Phase = 2.0 * std::numbers::pi * Time / Mover.PeriodSeconds;
	return Start + Mover.Offset * static_cast<float>(0.5 - 0.5 * std::cos(Phase));
}

[[nodiscard]] FPhysicsBodyTransform Interpolate(const FPhysicsBodyTransform& Previous, const FPhysicsBodyTransform& Current, const float Alpha)
{
	const FQuaternion& A = Previous.Rotation;
	const FQuaternion& B = Current.Rotation;
	const float Sign = A.X * B.X + A.Y * B.Y + A.Z * B.Z + A.W * B.W < 0.f ? -1.f : 1.f;
	return {.Position = Previous.Position * (1.f - Alpha) + Current.Position * Alpha,
	    .Rotation = FQuaternion{std::lerp(A.X, Sign * B.X, Alpha), std::lerp(A.Y, Sign * B.Y, Alpha), std::lerp(A.Z, Sign * B.Z, Alpha), std::lerp(A.W, Sign * B.W, Alpha)}.NormalizedOrIdentity()};
}

[[nodiscard]] FTransform DestructibleOrigin(const FLevelSimulationDestructible& Destructible, const FPhysicsBodyTransform& Body)
{
	return {Body.Position + Body.Rotation.RotateVector(Destructible.Offset), Body.Rotation, Destructible.Scale};
}

// Union-find over the chunks of one island, joined by the bonds that still hold.
std::vector<std::vector<std::uint32_t>> FindIslands(const FFracture& Fracture, const std::vector<bool>& BondIntact, const std::vector<std::uint32_t>& Chunks)
{
	std::vector<std::uint32_t> Parent(Fracture.Chunks.size());
	std::iota(Parent.begin(), Parent.end(), 0u);
	const auto Root = [&Parent](std::uint32_t Chunk)
	{
		while (Parent[Chunk] != Chunk)
		{
			Parent[Chunk] = Parent[Parent[Chunk]];
			Chunk = Parent[Chunk];
		}

		return Chunk;
	};

	std::vector<bool> Member(Fracture.Chunks.size(), false);
	for (const std::uint32_t Chunk : Chunks)
	{
		Member[Chunk] = true;
	}

	for (std::size_t Bond = 0; Bond < Fracture.Bonds.size(); ++Bond)
	{
		const FFractureBond& Link = Fracture.Bonds[Bond];
		if (BondIntact[Bond] && Member[Link.First] && Member[Link.Second])
		{
			// The smaller root wins, so islands come out ordered by their lowest chunk whatever the bond order.
			const std::uint32_t A = Root(Link.First);
			const std::uint32_t B = Root(Link.Second);
			Parent[std::max(A, B)] = std::min(A, B);
		}
	}

	std::vector<std::vector<std::uint32_t>> Islands;
	std::vector<std::size_t> IslandOfRoot(Fracture.Chunks.size(), SIZE_MAX);
	for (const std::uint32_t Chunk : Chunks)
	{
		const std::uint32_t Top = Root(Chunk);
		if (IslandOfRoot[Top] == SIZE_MAX)
		{
			IslandOfRoot[Top] = Islands.size();
			Islands.emplace_back();
		}

		Islands[IslandOfRoot[Top]].push_back(Chunk);
	}

	return Islands;
}
}

std::expected<void, FPhysicsError> FLevelSimulation::Start(const std::span<const FLevelSimulationBody> Bodies, const std::span<const FLevelSimulationSoftBody> SoftBodies, const std::span<const FLevelSimulationConstraint> Constraints, const FPhysicsWorldSettings& Settings, const std::span<const FLevelSimulationDestructible> InDestructibles, const FLevelDestructionSettings& Destruction)
{
	if (IsRunning())
	{
		return std::unexpected(FPhysicsError{"Level simulation is already running"});
	}

	if (Bodies.empty() && SoftBodies.empty() && InDestructibles.empty())
	{
		return std::unexpected(FPhysicsError{"Level simulation requires at least one body"});
	}

	std::size_t RequiredBodies = Bodies.size() + InDestructibles.size();
	for (const FLevelSimulationSoftBody& SoftBody : SoftBodies)
	{
		RequiredBodies += SoftBody.Rope ? SoftBody.Rope->Segments : 1;
	}

	if (RequiredBodies > Settings.MaxBodies)
	{
		return std::unexpected(FPhysicsError{std::format("Level simulation requires {} bodies but MaxBodies={}", RequiredBodies, Settings.MaxBodies)});
	}

	auto NewWorld = FPhysicsWorld::Create(Settings);
	if (!NewWorld)
	{
		return std::unexpected(NewWorld.error());
	}

	std::vector<FBodyState> NewBodyStates;
	std::vector<FLevelSimulationTransform> NewTransforms;
	std::unordered_map<std::uint32_t, std::size_t> NewBodyIndices;
	std::unordered_set<std::size_t> ObjectIndices;
	NewBodyStates.reserve(Bodies.size());
	NewTransforms.reserve(Bodies.size());
	ObjectIndices.reserve(Bodies.size());

	for (const FLevelSimulationBody& Body : Bodies)
	{
		if (!ObjectIndices.insert(Body.ObjectIndex).second)
		{
			return std::unexpected(FPhysicsError{"Level simulation contains a duplicate object"});
		}

		if (Body.Shape.HalfExtents.X <= 0.f || Body.Shape.HalfExtents.Y <= 0.f || Body.Shape.HalfExtents.Z <= 0.f)
		{
			return std::unexpected(FPhysicsError{"Simulated box half extents must be positive"});
		}

		const FVector3 HalfExtents = Absolute(Body.Transform.Scale3D.ComponentMultiply(Body.Shape.HalfExtents));
		const FVector3 Offset = Body.Transform.Scale3D.ComponentMultiply(Body.Shape.Center);
		const FVector3 Position = Body.Transform.Translation + Body.Transform.Rotation.RotateVector(Offset);
		if (Body.Mover && (!std::isfinite(Body.Mover->PeriodSeconds) || Body.Mover->PeriodSeconds <= 0.f))
		{
			return std::unexpected(FPhysicsError{"Mover periods must be positive"});
		}

		const float Radius = Body.Collision == EPhysicsShape::Capsule ? std::max(HalfExtents.X, HalfExtents.Z) : std::max({HalfExtents.X, HalfExtents.Y, HalfExtents.Z});
		const EPhysicsMotionType MotionType = Body.Mover || Body.bTrigger ? EPhysicsMotionType::Kinematic : Body.MotionType;
		const EPhysicsShape Collision = Body.bTrigger ? EPhysicsShape::Box : Body.Collision;
		const auto Id = (*NewWorld)->CreateBody({.Shape = Collision, .HalfExtents = HalfExtents, .Radius = Radius, .HalfHeight = std::max(HalfExtents.Y - Radius, 0.f), .Position = Position, .Rotation = Body.Transform.Rotation, .MotionType = MotionType, .Properties = Body.Properties, .bSensor = Body.bTrigger, .bReportContacts = MotionType == EPhysicsMotionType::Dynamic});
		if (!Id)
		{
			return std::unexpected(Id.error());
		}

		const FPhysicsBodyTransform Transform{.Position = Position, .Rotation = Body.Transform.Rotation.NormalizedOrIdentity()};
		NewBodyStates.push_back({.Id = *Id, .MotionType = MotionType, .OriginalTransform = Body.Transform, .Offset = Offset, .Previous = Transform, .Current = Transform, .Mover = Body.Mover, .Start = Transform, .bTrigger = Body.bTrigger});
		NewBodyIndices.emplace(Id->Value, NewBodyStates.size() - 1);
		NewTransforms.push_back({.ObjectIndex = Body.ObjectIndex, .Transform = Body.Transform});
	}

	std::vector<FSoftBodyState> NewSoftBodyStates;
	NewSoftBodyStates.reserve(SoftBodies.size());
	for (const FLevelSimulationSoftBody& SoftBody : SoftBodies)
	{
		if (!ObjectIndices.insert(SoftBody.ObjectIndex).second)
		{
			return std::unexpected(FPhysicsError{"Level simulation contains a duplicate object"});
		}

		if (SoftBody.Rope)
		{
			const auto Rope = (*NewWorld)->CreateRope(*SoftBody.Rope);
			if (!Rope)
			{
				return std::unexpected(Rope.error());
			}

			NewSoftBodyStates.push_back({.ObjectIndex = SoftBody.ObjectIndex, .Rope = *Rope, .Positions = std::vector<FVector3>(SoftBody.Settings.Vertices.size())});
			continue;
		}

		const auto Id = (*NewWorld)->CreateSoftBody(SoftBody.Settings);
		if (!Id)
		{
			return std::unexpected(Id.error());
		}

		NewSoftBodyStates.push_back({.ObjectIndex = SoftBody.ObjectIndex, .Id = *Id, .Positions = {}});
	}

	std::vector<FDestructibleState> NewDestructibles;
	std::unordered_map<std::uint32_t, std::pair<std::size_t, std::size_t>> NewIslandBodies;
	for (const FLevelSimulationDestructible& Destructible : InDestructibles)
	{
		if (!ObjectIndices.insert(Destructible.ObjectIndex).second)
		{
			return std::unexpected(FPhysicsError{"Level simulation contains a duplicate object"});
		}

		if (!Destructible.Fracture || Destructible.Fracture->Chunks.size() < 2 || !std::isfinite(Destructible.Strength) || Destructible.Strength <= 0.f)
		{
			return std::unexpected(FPhysicsError{"Destructibles need a fracture of at least two chunks and a positive strength"});
		}

		FDestructibleState State{.Settings = Destructible, .TransformIndex = NewTransforms.size(), .BondIntact = std::vector<bool>(Destructible.Fracture->Bonds.size(), true)};
		float Volume = 0.f;
		std::vector<std::uint32_t> All(Destructible.Fracture->Chunks.size());
		std::iota(All.begin(), All.end(), 0u);
		for (const FFractureChunk& Chunk : Destructible.Fracture->Chunks)
		{
			Volume += Chunk.Volume;
		}

		State.Density = Destructible.Properties.MassKg / std::max(Volume, 1e-6f);
		State.ChunkSize = std::cbrt(Volume / static_cast<float>(All.size()));
		const FPhysicsBodyTransform Pose{.Position = Destructible.Position, .Rotation = Destructible.Rotation.NormalizedOrIdentity()};
		const EPhysicsMotionType MotionType = Destructible.bAnchored ? EPhysicsMotionType::Static : EPhysicsMotionType::Dynamic;
		// Whole, the object is its box, which is cheaper than its chunks and identical in collision.
		const auto Id = (*NewWorld)->CreateBody({.HalfExtents = Destructible.Fracture->HalfExtents, .Position = Pose.Position, .Rotation = Pose.Rotation, .MotionType = MotionType, .Properties = Destructible.Properties, .bReportContacts = true});
		if (!Id)
		{
			return std::unexpected(Id.error());
		}

		State.Islands.push_back({.Id = *Id, .Chunks = std::move(All), .MotionType = MotionType, .Previous = Pose, .Current = Pose});
		NewIslandBodies.emplace(Id->Value, std::pair{NewDestructibles.size(), std::size_t{0}});
		NewTransforms.push_back({.ObjectIndex = Destructible.ObjectIndex, .Transform = DestructibleOrigin(Destructible, Pose)});
		NewDestructibles.push_back(std::move(State));
	}

	for (const FLevelSimulationConstraint& Constraint : Constraints)
	{
		const auto Body = std::ranges::find(NewTransforms, Constraint.ObjectIndex, &FLevelSimulationTransform::ObjectIndex);
		if (Body == NewTransforms.end() || static_cast<std::size_t>(Body - NewTransforms.begin()) >= NewBodyStates.size() || NewBodyStates[static_cast<std::size_t>(Body - NewTransforms.begin())].MotionType != EPhysicsMotionType::Dynamic)
		{
			return std::unexpected(FPhysicsError{"Constraints must belong to a Dynamic body in the simulation"});
		}

		std::optional<FPhysicsBodyId> Other;
		if (Constraint.TargetObjectIndex)
		{
			const auto Target = std::ranges::find(NewTransforms, *Constraint.TargetObjectIndex, &FLevelSimulationTransform::ObjectIndex);
			const auto Rope = std::ranges::find(NewSoftBodyStates, *Constraint.TargetObjectIndex, &FSoftBodyState::ObjectIndex);
			if (Target != NewTransforms.end() && static_cast<std::size_t>(Target - NewTransforms.begin()) < NewBodyStates.size())
			{
				Other = NewBodyStates[static_cast<std::size_t>(Target - NewTransforms.begin())].Id;
			}
			else if (Rope != NewSoftBodyStates.end() && Rope->Rope)
			{
				Other = Rope->Rope->Last;
			}
			else
			{
				return std::unexpected(FPhysicsError{"Constraint targets must be a rigid body or a rope in the simulation"});
			}
		}

		const auto Joined = (*NewWorld)->CreatePointConstraint({.Body = NewBodyStates[static_cast<std::size_t>(Body - NewTransforms.begin())].Id, .Other = Other, .Point = Constraint.Point});
		if (!Joined)
		{
			return Joined;
		}
	}

	World = std::move(*NewWorld);
	BodyStates = std::move(NewBodyStates);
	Transforms = std::move(NewTransforms);
	SoftBodyStates = std::move(NewSoftBodyStates);
	BodyIndices = std::move(NewBodyIndices);
	Destructibles = std::move(NewDestructibles);
	IslandBodies = std::move(NewIslandBodies);
	DestructionSettings = Destruction;
	TriggerOccupancy.clear();
	TriggerEvents.clear();
	SpawnStates.clear();
	FreeSpawnSlots.clear();
	Spawned.clear();
	Pieces.clear();
	BreakEvents.clear();
	ImpactEvents.clear();
	StaticGroups.clear();
	DebrisCount = 0;
	Accumulator = 0.0;
	Time = 0.0;
	return ReadSoftBodies();
}

std::expected<void, FPhysicsError> FLevelSimulation::ReadSoftBodies()
{
	for (FSoftBodyState& SoftBody : SoftBodyStates)
	{
		if (!SoftBody.Rope)
		{
			if (const auto Result = World->GetSoftBodyVertices(SoftBody.Id, SoftBody.Positions); !Result)
			{
				return Result;
			}

			continue;
		}

		if (const auto Result = World->GetRopePoints(*SoftBody.Rope, RopePoints); !Result)
		{
			return Result;
		}

		// Rope vertices are evenly spaced like the chain's joints, so each falls at the same fraction along the chain.
		const std::size_t Last = SoftBody.Positions.size() - 1;
		for (std::size_t Index = 0; Index <= Last; ++Index)
		{
			const float Along = static_cast<float>(Index) / static_cast<float>(Last) * static_cast<float>(RopePoints.size() - 1);
			const std::size_t Segment = std::min(static_cast<std::size_t>(Along), RopePoints.size() - 2);
			const float Fraction = Along - static_cast<float>(Segment);
			SoftBody.Positions[Index] = RopePoints[Segment] + (RopePoints[Segment + 1] - RopePoints[Segment]) * Fraction;
		}
	}

	++SoftBodyRevision;
	return {};
}

std::span<const FVector3> FLevelSimulation::GetSoftBodyPositions(const std::size_t ObjectIndex) const noexcept
{
	const auto Found = std::ranges::find(SoftBodyStates, ObjectIndex, &FSoftBodyState::ObjectIndex);
	return Found == SoftBodyStates.end() ? std::span<const FVector3>{} : std::span<const FVector3>{Found->Positions};
}

std::uint64_t FLevelSimulation::GetSoftBodyRevision() const noexcept
{
	return SoftBodyRevision;
}

bool FLevelSimulation::IsTriggerOccupied(const std::size_t ObjectIndex) const noexcept
{
	const auto Found = TriggerOccupancy.find(ObjectIndex);
	return Found != TriggerOccupancy.end() && Found->second > 0;
}

std::vector<FLevelTriggerEvent> FLevelSimulation::TakeTriggerEvents()
{
	return std::exchange(TriggerEvents, {});
}

void FLevelSimulation::Stop() noexcept
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

	for (const FDestructibleState& State : Destructibles)
	{
		Transforms[State.TransformIndex].Transform = DestructibleOrigin(State.Settings, {.Position = State.Settings.Position, .Rotation = State.Settings.Rotation.NormalizedOrIdentity()});
	}

	Destructibles.clear();
	IslandBodies.clear();
	SpawnStates.clear();
	FreeSpawnSlots.clear();
	Spawned.clear();
	Pieces.clear();
	BreakEvents.clear();
	ImpactEvents.clear();
	StaticGroups.clear();
	DebrisCount = 0;

	Accumulator = 0.0;
}

bool FLevelSimulation::IsRunning() const noexcept
{
	return World != nullptr;
}

std::span<const FLevelSimulationTransform> FLevelSimulation::GetTransforms() const noexcept
{
	return Transforms;
}

FPhysicsWorld* FLevelSimulation::GetPhysicsWorld() noexcept
{
	return World.get();
}

float FLevelSimulation::GetInterpolationAlpha() const noexcept
{
	return static_cast<float>(Accumulator / LevelFixedStep);
}

std::expected<void, FPhysicsError> FLevelSimulation::Update(const float DeltaSeconds, const FFixedStepCallback& OnFixedStep)
{
	if (!std::isfinite(DeltaSeconds) || DeltaSeconds < 0.f)
	{
		return std::unexpected(FPhysicsError{"Simulation frame delta must be finite and nonnegative"});
	}

	if (!IsRunning())
	{
		return {};
	}

	constexpr double FixedStep = LevelFixedStep;
	// Bound catch-up after a paused debugger or a stalled frame.
	Accumulator += std::min(static_cast<double>(DeltaSeconds), 0.25);
	const bool bStepping = Accumulator >= FixedStep;
	while (Accumulator >= FixedStep)
	{
		Time += FixedStep;
		for (FSpawnState& Spawn : SpawnStates)
		{
			Spawn.Previous = Spawn.Current;
		}

		for (FDestructibleState& State : Destructibles)
		{
			for (FIslandState& Island : State.Islands)
			{
				Island.Previous = Island.Current;
			}
		}

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

		if (OnFixedStep)
		{
			if (const auto Result = OnFixedStep(*World, static_cast<float>(FixedStep)); !Result)
			{
				return Result;
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

		// Contact events arrive sorted by body, so breaks happen in the same order on every machine and thread count.
		for (const FPhysicsContactEvent& Event : World->GetContactEvents())
		{
			// Bounded, so a pile settling at once cannot grow the list without limit between reads.
			if (Event.Phase == EPhysicsContactPhase::Began && Event.Impulse > 0.f && ImpactEvents.size() < 256)
			{
				ImpactEvents.push_back({.Point = Event.Point, .Normal = Event.Normal, .Impulse = Event.Impulse});
			}

			for (const FPhysicsBodyId Body : {Event.First, Event.Second})
			{
				const auto Island = Event.Phase == EPhysicsContactPhase::Began ? IslandBodies.find(Body.Value) : IslandBodies.end();
				if (Island != IslandBodies.end())
				{
					if (const auto Result = Damage(Island->second.first, Island->second.second, Event.Point, Event.Impulse); !Result)
					{
						return Result;
					}
				}
			}
		}

		for (std::size_t Index = 0; Index < Destructibles.size(); ++Index)
		{
			if (Destructibles[Index].bDirty)
			{
				if (const auto Result = SplitIslands(Index, Destructibles[Index].Impact, 0.f); !Result)
				{
					return Result;
				}
			}
		}

		if (const auto Result = ExpireDebris(); !Result)
		{
			return Result;
		}

		for (FSpawnState& Spawn : SpawnStates)
		{
			if (Spawn.bAlive)
			{
				if (const auto Transform = World->GetBodyTransform(Spawn.Id))
				{
					Spawn.Current = *Transform;
				}
			}
		}

		for (FDestructibleState& State : Destructibles)
		{
			for (FIslandState& Island : State.Islands)
			{
				if (Island.bAlive && Island.MotionType == EPhysicsMotionType::Dynamic)
				{
					if (const auto Transform = World->GetBodyTransform(Island.Id))
					{
						Island.Current = *Transform;
					}
				}
			}
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

	const float Alpha = GetInterpolationAlpha();

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

	Spawned.clear();
	for (std::uint32_t Index = 0; Index < SpawnStates.size(); ++Index)
	{
		const FSpawnState& Spawn = SpawnStates[Index];
		if (Spawn.bAlive)
		{
			Spawned.push_back({.Handle = {.Index = Index, .Generation = Spawn.Generation}, .Transform = Interpolate(Spawn.Previous, Spawn.Current, Alpha)});
		}
	}

	Pieces.clear();
	for (const FDestructibleState& State : Destructibles)
	{
		if (State.bIntact)
		{
			if (State.Islands.front().MotionType == EPhysicsMotionType::Dynamic)
			{
				Transforms[State.TransformIndex].Transform = DestructibleOrigin(State.Settings, Interpolate(State.Islands.front().Previous, State.Islands.front().Current, Alpha));
			}

			continue;
		}

		for (const FIslandState& Island : State.Islands)
		{
			if (!Island.bAlive)
			{
				continue;
			}

			const FPhysicsBodyTransform Pose = Island.MotionType == EPhysicsMotionType::Dynamic ? Interpolate(Island.Previous, Island.Current, Alpha) : Island.Current;
			for (const std::uint32_t Chunk : Island.Chunks)
			{
				Pieces.push_back({.ObjectIndex = State.Settings.ObjectIndex, .Chunk = Chunk, .Transform = Pose});
			}
		}
	}

	return {};
}

std::expected<void, FPhysicsError> FLevelSimulation::Damage(const std::size_t Destructible, const std::size_t IslandIndex, const FVector3& Point, const float Impulse)
{
	FDestructibleState& State = Destructibles[Destructible];
	const FIslandState& Island = State.Islands[IslandIndex];
	if (!Island.bAlive || Island.Chunks.size() < 2 || Impulse < State.Settings.Strength)
	{
		return {};
	}

	const auto Pose = World->GetBodyTransform(Island.Id);
	if (!Pose)
	{
		return std::unexpected(Pose.error());
	}

	const FFracture& Fracture = *State.Settings.Fracture;
	const FVector3 Local = Pose->Rotation.Conjugated().RotateVector(Point - Pose->Position);
	// Harder hits reach further: the radius grows with the square root of how far the impulse exceeds the strength, up to three chunks.
	const float Radius = State.ChunkSize * std::min(std::sqrt(Impulse / State.Settings.Strength), 3.f);
	std::vector<bool> Member(Fracture.Chunks.size(), false);
	for (const std::uint32_t Chunk : Island.Chunks)
	{
		Member[Chunk] = true;
	}

	std::uint32_t Broken = 0;
	for (std::size_t Bond = 0; Bond < Fracture.Bonds.size(); ++Bond)
	{
		const FFractureBond& Link = Fracture.Bonds[Bond];
		if (State.BondIntact[Bond] && Member[Link.First] && Member[Link.Second] && (Link.Center - Local).Length() <= Radius)
		{
			State.BondIntact[Bond] = false;
			++Broken;
		}
	}

	// A hit between bonds still frees the chunk it struck.
	if (Broken == 0)
	{
		const std::uint32_t Nearest = *std::ranges::min_element(Island.Chunks, {}, [&](const std::uint32_t Chunk)
		{
			return std::pair((Fracture.Chunks[Chunk].Centroid - Local).Length(), Chunk);
		});

		for (std::size_t Bond = 0; Bond < Fracture.Bonds.size(); ++Bond)
		{
			const FFractureBond& Link = Fracture.Bonds[Bond];
			if (State.BondIntact[Bond] && (Link.First == Nearest || Link.Second == Nearest) && Member[Link.First] && Member[Link.Second])
			{
				State.BondIntact[Bond] = false;
				++Broken;
			}
		}
	}

	if (Broken != 0)
	{
		State.bDirty = true;
		State.Impact = Point;
		BreakEvents.push_back({.ObjectIndex = State.Settings.ObjectIndex, .Point = Point, .BrokenBonds = Broken});
	}

	return {};
}

std::expected<FPhysicsBodyId, FPhysicsError> FLevelSimulation::CreateIsland(const FDestructibleState& State, const std::vector<std::uint32_t>& Chunks, const EPhysicsMotionType MotionType, const FPhysicsBodyTransform& Transform)
{
	const FFracture& Fracture = *State.Settings.Fracture;
	std::vector<std::span<const FVector3>> Hulls;
	float Volume = 0.f;
	for (const std::uint32_t Chunk : Chunks)
	{
		Hulls.emplace_back(Fracture.Chunks[Chunk].Points);
		Volume += Fracture.Chunks[Chunk].Volume;
	}

	FPhysicsBodyProperties Properties = State.Settings.Properties;
	Properties.MassKg = std::clamp(State.Density * Volume, 0.001f, 1000000.f);
	const bool bSingle = Chunks.size() == 1;
	// A lone chunk cannot break further, so it neither reports contacts nor blocks other debris or the player.
	return World->CreateBody({
	    .Shape = bSingle ? EPhysicsShape::ConvexHull : EPhysicsShape::Compound,
	    .Position = Transform.Position,
	    .Rotation = Transform.Rotation,
	    .MotionType = MotionType,
	    .Properties = Properties,
	    .bReportContacts = !bSingle,
	    .Layer = bSingle ? PhysicsLayerDebris : PhysicsLayerDynamic,
	    .HullPoints = bSingle ? Hulls.front() : std::span<const FVector3>{},
	    .CompoundHulls = bSingle ? std::span<const std::span<const FVector3>>{} : std::span<const std::span<const FVector3>>(Hulls),
	});
}

std::expected<void, FPhysicsError> FLevelSimulation::SplitIslands(const std::size_t Destructible, const FVector3& Impact, float)
{
	FDestructibleState& State = Destructibles[Destructible];
	State.bDirty = false;
	const FFracture& Fracture = *State.Settings.Fracture;
	const std::size_t Existing = State.Islands.size();
	for (std::size_t IslandIndex = 0; IslandIndex < Existing; ++IslandIndex)
	{
		if (!State.Islands[IslandIndex].bAlive || State.Islands[IslandIndex].Chunks.size() < 2)
		{
			continue;
		}

		std::vector<std::vector<std::uint32_t>> Parts = FindIslands(Fracture, State.BondIntact, State.Islands[IslandIndex].Chunks);
		if (Parts.size() < 2)
		{
			continue;
		}

		const FIslandState Old = State.Islands[IslandIndex];
		const auto Pose = World->GetBodyTransform(Old.Id);
		const auto Velocity = Old.MotionType == EPhysicsMotionType::Dynamic ? World->GetLinearVelocity(Old.Id) : std::expected<FVector3, FPhysicsError>(FVector3::Zero());
		if (!Pose || !Velocity)
		{
			return std::unexpected(!Pose ? Pose.error() : Velocity.error());
		}

		if (const auto Destroyed = World->DestroyBody(Old.Id); !Destroyed)
		{
			return Destroyed;
		}

		IslandBodies.erase(Old.Id.Value);
		State.Islands[IslandIndex].bAlive = false;
		State.bIntact = false;
		for (std::vector<std::uint32_t>& Part : Parts)
		{
			const bool bOnFooting = std::ranges::any_of(Part, [&Fracture](const std::uint32_t Chunk)
			{
				return Fracture.Chunks[Chunk].bTouchesBottom;
			});

			// Pieces still standing on the footing of an anchored object stay put; everything else falls.
			const EPhysicsMotionType MotionType = State.Settings.bAnchored && Old.MotionType == EPhysicsMotionType::Static && bOnFooting ? EPhysicsMotionType::Static : EPhysicsMotionType::Dynamic;
			const auto Id = CreateIsland(State, Part, MotionType, *Pose);
			if (!Id)
			{
				return std::unexpected(Id.error());
			}

			if (MotionType == EPhysicsMotionType::Dynamic)
			{
				// Freed pieces keep the motion of what they broke from, plus a push away from the impact that fades over a few chunks.
				FVector3 Center = FVector3::Zero();
				for (const std::uint32_t Chunk : Part)
				{
					Center = Center + Fracture.Chunks[Chunk].Centroid;
				}

				const FVector3 World3 = Pose->Position + Pose->Rotation.RotateVector(Center * (1.f / static_cast<float>(Part.size())));
				const FVector3 Away = World3 - Impact;
				const float Distance = Away.Length();
				const float Push = 3.f * std::max(0.f, 1.f - Distance / (3.f * State.ChunkSize));
				const FVector3 Kick = Distance > 1e-4f ? Away * (Push / Distance) : FVector3::Zero();
				if (const auto Moved = World->SetVelocity(*Id, *Velocity + Kick); !Moved)
				{
					return Moved;
				}
			}

			const bool bDebris = MotionType == EPhysicsMotionType::Dynamic && Part.size() == 1;
			IslandBodies.emplace(Id->Value, std::pair{Destructible, State.Islands.size()});
			State.Islands.push_back({.Id = *Id, .Chunks = std::move(Part), .MotionType = MotionType, .Previous = Old.Previous, .Current = *Pose, .DebrisSince = bDebris ? std::optional(Time) : std::nullopt});
		}
	}

	return {};
}

std::expected<void, FPhysicsError> FLevelSimulation::ExpireDebris()
{
	std::vector<std::tuple<double, std::size_t, std::size_t>> Debris;
	for (std::size_t Destructible = 0; Destructible < Destructibles.size(); ++Destructible)
	{
		for (std::size_t Island = 0; Island < Destructibles[Destructible].Islands.size(); ++Island)
		{
			const FIslandState& State = Destructibles[Destructible].Islands[Island];
			if (State.bAlive && State.DebrisSince)
			{
				Debris.emplace_back(*State.DebrisSince, Destructible, Island);
			}
		}
	}

	// Oldest first, with ties broken by position in the level, so budget despawns are the same everywhere.
	std::ranges::sort(Debris);
	const std::size_t Budget = DestructionSettings.MaxDebris;
	std::size_t Remaining = Debris.size();
	for (const auto& [Since, Destructible, Island] : Debris)
	{
		if (Remaining <= Budget && Time - Since < DestructionSettings.DebrisLifetimeSeconds)
		{
			continue;
		}

		FIslandState& State = Destructibles[Destructible].Islands[Island];
		if (const auto Destroyed = World->DestroyBody(State.Id); !Destroyed)
		{
			return Destroyed;
		}

		IslandBodies.erase(State.Id.Value);
		State.bAlive = false;
		--Remaining;
	}

	DebrisCount = Remaining;
	return {};
}

std::expected<FLevelSimulationHandle, FPhysicsError> FLevelSimulation::Spawn(const FPhysicsBodySettings& Body)
{
	if (!IsRunning())
	{
		return std::unexpected(FPhysicsError{"Spawning needs a running simulation"});
	}

	if (Body.MotionType == EPhysicsMotionType::Static)
	{
		return std::unexpected(FPhysicsError{"Spawned bodies move; static scenery belongs to the level"});
	}

	const auto Id = World->CreateBody(Body);
	if (!Id)
	{
		return std::unexpected(Id.error());
	}

	std::uint32_t Index = 0;
	if (FreeSpawnSlots.empty())
	{
		Index = static_cast<std::uint32_t>(SpawnStates.size());
		SpawnStates.emplace_back();
	}
	else
	{
		Index = FreeSpawnSlots.back();
		FreeSpawnSlots.pop_back();
	}

	FSpawnState& State = SpawnStates[Index];
	const FPhysicsBodyTransform Pose{.Position = Body.Position, .Rotation = Body.Rotation.NormalizedOrIdentity()};
	State.Id = *Id;
	State.bAlive = true;
	State.Previous = Pose;
	State.Current = Pose;
	return FLevelSimulationHandle{.Index = Index, .Generation = State.Generation};
}

std::expected<void, FPhysicsError> FLevelSimulation::Despawn(const FLevelSimulationHandle Handle)
{
	if (Handle.Index >= SpawnStates.size() || !SpawnStates[Handle.Index].bAlive || SpawnStates[Handle.Index].Generation != Handle.Generation)
	{
		return std::unexpected(FPhysicsError{"Unknown or despawned body"});
	}

	FSpawnState& State = SpawnStates[Handle.Index];
	if (const auto Destroyed = World->DestroyBody(State.Id); !Destroyed)
	{
		return Destroyed;
	}

	State.bAlive = false;
	++State.Generation;
	FreeSpawnSlots.push_back(Handle.Index);
	std::erase_if(Spawned, [Handle](const FLevelSimulationSpawn& Spawn)
	{
		return Spawn.Handle == Handle;
	});

	return {};
}

std::optional<FPhysicsBodyId> FLevelSimulation::FindSpawnedBody(const FLevelSimulationHandle Handle) const noexcept
{
	if (Handle.Index < SpawnStates.size() && SpawnStates[Handle.Index].bAlive && SpawnStates[Handle.Index].Generation == Handle.Generation)
	{
		return SpawnStates[Handle.Index].Id;
	}

	return std::nullopt;
}

std::span<const FLevelSimulationSpawn> FLevelSimulation::GetSpawned() const noexcept
{
	return Spawned;
}

bool FLevelSimulation::IsIntact(const std::size_t ObjectIndex) const noexcept
{
	const auto Found = std::ranges::find(Destructibles, ObjectIndex, [](const FDestructibleState& State)
	{
		return State.Settings.ObjectIndex;
	});

	return Found == Destructibles.end() || Found->bIntact;
}

std::span<const FLevelSimulationPiece> FLevelSimulation::GetPieces() const noexcept
{
	return Pieces;
}

std::size_t FLevelSimulation::GetDebrisCount() const noexcept
{
	return DebrisCount;
}

std::vector<FLevelBreakEvent> FLevelSimulation::TakeBreakEvents()
{
	return std::exchange(BreakEvents, {});
}

std::expected<FLevelStaticGroup, FPhysicsError> FLevelSimulation::AddStaticBodies(const std::span<const FLevelSimulationBody> Bodies)
{
	if (!IsRunning())
	{
		return std::unexpected(FPhysicsError{"Adding static bodies needs a running simulation"});
	}

	std::vector<FPhysicsBodyId> Created;
	const auto Undo = [this, &Created]
	{
		for (const FPhysicsBodyId Body : Created)
		{
			(void)World->DestroyBody(Body);
		}
	};

	for (const FLevelSimulationBody& Body : Bodies)
	{
		if (Body.MotionType != EPhysicsMotionType::Static || Body.Mover || Body.bTrigger || Body.Shape.HalfExtents.X <= 0.f || Body.Shape.HalfExtents.Y <= 0.f || Body.Shape.HalfExtents.Z <= 0.f)
		{
			Undo();
			return std::unexpected(FPhysicsError{"Static groups hold only static bodies with positive extents"});
		}

		const FVector3 HalfExtents = Absolute(Body.Transform.Scale3D.ComponentMultiply(Body.Shape.HalfExtents));
		const FVector3 Offset = Body.Transform.Scale3D.ComponentMultiply(Body.Shape.Center);
		const float Radius = Body.Collision == EPhysicsShape::Capsule ? std::max(HalfExtents.X, HalfExtents.Z) : std::max({HalfExtents.X, HalfExtents.Y, HalfExtents.Z});
		const auto Id = World->CreateBody({.Shape = Body.Collision, .HalfExtents = HalfExtents, .Radius = Radius, .HalfHeight = std::max(HalfExtents.Y - Radius, 0.f), .Position = Body.Transform.Translation + Body.Transform.Rotation.RotateVector(Offset), .Rotation = Body.Transform.Rotation, .MotionType = EPhysicsMotionType::Static, .Properties = Body.Properties});
		if (!Id)
		{
			Undo();
			return std::unexpected(Id.error());
		}

		Created.push_back(*Id);
	}

	const FLevelStaticGroup Group{++NextStaticGroup};
	StaticGroups.emplace(Group.Value, std::move(Created));
	return Group;
}

std::expected<void, FPhysicsError> FLevelSimulation::RemoveStaticBodies(const FLevelStaticGroup Group)
{
	const auto Found = StaticGroups.find(Group.Value);
	if (Found == StaticGroups.end() || !World)
	{
		return std::unexpected(FPhysicsError{"Unknown static group"});
	}

	for (const FPhysicsBodyId Body : Found->second)
	{
		if (auto Destroyed = World->DestroyBody(Body); !Destroyed)
		{
			return Destroyed;
		}
	}

	StaticGroups.erase(Found);
	return {};
}

std::size_t FLevelSimulation::GetStaticGroupBodyCount() const noexcept
{
	std::size_t Count = 0;
	for (const auto& [Group, Bodies] : StaticGroups)
	{
		Count += Bodies.size();
	}

	return Count;
}

std::vector<bool> FLevelSimulation::GetIntactBonds(const std::size_t ObjectIndex) const
{
	const auto Found = std::ranges::find(Destructibles, ObjectIndex, [](const FDestructibleState& State)
	{
		return State.Settings.ObjectIndex;
	});

	return Found == Destructibles.end() ? std::vector<bool>{} : Found->BondIntact;
}

std::expected<void, FPhysicsError> FLevelSimulation::RestoreBonds(const std::size_t ObjectIndex, const std::vector<bool>& Intact)
{
	const auto Found = std::ranges::find(Destructibles, ObjectIndex, [](const FDestructibleState& State)
	{
		return State.Settings.ObjectIndex;
	});

	if (Found == Destructibles.end() || Found->BondIntact.size() != Intact.size())
	{
		return std::unexpected(FPhysicsError{"The saved bonds do not match the destructible"});
	}

	bool bChanged = false;
	for (std::size_t Bond = 0; Bond < Intact.size(); ++Bond)
	{
		if (Found->BondIntact[Bond] && !Intact[Bond])
		{
			Found->BondIntact[Bond] = false;
			bChanged = true;
		}
	}

	if (!bChanged)
	{
		return {};
	}

	return SplitIslands(static_cast<std::size_t>(Found - Destructibles.begin()), Found->Settings.Position, 0.f);
}

std::vector<FLevelImpactEvent> FLevelSimulation::TakeImpactEvents()
{
	return std::exchange(ImpactEvents, {});
}
}
