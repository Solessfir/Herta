#include "EditorFrameworkPrivate.h"
#include "Herta/Navigation/NavigationWorld.h"

#include <chrono>
#include <functional>

namespace Herta
{
namespace
{
// Long enough that a drag settles before a build starts, short enough that the result follows the edit.
constexpr std::chrono::milliseconds NavigationSettleTime{300};

[[nodiscard]] std::uint64_t Mix(const std::uint64_t Seed, const std::uint64_t Value)
{
	return Seed ^ (Value + 0x9e3779b97f4a7c15ull + (Seed << 6) + (Seed >> 2));
}
}

void FEditorFramework::FImplementation::UpdateNavigation()
{
	if (!Tasks || !Level || !Assets)
	{
		return;
	}

	if (!Navigation)
	{
		Navigation = FEditorNavigation::Create(*Tasks);
		if (!Navigation)
		{
			return;
		}
	}

	if (Navigation->Poll() && !Navigation->GetError().empty())
	{
		HERTA_LOG_WARNING(*Log, EditorLog, "Navigation could not build: {}", Navigation->GetError());
	}

	// Play walks the level as it started, so a build that finishes during Play is the one for that level.
	if (const std::optional<std::uint64_t> NavigationKey = Navigation->GetKey(); PlaySession && NavigationKey && *NavigationKey != PlayNavigationKey)
	{
		PlayNavigationKey = *NavigationKey;
		if (auto Applied = PlaySession->SetNavigation(Navigation->GetFile()); !Applied)
		{
			HERTA_LOG_WARNING(*Log, EditorLog, "Play has no navigation: {}", Applied.error().Message);
		}
	}

	if (!bNavigationVisible && !PlaySession)
	{
		return;
	}

	// Region levels the viewport has not read yet are read here, so navigation covers every region whether or not it is previewed.
	std::vector<const std::vector<FLevelEntity>*> RegionEntities;
	if (!Level->GetPath().empty())
	{
		for (const FLevelRegion& Region : Level->GetRegions())
		{
			const std::filesystem::path Path = ResolveLevelRegionPath(Level->GetPath(), Region);
			auto [Found, bInserted] = RegionPreviewLevels.try_emplace(Path);
			if (bInserted)
			{
				if (auto Document = LoadLevel(Path))
				{
					Found->second.Entities = std::move(Document->Entities);
				}
			}

			RegionEntities.push_back(&Found->second.Entities);
		}
	}

	// The level's edits, its regions, and how many of its static bodies' meshes have loaded decide whether the build is current, since a
	// mesh still loading collides as a placeholder cube.
	std::map<FAssetId, FNavigationBox> Bounds;
	const auto GatherBounds = [this, &Bounds](const std::vector<FLevelEntity>& Entities)
	{
		for (const FLevelEntity& Entity : Entities)
		{
			if (!Entity.Mesh || Entity.BodyType != ELevelBodyType::Static || Bounds.contains(Entity.Mesh->Asset))
			{
				continue;
			}

			const FPreviewMeshSlot* const Slot = Assets->GetCachedAsset(Entity.Mesh->Asset);
			if (Slot && Slot->Mesh)
			{
				Bounds.emplace(Entity.Mesh->Asset, FNavigationBox{.Minimum = Slot->Mesh->GetBoundsMinimum(), .Maximum = Slot->Mesh->GetBoundsMaximum()});
			}
		}
	};

	GatherBounds(VisualEntities);
	for (const std::vector<FLevelEntity>* const Entities : RegionEntities)
	{
		GatherBounds(*Entities);
	}

	std::uint64_t Key = Mix(Level->GetGeneration(), std::hash<std::filesystem::path>{}(Level->GetPath()));
	Key = Mix(Mix(Key, Bounds.size()), RegionEntities.size());
	for (const std::vector<FLevelEntity>* const Entities : RegionEntities)
	{
		Key = Mix(Key, Entities->size());
	}

	if (Navigation->GetKey() == Key)
	{
		return;
	}

	const auto Now = std::chrono::steady_clock::now();
	if (NavigationSettlingKey != Key)
	{
		NavigationSettlingKey = Key;
		NavigationChangedAt = Now;
	}

	// Play cannot wait for edits to settle; there are none while it runs.
	if (!PlaySession && Now - NavigationChangedAt < NavigationSettleTime)
	{
		return;
	}

	FEditorNavigationInputs Inputs{.Level = {.Id = FObjectId{1, 1}, .Name = std::string(Level->GetName()), .Entities = VisualEntities, .Regions = {Level->GetRegions().begin(), Level->GetRegions().end()}}, .Bounds = std::move(Bounds)};
	for (const std::vector<FLevelEntity>* const Entities : RegionEntities)
	{
		Inputs.Regions.push_back(FLevelDocument{.Id = FObjectId{1, 2}, .Name = "Region", .Entities = *Entities});
	}

	if (RegionEntities.empty())
	{
		Inputs.Level.Regions.clear();
	}

	Navigation->Request(Key, std::move(Inputs));
}
}
