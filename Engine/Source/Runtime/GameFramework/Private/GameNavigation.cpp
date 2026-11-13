#include "GameNavigation.h"

#include "Herta/Math/AffineTransform.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <utility>

namespace Herta
{
namespace
{
// Bodies shorter than this are stepped over rather than walked around, such as thrown balls and loose debris.
constexpr float MinimumObstacleHeight = 0.3f;

// Matches NavigationBuilder's GetRegionNavigationKey, which games do not link.
[[nodiscard]] FHash128 RegionKey(const FObjectId& Region)
{
	return {.High = Region.GetHigh(), .Low = Region.GetLow()};
}
}

void FGameNavigation::SetData(std::shared_ptr<const FNavigationFile> InData)
{
	Data = std::move(InData);
}

FNavigationObstacle FGameNavigation::Place(const FObstacle& Obstacle, const FMatrix4& Model)
{
	const auto Transform = TryDecomposeTransform(Model, 1.e-3f);
	if (!Transform)
	{
		return {.Center = {Model(0, 3), Model(1, 3), Model(2, 3)}, .HalfExtents = Obstacle.HalfExtents};
	}

	// Obstacles turn only about the vertical axis, so a tipped body cuts the box around its heading.
	const FVector3 Forward = Transform->Rotation.RotateVector(FVector3::Forward());
	const FVector3 Half = Transform->Scale3D.ComponentMultiply(Obstacle.HalfExtents);
	return {
	    .Center = Transform->TransformPosition(Obstacle.Center),
	    .HalfExtents = {std::abs(Half.X), std::abs(Half.Y), std::abs(Half.Z)},
	    .YawRadians = std::atan2(Forward.X, Forward.Z),
	};
}

std::expected<void, FGameError> FGameNavigation::Start(const std::span<const FLevelEntity> Entities, const std::span<const FMatrix4> InAuthoredWorld, const FLevelSimulationSetup& Setup)
{
	World.reset();
	++Generation;
	Obstacles.clear();
	Patches.clear();
	AuthoredWorld.assign(InAuthoredWorld.begin(), InAuthoredWorld.end());
	if (!Data)
	{
		return {};
	}

	auto Created = FNavigationWorld::Create(Data->Base.Settings);
	if (!Created)
	{
		return std::unexpected(FGameError{std::format("Cannot create navigation: {}", Created.error().Message)});
	}

	World = std::move(*Created);
	if (auto Added = World->AddData(Data->Base); !Added)
	{
		return std::unexpected(FGameError{std::format("Cannot load the level's navigation: {}", Added.error().Message)});
	}

	for (const FObjectId& Region : std::exchange(ActiveRegions, {}))
	{
		ActivateRegion(Region);
	}

	for (std::size_t Index = 0; Index < Entities.size(); ++Index)
	{
		if (const auto& Obstacle = Entities[Index].NavigationObstacle)
		{
			Obstacles.push_back({.Entity = Index, .HalfExtents = Obstacle->Size * 0.5f});
		}
	}

	for (const FLevelSimulationBody& Body : Setup.Bodies)
	{
		const bool bTall = std::abs(Body.Transform.Scale3D.Y * Body.Shape.HalfExtents.Y) * 2.f >= MinimumObstacleHeight;
		if (Body.MotionType == EPhysicsMotionType::Dynamic && !Body.bTrigger && bTall && !Entities[Body.ObjectIndex].NavigationObstacle)
		{
			Obstacles.push_back({.Entity = Body.ObjectIndex, .Center = Body.Shape.Center, .HalfExtents = Body.Shape.HalfExtents, .bSimulated = true});
		}
	}

	for (const FLevelSimulationDestructible& Destructible : Setup.Destructibles)
	{
		if (Destructible.Fracture && !Entities[Destructible.ObjectIndex].NavigationObstacle)
		{
			Obstacles.push_back({.Entity = Destructible.ObjectIndex, .HalfExtents = Destructible.Fracture->HalfExtents, .bDestructible = true});
		}
	}

	for (FObstacle& Obstacle : Obstacles)
	{
		FNavigationObstacle Placement;
		if (Obstacle.bDestructible)
		{
			const auto Found = std::ranges::find(Setup.Destructibles, Obstacle.Entity, &FLevelSimulationDestructible::ObjectIndex);
			const FVector3 Forward = Found->Rotation.RotateVector(FVector3::Forward());
			Placement = {.Center = Found->Position, .HalfExtents = Obstacle.HalfExtents, .YawRadians = std::atan2(Forward.X, Forward.Z)};
		}
		else
		{
			Placement = Place(Obstacle, AuthoredWorld[Obstacle.Entity]);
		}

		auto Id = World->AddObstacle(Placement);
		if (!Id)
		{
			return std::unexpected(FGameError{std::format("Cannot add the navigation obstacle of {}: {}", Entities[Obstacle.Entity].Name, Id.error().Message)});
		}

		Obstacle.Id = *Id;
	}

	return {};
}

void FGameNavigation::ActivateRegion(const FObjectId& Region)
{
	// ClangCL rejects the vectorized find on 16-byte IDs, so the region is compared by hand.
	const auto IsRegion = [&Region](const FObjectId& Active)
	{
		return Active == Region;
	};

	if (std::ranges::any_of(ActiveRegions, IsRegion))
	{
		return;
	}

	ActiveRegions.push_back(Region);
	if (!World || !Data)
	{
		return;
	}

	const auto Patch = std::ranges::find(Data->Patches, RegionKey(Region), &FNavigationPatch::Key);
	if (Patch == Data->Patches.end())
	{
		return;
	}

	// A patch built with the same settings as its level always fits a world made for that level, short of running out of tiles.
	if (auto Added = World->AddData(Patch->Data))
	{
		Patches.push_back({.Region = Region, .Source = *Added});
	}
}

void FGameNavigation::UnloadRegion(const FObjectId& Region)
{
	std::erase_if(ActiveRegions, [&Region](const FObjectId& Active)
	{
		return Active == Region;
	});

	const auto Patch = std::ranges::find(Patches, Region, &FPatch::Region);
	if (Patch != Patches.end())
	{
		if (World)
		{
			World->RemoveData(Patch->Source);
		}

		Patches.erase(Patch);
	}
}

void FGameNavigation::Step(const FLevelSimulation& Simulation, const float StepSeconds)
{
	if (!World)
	{
		return;
	}

	const std::span<const FLevelSimulationTransform> Transforms = Simulation.GetTransforms();
	for (FObstacle& Obstacle : Obstacles)
	{
		if (Obstacle.Id.Value == 0)
		{
			continue;
		}

		if (Obstacle.bDestructible)
		{
			if (!Simulation.IsIntact(Obstacle.Entity))
			{
				World->RemoveObstacle(Obstacle.Id);
				Obstacle.Id = {};
			}

			continue;
		}

		if (Obstacle.bSimulated)
		{
			const auto Found = std::ranges::find(Transforms, Obstacle.Entity, &FLevelSimulationTransform::ObjectIndex);
			if (Found != Transforms.end())
			{
				World->MoveObstacle(Obstacle.Id, Place(Obstacle, Found->Transform.ToMatrix()));
			}
		}
	}

	World->Update(StepSeconds);
}

FNavigationWorld* FGameNavigation::GetWorld() noexcept
{
	return World.get();
}

const FNavigationWorld* FGameNavigation::GetWorld() const noexcept
{
	return World.get();
}

std::uint64_t FGameNavigation::GetGeneration() const noexcept
{
	return Generation;
}
}
