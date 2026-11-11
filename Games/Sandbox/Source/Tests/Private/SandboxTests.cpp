// The Sandbox's own checks: its content stays valid as the engine changes. Engine tests use their own fixtures and never read the Sandbox.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "Herta/AnimationGraph/AnimationGraph.h"
#include "Herta/AssetPipeline/AssetCooker.h"
#include "Herta/AssetPipeline/AssetMetadata.h"
#include "Herta/GameFramework/GameModules.h"
#include "Herta/GameFramework/GameSession.h"
#include "Herta/GameplayGraph/GameplayGraph.h"
#include "Herta/Level/LevelSerialization.h"
#include "Herta/Platform/Process.h"

#include <doctest/doctest.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace Herta;

namespace
{
// The project root, found from the working directory up, so the tests run from the project or any folder in it.
std::filesystem::path FindProjectRoot()
{
	std::error_code Error;
	for (std::filesystem::path Path = std::filesystem::current_path(Error); !Path.empty(); Path = Path.parent_path())
	{
		if (std::filesystem::exists(Path / "Sandbox.hertaproject", Error))
		{
			return Path;
		}

		if (std::filesystem::exists(Path / "Games/Sandbox/Sandbox.hertaproject", Error))
		{
			return Path / "Games/Sandbox";
		}

		if (Path == Path.parent_path())
		{
			break;
		}
	}

	return {};
}

std::string ReadText(const std::filesystem::path& Path)
{
	std::ifstream Stream(Path, std::ios::binary);
	return {std::istreambuf_iterator<char>(Stream), std::istreambuf_iterator<char>()};
}
}

// Hand edits to the playground must match what the editor writes, so the next save in the editor shows no unrelated changes.
TEST_CASE("Sandbox levels are stored in canonical form")
{
	const std::filesystem::path Root = FindProjectRoot();
	REQUIRE_FALSE(Root.empty());
	std::size_t Levels = 0;
	for (const auto& Entry : std::filesystem::recursive_directory_iterator(Root / "Levels"))
	{
		if (Entry.path().extension() != ".hlevel")
		{
			continue;
		}

		CAPTURE(Entry.path().generic_string());
		const std::string Text = ReadText(Entry.path());
		const auto Parsed = ParseLevel(Text);
		REQUIRE_MESSAGE(Parsed, (Parsed ? std::string() : Parsed.error().Message));
		const auto Written = SerializeLevel(*Parsed);
		REQUIRE(Written);
		CHECK(*Written == Text);
		++Levels;
	}

	CHECK(Levels >= 2);
}

// Every graph the Sandbox authors cooks headless as the packaged game will load it, so a schema or compiler change that breaks one shows here.
TEST_CASE("Every Sandbox graph asset cooks")
{
	const std::filesystem::path Root = FindProjectRoot();
	REQUIRE_FALSE(Root.empty());
	const std::filesystem::path Cache = std::filesystem::temp_directory_path() / "HertaSandboxTests" / "DerivedDataCache";
	std::error_code Error;
	std::filesystem::remove_all(Cache, Error);
	constexpr std::array<std::string_view, 4> Importers{"AudioGraph", "VfxGraph", "StateTree", "GameplayGraph"};
	std::size_t Cooked = 0;
	for (const auto& Entry : std::filesystem::recursive_directory_iterator(Root / "Content"))
	{
		if (Entry.path().extension() != ".hmeta")
		{
			continue;
		}

		const auto Metadata = ParseAssetMetadata(ReadText(Entry.path()));
		if (!Metadata || !std::ranges::contains(Importers, Metadata->Importer))
		{
			continue;
		}

		std::filesystem::path Source = Entry.path();
		Source.replace_extension();
		const std::string Relative = Source.lexically_relative(Root / "Content").generic_string();
		CAPTURE(Relative);
		const auto Result = CookAsset({.ContentRoot = Root / "Content", .DerivedDataRoot = Cache, .SourcePath = Relative, .TargetPlatform = "SandboxTests"});
		CHECK_MESSAGE(Result, (Result ? std::string() : Result.error().Message));
		++Cooked;
	}

	CHECK(Cooked >= 10);
	std::filesystem::remove_all(Cache.parent_path(), Error);
}

// The Mannequin plays the built-in machine from its own graph file; when they part, the expected file is written beside the build to copy
// over it.
TEST_CASE("The Mannequin's animation graph is the built-in player machine")
{
	const std::filesystem::path Root = FindProjectRoot();
	REQUIRE_FALSE(Root.empty());
	const auto Text = SerializeGraph(MakeAnimationGraph(MakeDefaultPlayerStateMachine()), GetAnimationGraphSchema());
	REQUIRE(Text);
	const std::string File = ReadText(Root / "Content/Characters/Mannequin/Mannequin.hanimgraph");
	if (File != *Text)
	{
		std::filesystem::create_directories(Root / "Saved/Tests");
		std::ofstream(Root / "Saved/Tests/Mannequin.hanimgraph", std::ios::binary) << *Text;
	}

	CHECK_MESSAGE(File == *Text, "Copy Saved/Tests/Mannequin.hanimgraph over Content/Characters/Mannequin/Mannequin.hanimgraph to match the built-in machine");
}

// Shipping runs the shrine and the other graphs from C++ the module carries, which must match the graphs as saved.
TEST_CASE("The Sandbox module's generated gameplay code is current")
{
	const std::filesystem::path Root = FindProjectRoot();
	REQUIRE_FALSE(Root.empty());
	const auto Sources = CollectGameplayGraphs(Root / "Content");
	REQUIRE_MESSAGE(Sources, (Sources ? std::string() : Sources.error()));
	const auto Generated = GenerateGameplayGraphCpp(*Sources, "GetSandboxGameplayGraphs");
	REQUIRE(Generated);
	std::string File = ReadText(Root / "Source/SandboxGame/Private/GeneratedSandboxGameplayGraphs.cpp");
	std::erase(File, '\r');
	CHECK_MESSAGE(*Generated == File, "Run HertaEditorCmd gameplay.generate Content Source/SandboxGame/Private/GeneratedSandboxGameplayGraphs.cpp GetSandboxGameplayGraphs from Games/Sandbox");
}

// The module loads as the editor and the game load it, from where the build puts it beside these tests.
TEST_CASE("The Sandbox module loads with its run statistics system")
{
	const std::vector<std::string> Names{"SandboxGame"};
	auto Modules = FGameModules::Load(Names, {.Directory = GetExecutablePath().parent_path()});
	REQUIRE_MESSAGE(Modules, (Modules ? std::string() : Modules.error()));
	CHECK((*Modules)->GetSystemNames() == std::vector<std::string>{"RunStats"});
}
