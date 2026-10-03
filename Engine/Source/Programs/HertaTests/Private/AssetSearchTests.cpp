#include "Herta/Assets/AssetSearch.h"

#include <array>
#include <doctest/doctest.h>
#include <ostream>
#include <string_view>
#include <vector>

namespace Herta
{
namespace
{
constexpr std::array<std::string_view, 6> Paths{
    "Game/Models/SmallTank/SmallTank.blend",
    "Engine/Shapes/Cube.gltf",
    "Game/Textures/Wood.png",
    "Game/Models/Crate/CrateLid.gltf",
    "Game/Textures/Concrete_Wall.png",
    "Game/Models/Crate/Crate.gltf"};

[[nodiscard]] std::vector<std::string_view> Search(const std::string_view Query)
{
	const auto Matches = SearchAssets(Paths, Query);
	REQUIRE(Matches);
	std::vector<std::string_view> Result;
	for (const FAssetSearchMatch& Match : *Matches)
	{
		Result.push_back(Paths[Match.Index]);
	}
	return Result;
}
}

TEST_CASE("Asset search matches subsequences case-insensitively and excludes non-matches")
{
	CHECK(Search("cube") == std::vector<std::string_view>{"Engine/Shapes/Cube.gltf"});
	CHECK(Search("WOOD") == std::vector<std::string_view>{"Game/Textures/Wood.png"});
	CHECK(Search("stk").front() == "Game/Models/SmallTank/SmallTank.blend");
	CHECK(Search("zzz").empty());
}

TEST_CASE("Asset search favors word starts, consecutive runs, and file names")
{
	// Both crates match; the exact file name outranks the longer CrateLid.
	const std::vector<std::string_view> Crates = Search("crate.gltf");
	REQUIRE(Crates.size() >= 2);
	CHECK(Crates.front() == "Game/Models/Crate/Crate.gltf");
	// "cw" hits the starts of Concrete and Wall.
	CHECK(Search("cw").front() == "Game/Textures/Concrete_Wall.png");
}

TEST_CASE("Asset search is deterministic, keeps order for an empty query, and can be cancelled")
{
	const std::vector<std::string_view> Everything = Search("");
	CHECK(Everything == std::vector<std::string_view>(Paths.begin(), Paths.end()));
	CHECK(Search("g") == Search("g"));
	CHECK_FALSE(SearchAssets(Paths, "crate", []
	                         {
		                         return true;
	                         }));
}
}
