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

FLevelDocument MakeLevelFolderDocument()
{
	FLevelDocument Document = MakeLevelDocument();
	Document.Folders = {
	    FLevelFolder{
	        .Id = FObjectId{4, 2},
	        .Name = "Nested \\\"folder\\\"",
	        .Parent = FObjectId{4, 1},
	        .Entities = {Document.Entities[0].Id, Document.Entities[1].Id},
	    },
	    FLevelFolder{.Id = FObjectId{4, 1}, .Name = "Root folder"},
	    FLevelFolder{.Id = FObjectId{4, 3}, .Name = "Empty folder", .Parent = FObjectId{4, 2}},
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

// Schema 5 always writes camera bookmarks; older schemas must not contain them.
std::string WithoutCameraBookmarks(std::string Text)
{
	return ReplaceLevelText(std::move(Text), ",\n  \"cameraBookmarks\": []", "");
}

// Removes everything schema 5 adds to otherwise older documents: camera bookmarks and rigid body collision shapes.
std::string WithoutSchemaFiveFields(std::string Text)
{
	constexpr std::string_view Collision = ",\n          \"collision\": \"box\"";
	for (std::size_t Position = Text.find(Collision); Position != std::string::npos; Position = Text.find(Collision))
	{
		Text.erase(Position, Collision.size());
	}

	return WithoutCameraBookmarks(std::move(Text));
}

std::string MakeSchemaTwoText(std::string Text)
{
	Text = ReplaceLevelText(WithoutSchemaFiveFields(std::move(Text)), "\"engineSchemaVersion\": 5", "\"engineSchemaVersion\": 2");

	while (Text.contains(", \"materials\": []"))
	{
		Text = ReplaceLevelText(std::move(Text), ", \"materials\": []", "");
	}

	return ReplaceLevelText(std::move(Text), ",\n  \"folders\": []", "");
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
	CHECK(Serialized->find("\"engineSchemaVersion\": 5") != std::string::npos);
	CHECK(Serialized->find("\"folders\": []") != std::string::npos);
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

TEST_CASE("Schema 1 body types migrate to canonical schema 3 settings and empty folders")
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
	CHECK(Migrated->Folders.empty());
	CHECK_FALSE(ParseLevel(ReplaceLevelText(std::string(VersionOne), "\"type\": \"dynamic\"", "\"type\": \"dynamic\", \"massKg\": 1")));

	const auto Canonical = SerializeLevel(*Migrated);
	REQUIRE(Canonical);
	CHECK(Canonical->find("\"format\": \"HertaLevel\"") != std::string::npos);
	CHECK(Canonical->find("\"formatVersion\": 2") != std::string::npos);
	CHECK(Canonical->find("\"engineSchemaVersion\": 5") != std::string::npos);
	CHECK(Canonical->find("\"folders\": []") != std::string::npos);
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
	const auto Legacy = ReplaceLevelText(ReplaceLevelText(MakeSchemaTwoText(*Current), "\"HertaLevel\"", "\"HertaScene\""), "\"formatVersion\": 2", "\"formatVersion\": 1");
	const auto Migrated = ParseLevel(Legacy);
	REQUIRE(Migrated);
	CHECK(Migrated->Folders.empty());
	const auto SchemaTwo = ParseLevel(MakeSchemaTwoText(*Current));
	REQUIRE(SchemaTwo);
	CHECK(SchemaTwo->Entities == Migrated->Entities);
	CHECK(SchemaTwo->Folders.empty());
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
	const auto Legacy = ReplaceLevelText(ReplaceLevelText(MakeSchemaTwoText(*Current), "\"HertaLevel\"", "\"HertaScene\""), "\"formatVersion\": 2", "\"formatVersion\": 1");
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
	CHECK(EmptyLevel->Folders.empty());
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
	    ReplaceLevelText(*Serialized, "\"engineSchemaVersion\": 5", "\"engineSchemaVersion\": 6"),
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

TEST_CASE("Level folders round-trip canonically without changing runtime hierarchy")
{
	FLevelDocument Document = MakeLevelFolderDocument();
	const std::u8string UnicodeName = u8"Zażółć 世界 🙂";
	Document.Folders[1].Name.assign(UnicodeName.begin(), UnicodeName.end());
	const auto Serialized = SerializeLevel(Document);
	REQUIRE(Serialized);
	const std::size_t FolderOne = Serialized->find("\"id\": \"00000000-0000-0004-0000-000000000001\"");
	const std::size_t FolderTwo = Serialized->find("\"id\": \"00000000-0000-0004-0000-000000000002\"");
	const std::size_t FolderThree = Serialized->find("\"id\": \"00000000-0000-0004-0000-000000000003\"");
	CHECK(FolderOne < FolderTwo);
	CHECK(FolderTwo < FolderThree);
	CHECK(Serialized->find("\"entities\": [\"00000000-0000-0002-0000-000000000001\", \"00000000-0000-0002-0000-000000000002\"]") != std::string::npos);
	CHECK(Serialized->find("\"entities\": []") != std::string::npos);
	const auto Parsed = ParseLevel(*Serialized);
	REQUIRE(Parsed);
	std::ranges::sort(Document.Entities, {}, &FLevelEntity::Id);
	std::ranges::sort(Document.Folders, {}, &FLevelFolder::Id);

	for (auto& Folder : Document.Folders)
	{
		std::ranges::sort(Folder.Entities);
	}

	CHECK(Parsed->Entities == Document.Entities);
	CHECK(Parsed->Folders == Document.Folders);
	CHECK(Parsed->Entities[0].Parent == Parsed->Entities[1].Id);
	const auto Canonical = SerializeLevel(*Parsed);
	REQUIRE(Canonical);
	CHECK(*Canonical == *Serialized);
	std::ranges::reverse(Document.Folders);
	std::ranges::reverse(Document.Folders[1].Entities);
	const auto Reordered = SerializeLevel(Document);
	REQUIRE(Reordered);
	CHECK(*Reordered == *Serialized);

	FWorld World;
	REQUIRE(World.ReplaceEntities(Parsed->Entities));
	CHECK(World.SnapshotEntities() == Parsed->Entities);

	Tests::FScratchDirectory Directory("HertaLevelFolders");
	const auto Path = Directory.GetPath() / "Folders.hlevel";
	REQUIRE(SaveLevel(Path, Document));
	const auto Loaded = LoadLevel(Path);
	REQUIRE(Loaded);
	CHECK(Loaded->Folders == Parsed->Folders);
}

TEST_CASE("Level folder JSON rejects unknown, duplicate, missing, and invalid data")
{
	const auto Serialized = SerializeLevel(MakeLevelFolderDocument());
	REQUIRE(Serialized);
	constexpr std::string_view Members = "\"entities\": [\"00000000-0000-0002-0000-000000000001\", \"00000000-0000-0002-0000-000000000002\"]";
	const std::array InvalidTexts{
	    ReplaceLevelText(*Serialized, "\"folders\": [", "\"folders\": [], \"folders\": ["),
	    ReplaceLevelText(*Serialized, "\"folders\": [", "\"folders\": [null,"),
	    ReplaceLevelText(*Serialized, "      \"id\": \"00000000-0000-0004-0000-000000000001\",\n", ""),
	    ReplaceLevelText(*Serialized, "      \"name\": \"Root folder\",\n", ""),
	    ReplaceLevelText(*Serialized, "\"name\": \"Root folder\"", "\"name\": \"Root folder\", \"extra\": 0"),
	    ReplaceLevelText(*Serialized, "\"name\": \"Root folder\"", "\"name\": \"Root folder\", \"name\": \"Again\""),
	    ReplaceLevelText(*Serialized, "\"name\": \"Root folder\"", "\"name\": \"\""),
	    ReplaceLevelText(*Serialized, "\"name\": \"Root folder\"", "\"name\": null"),
	    ReplaceLevelText(*Serialized, "\"name\": \"Root folder\"", "\"name\": \"Bad\\n\""),
	    ReplaceLevelText(*Serialized, "\"name\": \"Root folder\"", "\"name\": \"Bad\\ud800\""),
	    ReplaceLevelText(*Serialized, "\"name\": \"Root folder\"", "\"name\": \"" + std::string(1025, 'x') + "\""),
	    ReplaceLevelText(*Serialized, "\"id\": \"00000000-0000-0004-0000-000000000001\"", "\"id\": null"),
	    ReplaceLevelText(*Serialized, "\"id\": \"00000000-0000-0004-0000-000000000001\"", "\"id\": \"00000000-0000-0004-0000-000000000001\", \"id\": \"00000000-0000-0004-0000-000000000001\""),
	    ReplaceLevelText(*Serialized, "\"id\": \"00000000-0000-0004-0000-000000000001\"", "\"id\": \"invalid\""),
	    ReplaceLevelText(*Serialized, "\"id\": \"00000000-0000-0004-0000-000000000001\"", "\"id\": \"00000000-0000-0000-0000-000000000000\""),
	    ReplaceLevelText(*Serialized, "\"id\": \"00000000-0000-0004-0000-000000000003\"", "\"id\": \"00000000-0000-0004-0000-000000000002\""),
	    ReplaceLevelText(*Serialized, "\"id\": \"00000000-0000-0004-0000-000000000003\"", "\"id\": \"00000000-0000-0002-0000-000000000001\""),
	    ReplaceLevelText(*Serialized, "      \"parent\": \"00000000-0000-0004-0000-000000000001\",\n", ""),
	    ReplaceLevelText(*Serialized, "\"parent\": \"00000000-0000-0004-0000-000000000001\"", "\"parent\": 1"),
	    ReplaceLevelText(*Serialized, "\"parent\": \"00000000-0000-0004-0000-000000000001\"", "\"parent\": null, \"parent\": null"),
	    ReplaceLevelText(*Serialized, "\"parent\": \"00000000-0000-0004-0000-000000000001\"", "\"parent\": \"00000000-0000-0009-0000-000000000009\""),
	    ReplaceLevelText(*Serialized, "\"parent\": \"00000000-0000-0004-0000-000000000001\"", "\"parent\": \"00000000-0000-0002-0000-000000000001\""),
	    ReplaceLevelText(*Serialized, "\"parent\": \"00000000-0000-0004-0000-000000000001\"", "\"parent\": \"00000000-0000-0004-0000-000000000003\""),
	    ReplaceLevelText(*Serialized, "\"parent\": \"00000000-0000-0004-0000-000000000001\"", "\"parent\": \"00000000-0000-0004-0000-000000000002\""),
	    ReplaceLevelText(*Serialized, ",\n      " + std::string(Members), ""),
	    ReplaceLevelText(*Serialized, Members, "\"entities\": null"),
	    ReplaceLevelText(*Serialized, Members, "\"entities\": [], \"entities\": []"),
	    ReplaceLevelText(*Serialized, Members, "\"entities\": [null]"),
	    ReplaceLevelText(*Serialized, Members, "\"entities\": [\"invalid\"]"),
	    ReplaceLevelText(*Serialized, Members, "\"entities\": [\"00000000-0000-0009-0000-000000000009\"]"),
	    ReplaceLevelText(*Serialized, Members, "\"entities\": [\"00000000-0000-0004-0000-000000000001\"]"),
	    ReplaceLevelText(*Serialized, Members, "\"entities\": [\"00000000-0000-0002-0000-000000000001\", \"00000000-0000-0002-0000-000000000001\"]"),
	    ReplaceLevelText(*Serialized, "\"entities\": []", "\"entities\": [\"00000000-0000-0002-0000-000000000001\"]"),
	};

	for (std::size_t Index = 0; Index < InvalidTexts.size(); ++Index)
	{
		CAPTURE(Index);
		CHECK_FALSE(ParseLevel(InvalidTexts[Index]));
	}
}

TEST_CASE("Schemas 3 and 4 accept folders and require the folders array")
{
	const auto Serialized = SerializeLevel(FLevelDocument{.Id = FObjectId{1, 1}, .Name = "Empty"});
	REQUIRE(Serialized);
	CHECK_FALSE(ParseLevel(ReplaceLevelText(*Serialized, ",\n  \"folders\": []", "")));
	CHECK_FALSE(ParseLevel(ReplaceLevelText(*Serialized, "\"folders\": []", "\"folders\": null")));
	CHECK_FALSE(ParseLevel(ReplaceLevelText(*Serialized, "\"engineSchemaVersion\": 5", "\"engineSchemaVersion\": 1")));
	CHECK_FALSE(ParseLevel(ReplaceLevelText(*Serialized, "\"engineSchemaVersion\": 5", "\"engineSchemaVersion\": 2")));
	const auto SchemaTwo = ParseLevel(MakeSchemaTwoText(*Serialized));
	REQUIRE(SchemaTwo);
	CHECK(SchemaTwo->Folders.empty());
}

TEST_CASE("Level folder metadata is validated before serialization")
{
	FLevelDocument Document = MakeLevelFolderDocument();

	SUBCASE("Invalid ID")
	{
		Document.Folders[0].Id = {};
	}

	SUBCASE("Duplicate ID")
	{
		Document.Folders[0].Id = Document.Folders[1].Id;
	}

	SUBCASE("Entity ID conflict")
	{
		Document.Folders[0].Id = Document.Entities[0].Id;
	}

	SUBCASE("Empty name")
	{
		Document.Folders[0].Name.clear();
	}

	SUBCASE("Oversized name")
	{
		Document.Folders[0].Name = std::string(1025, 'x');
	}

	SUBCASE("Invalid UTF-8 name")
	{
		Document.Folders[0].Name = std::string("Bad\xc0\xaf", 5);
	}

	SUBCASE("Control character name")
	{
		Document.Folders[0].Name = "Bad\x7f";
	}

	SUBCASE("Missing parent")
	{
		Document.Folders[0].Parent = FObjectId{9, 9};
	}

	SUBCASE("Entity parent")
	{
		Document.Folders[0].Parent = Document.Entities[0].Id;
	}

	SUBCASE("Self parent")
	{
		Document.Folders[0].Parent = Document.Folders[0].Id;
	}

	SUBCASE("Parent cycle")
	{
		Document.Folders[1].Parent = Document.Folders[2].Id;
	}

	SUBCASE("Unknown member")
	{
		Document.Folders[0].Entities.push_back(FObjectId{9, 9});
	}

	SUBCASE("Invalid member")
	{
		Document.Folders[0].Entities.push_back(FObjectId{});
	}

	SUBCASE("Duplicate member")
	{
		Document.Folders[0].Entities.push_back(Document.Folders[0].Entities[0]);
	}

	SUBCASE("Member in multiple folders")
	{
		Document.Folders[1].Entities.push_back(Document.Folders[0].Entities[0]);
	}

	CHECK_FALSE(ValidateLevelDocument(Document));
	CHECK_FALSE(SerializeLevel(Document));
}

TEST_CASE("Level folder bounds reject excessive metadata before processing it")
{
	FLevelDocument Document = MakeLevelFolderDocument();

	SUBCASE("Folder count")
	{
		Document.Folders.resize(1'000'001);
	}

	SUBCASE("Membership count")
	{
		Document.Folders[0].Entities.resize(1'000'001);
	}

	CHECK_FALSE(ValidateLevelDocument(Document));
	CHECK_FALSE(SerializeLevel(Document));
}

TEST_CASE("Deep folder hierarchies validate iteratively and allow unassigned entities")
{
	FLevelDocument Document = MakeLevelDocument();
	constexpr std::uint64_t FolderCount = 4096;

	for (std::uint64_t Index = 0; Index < FolderCount; ++Index)
	{
		Document.Folders.push_back(FLevelFolder{.Id = FObjectId{4, Index + 1}, .Name = "Folder", .Parent = Index + 1 < FolderCount ? FObjectId{4, Index + 2} : FObjectId{}});
	}

	REQUIRE(ValidateLevelDocument(Document));
	const auto Serialized = SerializeLevel(Document);
	REQUIRE(Serialized);
	const auto Parsed = ParseLevel(*Serialized);
	REQUIRE(Parsed);
	CHECK(Parsed->Folders == Document.Folders);
	Document.Folders.back().Parent = Document.Folders.front().Id;
	CHECK_FALSE(ValidateLevelDocument(Document));
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
	Document.Folders.push_back(FLevelFolder{.Id = FObjectId{4, 1}, .Name = "Folder", .Entities = {FObjectId{9, 9}}});
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

TEST_CASE("Visual level components and material slots have deterministic schema four persistence")
{
	FLevelDocument Document = MakeLevelFolderDocument();
	Document.Entities[0].Mesh->Materials = {FAssetId{8, 1}, FAssetId{}, FAssetId{8, 2}};
	Document.Entities[0].Light = FLightComponent{.Type = ELightType::Rect, .Color = {0.1f, 0.2f, 0.3f}, .Intensity = 1800.f, .Width = 2.f, .Height = 3.f, .Environment = FAssetId{9, 1}};
	Document.Entities[1].SkyAtmosphere = FSkyAtmosphereComponent{.Sun = Document.Entities[0].Id, .RayleighScattering = 1.5f};
	Document.Entities[1].HeightFog = FHeightFogComponent{.Density = 0.04f, .Albedo = {0.5f, 0.8f, 1.f}, .Quality = EFogQuality::High};
	const auto Text = SerializeLevel(Document);
	REQUIRE(Text);
	CHECK(Text->find("\"engineSchemaVersion\": 5") != std::string::npos);
	CHECK(Text->find("\"materials\": [\"" + FAssetId{8, 1}.ToString() + "\", null,") != std::string::npos);
	const auto Restored = ParseLevel(*Text);
	REQUIRE(Restored);
	std::ranges::sort(Document.Entities, {}, &FLevelEntity::Id);
	CHECK(Restored->Entities == Document.Entities);
	const auto Canonical = SerializeLevel(*Restored);
	REQUIRE(Canonical);
	CHECK(*Canonical == *Text);
	CHECK_FALSE(ParseLevel(ReplaceLevelText(*Text, "\"density\": 0.04", "\"density\": -1")));
	CHECK_FALSE(ParseLevel(ReplaceLevelText(*Text, "\"type\": \"rect\"", "\"type\": \"unknown\"")));
	CHECK_FALSE(ParseLevel(ReplaceLevelText(*Text, "\"volumetric\": true", "\"volumetric\": 1")));
	CHECK_FALSE(ParseLevel(ReplaceLevelText(*Text, "\"enabled\": true", "\"enabled\": true, \"enabled\": true")));
	CHECK_FALSE(ParseLevel(ReplaceLevelText(WithoutSchemaFiveFields(*Text), "\"engineSchemaVersion\": 5", "\"engineSchemaVersion\": 3")));
}

TEST_CASE("Soft body components persist in schema five and are unknown to schema four")
{
	FLevelDocument Document = MakeLevelFolderDocument();
	Document.Entities[0].SoftBody = FSoftBodyComponent{.Shape = ESoftBodyShape::Cloth, .Length = 2.5f, .Height = 1.5f, .Thickness = 0.02f, .MassKg = 0.75f, .Stiffness = 0.6f, .Pressure = 0.f, .Friction = 0.5f, .bPinned = false, .Material = FAssetId{8, 3}};
	Document.Entities[1].SoftBody = FSoftBodyComponent{.Shape = ESoftBodyShape::Rope, .Attachment = Document.Entities[0].Id};
	const auto Text = SerializeLevel(Document);
	REQUIRE(Text);
	CHECK(Text->find("\"softBody\": {") != std::string::npos);
	CHECK(Text->find("\"attachment\": \"" + Document.Entities[0].Id.ToString() + "\"") != std::string::npos);
	CHECK(Text->find("\"shape\": \"cloth\"") != std::string::npos);
	const auto Restored = ParseLevel(*Text);
	REQUIRE(Restored);
	std::ranges::sort(Document.Entities, {}, &FLevelEntity::Id);
	CHECK(Restored->Entities == Document.Entities);
	const auto Canonical = SerializeLevel(*Restored);
	REQUIRE(Canonical);
	CHECK(*Canonical == *Text);
	CHECK_FALSE(ParseLevel(ReplaceLevelText(*Text, "\"shape\": \"cloth\"", "\"shape\": \"jelly\"")));
	CHECK_FALSE(ParseLevel(ReplaceLevelText(*Text, "\"stiffness\": 0.6", "\"stiffness\": 2")));
	CHECK_FALSE(ParseLevel(ReplaceLevelText(WithoutSchemaFiveFields(*Text), "\"engineSchemaVersion\": 5", "\"engineSchemaVersion\": 4")));
}

TEST_CASE("Rigid body collision shapes persist in schema five and older bodies load as boxes")
{
	FLevelDocument Document = MakeLevelDocument();
	REQUIRE(!Document.Entities.empty());
	Document.Entities[0].BodyType = ELevelBodyType::Dynamic;
	Document.Entities[0].BodySettings.Collision = ELevelCollisionShape::Sphere;
	const auto Text = SerializeLevel(Document);
	REQUIRE(Text);
	CHECK(Text->find("\"collision\": \"sphere\"") != std::string::npos);
	const auto Restored = ParseLevel(*Text);
	REQUIRE(Restored);
	const auto Found = std::ranges::find(Restored->Entities, Document.Entities[0].Id, &FLevelEntity::Id);
	REQUIRE(Found != Restored->Entities.end());
	CHECK(Found->BodySettings.Collision == ELevelCollisionShape::Sphere);
	CHECK_FALSE(ParseLevel(ReplaceLevelText(*Text, "\"collision\": \"sphere\"", "\"collision\": \"cone\"")));
	CHECK_FALSE(ParseLevel(ReplaceLevelText(*Text, ",\n          \"collision\": \"sphere\"", "")));

	const auto Older = ParseLevel(MakeSchemaTwoText(*SerializeLevel(MakeLevelDocument())));
	REQUIRE(Older);
	for (const FLevelEntity& Entity : Older->Entities)
	{
		CHECK(Entity.BodySettings.Collision == ELevelCollisionShape::Box);
	}
}

TEST_CASE("Camera bookmarks persist sorted by slot in schema five and are rejected when invalid")
{
	FLevelDocument Document = MakeLevelFolderDocument();
	Document.CameraBookmarks = {
	    {.Slot = 3, .Name = "Shadow lane", .Position = FWorldPosition{15., 1.7, 8.}, .Yaw = 0.25f, .Pitch = -0.1f},
	    {.Slot = 1, .Name = "Overview", .Position = FWorldPosition{0., 12., -18.}, .Yaw = 0.f, .Pitch = -0.5f},
	};
	const auto Text = SerializeLevel(Document);
	REQUIRE(Text);
	CHECK(Text->find("\"cameraBookmarks\": [\n    {\n      \"slot\": 1,\n      \"name\": \"Overview\"") != std::string::npos);
	const auto Restored = ParseLevel(*Text);
	REQUIRE(Restored);
	REQUIRE(Restored->CameraBookmarks.size() == 2);
	CHECK(Restored->CameraBookmarks[0] == Document.CameraBookmarks[1]);
	CHECK(Restored->CameraBookmarks[1] == Document.CameraBookmarks[0]);
	const auto Canonical = SerializeLevel(*Restored);
	REQUIRE(Canonical);
	CHECK(*Canonical == *Text);
	CHECK_FALSE(ParseLevel(ReplaceLevelText(*Text, "\"slot\": 3", "\"slot\": 1")));
	CHECK_FALSE(ParseLevel(ReplaceLevelText(*Text, "\"slot\": 3", "\"slot\": 10")));
	CHECK_FALSE(ParseLevel(ReplaceLevelText(*Text, "\"slot\": 3", "\"slot\": 0")));
	CHECK_FALSE(ParseLevel(ReplaceLevelText(*Text, "\"name\": \"Overview\"", "\"name\": \"\"")));
	CHECK_FALSE(ParseLevel(ReplaceLevelText(*Text, "\"pitch\": -0.5", "\"pitch\": -1.6")));
	CHECK_FALSE(ParseLevel(ReplaceLevelText(*Text, "\"pitch\": -0.5", "\"pitch\": -0.5, \"roll\": 0")));
	CHECK_FALSE(ParseLevel(ReplaceLevelText(*Text, "\"engineSchemaVersion\": 5", "\"engineSchemaVersion\": 4")));

	const auto Empty = SerializeLevel(MakeLevelFolderDocument());
	REQUIRE(Empty);
	CHECK_FALSE(ParseLevel(WithoutCameraBookmarks(*Empty)));
	Document.CameraBookmarks[0].Yaw = std::numeric_limits<float>::infinity();
	CHECK_FALSE(SerializeLevel(Document));
}

TEST_CASE("Schema three folder metadata migrates without introducing visual components")
{
	const auto Text = SerializeLevel(MakeLevelFolderDocument());
	REQUIRE(Text);
	std::string Legacy = ReplaceLevelText(WithoutSchemaFiveFields(*Text), "\"engineSchemaVersion\": 5", "\"engineSchemaVersion\": 3");
	Legacy = ReplaceLevelText(std::move(Legacy), ", \"materials\": []", "");
	const auto Restored = ParseLevel(Legacy);
	REQUIRE(Restored);
	CHECK(Restored->Folders.size() == 3);

	for (const auto& Entity : Restored->Entities)
	{
		CHECK_FALSE(Entity.Light);
		CHECK_FALSE(Entity.SkyAtmosphere);
		CHECK_FALSE(Entity.HeightFog);
		CHECK((!Entity.Mesh || Entity.Mesh->Materials.empty()));
	}
}

TEST_CASE("Visual component validation rejects invalid values before atomic publication")
{
	FLevelEntity Entity{.Id = FObjectId{1, 1}, .Name = "Visual", .Light = FLightComponent{}, .SkyAtmosphere = FSkyAtmosphereComponent{}, .HeightFog = FHeightFogComponent{}};
	FWorld World;
	REQUIRE(World.ReplaceEntities(std::array{Entity}));
	const auto Handle = *World.FindEntity(Entity.Id);
	const auto Before = *World.GetEntity(Handle);
	Entity.Light->Intensity = std::numeric_limits<float>::infinity();
	CHECK_FALSE(World.ApplyEntityChanges(std::array{FLevelEntityChange{.Before = Before, .After = Entity}}));
	CHECK(World.GetEntity(Handle) == Before);
	Entity = Before;
	Entity.Light->InnerConeAngle = Entity.Light->OuterConeAngle + 0.01f;
	CHECK_FALSE(ValidateLevelEntities(std::array{Entity}));
	Entity = Before;
	Entity.SkyAtmosphere->MieAnisotropy = 1.f;
	CHECK_FALSE(ValidateLevelEntities(std::array{Entity}));
	Entity = Before;
	Entity.HeightFog->Quality = static_cast<EFogQuality>(255);
	CHECK_FALSE(ValidateLevelEntities(std::array{Entity}));
	Entity = Before;
	Entity.Mesh = FStaticMeshComponent{.Asset = FAssetId{1, 3}, .Materials = std::vector<FAssetId>(257)};
	CHECK_FALSE(ValidateLevelEntities(std::array{Entity}));
	Entity = Before;
	Entity.SkyAtmosphere->Sun = FObjectId{9, 9};
	CHECK(ValidateLevelEntities(std::array{Entity}));
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
