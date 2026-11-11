#pragma once

#include "Herta/GameFramework/GameSession.h"
#include "Herta/GameFramework/GameplayProgram.h"
#include "Herta/Level/World.h"

#include <map>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace Herta
{
// Runs the gameplay graphs of the level's Gameplay Graph components. Graphs drive sounds, effects, and the event log, never the simulation,
// so their frames stay out of rollback snapshots.
class FGameGraphs final
{
public:
	[[nodiscard]] static std::vector<FAssetId> GetGraphs(std::span<const FLevelEntity> Entities);

	void Add(const FAssetId& Id, std::shared_ptr<const FCookedGameplayGraph> Graph);
	// Forgets every instance's variables; each graph runs On Start again on its first update.
	void Start(std::span<const FLevelEntity> Entities, std::span<const FMatrix4> AuthoredWorld);
	void Update(float DeltaSeconds, std::span<const FVector3> Players, IGameplayHost& Host);
	[[nodiscard]] std::vector<FGameGraphNodeActivity> GetActivity(const FAssetId& Graph) const;

private:
	struct FInstance
	{
		FAssetId Graph{};
		FVector3 Position{};
		// The entity's trigger box, when it has one.
		std::optional<FMatrix4> WorldToLocal{};
		FVector3 HalfExtents{};
		std::vector<float> Frame{};
		bool bStarted = false;
		bool bInside = false;
	};

	std::map<FAssetId, FGameplayProgram> Programs{};
	std::vector<FInstance> Instances{};
	// Seconds since each node last ran, per graph, for the editor to light what is running.
	std::map<FAssetId, std::map<std::uint32_t, float>> Activity{};
	std::vector<std::uint32_t> Trace{};
};
}
