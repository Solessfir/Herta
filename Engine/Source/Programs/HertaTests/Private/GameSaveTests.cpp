#include "TestFiles.h"

#include "Herta/GameFramework/GameAssets.h"
#include "Herta/GameFramework/GameSave.h"
#include "Herta/GameFramework/GameSession.h"

#include <doctest/doctest.h>

#include <fstream>

namespace Herta
{
namespace
{
class FNoSaveGameAssets final : public IGameAssets
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

[[nodiscard]] FGameSave MakeSave()
{
	return {
	    .Level = FObjectId{3, 1},
	    .ObjectiveState = 2,
	    .ElapsedSeconds = 4.25,
	    .BestSeconds = 12.5,
	    .Respawns = 3,
	    .PlayerFoot = {1.5f, 0.f, -2.25f},
	    .PlayerYaw = 0.75f,
	    .Destructibles = {{.Object = FObjectId{3, 2}, .IntactBonds = {true, false, true, true, false, true, true}}},
	    .DeadCharacters = {FObjectId{3, 3}},
	};
}

void WriteText(const std::filesystem::path& Path, const std::string_view Text)
{
	std::ofstream(Path, std::ios::binary | std::ios::trunc).write(Text.data(), static_cast<std::streamsize>(Text.size()));
}
}

TEST_CASE("Saves round-trip their progress and reject damage, truncation, and other versions")
{
	const FGameSave Save = MakeSave();
	const std::string Text = SerializeGameSave(Save);
	const auto Parsed = ParseGameSave(Text);
	REQUIRE_MESSAGE(Parsed, (Parsed ? std::string() : Parsed.error().Message));
	CHECK(*Parsed == Save);

	FGameSave Unfinished = Save;
	Unfinished.BestSeconds.reset();
	Unfinished.Destructibles.clear();
	CHECK(ParseGameSave(SerializeGameSave(Unfinished)).value_or(FGameSave{}) == Unfinished);

	std::string Damaged = Text;
	Damaged[Damaged.find("respawns=3") + 9] = '4';
	CHECK_FALSE(ParseGameSave(Damaged));
	CHECK_FALSE(ParseGameSave(std::string_view(Text).substr(0, Text.size() / 2)));
	CHECK_FALSE(ParseGameSave(""));
	CHECK_FALSE(ParseGameSave("HertaSave\nchecksum=0\n"));

	// A save of another version fails even with a valid checksum.
	std::string Body = Text.substr(0, Text.rfind("checksum="));
	Body.replace(Body.find("version=1"), 9, "version=9");
	CHECK_FALSE(ParseGameSave(Body + "checksum=" + Text.substr(Text.rfind("checksum=") + 9)));
}

TEST_CASE("Saves write atomically, keep the previous save, and recover from it")
{
	const Tests::FScratchDirectory Directory("GameSave");
	const std::filesystem::path File = Directory.GetPath() / "Saves" / "Save.hsave";
	CHECK_FALSE(LoadGameSave(File));

	FGameSave First = MakeSave();
	REQUIRE(WriteGameSave(File, First));
	CHECK_FALSE(std::filesystem::exists(GetGameSaveBackupPath(File)));
	FGameSave Second = First;
	Second.Respawns = 9;
	REQUIRE(WriteGameSave(File, Second));
	const auto Loaded = LoadGameSave(File);
	REQUIRE(Loaded);
	CHECK(Loaded->Save == Second);
	CHECK_FALSE(Loaded->bRecovered);

	// A damaged save falls back to the one before it, and says so.
	WriteText(File, "HertaSave\nversion=1\nlev");
	const auto Recovered = LoadGameSave(File);
	REQUIRE(Recovered);
	CHECK(Recovered->bRecovered);
	CHECK(Recovered->Save == First);
	CHECK_FALSE(Recovered->Problem.empty());

	WriteText(GetGameSaveBackupPath(File), "garbage");
	CHECK_FALSE(LoadGameSave(File));
	CHECK_FALSE(std::filesystem::exists(std::filesystem::path(File).concat(".tmp")));
}

TEST_CASE("Sessions save broken walls, the player, and the run, and put them back after a restart")
{
	const FNoSaveGameAssets Assets;
	const FObjectId WallId{3, 20};
	const std::vector<FLevelEntity> Level{
	    FLevelEntity{.Id = FObjectId{3, 10}, .Name = "Floor", .Transform = {.Translation = {0., -0.5, 0.}, .Scale = {40.f, 1.f, 40.f}}, .Mesh = FStaticMeshComponent{.Asset = FAssetId{1, 1}}, .BodyType = ELevelBodyType::Static},
	    FLevelEntity{.Id = FObjectId{3, 11}, .Name = "Start", .Transform = {.Translation = {0., 1., 0.}}, .Trigger = FTriggerComponent{.Size = {2.f, 2.f, 2.f}, .Role = ETriggerRole::Start}},
	    FLevelEntity{.Id = FObjectId{3, 12}, .Name = "Finish", .Transform = {.Translation = {0., 1., 20.}}, .Trigger = FTriggerComponent{.Size = {4.f, 2.f, 2.f}, .Role = ETriggerRole::Finish}},
	    FLevelEntity{.Id = WallId, .Name = "Wall", .Transform = {.Translation = {0., 1.5, 4.}, .Scale = {3.f, 3.f, 0.3f}}, .Mesh = FStaticMeshComponent{.Asset = FAssetId{1, 1}}, .Destructible = FDestructibleComponent{.Cells = 12.f, .Strength = 5.f}},
	};

	auto Session = FGameSession::Create(Level, Assets, nullptr);
	REQUIRE(Session);
	const auto Step = [&](const FGameInput& Input, const int Frames)
	{
		for (int Frame = 0; Frame < Frames; ++Frame)
		{
			REQUIRE((*Session)->Update(1.f / 60.f, Frame == 0 ? Input : FGameInput{.Move = Input.Move}));
		}
	};

	Step({}, 20);
	CHECK((*Session)->CaptureSave(FObjectId{3, 1}).Destructibles.empty());
	Step({.bThrow = true}, 60);
	Step({.Move = {0.f, 1.f}}, 40);
	const FGameSave Saved = (*Session)->CaptureSave(FObjectId{3, 1});
	REQUIRE(Saved.Destructibles.size() == 1);
	CHECK(Saved.Destructibles[0].Object == WallId);
	CHECK(std::ranges::count(Saved.Destructibles[0].IntactBonds, false) > 0);
	CHECK(Saved.ObjectiveState == static_cast<std::uint32_t>(EGameObjectiveState::Running));
	CHECK(Saved.PlayerFoot.Z > 0.5f);

	// A fresh session of the same level loads the save: the wall breaks the same way and the player stands where it was saved.
	auto Loaded = FGameSession::Create(Level, Assets, nullptr);
	REQUIRE(Loaded);
	const auto Applied = (*Loaded)->ApplySave(*ParseGameSave(SerializeGameSave(Saved)));
	REQUIRE_MESSAGE(Applied, (Applied ? std::string() : Applied.error().Message));
	CHECK(Applied->empty());
	const FGameSave Again = (*Loaded)->CaptureSave(FObjectId{3, 1});
	CHECK(Again.Destructibles == Saved.Destructibles);
	CHECK(Again.ObjectiveState == Saved.ObjectiveState);
	CHECK(Again.ElapsedSeconds == Saved.ElapsedSeconds);
	CHECK((Again.PlayerFoot - Saved.PlayerFoot).Length() < 0.1f);
	CHECK(Again.PlayerYaw == doctest::Approx(Saved.PlayerYaw));

	// Objects the level no longer has are reported, not fatal.
	FGameSave Stale = Saved;
	Stale.Destructibles[0].Object = FObjectId{3, 99};
	Stale.DeadCharacters = {FObjectId{3, 98}};
	const auto Partial = (*Loaded)->ApplySave(Stale);
	REQUIRE(Partial);
	CHECK(Partial->size() == 2);
}
}
