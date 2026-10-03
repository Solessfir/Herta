#include "Herta/Scene/SceneSerialization.h"
#include "TestFiles.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>

#ifdef _WIN32
	#include <Windows.h>
#endif

namespace Herta
{
namespace
{
FSceneDocument MakeSceneDocument()
{
	FSceneDocument Document{
	    .Id = FObjectId{1, 1},
	    .Name = "Scene \\\"quoted\\\"",
	    .Entities = {
	        FSceneEntity{
	            .Id = FObjectId{2, 2},
	            .Name = "Parent",
	            .Transform = FSceneTransform{
	                .Translation = FWorldPosition{1234567890.125, -0.001234567890123, 0.},
	                .Rotation = FQuaternion::FromAxisAngle(FVector3::Up(), 0.75f),
	                .Scale = FVector3{1.25f, 0.125f, 4.f},
	            },
	            .Mesh = FStaticMeshComponent{.Asset = FAssetId{3, 3}},
	            .BodyType = ESceneBodyType::Static,
	        },
	        FSceneEntity{
	            .Id = FObjectId{2, 1},
	            .Name = "Child",
	            .Parent = FObjectId{2, 2},
	            .BodyType = ESceneBodyType::Dynamic,
	        },
	    },
	};

	return Document;
}

std::string ReplaceSceneText(std::string Text, const std::string_view From, const std::string_view To)
{
	const std::size_t Position = Text.find(From);
	REQUIRE(Position != std::string::npos);
	Text.replace(Position, From.size(), To);
	return Text;
}

std::string ReadSceneText(const std::filesystem::path& Path)
{
	std::ifstream Stream(Path, std::ios::binary);
	return {std::istreambuf_iterator<char>(Stream), std::istreambuf_iterator<char>()};
}

std::size_t CountSceneTemporaryFiles(const std::filesystem::path& Directory)
{
	std::size_t Count = 0;

	for (const auto& Entry : std::filesystem::directory_iterator(Directory))
	{
		if (Entry.path().extension() == ".tmp")
		{
			++Count;
		}
	}

	return Count;
}
}

TEST_CASE("Scenes round-trip in canonical sorted UTF-8 JSON")
{
	FSceneDocument Document = MakeSceneDocument();
	const auto Serialized = SerializeScene(Document);
	REQUIRE(Serialized);
	CHECK(Serialized->find("\"type\": \"dynamic\"") != std::string::npos);
	CHECK(Serialized->find("\"motion\"") == std::string::npos);
	CHECK_FALSE(ParseScene(ReplaceSceneText(*Serialized, "\"type\": \"dynamic\"", "\"motion\": \"dynamic\"")));
	CHECK(Serialized->back() == '\n');
	CHECK(Serialized->find('\r') == std::string::npos);
	CHECK(Serialized->find("\"format\": \"HertaScene\"") != std::string::npos);
	CHECK_FALSE(ParseScene(ReplaceSceneText(*Serialized, "\"format\":", "\"magic\":")));
	CHECK(Serialized->find("\"rotation\": [") != std::string::npos);
	CHECK(Serialized->find("\"id\": \"00000000-0000-0002-0000-000000000001\"") < Serialized->find("\"id\": \"00000000-0000-0002-0000-000000000002\""));

	const auto Parsed = ParseScene(*Serialized);
	REQUIRE(Parsed);
	CHECK(Parsed->Id == Document.Id);
	CHECK(Parsed->Name == Document.Name);
	std::ranges::sort(Document.Entities, {}, &FSceneEntity::Id);
	CHECK(Parsed->Entities == Document.Entities);

	const auto Reserialized = SerializeScene(*Parsed);
	REQUIRE(Reserialized);
	CHECK(*Reserialized == *Serialized);
	std::ranges::reverse(Document.Entities);
	const auto Reordered = SerializeScene(Document);
	REQUIRE(Reordered);
	CHECK(*Reordered == *Serialized);
}

TEST_CASE("Scene labels and filesystem paths preserve Unicode")
{
	Tests::FScratchDirectory Directory("HertaSceneUnicode");
	const auto Path = Directory.GetPath() / std::filesystem::path(u8"世界-é.hscene");
	FSceneDocument Document = MakeSceneDocument();
	const std::u8string Name = u8"Zażółć 世界 🙂";
	Document.Name.assign(Name.begin(), Name.end());
	Document.Entities[0].Name = Document.Name;
	REQUIRE(SaveScene(Path, Document));
	const auto Loaded = LoadScene(Path);
	REQUIRE(Loaded);
	CHECK(Loaded->Name == Document.Name);
	CHECK(Loaded->Entities[1].Name == Document.Entities[0].Name);
	CHECK(CountSceneTemporaryFiles(Directory.GetPath()) == 0);

	Document.Name = "Updated";
	REQUIRE(SaveScene(Path, Document));
	const auto Replaced = LoadScene(Path);
	REQUIRE(Replaced);
	CHECK(Replaced->Name == "Updated");
	CHECK(CountSceneTemporaryFiles(Directory.GetPath()) == 0);
}

TEST_CASE("Empty scenes and entities without optional components round-trip")
{
	FSceneDocument Document{.Id = FObjectId{1, 1}, .Name = "Empty"};
	const auto EmptyText = SerializeScene(Document);
	REQUIRE(EmptyText);
	const auto EmptyScene = ParseScene(*EmptyText);
	REQUIRE(EmptyScene);
	CHECK(EmptyScene->Entities.empty());
	Document.Entities.push_back(FSceneEntity{.Id = FObjectId{2, 1}, .Name = "Plain"});
	const auto PlainText = SerializeScene(Document);
	REQUIRE(PlainText);
	CHECK(PlainText->find("\"components\": {}") != std::string::npos);
	const auto PlainScene = ParseScene(*PlainText);
	REQUIRE(PlainScene);
	CHECK(PlainScene->Entities == Document.Entities);
}

TEST_CASE("Scenes reject unsupported versions, malformed input, and unknown data")
{
	const auto Serialized = SerializeScene(MakeSceneDocument());
	REQUIRE(Serialized);

	const std::array InvalidTexts{
	    ReplaceSceneText(*Serialized, "\"HertaScene\"", "\"OtherScene\""),
	    ReplaceSceneText(*Serialized, "\"formatVersion\": 1", "\"formatVersion\": 0"),
	    ReplaceSceneText(*Serialized, "\"formatVersion\": 1", "\"formatVersion\": 2"),
	    ReplaceSceneText(*Serialized, "\"engineSchemaVersion\": 1", "\"engineSchemaVersion\": 2"),
	    ReplaceSceneText(*Serialized, "\"formatVersion\": 1", "\"formatVersion\": 1.5"),
	    ReplaceSceneText(*Serialized, "\"formatVersion\": 1", "\"formatVersion\": \"1\""),
	    ReplaceSceneText(*Serialized, "\"formatVersion\": 1", "\"formatVersion\": true"),
	    ReplaceSceneText(*Serialized, "\"formatVersion\": 1", "\"formatVersion\": 1, \"formatVersion\": 1"),
	    ReplaceSceneText(*Serialized, "\"formatVersion\": 1", "\"formatVersion\": 1, \"extra\": 0"),
	    ReplaceSceneText(*Serialized, "\"name\": \"Child\"", "\"name\": \"Child\", \"name\": \"Again\""),
	    ReplaceSceneText(*Serialized, "\"parent\": null", "\"parent\": null, \"unexpected\": 1"),
	    ReplaceSceneText(*Serialized, "\"scale\": [1, 1, 1]", "\"scale\": [1, 1, 1], \"scale\": [1, 1, 1]"),
	    ReplaceSceneText(*Serialized, "\"type\": \"dynamic\"", "\"type\": \"dynamic\", \"type\": \"static\""),
	    ReplaceSceneText(*Serialized, "\"body\": {\"type\": \"dynamic\"}", "\"body\": {\"type\": \"dynamic\"}, \"body\": {\"type\": \"static\"}"),
	    ReplaceSceneText(*Serialized, "\"type\": \"dynamic\"", "\"type\": \"kinematic\""),
	    ReplaceSceneText(*Serialized, "\"type\": \"dynamic\"", "\"type\": \"dynamic\", \"mass\": 1"),
	    ReplaceSceneText(*Serialized, "\"components\": {", "\"components\": {\"script\": {},"),
	    ReplaceSceneText(*Serialized, "00000000-0000-0001-0000-000000000001", "invalid-id"),
	    ReplaceSceneText(*Serialized, "00000000-0000-0001-0000-000000000001", "00000000-0000-0000-0000-000000000000"),
	    ReplaceSceneText(*Serialized, "00000000-0000-0003-0000-000000000003", "invalid-asset"),
	    ReplaceSceneText(*Serialized, "\"translation\": [0, 0, 0]", "\"translation\": [NaN, 0, 0]"),
	    ReplaceSceneText(*Serialized, "\"translation\": [0, 0, 0]", "\"translation\": [1e999, 0, 0]"),
	    ReplaceSceneText(*Serialized, "\"translation\": [0, 0, 0]", "\"translation\": [01, 0, 0]"),
	    ReplaceSceneText(*Serialized, "\"translation\": [0, 0, 0]", "\"translation\": [1., 0, 0]"),
	    ReplaceSceneText(*Serialized, "\"translation\": [0, 0, 0]", "\"translation\": [\"0\", 0, 0]"),
	    ReplaceSceneText(*Serialized, "\"translation\": [0, 0, 0]", "\"translation\": [0, 0]"),
	    ReplaceSceneText(*Serialized, "\"scale\": [1, 1, 1]", "\"scale\": [1e100, 1, 1]"),
	    ReplaceSceneText(*Serialized, "\"rotation\": [0, 0, 0, 1]", "\"rotation\": [0, 0, 0, 0]"),
	    ReplaceSceneText(*Serialized, "\"name\": \"Child\"", "\"name\": \"Child\\n\""),
	    ReplaceSceneText(*Serialized, "\"name\": \"Child\"", "\"name\": \"Child\\u0000\""),
	    ReplaceSceneText(*Serialized, "\"name\": \"Child\"", "\"name\": \"Child\\ud800\""),
	    Serialized->substr(0, Serialized->size() - 3),
	    *Serialized + "{}",
	    std::string{},
	};

	for (std::size_t Index = 0; Index < InvalidTexts.size(); ++Index)
	{
		CAPTURE(Index);
		CHECK_FALSE(ParseScene(InvalidTexts[Index]));
	}
}

TEST_CASE("Scenes validate graph references before accepting or saving documents")
{
	FSceneDocument Document = MakeSceneDocument();
	const auto Serialized = SerializeScene(Document);
	REQUIRE(Serialized);
	const auto MissingParent = ReplaceSceneText(*Serialized, "\"parent\": \"00000000-0000-0002-0000-000000000002\"", "\"parent\": \"00000000-0000-0009-0000-000000000009\"");
	CHECK_FALSE(ParseScene(MissingParent));
	const auto Cycle = ReplaceSceneText(*Serialized, "\"parent\": null", "\"parent\": \"00000000-0000-0002-0000-000000000001\"");
	CHECK_FALSE(ParseScene(Cycle));
	const auto DuplicateId = ReplaceSceneText(*Serialized, "\"id\": \"00000000-0000-0002-0000-000000000001\"", "\"id\": \"00000000-0000-0002-0000-000000000002\"");
	CHECK_FALSE(ParseScene(DuplicateId));
	Document.Entities[0].Parent = Document.Entities[1].Id;
	CHECK_FALSE(SerializeScene(Document));

	FWorld World;
	REQUIRE(World.ReplaceEntities(MakeSceneDocument().Entities));
	const auto Before = World.SnapshotEntities();
	CHECK_FALSE(ParseScene(Cycle));
	CHECK(World.SnapshotEntities() == Before);
}

TEST_CASE("Scenes bound names and reject invalid UTF-8 and non-finite state")
{
	FSceneDocument Document = MakeSceneDocument();
	Document.Name = std::string(1024, 'x');
	REQUIRE(SerializeScene(Document));
	Document.Name += 'x';
	CHECK_FALSE(SerializeScene(Document));
	Document.Name = "Scene";
	Document.Entities[0].Name = std::string(1025, 'x');
	CHECK_FALSE(SerializeScene(Document));
	Document.Entities[0].Name = std::string("Bad\xc0\xaf", 5);
	CHECK_FALSE(SerializeScene(Document));
	Document.Entities[0].Name = "Bad\x7f";
	CHECK_FALSE(SerializeScene(Document));
	Document = MakeSceneDocument();
	Document.Entities[0].Transform.Translation.Meters.X = std::numeric_limits<double>::quiet_NaN();
	CHECK_FALSE(SerializeScene(Document));
	Document = MakeSceneDocument();
	Document.Entities[0].Transform.Scale.X = std::numeric_limits<float>::infinity();
	CHECK_FALSE(SerializeScene(Document));
	Document = MakeSceneDocument();
	const auto Serialized = SerializeScene(Document);
	REQUIRE(Serialized);
	CHECK_FALSE(ParseScene(ReplaceSceneText(*Serialized, "\"name\": \"Child\"", "\"name\": \"" + std::string(1025, 'x') + "\"")));
	CHECK_FALSE(ParseScene(ReplaceSceneText(*Serialized, "\"name\": \"Child\"", std::string("\"name\": \"Bad\xc0\xaf\""))));
}

TEST_CASE("Failed scene saves preserve existing files and clean sibling temporary files")
{
	Tests::FScratchDirectory Directory("HertaSceneAtomic");
	const auto Path = Directory.GetPath() / "Scene.hscene";
	FSceneDocument Document = MakeSceneDocument();
	REQUIRE(SaveScene(Path, Document));
	const std::string Original = ReadSceneText(Path);
	Document.Name = std::string(1025, 'x');
	CHECK_FALSE(SaveScene(Path, Document));
	CHECK(ReadSceneText(Path) == Original);
	CHECK(CountSceneTemporaryFiles(Directory.GetPath()) == 0);
	Document = MakeSceneDocument();

#ifdef _WIN32
	const HANDLE LockedFile = CreateFileW(Path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	REQUIRE(LockedFile != INVALID_HANDLE_VALUE);
	Document.Name = "Cannot replace locked file";
	const auto SaveResult = SaveScene(Path, Document);
	const bool bClosed = CloseHandle(LockedFile) != FALSE;
	CHECK(bClosed);
	CHECK_FALSE(SaveResult);
	CHECK(ReadSceneText(Path) == Original);
	CHECK(CountSceneTemporaryFiles(Directory.GetPath()) == 0);
#endif

	const auto BlockedPath = Directory.GetPath() / "Blocked.hscene";
	std::filesystem::create_directory(BlockedPath);
	Tests::WriteText(BlockedPath / "Original", "Preserved");
	CHECK_FALSE(SaveScene(BlockedPath, Document));
	CHECK(ReadSceneText(BlockedPath / "Original") == "Preserved");
	CHECK(CountSceneTemporaryFiles(Directory.GetPath()) == 0);
	CHECK_FALSE(SaveScene(Directory.GetPath() / "Missing" / "Scene.hscene", Document));
	CHECK_FALSE(LoadScene(Directory.GetPath() / "Missing.hscene"));
	Tests::WriteText(Path, Original.substr(0, Original.size() - 3));
	CHECK_FALSE(LoadScene(Path));
}

TEST_CASE("Scene loads reject oversized files before allocating or parsing")
{
	Tests::FScratchDirectory Directory("HertaSceneLimits");
	const auto Path = Directory.GetPath() / "Oversized.hscene";
	std::ofstream Stream(Path, std::ios::binary);
	Stream.seekp(64 * 1024 * 1024);
	Stream.put('\0');
	Stream.close();
	REQUIRE(Stream);
	CHECK_FALSE(LoadScene(Path));
}
}
