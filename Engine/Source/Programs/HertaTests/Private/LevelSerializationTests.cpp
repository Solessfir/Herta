#include "Herta/EditorCore/LevelCommands.h"
#include "Herta/Level/LevelSerialization.h"
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
FLevelDocument MakeLevelDocument()
{
	FLevelDocument Document{
	    .Id = FObjectId{1, 1},
	    .Name = "Level \\\"quoted\\\"",
	    .Entities = {
	        FLevelEntity{
	            .Id = FObjectId{2, 2},
	            .Name = "Parent",
	            .Transform = FLevelTransform{
	                .Translation = FWorldPosition{1234567890.125, -0.001234567890123, 0.},
	                .Rotation = FQuaternion::FromAxisAngle(FVector3::Up(), 0.75f),
	                .Scale = FVector3{1.25f, 0.125f, 4.f},
	            },
	            .Mesh = FStaticMeshComponent{.Asset = FAssetId{3, 3}},
	            .BodyType = ELevelBodyType::Static,
	        },
	        FLevelEntity{
	            .Id = FObjectId{2, 1},
	            .Name = "Child",
	            .Parent = FObjectId{2, 2},
	            .BodyType = ELevelBodyType::Dynamic,
	            .BodySettings = {
	                .MassKg = 2.5f,
	                .Friction = 0.75f,
	                .Restitution = 0.25f,
	                .LinearDamping = 0.1f,
	                .AngularDamping = 0.2f,
	                .GravityScale = 1.5f,
	            },
	        },
	    },
	};

	return Document;
}

std::string ReplaceLevelText(std::string Text, const std::string_view From, const std::string_view To)
{
	const std::size_t Position = Text.find(From);
	REQUIRE(Position != std::string::npos);
	Text.replace(Position, From.size(), To);
	return Text;
}

std::string ReadLevelText(const std::filesystem::path& Path)
{
	std::ifstream Stream(Path, std::ios::binary);
	return {std::istreambuf_iterator<char>(Stream), std::istreambuf_iterator<char>()};
}

std::size_t CountLevelTemporaryFiles(const std::filesystem::path& Directory)
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

TEST_CASE("Levels round-trip in canonical sorted UTF-8 JSON")
{
	FLevelDocument Document = MakeLevelDocument();
	const auto Serialized = SerializeLevel(Document);
	REQUIRE(Serialized);
	CHECK(Serialized->find("\"type\": \"dynamic\"") != std::string::npos);
	CHECK(Serialized->find("\"motion\"") == std::string::npos);
	CHECK_FALSE(ParseLevel(ReplaceLevelText(*Serialized, "\"type\": \"dynamic\"", "\"motion\": \"dynamic\"")));
	CHECK(Serialized->back() == '\n');
	CHECK(Serialized->find('\r') == std::string::npos);
	CHECK(Serialized->find("\"format\": \"HertaLevel\"") != std::string::npos);
	CHECK(Serialized->find("\"formatVersion\": 2") != std::string::npos);
	CHECK(Serialized->find("\"engineSchemaVersion\": 2") != std::string::npos);
	CHECK_FALSE(ParseLevel(ReplaceLevelText(*Serialized, "\"format\":", "\"magic\":")));
	CHECK(Serialized->find("\"rotation\": [") != std::string::npos);
	const std::size_t TypePosition = Serialized->find("\"type\": \"dynamic\"");
	const std::size_t MassPosition = Serialized->find("\"massKg\": 2.5");
	const std::size_t FrictionPosition = Serialized->find("\"friction\": 0.75");
	const std::size_t RestitutionPosition = Serialized->find("\"restitution\": 0.25");
	const std::size_t LinearDampingPosition = Serialized->find("\"linearDamping\": 0.1");
	const std::size_t AngularDampingPosition = Serialized->find("\"angularDamping\": 0.2");
	const std::size_t GravityScalePosition = Serialized->find("\"gravityScale\": 1.5");
	CHECK(TypePosition < MassPosition);
	CHECK(MassPosition < FrictionPosition);
	CHECK(FrictionPosition < RestitutionPosition);
	CHECK(RestitutionPosition < LinearDampingPosition);
	CHECK(LinearDampingPosition < AngularDampingPosition);
	CHECK(AngularDampingPosition < GravityScalePosition);
	CHECK(Serialized->find("\"id\": \"00000000-0000-0002-0000-000000000001\"") < Serialized->find("\"id\": \"00000000-0000-0002-0000-000000000002\""));

	const auto Parsed = ParseLevel(*Serialized);
	REQUIRE(Parsed);
	CHECK(Parsed->Id == Document.Id);
	CHECK(Parsed->Name == Document.Name);
	std::ranges::sort(Document.Entities, {}, &FLevelEntity::Id);
	CHECK(Parsed->Entities == Document.Entities);

	const auto Reserialized = SerializeLevel(*Parsed);
	REQUIRE(Reserialized);
	CHECK(*Reserialized == *Serialized);
	std::ranges::reverse(Document.Entities);
	const auto Reordered = SerializeLevel(Document);
	REQUIRE(Reordered);
	CHECK(*Reordered == *Serialized);
}

TEST_CASE("Schema 1 body types migrate to canonical schema 2 settings")
{
	constexpr std::string_view VersionOne = R"({
  "format": "HertaScene",
  "formatVersion": 1,
  "engineSchemaVersion": 1,
  "id": "00000000-0000-0001-0000-000000000001",
  "name": "Legacy",
  "entities": [
    {
      "id": "00000000-0000-0002-0000-000000000001",
      "name": "Body",
      "parent": null,
      "transform": {
        "translation": [0, 0, 0],
        "rotation": [0, 0, 0, 1],
        "scale": [1, 1, 1]
      },
      "components": {
        "body": {"type": "dynamic"}
      }
    }
  ]
}
)";

	const auto Migrated = ParseLevel(VersionOne);
	REQUIRE(Migrated);
	REQUIRE(Migrated->Entities.size() == 1);
	CHECK(Migrated->Entities[0].BodyType == ELevelBodyType::Dynamic);
	CHECK(Migrated->Entities[0].BodySettings == FLevelRigidBodySettings{});
	CHECK_FALSE(ParseLevel(ReplaceLevelText(std::string(VersionOne), "\"type\": \"dynamic\"", "\"type\": \"dynamic\", \"massKg\": 1")));

	const auto Canonical = SerializeLevel(*Migrated);
	REQUIRE(Canonical);
	CHECK(Canonical->find("\"format\": \"HertaLevel\"") != std::string::npos);
	CHECK(Canonical->find("\"formatVersion\": 2") != std::string::npos);
	CHECK(Canonical->find("\"engineSchemaVersion\": 2") != std::string::npos);
	CHECK(Canonical->find("\"massKg\": 1") != std::string::npos);
	CHECK(Canonical->find("\"gravityScale\": 1") != std::string::npos);
	const auto Reparsed = ParseLevel(*Canonical);
	REQUIRE(Reparsed);
	CHECK(Reparsed->Id == Migrated->Id);
	CHECK(Reparsed->Name == Migrated->Name);
	CHECK(Reparsed->Entities == Migrated->Entities);
}

TEST_CASE("Legacy scene documents migrate to the level format without changing authored state")
{
	const auto Current = SerializeLevel(MakeLevelDocument());
	REQUIRE(Current);
	const auto Legacy = ReplaceLevelText(ReplaceLevelText(*Current, "\"HertaLevel\"", "\"HertaScene\""), "\"formatVersion\": 2", "\"formatVersion\": 1");
	const auto Migrated = ParseLevel(Legacy);
	REQUIRE(Migrated);
	const auto Canonical = SerializeLevel(*Migrated);
	REQUIRE(Canonical);
	CHECK(*Canonical == *Current);
	CHECK_FALSE(ParseLevel(ReplaceLevelText(*Current, "\"HertaLevel\"", "\"HertaScene\"")));
	CHECK_FALSE(ParseLevel(ReplaceLevelText(*Current, "\"formatVersion\": 2", "\"formatVersion\": 1")));
	CHECK_FALSE(ParseLevel(ReplaceLevelText(Legacy, "\"formatVersion\": 1", "\"formatVersion\": 0")));
	CHECK_FALSE(ParseLevel(ReplaceLevelText(Legacy, "\"formatVersion\": 1", "\"formatVersion\": 3")));
}

TEST_CASE("Level labels and filesystem paths preserve Unicode")
{
	Tests::FScratchDirectory Directory("HertaLevelUnicode");
	const auto Path = Directory.GetPath() / std::filesystem::path(u8"世界-é.hlevel");
	FLevelDocument Document = MakeLevelDocument();
	const std::u8string Name = u8"Zażółć 世界 🙂";
	Document.Name.assign(Name.begin(), Name.end());
	Document.Entities[0].Name = Document.Name;
	REQUIRE(SaveLevel(Path, Document));
	const auto Loaded = LoadLevel(Path);
	REQUIRE(Loaded);
	CHECK(Loaded->Name == Document.Name);
	CHECK(Loaded->Entities[1].Name == Document.Entities[0].Name);
	CHECK(CountLevelTemporaryFiles(Directory.GetPath()) == 0);

	Document.Name = "Updated";
	REQUIRE(SaveLevel(Path, Document));
	const auto Replaced = LoadLevel(Path);
	REQUIRE(Replaced);
	CHECK(Replaced->Name == "Updated");
	CHECK(CountLevelTemporaryFiles(Directory.GetPath()) == 0);
}

TEST_CASE("Level canonicalization explicitly migrates legacy files and preserves the source")
{
	Tests::FScratchDirectory Directory("HertaLevelMigration");
	const auto Source = Directory.GetPath() / "Legacy.hscene";
	const auto Destination = Directory.GetPath() / "Migrated.hlevel";
	const auto Current = SerializeLevel(MakeLevelDocument());
	REQUIRE(Current);
	const auto Legacy = ReplaceLevelText(ReplaceLevelText(*Current, "\"HertaLevel\"", "\"HertaScene\""), "\"formatVersion\": 2", "\"formatVersion\": 1");
	Tests::WriteText(Source, Legacy);
	FEditorCommandRegistry Commands;
	REQUIRE(RegisterLevelFileCommands(Commands));
	REQUIRE(Commands.Execute("level.validate \"" + Source.generic_string() + "\""));
	CHECK(ReadLevelText(Source) == Legacy);
	REQUIRE(Commands.Execute("level.canonicalize \"" + Source.generic_string() + "\" \"" + Destination.generic_string() + "\""));
	CHECK(ReadLevelText(Source) == Legacy);
	CHECK(ReadLevelText(Destination) == *Current);
	CHECK_FALSE(Commands.Execute("scene.validate \"" + Source.generic_string() + "\""));
	CHECK_FALSE(Commands.Execute("scene.canonicalize \"" + Source.generic_string() + "\" \"" + Destination.generic_string() + "\""));
}

TEST_CASE("Empty levels and entities without optional components round-trip")
{
	FLevelDocument Document{.Id = FObjectId{1, 1}, .Name = "Empty"};
	const auto EmptyText = SerializeLevel(Document);
	REQUIRE(EmptyText);
	const auto EmptyLevel = ParseLevel(*EmptyText);
	REQUIRE(EmptyLevel);
	CHECK(EmptyLevel->Entities.empty());
	Document.Entities.push_back(FLevelEntity{.Id = FObjectId{2, 1}, .Name = "Plain"});
	const auto PlainText = SerializeLevel(Document);
	REQUIRE(PlainText);
	CHECK(PlainText->find("\"components\": {}") != std::string::npos);
	const auto PlainLevel = ParseLevel(*PlainText);
	REQUIRE(PlainLevel);
	CHECK(PlainLevel->Entities == Document.Entities);
}

TEST_CASE("Levels reject unsupported versions, malformed input, and unknown data")
{
	const auto Serialized = SerializeLevel(MakeLevelDocument());
	REQUIRE(Serialized);

	const std::array InvalidTexts{
	    ReplaceLevelText(*Serialized, "\"HertaLevel\"", "\"OtherLevel\""),
	    ReplaceLevelText(*Serialized, "\"formatVersion\": 2", "\"formatVersion\": 0"),
	    ReplaceLevelText(*Serialized, "\"formatVersion\": 2", "\"formatVersion\": 3"),
	    ReplaceLevelText(*Serialized, "\"engineSchemaVersion\": 2", "\"engineSchemaVersion\": 3"),
	    ReplaceLevelText(*Serialized, "\"formatVersion\": 2", "\"formatVersion\": 1.5"),
	    ReplaceLevelText(*Serialized, "\"formatVersion\": 2", "\"formatVersion\": \"2\""),
	    ReplaceLevelText(*Serialized, "\"formatVersion\": 2", "\"formatVersion\": true"),
	    ReplaceLevelText(*Serialized, "\"formatVersion\": 2", "\"formatVersion\": 2, \"formatVersion\": 2"),
	    ReplaceLevelText(*Serialized, "\"formatVersion\": 2", "\"formatVersion\": 2, \"extra\": 0"),
	    ReplaceLevelText(*Serialized, "\"name\": \"Child\"", "\"name\": \"Child\", \"name\": \"Again\""),
	    ReplaceLevelText(*Serialized, "\"parent\": null", "\"parent\": null, \"unexpected\": 1"),
	    ReplaceLevelText(*Serialized, "\"scale\": [1, 1, 1]", "\"scale\": [1, 1, 1], \"scale\": [1, 1, 1]"),
	    ReplaceLevelText(*Serialized, "\"type\": \"dynamic\"", "\"type\": \"dynamic\", \"type\": \"static\""),
	    ReplaceLevelText(*Serialized, "\"body\": {", "\"body\": {\"type\": \"static\", \"massKg\": 1, \"friction\": 0.2, \"restitution\": 0, \"linearDamping\": 0.05, \"angularDamping\": 0.05, \"gravityScale\": 1}, \"body\": {"),
	    ReplaceLevelText(*Serialized, "\"type\": \"dynamic\"", "\"type\": \"kinematic\""),
	    ReplaceLevelText(*Serialized, "\"type\": \"dynamic\"", "\"type\": \"dynamic\", \"mass\": 1"),
	    ReplaceLevelText(*Serialized, "          \"massKg\": 2.5,\n", ""),
	    ReplaceLevelText(*Serialized, "\"gravityScale\": 1.5", "\"gravityScale\": 11"),
	    ReplaceLevelText(*Serialized, "\"components\": {", "\"components\": {\"script\": {},"),
	    ReplaceLevelText(*Serialized, "00000000-0000-0001-0000-000000000001", "invalid-id"),
	    ReplaceLevelText(*Serialized, "00000000-0000-0001-0000-000000000001", "00000000-0000-0000-0000-000000000000"),
	    ReplaceLevelText(*Serialized, "00000000-0000-0003-0000-000000000003", "invalid-asset"),
	    ReplaceLevelText(*Serialized, "\"translation\": [0, 0, 0]", "\"translation\": [NaN, 0, 0]"),
	    ReplaceLevelText(*Serialized, "\"translation\": [0, 0, 0]", "\"translation\": [1e999, 0, 0]"),
	    ReplaceLevelText(*Serialized, "\"translation\": [0, 0, 0]", "\"translation\": [01, 0, 0]"),
	    ReplaceLevelText(*Serialized, "\"translation\": [0, 0, 0]", "\"translation\": [1., 0, 0]"),
	    ReplaceLevelText(*Serialized, "\"translation\": [0, 0, 0]", "\"translation\": [\"0\", 0, 0]"),
	    ReplaceLevelText(*Serialized, "\"translation\": [0, 0, 0]", "\"translation\": [0, 0]"),
	    ReplaceLevelText(*Serialized, "\"scale\": [1, 1, 1]", "\"scale\": [1e100, 1, 1]"),
	    ReplaceLevelText(*Serialized, "\"rotation\": [0, 0, 0, 1]", "\"rotation\": [0, 0, 0, 0]"),
	    ReplaceLevelText(*Serialized, "\"name\": \"Child\"", "\"name\": \"Child\\n\""),
	    ReplaceLevelText(*Serialized, "\"name\": \"Child\"", "\"name\": \"Child\\u0000\""),
	    ReplaceLevelText(*Serialized, "\"name\": \"Child\"", "\"name\": \"Child\\ud800\""),
	    Serialized->substr(0, Serialized->size() - 3),
	    *Serialized + "{}",
	    std::string{},
	};

	for (std::size_t Index = 0; Index < InvalidTexts.size(); ++Index)
	{
		CAPTURE(Index);
		CHECK_FALSE(ParseLevel(InvalidTexts[Index]));
	}
}

TEST_CASE("Levels validate graph references before accepting or saving documents")
{
	FLevelDocument Document = MakeLevelDocument();
	const auto Serialized = SerializeLevel(Document);
	REQUIRE(Serialized);
	const auto MissingParent = ReplaceLevelText(*Serialized, "\"parent\": \"00000000-0000-0002-0000-000000000002\"", "\"parent\": \"00000000-0000-0009-0000-000000000009\"");
	CHECK_FALSE(ParseLevel(MissingParent));
	const auto Cycle = ReplaceLevelText(*Serialized, "\"parent\": null", "\"parent\": \"00000000-0000-0002-0000-000000000001\"");
	CHECK_FALSE(ParseLevel(Cycle));
	const auto DuplicateId = ReplaceLevelText(*Serialized, "\"id\": \"00000000-0000-0002-0000-000000000001\"", "\"id\": \"00000000-0000-0002-0000-000000000002\"");
	CHECK_FALSE(ParseLevel(DuplicateId));
	Document.Entities[0].Parent = Document.Entities[1].Id;
	CHECK_FALSE(SerializeLevel(Document));

	FWorld World;
	REQUIRE(World.ReplaceEntities(MakeLevelDocument().Entities));
	const auto Before = World.SnapshotEntities();
	CHECK_FALSE(ParseLevel(Cycle));
	CHECK(World.SnapshotEntities() == Before);
}

TEST_CASE("Levels bound names and reject invalid UTF-8 and non-finite state")
{
	FLevelDocument Document = MakeLevelDocument();
	Document.Name = std::string(1024, 'x');
	REQUIRE(SerializeLevel(Document));
	Document.Name += 'x';
	CHECK_FALSE(SerializeLevel(Document));
	Document.Name = "Level";
	Document.Entities[0].Name = std::string(1025, 'x');
	CHECK_FALSE(SerializeLevel(Document));
	Document.Entities[0].Name = std::string("Bad\xc0\xaf", 5);
	CHECK_FALSE(SerializeLevel(Document));
	Document.Entities[0].Name = "Bad\x7f";
	CHECK_FALSE(SerializeLevel(Document));
	Document = MakeLevelDocument();
	Document.Entities[0].Transform.Translation.Meters.X = std::numeric_limits<double>::quiet_NaN();
	CHECK_FALSE(SerializeLevel(Document));
	Document = MakeLevelDocument();
	Document.Entities[0].Transform.Scale.X = std::numeric_limits<float>::infinity();
	CHECK_FALSE(SerializeLevel(Document));
	Document = MakeLevelDocument();
	const auto Serialized = SerializeLevel(Document);
	REQUIRE(Serialized);
	CHECK_FALSE(ParseLevel(ReplaceLevelText(*Serialized, "\"name\": \"Child\"", "\"name\": \"" + std::string(1025, 'x') + "\"")));
	CHECK_FALSE(ParseLevel(ReplaceLevelText(*Serialized, "\"name\": \"Child\"", std::string("\"name\": \"Bad\xc0\xaf\""))));
}

TEST_CASE("Failed level saves preserve existing files and clean sibling temporary files")
{
	Tests::FScratchDirectory Directory("HertaLevelAtomic");
	const auto Path = Directory.GetPath() / "Level.hlevel";
	FLevelDocument Document = MakeLevelDocument();
	REQUIRE(SaveLevel(Path, Document));
	const std::string Original = ReadLevelText(Path);
	Document.Name = std::string(1025, 'x');
	CHECK_FALSE(SaveLevel(Path, Document));
	CHECK(ReadLevelText(Path) == Original);
	CHECK(CountLevelTemporaryFiles(Directory.GetPath()) == 0);
	Document = MakeLevelDocument();

#ifdef _WIN32
	const HANDLE LockedFile = CreateFileW(Path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	REQUIRE(LockedFile != INVALID_HANDLE_VALUE);
	Document.Name = "Cannot replace locked file";
	const auto SaveResult = SaveLevel(Path, Document);
	const bool bClosed = CloseHandle(LockedFile) != FALSE;
	CHECK(bClosed);
	CHECK_FALSE(SaveResult);
	CHECK(ReadLevelText(Path) == Original);
	CHECK(CountLevelTemporaryFiles(Directory.GetPath()) == 0);
#endif

	const auto BlockedPath = Directory.GetPath() / "Blocked.hlevel";
	std::filesystem::create_directory(BlockedPath);
	Tests::WriteText(BlockedPath / "Original", "Preserved");
	CHECK_FALSE(SaveLevel(BlockedPath, Document));
	CHECK(ReadLevelText(BlockedPath / "Original") == "Preserved");
	CHECK(CountLevelTemporaryFiles(Directory.GetPath()) == 0);
	CHECK_FALSE(SaveLevel(Directory.GetPath() / "Missing" / "Level.hlevel", Document));
	CHECK_FALSE(LoadLevel(Directory.GetPath() / "Missing.hlevel"));
	Tests::WriteText(Path, Original.substr(0, Original.size() - 3));
	CHECK_FALSE(LoadLevel(Path));
}

TEST_CASE("Level loads reject oversized files before allocating or parsing")
{
	Tests::FScratchDirectory Directory("HertaLevelLimits");
	const auto Path = Directory.GetPath() / "Oversized.hlevel";
	std::ofstream Stream(Path, std::ios::binary);
	Stream.seekp(64 * 1024 * 1024);
	Stream.put('\0');
	Stream.close();
	REQUIRE(Stream);
	CHECK_FALSE(LoadLevel(Path));
}
}
