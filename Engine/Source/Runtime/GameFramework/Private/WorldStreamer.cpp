#include "WorldStreamer.h"

#include "Herta/Tasks/TaskSystem.h"

#include <algorithm>
#include <limits>
#include <cmath>

namespace Herta
{
FWorldStreamer::~FWorldStreamer()
{
	Reset();
}

void FWorldStreamer::Configure(std::vector<FLevelRegion> InRegions, FRegionLoader InLoader, FTaskSystem* const InTasks)
{
	Reset();
	Regions = std::move(InRegions);
	Slots.assign(Regions.size(), {});
	Loader = std::move(InLoader);
	Tasks = InTasks;
	if (Tasks && !Scope)
	{
		if (auto Created = Tasks->CreateScope("World streaming"))
		{
			Scope = std::move(*Created);
		}
	}
}

void FWorldStreamer::Reset()
{
	for (FSlot& Slot : Slots)
	{
		if (Slot.Load)
		{
			Slot.Load->bCancel = true;
		}

		Slot = {};
	}

	// Loads that already started finish into their own state, which nothing reads any longer.
	if (Scope)
	{
		Scope->RequestCancellation();
		Scope->Wait();
		Scope.reset();
		if (Tasks)
		{
			if (auto Created = Tasks->CreateScope("World streaming"))
			{
				Scope = std::move(*Created);
			}
		}
	}
}

float FWorldStreamer::Distance(const std::size_t Region) const
{
	// Regions stream around whichever player is nearest, so every player stands on loaded ground.
	const FVector3d& Center = Regions[Region].Offset.Meters;
	float Nearest = std::numeric_limits<float>::infinity();
	for (const FVector3& Player : Players)
	{
		Nearest = std::min(Nearest, static_cast<float>(std::hypot(static_cast<double>(Player.X) - Center.X, static_cast<double>(Player.Z) - Center.Z)));
	}

	return Nearest;
}

void FWorldStreamer::Update(const std::span<const FVector3> InPlayers, const std::function<void(std::size_t, FLoadedRegion&&)>& OnLoaded, const std::function<void(std::size_t)>& OnUnload)
{
	Players.assign(InPlayers.begin(), InPlayers.end());
	for (std::size_t Region = 0; Region < Regions.size(); ++Region)
	{
		FSlot& Slot = Slots[Region];
		const float Away = Distance(Region);
		const bool bOutside = Away > Regions[Region].UnloadRadius;
		if (Slot.State == EState::Loading)
		{
			if (bOutside)
			{
				Slot.Load->bCancel = true;
				Slot = {};
				++Totals.Cancelled;
				continue;
			}

			if (!Slot.Load->bDone.load(std::memory_order_acquire))
			{
				continue;
			}

			std::optional<std::expected<FLoadedRegion, FGameError>> Result;
			{
				const std::scoped_lock Lock(Slot.Load->Mutex);
				Result = std::move(Slot.Load->Result);
			}

			Slot.Load.reset();
			if (!Result || !*Result)
			{
				Slot.State = EState::Failed;
				++Totals.Failed;
				Totals.LastError = Result ? Result->error().Message : std::string("The load did not run");
				continue;
			}

			Slot.State = EState::Loaded;
			++Totals.Loads;
			OnLoaded(Region, std::move(**Result));
		}
		else if ((Slot.State == EState::Loaded || Slot.State == EState::Failed) && bOutside)
		{
			if (Slot.State == EState::Loaded)
			{
				OnUnload(Region);
				++Totals.Unloads;
			}

			Slot.State = EState::Unloaded;
		}
	}

	// Start the nearest wanted regions first, within the budgets.
	std::vector<std::size_t> Wanted;
	for (std::size_t Region = 0; Region < Regions.size(); ++Region)
	{
		if (Slots[Region].State == EState::Unloaded && Distance(Region) <= Regions[Region].LoadRadius)
		{
			Wanted.push_back(Region);
		}
	}

	std::ranges::sort(Wanted, {}, [&](const std::size_t Region)
	{
		return Distance(Region);
	});

	for (const std::size_t Region : Wanted)
	{
		const auto Count = [this](const EState State)
		{
			return static_cast<std::size_t>(std::ranges::count(Slots, State, &FSlot::State));
		};

		if (Count(EState::Loading) >= MaximumRegionLoads)
		{
			break;
		}

		if (Count(EState::Loading) + Count(EState::Loaded) >= MaximumLoadedRegions)
		{
			// Make room by unloading the farthest region the player is not inside, or wait.
			std::optional<std::size_t> Farthest;
			for (std::size_t Other = 0; Other < Regions.size(); ++Other)
			{
				if (Slots[Other].State == EState::Loaded && Distance(Other) > Regions[Other].LoadRadius && (!Farthest || Distance(Other) > Distance(*Farthest)))
				{
					Farthest = Other;
				}
			}

			if (!Farthest)
			{
				++Totals.Deferred;
				break;
			}

			OnUnload(*Farthest);
			Slots[*Farthest].State = EState::Unloaded;
			++Totals.Unloads;
		}

		auto Load = std::make_shared<FLoad>();
		Slots[Region] = {.State = EState::Loading, .Load = Load};
		auto Run = [Load, Region = Regions[Region], Loader = Loader]
		{
			auto Result = Load->bCancel ? std::expected<FLoadedRegion, FGameError>(std::unexpected(FGameError{"Cancelled"})) : Loader(Region, Load->bCancel);
			const std::scoped_lock Lock(Load->Mutex);
			Load->Result = std::move(Result);
			Load->bDone.store(true, std::memory_order_release);
		};

		const bool bQueued = Tasks && Scope && Tasks->Submit(*Scope, {.Name = "Load region " + Regions[Region].Name, .Lane = ETaskLane::BlockingIo}, [Run = std::move(Run)](FTaskContext&) mutable
		{
			Run();
		}).has_value();

		// Without a task system the load runs here; the result is still taken next update, the same as an asynchronous one.
		if (!bQueued)
		{
			auto Result = Loader(Regions[Region], Load->bCancel);
			const std::scoped_lock Lock(Load->Mutex);
			Load->Result = std::move(Result);
			Load->bDone.store(true, std::memory_order_release);
		}
	}
}

std::span<const FLevelRegion> FWorldStreamer::GetRegions() const noexcept
{
	return Regions;
}

FRegionStats FWorldStreamer::GetStats() const
{
	FRegionStats Stats = Totals;
	Stats.Loading = static_cast<std::uint32_t>(std::ranges::count(Slots, EState::Loading, &FSlot::State));
	Stats.Loaded = static_cast<std::uint32_t>(std::ranges::count(Slots, EState::Loaded, &FSlot::State));
	return Stats;
}
}
