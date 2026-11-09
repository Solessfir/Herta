#include "Herta/GameFramework/LevelSimulationSetup.h"

#include "Herta/Math/AffineTransform.h"
#include "Herta/Renderer/MeshRenderer.h"

#include <algorithm>
#include <format>
#include <optional>

namespace Herta
{
namespace
{
[[nodiscard]] EPhysicsShape ToPhysicsShape(const ELevelCollisionShape Shape)
{
	return Shape == ELevelCollisionShape::Sphere ? EPhysicsShape::Sphere : Shape == ELevelCollisionShape::Capsule ? EPhysicsShape::Capsule
	                                                                                                              : EPhysicsShape::Box;
}

[[nodiscard]] FVector3 TransformPoint(const FMatrix4& Matrix, const FVector3& Point)
{
	const FVector4 World = Matrix * FVector4{Point.X, Point.Y, Point.Z, 1.f};
	return {World.X, World.Y, World.Z};
}
}

FLevelBodyShape GetLevelBodyShape(const FLevelEntity& Entity, const FRenderMesh* const Mesh)
{
	if (!Entity.Mesh && Entity.Trigger)
	{
		return {.Center = {}, .HalfExtents = Entity.Trigger->Size * 0.5f};
	}

	if (!Mesh)
	{
		return {.Center = {}, .HalfExtents = Entity.Mesh ? FVector3{0.5f, 0.5f, 0.5f} : FVector3{0.15f, 0.15f, 0.15f}};
	}

	const FVector3& Minimum = Mesh->GetBoundsMinimum();
	const FVector3& Maximum = Mesh->GetBoundsMaximum();
	// Flat meshes still need a collidable volume.
	constexpr float MinimumHalfExtent = 0.001f;
	return {.Center = (Minimum + Maximum) * 0.5f, .HalfExtents = {std::max((Maximum.X - Minimum.X) * 0.5f, MinimumHalfExtent), std::max((Maximum.Y - Minimum.Y) * 0.5f, MinimumHalfExtent), std::max((Maximum.Z - Minimum.Z) * 0.5f, MinimumHalfExtent)}};
}

FLevelSimulationSetup BuildLevelSimulation(const std::span<const FLevelEntity> Entities, const std::span<const FMatrix4> WorldMatrices, const std::span<const FRenderMesh* const> Meshes)
{
	FLevelSimulationSetup Setup;
	const std::size_t Count = std::min({Entities.size(), WorldMatrices.size(), Meshes.size()});
	std::vector<std::optional<FTransform>> Transforms(Count);
	for (std::size_t Index = 0; Index < Count; ++Index)
	{
		Transforms[Index] = TryDecomposeTransform(WorldMatrices[Index]);
	}

	const auto IsSimulated = [&Setup](const std::size_t Index)
	{
		return std::ranges::contains(Setup.Bodies, Index, &FLevelSimulationBody::ObjectIndex);
	};

	for (const ELevelBodyType Type : {ELevelBodyType::Static, ELevelBodyType::Dynamic})
	{
		for (std::size_t Index = 0; Index < Count; ++Index)
		{
			const FLevelEntity& Entity = Entities[Index];
			// A soft body owns its own deformable collision, and a destructible is its own body; a Rigid Body on the same entity would fight either.
			if (!Entity.Mesh || Entity.BodyType != Type || Entity.SoftBody || Entity.Destructible)
			{
				continue;
			}

			if (!Transforms[Index])
			{
				Setup.Warnings.push_back(std::format("{} is skipped: its world transform is sheared", Entity.Name));
				continue;
			}

			const FLevelRigidBodySettings& Settings = Entity.BodySettings;
			Setup.Bodies.push_back({
			    .ObjectIndex = Index,
			    .Transform = *Transforms[Index],
			    .Shape = GetLevelBodyShape(Entity, Meshes[Index]),
			    .Collision = ToPhysicsShape(Settings.Collision),
			    .MotionType = Type == ELevelBodyType::Dynamic ? EPhysicsMotionType::Dynamic : EPhysicsMotionType::Static,
			    .Properties = {
			        .MassKg = Settings.MassKg,
			        .Friction = Settings.Friction,
			        .Restitution = Settings.Restitution,
			        .LinearDamping = Settings.LinearDamping,
			        .AngularDamping = Settings.AngularDamping,
			        .GravityScale = Settings.GravityScale,
			    },
			    .Mover = Entity.Mover ? std::optional{FLevelSimulationMover{.Offset = Entity.Mover->Offset, .PeriodSeconds = Entity.Mover->PeriodSeconds}} : std::nullopt,
			});
		}
	}

	// Destructibles fracture their mesh bounds in world units, so chunks need no scale when drawn or simulated.
	for (std::size_t Index = 0; Index < Count; ++Index)
	{
		const FLevelEntity& Entity = Entities[Index];
		if (!Entity.Destructible || !Entity.Mesh || Entity.SoftBody)
		{
			continue;
		}

		if (!Transforms[Index])
		{
			Setup.Warnings.push_back(std::format("{} is skipped: its world transform is sheared", Entity.Name));
			continue;
		}

		const FDestructibleComponent& Destructible = *Entity.Destructible;
		const FTransform& Transform = *Transforms[Index];
		const FLevelBodyShape Shape = GetLevelBodyShape(Entity, Meshes[Index]);
		const FVector3 Scaled = Transform.Scale3D.ComponentMultiply(Shape.HalfExtents);
		const FVector3 Half{std::abs(Scaled.X), std::abs(Scaled.Y), std::abs(Scaled.Z)};
		const FVector3 Center = Transform.Scale3D.ComponentMultiply(Shape.Center);
		const FPhysicsBodyProperties Properties{.MassKg = Destructible.MassKg, .Friction = 0.6f, .Restitution = 0.05f};
		Setup.Destructibles.push_back({
		    .ObjectIndex = Index,
		    .Position = Transform.Translation + Transform.Rotation.RotateVector(Center),
		    .Rotation = Transform.Rotation,
		    .Offset = -Center,
		    .Scale = Transform.Scale3D,
		    .Fracture = std::make_shared<const FFracture>(FractureBox(Half, static_cast<std::uint32_t>(Destructible.Cells), static_cast<std::uint32_t>(Destructible.Seed))),
		    .Strength = Destructible.Strength,
		    .bAnchored = Destructible.bAnchored,
		    .Properties = Properties,
		});
		Setup.IntactDestructibles.push_back({.ObjectIndex = Index, .Transform = Transform, .Shape = Shape, .MotionType = Destructible.bAnchored ? EPhysicsMotionType::Static : EPhysicsMotionType::Dynamic, .Properties = Properties});
	}

	// Movers without a Rigid Body still collide, as kinematic boxes; triggers become sensors unless the entity already simulates.
	for (std::size_t Index = 0; Index < Count; ++Index)
	{
		const FLevelEntity& Entity = Entities[Index];
		const bool bSimulated = IsSimulated(Index) || Entity.SoftBody;
		if (!Transforms[Index] || (!Entity.Mover && !Entity.Trigger))
		{
			continue;
		}

		if (Entity.Mover && Entity.Mesh && !bSimulated)
		{
			Setup.Bodies.push_back({
			    .ObjectIndex = Index,
			    .Transform = *Transforms[Index],
			    .Shape = GetLevelBodyShape(Entity, Meshes[Index]),
			    .Mover = FLevelSimulationMover{.Offset = Entity.Mover->Offset, .PeriodSeconds = Entity.Mover->PeriodSeconds},
			});
		}
		else if (Entity.Trigger && bSimulated)
		{
			Setup.Warnings.push_back(std::format("Trigger on {} is skipped: the entity already simulates as a body", Entity.Name));
		}
		else if (Entity.Trigger)
		{
			Setup.Bodies.push_back({.ObjectIndex = Index, .Transform = *Transforms[Index], .Shape = {.HalfExtents = Entity.Trigger->Size * 0.5f}, .bTrigger = true});
		}
	}

	// Soft body settings reference these vectors, so they are sized once and never reallocate.
	const auto SoftBodyCount = static_cast<std::size_t>(std::ranges::count_if(Entities.first(Count), [](const FLevelEntity& Entity)
	{
		return Entity.SoftBody && !Entity.Mesh;
	}));

	Setup.Topologies.reserve(SoftBodyCount);
	Setup.WorldVertices.reserve(SoftBodyCount);
	for (std::size_t Index = 0; Index < Count; ++Index)
	{
		const FLevelEntity& Entity = Entities[Index];
		if (!Entity.SoftBody || Entity.Mesh)
		{
			continue;
		}

		const FSoftBodyComponent& SoftBody = *Entity.SoftBody;
		const FSoftBodyTopology& Topology = Setup.Topologies.emplace_back(BuildSoftBodyTopology(SoftBody));
		std::vector<FVector3>& Vertices = Setup.WorldVertices.emplace_back();
		Vertices.reserve(Topology.Vertices.size());
		for (const FVector3& Vertex : Topology.Vertices)
		{
			Vertices.push_back(TransformPoint(WorldMatrices[Index], Vertex));
		}

		Setup.SoftBodies.push_back({
		    .ObjectIndex = Index,
		    .Settings = MakeSoftBodyPhysicsSettings(SoftBody, Topology, Vertices),
		    .Rope = SoftBody.Shape == ESoftBodyShape::Rope ? std::optional{MakeRopePhysicsSettings(SoftBody, Vertices)} : std::nullopt,
		});
	}

	for (std::size_t Index = 0; Index < Count; ++Index)
	{
		const std::optional<FConstraintComponent>& Constraint = Entities[Index].Constraint;
		if (!Constraint)
		{
			continue;
		}

		const bool bDynamic = std::ranges::any_of(Setup.Bodies, [Index](const FLevelSimulationBody& Body)
		{
			return Body.ObjectIndex == Index && Body.MotionType == EPhysicsMotionType::Dynamic && !Body.Mover && !Body.bTrigger;
		});

		if (!bDynamic)
		{
			Setup.Warnings.push_back(std::format("Constraint on {} is skipped: the entity is not a meshed Dynamic rigid body", Entities[Index].Name));
			continue;
		}

		std::optional<std::size_t> Target;
		if (Constraint->Target.IsValid())
		{
			const auto Found = std::ranges::find(Entities.first(Count), Constraint->Target, &FLevelEntity::Id);
			const auto TargetIndex = static_cast<std::size_t>(Found - Entities.begin());
			const bool bBody = std::ranges::any_of(Setup.Bodies, [TargetIndex](const FLevelSimulationBody& Body)
			{
				return Body.ObjectIndex == TargetIndex && !Body.bTrigger;
			});

			const bool bRope = std::ranges::any_of(Setup.SoftBodies, [TargetIndex](const FLevelSimulationSoftBody& SoftBody)
			{
				return SoftBody.ObjectIndex == TargetIndex && SoftBody.Rope;
			});

			if (TargetIndex >= Count || TargetIndex == Index || (!bBody && !bRope))
			{
				Setup.Warnings.push_back(std::format("Constraint on {} is skipped: its target is not a rope or another simulated rigid body", Entities[Index].Name));
				continue;
			}

			Target = TargetIndex;
		}

		Setup.Constraints.push_back({.ObjectIndex = Index, .TargetObjectIndex = Target, .Point = TransformPoint(WorldMatrices[Index], Constraint->Anchor)});
	}

	return Setup;
}
}
