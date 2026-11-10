#pragma once

#include "Herta/GameFramework/GameSession.h"
#include "Herta/Level/LevelSerialization.h"
#include "Herta/Tasks/TaskSystem.h"

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace Herta
{
// Decides which regions should be loaded from the player's position, loads them on blocking-IO workers, and hands finished loads back on
// the game thread. A region loads within its load radius and unloads beyond its unload radius, so standing on the boundary never thrashes.
// Leaving a region while it loads cancels the load and drops its result. At most MaximumRegionLoads load at once and MaximumLoadedRegions
// stay loaded; past that, the farthest loaded region outside its load radius goes first.
class FWorldStreamer final
{
public:
	FWorldStreamer() = default;
	~FWorldStreamer();
	FWorldStreamer(const FWorldStreamer&) = delete;
	FWorldStreamer& operator=(const FWorldStreamer&) = delete;
	FWorldStreamer(FWorldStreamer&&) = delete;
	FWorldStreamer& operator=(FWorldStreamer&&) = delete;

	void Configure(std::vector<FLevelRegion> InRegions, FRegionLoader InLoader, FTaskSystem* InTasks);
	// Forgets every load, cancelling those in flight, as a session restart does; the caller unloads what was loaded.
	void Reset();
	// Players are everyone the regions stream around.
	void Update(std::span<const FVector3> InPlayers, const std::function<void(std::size_t Region, FLoadedRegion&& Loaded)>& OnLoaded, const std::function<void(std::size_t Region)>& OnUnload);

	[[nodiscard]] std::span<const FLevelRegion> GetRegions() const noexcept;
	[[nodiscard]] FRegionStats GetStats() const;

private:
	enum class EState : std::uint8_t
	{
		Unloaded,
		Loading,
		Loaded,
		// The load failed; it is tried again once the player has left and come back.
		Failed,
	};

	struct FLoad
	{
		std::atomic<bool> bCancel = false;
		std::atomic<bool> bDone = false;
		std::mutex Mutex;
		std::optional<std::expected<FLoadedRegion, FGameError>> Result;
	};

	struct FSlot
	{
		EState State = EState::Unloaded;
		std::shared_ptr<FLoad> Load{};
	};

	// To the nearest player.
	[[nodiscard]] float Distance(std::size_t Region) const;

	std::vector<FLevelRegion> Regions{};
	std::vector<FVector3> Players{};
	std::vector<FSlot> Slots{};
	FRegionLoader Loader{};
	FTaskSystem* Tasks = nullptr;
	std::unique_ptr<FTaskScope> Scope{};
	FRegionStats Totals{};
};
}
