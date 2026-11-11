#include "Herta/GameFramework/GameAssets.h"
#include "Herta/GameFramework/GameModules.h"
#include "Herta/GameFramework/GameSession.h"
#include "Herta/Platform/DynamicLibrary.h"
#include "TestFiles.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <iterator>
#include <string>
#include <vector>

using namespace Herta;

namespace
{
class FNoModuleAssets final : public IGameAssets
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

// The built test module, under the name a project would give it, copied where the test can replace it as a build would.
void PlaceModule(const std::filesystem::path& Directory, const std::string_view Built)
{
	std::filesystem::create_directories(Directory);
	const std::filesystem::path Source = Tests::GetSiblingExecutable(std::string(Built)).parent_path() / GetDynamicLibraryFileName(Built);
	REQUIRE_MESSAGE(std::filesystem::exists(Source), Source.string());
	std::filesystem::copy_file(Source, Directory / GetDynamicLibraryFileName("HertaTestModule"), std::filesystem::copy_options::overwrite_existing);
}

std::vector<std::string> Update(FGameSession& Session, const int Frames)
{
	std::vector<std::string> Events;
	for (int Frame = 0; Frame < Frames; ++Frame)
	{
		REQUIRE(Session.Update(1.f / 60.f, {}));
		std::ranges::move(Session.TakeEvents(), std::back_inserter(Events));
	}

	return Events;
}
}

TEST_CASE("Game modules load, run their systems, and reload keeping state or roll back when they cannot")
{
	const Tests::FScratchDirectory Scratch("GameModules");
	const std::filesystem::path Directory = Scratch.GetPath() / "Binaries";
	PlaceModule(Directory, "HertaTestModule");

	const std::vector<std::string> Names{"HertaTestModule"};
	auto Modules = FGameModules::Load(Names, {.Directory = Directory, .ShadowDirectory = Scratch.GetPath() / "HotReload"});
	REQUIRE_MESSAGE(Modules, (Modules ? std::string() : Modules.error()));
	CHECK((*Modules)->GetSystemNames() == std::vector<std::string>{"Counter"});

	const FNoModuleAssets Assets;
	const std::vector<FLevelEntity> Level{FLevelEntity{.Id = FObjectId{13, 10}, .Name = "Floor", .Transform = {.Translation = {0., -0.5, 0.}, .Scale = {20.f, 1.f, 20.f}}, .Mesh = FStaticMeshComponent{.Asset = FAssetId{1, 1}}, .BodyType = ELevelBodyType::Static}};
	auto Session = FGameSession::Create(Level, Assets, nullptr);
	REQUIRE(Session);
	(*Modules)->Start(**Session);
	CHECK(std::ranges::contains(Update(**Session, 3), std::string("Counter 3 at version 1")));

	// The same build again, as after a rebuild that kept the state's shape: the count carries on.
	const FGameModuleReloadResult Same = (*Modules)->Reload("HertaTestModule", Session->get());
	CHECK_MESSAGE(Same.Result == EGameModuleReload::Reloaded, Same.Message);
	CHECK(std::ranges::contains(Update(**Session, 1), std::string("Counter 4 at version 1")));

	// A build whose state changed cannot take the running count, so the previous build keeps running.
	PlaceModule(Directory, "HertaTestModuleV2");
	const FGameModuleReloadResult Changed = (*Modules)->Reload("HertaTestModule", Session->get());
	CHECK_MESSAGE(Changed.Result == EGameModuleReload::RestartRequired, Changed.Message);
	CHECK(std::ranges::contains(Update(**Session, 1), std::string("Counter 5 at version 1")));

	// A fresh session starts with the new build, which a restart gives. The old session's systems go first, since their code unloads.
	(void)(*Session)->TakeGameSystems();
	CHECK((*Modules)->Reload("HertaTestModule", nullptr).Result == EGameModuleReload::Reloaded);
	auto Restarted = FGameSession::Create(Level, Assets, nullptr);
	REQUIRE(Restarted);
	(*Modules)->Start(**Restarted);
	CHECK(std::ranges::contains(Update(**Restarted, 1), std::string("Counter 1 at version 2")));

	// Systems go before their code does.
	(void)(*Restarted)->TakeGameSystems();
	CHECK((*Modules)->Reload("Missing", nullptr).Result == EGameModuleReload::Failed);
	CHECK_FALSE(FGameModules::Load(std::vector<std::string>{"NotBuilt"}, {.Directory = Directory}));
}
