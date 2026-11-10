#include "Herta/Core/Log.h"
#include "Herta/GameFramework/GameAssets.h"
#include "Herta/GameFramework/GameSession.h"
#include "Herta/Renderer/MeshRenderer.h"
#include "Herta/Tasks/TaskSystem.h"
#include "TestFiles.h"
#include "TestGraphicsDevice.h"

#include <doctest/doctest.h>

#include <chrono>
#include <format>
#include <string>
#include <thread>

namespace Herta
{
namespace
{
class FNoRegionAssets final : public IGameAssets
{
public:
	[[nodiscard]] const FRenderMesh* FindMesh(const FAssetId&) const override
	{
		return nullptr;
	}

	[[nodiscard]] const FRenderMaterial* FindMaterial(const FAssetId&) const override
	{
		return nullptr;
	}

	[[nodiscard]] std::shared_ptr<const FCookedTexture> FindEnvironment(const FAssetId&) const override
	{
		return nullptr;
	}
};

constexpr FAssetId CubeAsset{0x59f13694df4844e5, 0x863555194493f242};

// A 40 m floor around the spawn, ending 20 m ahead.
[[nodiscard]] std::vector<FLevelEntity> MakeIsland()
{
	return {
	    FLevelEntity{.Id = FObjectId{20, 1}, .Name = "Floor", .Transform = {.Translation = {0., -0.5, 0.}, .Scale = {40.f, 1.f, 40.f}}, .Mesh = FStaticMeshComponent{.Asset = CubeAsset}, .BodyType = ELevelBodyType::Static},
	};
}

// A 10 m platform, a lamp, and a crate that regions leave out because it moves.
[[nodiscard]] FLevelDocument MakePlatform()
{
	return {
	    .Id = FObjectId{21, 1},
	    .Name = "Platform",
	    .Entities = {
	        FLevelEntity{.Id = FObjectId{21, 2}, .Name = "Deck", .Transform = {.Translation = {0., -0.5, 0.}, .Scale = {10.f, 1.f, 10.f}}, .Mesh = FStaticMeshComponent{.Asset = CubeAsset}, .BodyType = ELevelBodyType::Static},
	        FLevelEntity{.Id = FObjectId{21, 3}, .Name = "Lamp", .Transform = {.Translation = {0., 3., 0.}}, .Light = FLightComponent{.Type = ELightType::Point}},
	        FLevelEntity{.Id = FObjectId{21, 4}, .Name = "Crate", .Transform = {.Translation = {2., 1., 0.}}, .Mesh = FStaticMeshComponent{.Asset = CubeAsset}, .BodyType = ELevelBodyType::Dynamic},
	    },
	};
}

[[nodiscard]] FLevelRegion MakeRegion(const std::uint64_t Index, const double Z)
{
	return {.Id = FObjectId{22, Index}, .Name = std::format("Platform {}", Index), .Level = "Regions/Platform.hlevel", .Offset = FWorldPosition{0., 0., Z}, .LoadRadius = 20.f, .UnloadRadius = 25.f};
}

void Advance(FGameSession& Session, const int Frames)
{
	for (int Frame = 0; Frame < Frames; ++Frame)
	{
		REQUIRE(Session.Update(1.f / 60.f, {}));
	}
}

[[nodiscard]] bool Contains(const std::vector<std::string>& Events, const std::string_view Text)
{
	return std::ranges::any_of(Events, [Text](const std::string& Event)
	{
		return Event.find(Text) != std::string::npos;
	});
}
}

TEST_CASE("Regions stream in near the player, collide once their assets are ready, survive restarts, and stream out")
{
	const FNoRegionAssets Assets;
	auto Session = FGameSession::Create(MakeIsland(), Assets, nullptr);
	REQUIRE(Session);

	int Loads = 0;
	int Prepares = 0;
	int Releases = 0;
	const std::array Regions{MakeRegion(1, 30.)};
	(*Session)->SetRegions(Regions, [&](const FLevelRegion& Region, const std::atomic<bool>&) -> std::expected<FLoadedRegion, FGameError>
	{
		CHECK(Region.Level == "Regions/Platform.hlevel");
		++Loads;
		return FLoadedRegion{
		    .Document = MakePlatform(),
		    .Prepare = [&Prepares]() -> std::expected<bool, FGameError>
		{
			// The assets take one more update to be ready.
			return ++Prepares > 1;
		},
		    .Release = [&Releases]
		{
			++Releases;
		},
		};
	});

	// 30 m away the region stays unloaded.
	Advance(**Session, 5);
	CHECK(Loads == 0);
	CHECK((*Session)->GetRegionStats().Loaded == 0);

	REQUIRE((*Session)->TeleportPlayer({0.f, 0.05f, 15.f}, 0.f));
	Advance(**Session, 2);
	CHECK(Loads == 1);
	CHECK((*Session)->GetRegionStats().Preparing == 1);
	Advance(**Session, 1);
	const FRegionStats Active = (*Session)->GetRegionStats();
	CHECK(Active.Loaded == 1);
	CHECK(Active.Preparing == 0);
	CHECK(Active.Loads == 1);
	const std::vector<std::string> In = (*Session)->TakeEvents();
	CHECK(Contains(In, "Region 'Platform 1' streamed in"));
	CHECK(std::ranges::any_of((*Session)->GetWarnings(), [](const std::string& Warning)
	{
		return Warning.find("1 entities left out") != std::string::npos;
	}));

	// Past the island's edge the platform's deck holds the player up.
	REQUIRE((*Session)->TeleportPlayer({0.f, 0.05f, 32.f}, 0.f));
	Advance(**Session, 60);
	CHECK((*Session)->IsPlayerGrounded());
	CHECK((*Session)->GetPlayerFootPosition().Y == doctest::Approx(0.f).epsilon(0.05));
	CHECK(Loads == 1);

	// A restart rebuilds physics with the loaded region's bodies, without loading it again.
	REQUIRE((*Session)->Restart());
	REQUIRE((*Session)->TeleportPlayer({0.f, 0.05f, 32.f}, 0.f));
	Advance(**Session, 60);
	CHECK((*Session)->IsPlayerGrounded());
	CHECK(Loads == 1);

	// Inside the unload radius the region stays; beyond it, it goes.
	REQUIRE((*Session)->TeleportPlayer({0.f, 0.05f, 8.f}, 0.f));
	Advance(**Session, 2);
	CHECK((*Session)->GetRegionStats().Loaded == 1);
	REQUIRE((*Session)->TeleportPlayer({0.f, 0.05f, 0.f}, 0.f));
	Advance(**Session, 2);
	const FRegionStats Gone = (*Session)->GetRegionStats();
	CHECK(Gone.Loaded == 0);
	CHECK(Gone.Unloads == 1);
	CHECK(Releases == 1);
	CHECK(Contains((*Session)->TakeEvents(), "Region 'Platform 1' streamed out"));

	// Without the region the player falls where its deck stood.
	REQUIRE((*Session)->TeleportPlayer({0.f, 0.05f, 32.f}, 0.f));
	REQUIRE((*Session)->Update(1.f / 60.f, {}));
	CHECK((*Session)->GetRegionStats().Loading + (*Session)->GetRegionStats().Loaded <= 1);
}

TEST_CASE("Region streaming keeps to its load budgets and retries failed loads after the player returns")
{
	const FNoRegionAssets Assets;
	auto Session = FGameSession::Create(MakeIsland(), Assets, nullptr);
	REQUIRE(Session);

	std::vector<FLevelRegion> Regions;
	for (std::uint64_t Index = 1; Index <= 12; ++Index)
	{
		Regions.push_back(MakeRegion(Index, 5.));
	}

	Regions.push_back(MakeRegion(13, 100.));
	Regions.back().Level = "Regions/Missing.hlevel";
	int Failures = 0;
	(*Session)->SetRegions(Regions, [&](const FLevelRegion& Region, const std::atomic<bool>&) -> std::expected<FLoadedRegion, FGameError>
	{
		if (Region.Level == "Regions/Missing.hlevel")
		{
			++Failures;
			return std::unexpected(FGameError{"No such level"});
		}

		return FLoadedRegion{.Document = MakePlatform()};
	});

	for (int Frame = 0; Frame < 20; ++Frame)
	{
		REQUIRE((*Session)->Update(1.f / 60.f, {}));
		const FRegionStats Stats = (*Session)->GetRegionStats();
		CHECK(Stats.Loading <= MaximumRegionLoads);
		CHECK(Stats.Loading + Stats.Loaded <= MaximumLoadedRegions);
	}

	const FRegionStats Full = (*Session)->GetRegionStats();
	CHECK(Full.Loaded == MaximumLoadedRegions);
	CHECK(Full.Deferred > 0);

	REQUIRE((*Session)->TeleportPlayer({0.f, 0.05f, 95.f}, 0.f));
	Advance(**Session, 3);
	CHECK(Failures == 1);
	CHECK((*Session)->GetRegionStats().Failed == 1);
	CHECK((*Session)->GetRegionStats().LastError == "No such level");
	Advance(**Session, 3);
	CHECK(Failures == 1);

	REQUIRE((*Session)->TeleportPlayer({0.f, 0.05f, 0.f}, 0.f));
	Advance(**Session, 2);
	REQUIRE((*Session)->TeleportPlayer({0.f, 0.05f, 95.f}, 0.f));
	Advance(**Session, 3);
	CHECK(Failures == 2);
}

TEST_CASE("Leaving a region while a worker loads it cancels the load")
{
	auto Log = FLogService::Create({.bConsoleOutput = false, .bDebuggerOutput = false, .bFileOutput = false});
	REQUIRE(Log);
	auto Tasks = FTaskSystem::Create({.CpuWorkerCount = 2, .Log = Log->get()});
	REQUIRE(Tasks);
	{
		const FNoRegionAssets Assets;
		auto Session = FGameSession::Create(MakeIsland(), Assets, nullptr, {}, Tasks->get());
		REQUIRE(Session);

		std::atomic<bool> bStarted = false;
		std::atomic<bool> bSawCancel = false;
		const std::array Regions{MakeRegion(1, 30.)};
		(*Session)->SetRegions(Regions, [&](const FLevelRegion&, const std::atomic<bool>& Cancel) -> std::expected<FLoadedRegion, FGameError>
		{
			bStarted = true;
			const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
			while (!Cancel && std::chrono::steady_clock::now() < Deadline)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}

			bSawCancel = Cancel.load();
			return FLoadedRegion{.Document = MakePlatform()};
		});

		REQUIRE((*Session)->TeleportPlayer({0.f, 0.05f, 15.f}, 0.f));
		REQUIRE((*Session)->Update(1.f / 60.f, {}));
		CHECK((*Session)->GetRegionStats().Loading == 1);
		const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (!bStarted && std::chrono::steady_clock::now() < Deadline)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}

		REQUIRE((*Session)->TeleportPlayer({0.f, 0.05f, 0.f}, 0.f));
		REQUIRE((*Session)->Update(1.f / 60.f, {}));
		const FRegionStats Stats = (*Session)->GetRegionStats();
		CHECK(Stats.Cancelled == 1);
		CHECK(Stats.Loading == 0);
		CHECK(Stats.Loaded == 0);

		// Destroying the session waits for the worker, which sees the cancellation.
		Session->reset();
		CHECK(bSawCancel);
	}

	(*Tasks)->Shutdown();
}

TEST_CASE("Staged region loads decode on the worker, upload when prepared, and free what no one else uses on release")
{
	const Tests::FScratchDirectory Scratch("HertaRegionLoad");
	const std::filesystem::path Content = Scratch.GetPath() / "Content";
	FCookedTexture White{.ColorSpace = ETextureColorSpace::Srgb, .Mips = {{.Width = 1, .Height = 1, .Pixels = std::vector<std::byte>(4, std::byte{255})}}};
	constexpr FAssetId RegionCube{0x1234, 0x5678};
	const auto Bytes = SerializeCookedAsset(FCookedAsset{CreateTexturedCubeModel(std::move(White))});
	REQUIRE(Bytes);
	Tests::WriteBytes(GetStagedAssetPath(Content, CubeAsset), *Bytes);
	Tests::WriteBytes(GetStagedAssetPath(Content, RegionCube), *Bytes);

	FLevelDocument Platform = MakePlatform();
	Platform.Entities[0].Mesh->Asset = RegionCube;
	std::filesystem::create_directories(Scratch.GetPath() / "Levels/Regions");
	REQUIRE(SaveLevel(Scratch.GetPath() / "Levels/Regions/Platform.hlevel", Platform));

	Tests::FTestGraphicsDevice Device;
	const FCookedContent Cooked({Content});
	auto Assets = FStagedGameAssets::Create(&Device, Cooked, MakeIsland());
	REQUIRE(Assets);
	REQUIRE((*Assets)->FindMesh(CubeAsset));
	CHECK((*Assets)->GetLoadedAssetCount() == 1);

	const FRegionLoader Loader = MakeStagedRegionLoader(Scratch.GetPath() / "Levels/Island.hlevel", Cooked, **Assets, &Device);
	const std::atomic<bool> Cancel = false;
	auto First = Loader(MakeRegion(1, 30.), Cancel);
	REQUIRE_MESSAGE(First, (First ? "" : First.error().Message));
	auto Second = Loader(MakeRegion(2, 60.), Cancel);
	REQUIRE(Second);
	CHECK_FALSE((*Assets)->FindMesh(RegionCube));

	// Both placements share the region's cube; the island's own cube stays the island's.
	REQUIRE(First->Prepare);
	CHECK(First->Prepare() == true);
	CHECK(Second->Prepare() == true);
	CHECK((*Assets)->FindMesh(RegionCube));
	CHECK((*Assets)->GetLoadedAssetCount() == 2);
	First->Release();
	CHECK((*Assets)->FindMesh(RegionCube));
	Second->Release();
	CHECK_FALSE((*Assets)->FindMesh(RegionCube));
	CHECK((*Assets)->FindMesh(CubeAsset));
	CHECK((*Assets)->GetLoadedAssetCount() == 1);

	auto Missing = Loader(FLevelRegion{.Id = FObjectId{22, 9}, .Name = "Missing", .Level = "Regions/Missing.hlevel", .LoadRadius = 1.f, .UnloadRadius = 2.f}, Cancel);
	REQUIRE_FALSE(Missing);
	CHECK(Missing.error().Message.find("Missing") != std::string::npos);
}
}
