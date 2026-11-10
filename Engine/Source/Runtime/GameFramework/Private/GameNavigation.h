#pragma once

#include "Herta/GameFramework/GameSession.h"
#include "Herta/GameFramework/LevelSimulationSetup.h"
#include "Herta/Navigation/NavigationWorld.h"

#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace Herta
{
// A session's navigation: the level's cooked tiles, its regions' patches while they are loaded, and obstacles that follow the bodies agents
// must walk around. Unbroken destructibles, dynamic bodies, and Navigation Obstacle components cut obstacles; a destructible's frees its
// ground once it breaks.
class FGameNavigation final
{
public:
	void SetData(std::shared_ptr<const FNavigationFile> InData);
	// Rebuilds the navigation world for a session start: the level's tiles, patches of the regions already loaded, and the obstacles.
	[[nodiscard]] std::expected<void, FGameError> Start(std::span<const FLevelEntity> Entities, std::span<const FMatrix4> AuthoredWorld, const FLevelSimulationSetup& Setup);
	void ActivateRegion(const FObjectId& Region);
	void UnloadRegion(const FObjectId& Region);
	// Moves obstacles with their simulated bodies, frees broken destructibles' ground, and steps the crowd.
	void Step(const FLevelSimulation& Simulation, float StepSeconds);

	[[nodiscard]] FNavigationWorld* GetWorld() noexcept;
	[[nodiscard]] const FNavigationWorld* GetWorld() const noexcept;
	// Counts starts, so agents can tell whether the world they joined is still the current one.
	[[nodiscard]] std::uint64_t GetGeneration() const noexcept;

private:
	struct FObstacle
	{
		std::size_t Entity = 0;
		FNavigationObstacleId Id{};
		// In the entity's frame, or the destructible's box frame.
		FVector3 Center{};
		FVector3 HalfExtents{};
		bool bDestructible = false;
		bool bSimulated = false;
	};

	struct FPatch
	{
		FObjectId Region{};
		FNavigationSourceId Source{};
	};

	[[nodiscard]] static FNavigationObstacle Place(const FObstacle& Obstacle, const FMatrix4& Model);

	std::shared_ptr<const FNavigationFile> Data{};
	std::unique_ptr<FNavigationWorld> World{};
	std::vector<FObstacle> Obstacles{};
	std::vector<FMatrix4> AuthoredWorld{};
	// Regions loaded now, kept across restarts like their static bodies.
	std::vector<FObjectId> ActiveRegions{};
	std::vector<FPatch> Patches{};
	std::uint64_t Generation = 0;
};
}
