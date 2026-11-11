#include "Herta/AssetPipeline/AssetOperations.h"
#include "TestFiles.h"

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

using namespace Herta;

namespace
{
std::string ReadText(const std::filesystem::path& Path)
{
	std::ifstream Stream(Path, std::ios::binary);
	return {std::istreambuf_iterator<char>(Stream), std::istreambuf_iterator<char>()};
}
}

TEST_CASE("Asset moves and deletes carry the metadata and generated files that keep an asset's identity, or change nothing")
{
	const Tests::FScratchDirectory Scratch("AssetOperations");
	const std::filesystem::path Content = Scratch.GetPath() / "Content";
	const std::string Meta = "Format = HertaAssetMetadata\nVersion = 1\nId = 00000000-0000-4000-8000-000000000d01\nImporter = MaterialGraph\n";
	Tests::WriteText(Content / "Materials/Stripes.hmatgraph", "graph");
	Tests::WriteText(Content / "Materials/Stripes.hmatgraph.hmeta", Meta);
	Tests::WriteText(Content / "Materials/Stripes.hmatgraph.slang", "shader");
	Tests::WriteText(Content / "Materials/StripesOther.hmatgraph", "unrelated");

	const auto Files = GetAssetFiles(Content, "Materials/Stripes.hmatgraph");
	REQUIRE(Files);
	CHECK(Files->size() == 3);

	REQUIRE(MoveAsset(Content, "Materials/Stripes.hmatgraph", "Surfaces/Bands.hmatgraph"));
	CHECK(std::filesystem::exists(Content / "Surfaces/Bands.hmatgraph"));
	CHECK(std::filesystem::exists(Content / "Surfaces/Bands.hmatgraph.slang"));
	CHECK(ReadText(Content / "Surfaces/Bands.hmatgraph.hmeta") == Meta);
	CHECK_FALSE(std::filesystem::exists(Content / "Materials/Stripes.hmatgraph"));
	CHECK(std::filesystem::exists(Content / "Materials/StripesOther.hmatgraph"));

	// A taken destination, another extension, a path out of the content, or a file without metadata change nothing.
	Tests::WriteText(Content / "Taken/Bands.hmatgraph.slang", "in the way");
	CHECK_FALSE(MoveAsset(Content, "Surfaces/Bands.hmatgraph", "Taken/Bands.hmatgraph"));
	CHECK(std::filesystem::exists(Content / "Surfaces/Bands.hmatgraph.hmeta"));
	CHECK_FALSE(MoveAsset(Content, "Surfaces/Bands.hmatgraph", "Surfaces/Bands.hmat"));
	CHECK_FALSE(MoveAsset(Content, "Surfaces/Bands.hmatgraph", "../Outside.hmatgraph"));
	CHECK_FALSE(MoveAsset(Content, "Materials/StripesOther.hmatgraph", "Materials/Renamed.hmatgraph"));

	const auto Trashed = DeleteAsset(Content, "Surfaces/Bands.hmatgraph", Scratch.GetPath() / "Saved/Trash");
	REQUIRE(Trashed);
	CHECK_FALSE(std::filesystem::exists(Content / "Surfaces/Bands.hmatgraph"));
	CHECK(std::filesystem::exists(*Trashed / "Bands.hmatgraph.hmeta"));
	REQUIRE(RestoreAsset(*Trashed, Content, "Surfaces/Bands.hmatgraph"));
	CHECK(ReadText(Content / "Surfaces/Bands.hmatgraph.hmeta") == Meta);
	CHECK_FALSE(std::filesystem::exists(*Trashed));
}
