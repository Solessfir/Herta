#include "Herta/Assets/CookedAsset.h"
#include "Herta/Assets/DataMod.h"
#include "Herta/Assets/VirtualFileSystem.h"
#include "Herta/GameFramework/GameContent.h"
#include "Herta/PackageBuilder/PackageBuilder.h"
#include "TestFiles.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <format>
#include <fstream>
#include <string>

namespace Herta
{
namespace
{
[[nodiscard]] std::string Manifest(const std::string_view Namespace, const std::string_view Version = "1.0.0", const std::string_view Dependencies = "[]", const std::string_view LoadAfter = "[]", const std::string_view LoadBefore = "[]", const std::string_view Overrides = "[]", const std::uint32_t CookedFormat = CookedAssetFormatVersion)
{
	return std::format(R"({{"format": "HertaMod", "formatVersion": 1, "namespace": "{}", "version": "{}", "cookedFormat": {}, "dependencies": {}, "loadAfter": {}, "loadBefore": {}, "overrides": {}}})", Namespace, Version, CookedFormat, Dependencies, LoadAfter, LoadBefore, Overrides);
}

[[nodiscard]] FModCandidate Mod(const std::string& Text, std::vector<std::string> Files = {})
{
	auto Parsed = ParseModManifest(Text);
	REQUIRE_MESSAGE(Parsed, (Parsed ? std::string() : Parsed.error().Message));
	return FModCandidate{.Manifest = std::move(*Parsed), .Files = std::move(Files), .Source = "test"};
}

[[nodiscard]] std::vector<std::string> Order(const std::span<const FModCandidate> Mods, const FModResolution& Resolution)
{
	std::vector<std::string> Names;
	for (const std::size_t Index : Resolution.LoadOrder)
	{
		Names.push_back(Mods[Index].Manifest.Namespace);
	}

	return Names;
}

void WriteText(const std::filesystem::path& Path, const std::string_view Text)
{
	std::filesystem::create_directories(Path.parent_path());
	std::ofstream(Path, std::ios::binary) << Text;
}

[[nodiscard]] std::vector<std::byte> Bytes(const std::string_view Text)
{
	std::vector<std::byte> Result(Text.size());
	std::ranges::transform(Text, Result.begin(), [](const char Character)
	{
		return static_cast<std::byte>(Character);
	});

	return Result;
}
}

TEST_CASE("Mod manifests parse their identity, compatibility, dependencies, load constraints, and overrides, and refuse anything else")
{
	const auto Parsed = ParseModManifest(Manifest("NeonCoral", "1.2.3", R"([{"namespace": "Base_Pack", "minimumVersion": "2.0.0"}])", R"(["Other"])", "[]", R"(["Sandbox/Content/a.hasset"])"));
	REQUIRE_MESSAGE(Parsed, (Parsed ? std::string() : Parsed.error().Message));
	CHECK(Parsed->Namespace == "NeonCoral");
	CHECK(Parsed->Version == FModVersion{1, 2, 3});
	CHECK(Parsed->CookedFormat == CookedAssetFormatVersion);
	REQUIRE(Parsed->Dependencies.size() == 1);
	CHECK(Parsed->Dependencies[0] == FModDependency{.Namespace = "Base_Pack", .MinimumVersion = {2, 0, 0}});
	CHECK(Parsed->LoadAfter == std::vector<std::string>{"Other"});
	CHECK(Parsed->Overrides == std::vector<std::string>{"Sandbox/Content/a.hasset"});

	for (const std::string& Broken : {
	         Manifest("1Starts"), Manifest("Has-Dash"), Manifest("Ok", "1.0"), Manifest("Ok", "1.0.0.0"), Manifest("Ok", "1.x.0"),
	         Manifest("Ok", "1.0.0", R"([{"namespace": "A"}])"), Manifest("Ok", "1.0.0", "[]", "[]", "[]", R"(["../escape"])"),
	         std::string(R"({"format": "HertaMod"})"), Manifest("Ok").replace(1, 0, R"("extra": 1, )"), std::string("not json"),
	     })
	{
		CHECK_FALSE_MESSAGE(ParseModManifest(Broken), Broken);
	}
}

TEST_CASE("Mods load after their dependencies and constraints in a stable order, and refused mods take their dependents with them")
{
	const std::vector<FModCandidate> Mods{
	    Mod(Manifest("Zeta")),
	    Mod(Manifest("Alpha", "1.0.0", R"([{"namespace": "Zeta", "minimumVersion": "1.0.0"}])")),
	    Mod(Manifest("Beta", "1.0.0", "[]", "[]", R"(["Alpha"])")),
	    Mod(Manifest("Gamma", "1.0.0", "[]", R"(["Missing"])")),
	    Mod(Manifest("NeedsNew", "1.0.0", R"([{"namespace": "Zeta", "minimumVersion": "2.0.0"}])")),
	    Mod(Manifest("Old", "1.0.0", "[]", "[]", "[]", "[]", CookedAssetFormatVersion - 1)),
	    Mod(Manifest("NeedsOld", "1.0.0", R"([{"namespace": "Old", "minimumVersion": "1.0.0"}])")),
	};

	const FModResolution Resolution = ResolveMods(Mods, [](std::string_view)
	{
		return false;
	});

	// Zeta before Alpha, which depends on it; Beta before Alpha by its own constraint; an absent loadAfter target is ignored, and ties go by name.
	CHECK(Order(Mods, Resolution) == std::vector<std::string>{"Beta", "Gamma", "Zeta", "Alpha"});
	CHECK(Resolution.Diagnostics.size() == 3);
	CHECK(std::ranges::any_of(Resolution.Diagnostics, [](const std::string& Line)
	{
		return Line.starts_with("Mod NeedsNew") && Line.contains("2.0.0 or later");
	}));

	CHECK(std::ranges::any_of(Resolution.Diagnostics, [](const std::string& Line)
	{
		return Line.starts_with("Mod Old") && Line.contains("cook it again");
	}));

	CHECK(std::ranges::any_of(Resolution.Diagnostics, [](const std::string& Line)
	{
		return Line.starts_with("Mod NeedsOld");
	}));

	// Reordering the discovered mods gives the same order.
	std::vector<FModCandidate> Reversed(Mods.rbegin(), Mods.rend());
	CHECK(Order(Reversed, ResolveMods(Reversed, [](std::string_view)
	{
		return false;
	})) == std::vector<std::string>{"Beta", "Gamma", "Zeta", "Alpha"});

	// Duplicates and cycles refuse every mod involved.
	const std::vector<FModCandidate> Conflicts{
	    Mod(Manifest("Twin")),
	    Mod(Manifest("Twin", "2.0.0")),
	    Mod(Manifest("Loop1", "1.0.0", "[]", R"(["Loop2"])")),
	    Mod(Manifest("Loop2", "1.0.0", "[]", R"(["Loop1"])")),
	    Mod(Manifest("Fine")),
	};

	const FModResolution Refused = ResolveMods(Conflicts, [](std::string_view)
	{
		return false;
	});

	CHECK(Order(Conflicts, Refused) == std::vector<std::string>{"Fine"});
	CHECK(Refused.Diagnostics.size() == 4);
}

TEST_CASE("Mods replace only the files they declare, later mods win, and undeclared replacements refuse the mod")
{
	const std::string Base = "Sandbox/Content/base.hasset";
	const std::vector<FModCandidate> Mods{
	    Mod(Manifest("First", "1.0.0", "[]", "[]", "[]", std::format(R"(["{}"])", Base)), {Base, "Sandbox/Content/new.hasset"}),
	    Mod(Manifest("Second", "1.0.0", "[]", R"(["First"])", "[]", std::format(R"(["{}", "Sandbox/Content/stale.hasset"])", Base)), {Base, "Sandbox/Content/stale.hasset"}),
	    Mod(Manifest("Sneaky"), {Base}),
	    Mod(Manifest("Shadows", "1.0.0", "[]", R"(["First"])"), {"Sandbox/Content/new.hasset"}),
	    Mod(Manifest("Child", "1.0.0", R"([{"namespace": "Sneaky", "minimumVersion": "1.0.0"}])")),
	};

	const FModResolution Resolution = ResolveMods(Mods, [&Base](const std::string_view Path)
	{
		return Path == Base;
	});

	// Sneaky replaces the game's file without saying so, Shadows replaces First's new file without saying so, and Child depends on Sneaky.
	CHECK(Order(Mods, Resolution) == std::vector<std::string>{"First", "Second"});
	const auto Mentions = [&Resolution](const std::string_view Text)
	{
		return std::ranges::count_if(Resolution.Diagnostics, [Text](const std::string& Line)
		{
			return Line.contains(Text);
		});
	};

	CHECK(Mentions("Mod Sneaky") == 1);
	CHECK(Mentions("Mod Shadows") == 1);
	CHECK(Mentions("Mod Child") == 1);
	CHECK(Mentions("which mod First also replaces") == 1);
	CHECK(Mentions("nothing it replaces holds that file") == 1);
}

TEST_CASE("Game content mounts data mods in front of the base package without changing it, and skips refused ones")
{
	const Tests::FScratchDirectory Scratch("HertaMods");
	const std::filesystem::path Executables = Scratch.GetPath() / "Game";
	std::filesystem::create_directories(Executables);
	const std::string Target = "Sandbox/Content/coral.hasset";
	REQUIRE(WritePackage(Executables / "Sandbox.hpak", {{.Path = "Sandbox/Sandbox.hertaproject", .Bytes = Bytes("{}")}, {.Path = Target, .Bytes = Bytes("base coral")}}));
	const auto BaseBefore = ReadDiskFile(Executables / "Sandbox.hpak");
	REQUIRE(BaseBefore);

	// A packaged mod and a loose one: the loose one loads after it and wins.
	WriteText(Executables / "Mods/Neon/Mod.hmod", Manifest("Neon", "1.0.0", "[]", "[]", "[]", std::format(R"(["{}"])", Target)));
	REQUIRE(WritePackage(Executables / "Mods/Neon/Neon.hpak", {{.Path = Target, .Bytes = Bytes("neon coral")}}));
	WriteText(Executables / "Mods/Pastel/Mod.hmod", Manifest("Pastel", "1.0.0", "[]", R"(["Neon"])", "[]", std::format(R"(["{}"])", Target)));
	WriteText(Executables / "Mods/Pastel/Files" / Target, "pastel coral");
	WriteText(Executables / "Mods/Rogue/Mod.hmod", Manifest("Rogue"));
	WriteText(Executables / "Mods/Rogue/Files" / Target, "rogue coral");
	WriteText(Executables / "Mods/Broken/Mod.hmod", "{");

	const auto Content = MountGameContent({.ExecutableDirectory = Executables});
	REQUIRE_MESSAGE(Content, (Content ? std::string() : Content.error().Message));
	REQUIRE(Content->Mods.size() == 2);
	CHECK(Content->Mods[0].starts_with("Neon 1.0.0"));
	CHECK(Content->Mods[1].starts_with("Pastel 1.0.0"));
	CHECK(Content->ModDiagnostics.size() == 3);
	CHECK(Content->FileSystem->Read(Content->FileSystem->GetRoot() / Target) == Bytes("pastel coral"));
	CHECK(Content->FileSystem->GetMountCount() == 3);
	CHECK(ReadDiskFile(Executables / "Sandbox.hpak") == *BaseBefore);

	// Mods can be turned off.
	const auto Plain = MountGameContent({.ExecutableDirectory = Executables, .bLoadMods = false});
	REQUIRE(Plain);
	CHECK(Plain->FileSystem->Read(Plain->FileSystem->GetRoot() / Target) == Bytes("base coral"));
	CHECK(Plain->Mods.empty());
}
}
