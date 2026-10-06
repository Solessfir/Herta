#include "EditorLevel.h"
#include "TestFiles.h"

#include <doctest/doctest.h>

#include <array>
#include <limits>

namespace Herta
{
namespace
{
std::vector<FLevelFolder> FolderSnapshot(const FEditorLevel& Level)
{
	const auto Folders = Level.GetFolders();
	return {Folders.begin(), Folders.end()};
}

const FLevelFolder& FindFolder(const FEditorLevel& Level, const FObjectId Id)
{
	const auto Folders = Level.GetFolders();
	const auto Found = std::ranges::find(Folders, Id, &FLevelFolder::Id);
	REQUIRE(Found != Folders.end());
	return *Found;
}

FLevelDocument FolderLevelDocument()
{
	return {
	    .Id = FObjectId{1, 1},
	    .Name = "Folder fixtures",
	    .Entities = {
	        {.Id = FObjectId{2, 1}, .Name = "Root", .Transform = {.Translation = FWorldPosition{1., 2., 3.}}},
	        {.Id = FObjectId{3, 1}, .Name = "Child", .Parent = FObjectId{2, 1}, .Transform = {.Translation = FWorldPosition{4., 0., 0.}}},
	        {.Id = FObjectId{4, 1}, .Name = "Grandchild", .Parent = FObjectId{3, 1}},
	        {.Id = FObjectId{5, 1}, .Name = "Other"},
	    },
	    .Folders = {
	        {.Id = FObjectId{6, 1}, .Name = "First", .Entities = {FObjectId{2, 1}}},
	        {.Id = FObjectId{7, 1}, .Name = "Second", .Entities = {FObjectId{3, 1}, FObjectId{5, 1}}},
	    },
	};
}
}

TEST_CASE("Editor folders create rename and nest without changing runtime entities or preview generation")
{
	FEditorLevel Level;
	const auto Entities = Level.GetWorld().SnapshotEntities();
	const auto Generation = Level.GetGeneration();
	const auto First = Level.CreateFolder();
	REQUIRE(First);
	CHECK(FindFolder(Level, *First).Name == "New Folder");
	CHECK(Level.IsDirty());
	CHECK(Level.GetUndoLabel() == "Create folder");
	REQUIRE(Level.Undo());
	CHECK(Level.GetFolders().empty());
	CHECK_FALSE(Level.IsDirty());
	REQUIRE(Level.Redo());
	CHECK(FindFolder(Level, *First).Name == "New Folder");
	const auto Second = Level.CreateFolder();
	REQUIRE(Second);
	CHECK(FindFolder(Level, *Second).Name == "New Folder 2");
	const auto Child = Level.CreateFolder(*First);
	REQUIRE(Child);
	CHECK(FindFolder(Level, *Child).Name == "New Folder");
	REQUIRE(Level.RenameFolder(*Child, "Nested"));
	REQUIRE(Level.MoveFolder(*Child, *Second));
	CHECK(FindFolder(Level, *Child).Parent == *Second);
	REQUIRE(Level.Undo());
	CHECK(FindFolder(Level, *Child).Parent == *First);
	REQUIRE(Level.Undo());
	CHECK(FindFolder(Level, *Child).Name == "New Folder");
	REQUIRE(Level.Redo());
	REQUIRE(Level.Redo());
	CHECK(FindFolder(Level, *Child).Name == "Nested");
	CHECK(Level.GetWorld().SnapshotEntities() == Entities);
	CHECK(Level.GetGeneration() == Generation);
}

TEST_CASE("Editor folder admission rejects missing objects cycles invalid names and blocked authoring atomically")
{
	FEditorLevel Level;
	const auto Parent = Level.CreateFolder();
	REQUIRE(Parent);
	const auto Child = Level.CreateFolder(*Parent);
	REQUIRE(Child);
	const auto Before = FolderSnapshot(Level);
	const auto Entities = Level.GetWorld().SnapshotEntities();
	const auto Generation = Level.GetGeneration();
	const auto Missing = FObjectId::Generate();
	CHECK_FALSE(Level.MoveFolder(*Parent, *Child));
	CHECK_FALSE(Level.MoveFolder(*Child, *Child));
	CHECK_FALSE(Level.MoveFolder(*Child, Missing));
	CHECK_FALSE(Level.MoveFolder(Missing, {}));
	CHECK_FALSE(Level.RenameFolder(*Parent, ""));
	CHECK_FALSE(Level.RenameFolder(*Parent, std::string(1025, 'x')));
	CHECK_FALSE(Level.RenameFolder(*Parent, std::string("bad\xff")));
	CHECK_FALSE(Level.RenameFolder(Missing, "Missing"));
	CHECK_FALSE(Level.DeleteFolder(Missing));
	CHECK_FALSE(Level.CreateFolder(Missing));
	CHECK_FALSE(Level.CreateFolder({}, std::array{Missing}));
	CHECK_FALSE(Level.MoveEntitiesToFolder(std::array{Entities[0].Id, Missing}, *Parent));
	CHECK_FALSE(Level.MoveEntitiesToFolder(std::array{Entities[0].Id}, Missing));
	CHECK(FolderSnapshot(Level) == Before);
	CHECK(Level.GetWorld().SnapshotEntities() == Entities);
	CHECK(Level.GetGeneration() == Generation);
	CHECK(Level.GetUndoLabel() == "Create folder");

	for (const bool bSimulation : {false, true})
	{
		if (bSimulation)
		{
			Level.SetSimulationRunning(true);
		}
		else
		{
			REQUIRE(Level.BeginEdit("Blocked folders"));
		}

		CHECK_FALSE(Level.CreateFolder());
		CHECK_FALSE(Level.RenameFolder(*Parent, "Blocked"));
		CHECK_FALSE(Level.DeleteFolder(*Parent));
		CHECK_FALSE(Level.MoveFolder(*Child, {}));
		CHECK_FALSE(Level.MoveEntitiesToFolder(std::array{Entities[0].Id}, *Parent));
		CHECK(FolderSnapshot(Level) == Before);
		CHECK(Level.GetWorld().SnapshotEntities() == Entities);

		if (!bSimulation)
		{
			REQUIRE(Level.CancelEdit());
		}
	}
}

TEST_CASE("Editor folder entity moves organize whole assemblies without reparenting or pose changes")
{
	FEditorLevel Level;
	REQUIRE(Level.LoadDocument(FolderLevelDocument(), {}));
	const auto Before = FolderSnapshot(Level);
	const auto Entities = Level.GetWorld().SnapshotEntities();
	const auto Generation = Level.GetGeneration();
	const auto Root = FObjectId{2, 1};
	const auto Child = FObjectId{3, 1};
	const auto Second = FObjectId{7, 1};
	REQUIRE(Level.MoveEntitiesToFolder(std::array{Child, Root, Child}, Second));
	CHECK(FindFolder(Level, FObjectId{6, 1}).Entities.empty());
	CHECK(FindFolder(Level, Second).Entities == std::vector<FObjectId>{Root, FObjectId{5, 1}});
	CHECK(Level.GetWorld().SnapshotEntities() == Entities);
	CHECK(Level.GetGeneration() == Generation);
	REQUIRE(Level.Undo());
	CHECK(FolderSnapshot(Level) == Before);
	CHECK_FALSE(Level.IsDirty());
	REQUIRE(Level.Redo());
	REQUIRE(Level.MoveEntitiesToFolder(std::array{Child}, {}));
	CHECK(FindFolder(Level, Second).Entities == std::vector<FObjectId>{FObjectId{5, 1}});
	REQUIRE(Level.Undo());
	REQUIRE(Level.Undo());
	const auto Created = Level.CreateFolder({}, std::array{FObjectId{4, 1}});
	REQUIRE(Created);
	CHECK(FindFolder(Level, *Created).Entities == std::vector<FObjectId>{Root});
	CHECK(FindFolder(Level, Second).Entities == std::vector<FObjectId>{FObjectId{5, 1}});
	REQUIRE(Level.Undo());
	CHECK(FolderSnapshot(Level) == Before);
	CHECK(Level.GetWorld().SnapshotEntities() == Entities);
}

TEST_CASE("Deleting editor folders unwraps entities and child folders with undo and redo")
{
	FEditorLevel Level;
	REQUIRE(Level.LoadDocument(FolderLevelDocument(), {}));
	const auto First = FObjectId{6, 1};
	const auto Second = FObjectId{7, 1};
	REQUIRE(Level.MoveFolder(Second, First));
	const auto Child = Level.CreateFolder(Second);
	REQUIRE(Child);
	const auto Entities = Level.GetWorld().SnapshotEntities();
	const auto Before = FolderSnapshot(Level);
	REQUIRE(Level.DeleteFolder(Second));
	CHECK(FindFolder(Level, *Child).Parent == First);
	CHECK(FindFolder(Level, First).Entities == std::vector<FObjectId>{FObjectId{2, 1}, FObjectId{3, 1}, FObjectId{5, 1}});
	REQUIRE(Level.Undo());
	CHECK(FolderSnapshot(Level) == Before);
	REQUIRE(Level.Redo());
	REQUIRE(Level.DeleteFolder(First));
	CHECK(Level.GetFolders().size() == 1);
	CHECK_FALSE(FindFolder(Level, *Child).Parent.IsValid());
	CHECK(FindFolder(Level, *Child).Entities.empty());
	CHECK(Level.GetWorld().SnapshotEntities() == Entities);
	REQUIRE(Level.Undo());
	REQUIRE(Level.Undo());
	CHECK(FolderSnapshot(Level) == Before);
}

TEST_CASE("Editor folder save load and entity-only history preserve organization")
{
	Tests::FScratchDirectory Scratch("HertaEditorFolders");
	FEditorLevel Level;
	REQUIRE(Level.LoadDocument(FolderLevelDocument(), {}));
	const auto Before = FolderSnapshot(Level);
	const auto Path = Scratch.GetPath() / "Folders.hlevel";
	REQUIRE(Level.Save(Path));
	CHECK_FALSE(Level.IsDirty());
	FEditorLevel Loaded;
	REQUIRE(Loaded.Load(Path));
	CHECK(FolderSnapshot(Loaded) == Before);
	CHECK(Loaded.GetWorld().SnapshotEntities() == Level.GetWorld().SnapshotEntities());
	REQUIRE(Level.RenameFolder(FObjectId{6, 1}, "Saved name"));
	REQUIRE(Level.Save());
	CHECK_FALSE(Level.IsDirty());
	REQUIRE(Level.Undo());
	CHECK(Level.IsDirty());
	REQUIRE(Level.Redo());
	CHECK_FALSE(Level.IsDirty());
	const auto SavedFolders = FolderSnapshot(Level);
	Level.GetObjects()[0].Label = "Renamed entity";
	REQUIRE(Level.CommitEdits());
	REQUIRE(Level.Undo());
	CHECK(FolderSnapshot(Level) == SavedFolders);
	REQUIRE(Level.Redo());
	CHECK(FolderSnapshot(Level) == SavedFolders);
	auto Invalid = FolderLevelDocument();
	Invalid.Folders[0].Entities.push_back(FObjectId::Generate());
	CHECK_FALSE(Level.LoadDocument(std::move(Invalid), {}));
	CHECK(FolderSnapshot(Level) == SavedFolders);
	CHECK(Level.GetPath() == Path);
}

TEST_CASE("Entity deletion duplication and clipboard maintain editor folder referential integrity")
{
	FEditorLevel Level;
	REQUIRE(Level.LoadDocument(FolderLevelDocument(), {}));
	const auto Root = FObjectId{2, 1};
	const auto Before = FolderSnapshot(Level);
	const auto Entities = Level.GetWorld().SnapshotEntities();
	Level.SetSelection(std::array{Root, FObjectId{3, 1}}, Root);
	REQUIRE(Level.DuplicateSelected());
	CHECK(FindFolder(Level, FObjectId{6, 1}).Entities.size() == 2);
	CHECK(FindFolder(Level, FObjectId{7, 1}).Entities.size() == 3);
	const auto Duplicated = FolderSnapshot(Level);
	REQUIRE(Level.Undo());
	CHECK(FolderSnapshot(Level) == Before);
	CHECK(Level.GetWorld().SnapshotEntities() == Entities);
	REQUIRE(Level.Redo());
	CHECK(FolderSnapshot(Level) == Duplicated);
	REQUIRE(Level.DeleteSelected());
	CHECK(FolderSnapshot(Level) == Before);
	REQUIRE(Level.Undo());
	CHECK(FolderSnapshot(Level) == Duplicated);
	REQUIRE(Level.Undo());
	const auto Clipboard = Level.CopySelected();
	REQUIRE(Clipboard);
	const auto Document = ParseLevel(*Clipboard);
	REQUIRE(Document);
	CHECK(Document->Folders.empty());
	REQUIRE(Level.PasteEntities(*Clipboard));
	CHECK(FolderSnapshot(Level) == Before);
	REQUIRE(Level.Undo());
	Level.SetSelection(std::array{Root}, Root);
	REQUIRE(Level.DeleteSelected());
	CHECK(FindFolder(Level, FObjectId{6, 1}).Entities.empty());
	CHECK_FALSE(Level.GetWorld().GetEntity(*Level.GetWorld().FindEntity(FObjectId{3, 1}))->Parent.IsValid());
	REQUIRE(Level.Undo());
	CHECK(FolderSnapshot(Level) == Before);
	CHECK(Level.GetWorld().SnapshotEntities() == Entities);
}

TEST_CASE("Gesture duplication commits cancels and rolls back entity and folder snapshots together")
{
	for (const bool bCancel : {false, true})
	{
		FEditorLevel Level;
		REQUIRE(Level.LoadDocument(FolderLevelDocument(), {}));
		const auto Before = FolderSnapshot(Level);
		const auto Entities = Level.GetWorld().SnapshotEntities();
		Level.SetSelection(std::array{FObjectId{2, 1}}, FObjectId{2, 1});
		REQUIRE(Level.BeginEdit("Duplicate gesture"));
		REQUIRE(Level.DuplicateSelected(true));
		CHECK(Level.IsDirty());
		CHECK(FindFolder(Level, FObjectId{6, 1}).Entities.size() == 2);
		const auto Duplicated = FolderSnapshot(Level);

		if (bCancel)
		{
			REQUIRE(Level.CancelEdit());
		}
		else
		{
			REQUIRE(Level.EndEdit());
			REQUIRE(Level.Undo());
			CHECK(FolderSnapshot(Level) == Before);
			REQUIRE(Level.Redo());
			CHECK(FolderSnapshot(Level) == Duplicated);
			REQUIRE(Level.Undo());
		}

		CHECK(FolderSnapshot(Level) == Before);
		CHECK(Level.GetWorld().SnapshotEntities() == Entities);
		CHECK_FALSE(Level.IsDirty());
	}

	FEditorLevel Limited(0);
	REQUIRE(Limited.LoadDocument(FolderLevelDocument(), {}));
	const auto Before = FolderSnapshot(Limited);
	const auto Entities = Limited.GetWorld().SnapshotEntities();
	Limited.SetSelection(std::array{FObjectId{2, 1}});
	REQUIRE(Limited.BeginEdit("Rejected duplicate gesture"));
	REQUIRE(Limited.DuplicateSelected(true));
	CHECK_FALSE(Limited.EndEdit());
	CHECK_FALSE(Limited.HasActiveEdit());
	CHECK_FALSE(Limited.IsDirty());
	CHECK(FolderSnapshot(Limited) == Before);
	CHECK(Limited.GetWorld().SnapshotEntities() == Entities);
}

TEST_CASE("Detached hierarchy roots inherit organization without overriding explicit folder membership")
{
	for (const bool bDelete : {false, true})
	{
		for (const bool bExplicit : {false, true})
		{
			FEditorLevel Level;
			auto Document = FolderLevelDocument();
			if (!bExplicit)
			{
				Document.Folders[1].Entities.erase(Document.Folders[1].Entities.begin());
			}

			REQUIRE(Level.LoadDocument(std::move(Document), {}));
			const auto Before = FolderSnapshot(Level);
			const auto Entities = Level.GetWorld().SnapshotEntities();
			const auto ChildPose = Level.GetObjects()[1].Translation;
			const auto Root = FObjectId{2, 1};
			const auto Child = FObjectId{3, 1};

			if (bDelete)
			{
				Level.SetSelection(std::array{Root});
				REQUIRE(Level.DeleteSelected());
			}
			else
			{
				REQUIRE(Level.ReparentEntities(std::array{Child}, {}));
			}

			const auto& First = FindFolder(Level, FObjectId{6, 1});
			CHECK(std::ranges::any_of(First.Entities, [Child](const FObjectId Entity)
			{
				return Entity == Child;
			}) == !bExplicit);
			CHECK(std::ranges::any_of(FindFolder(Level, FObjectId{7, 1}).Entities, [Child](const FObjectId Entity)
			{
				return Entity == Child;
			}) == bExplicit);
			const auto Found = std::ranges::find(Level.GetObjects(), Child, &FPreviewObject::Id);
			REQUIRE(Found != Level.GetObjects().end());
			CHECK(Found->Translation.x == ChildPose.x);
			CHECK(Found->Translation.y == ChildPose.y);
			CHECK(Found->Translation.z == ChildPose.z);
			CHECK_FALSE(Found->Parent);
			const auto After = FolderSnapshot(Level);
			REQUIRE(Level.Undo());
			CHECK(FolderSnapshot(Level) == Before);
			CHECK(Level.GetWorld().SnapshotEntities() == Entities);
			REQUIRE(Level.Redo());
			CHECK(FolderSnapshot(Level) == After);
		}
	}
}

TEST_CASE("Folder no-ops preserve redo and metadata is included in bounded history admission")
{
	FEditorLevel Level;
	REQUIRE(Level.LoadDocument(FolderLevelDocument(), {}));
	const auto First = FObjectId{6, 1};
	REQUIRE(Level.RenameFolder(First, "Changed"));
	REQUIRE(Level.Undo());
	REQUIRE(Level.RenameFolder(First, "First"));
	REQUIRE(Level.MoveFolder(First, {}));
	REQUIRE(Level.MoveEntitiesToFolder(std::array{FObjectId{5, 1}}, FObjectId{7, 1}));
	CHECK(Level.CanRedo());
	CHECK_FALSE(Level.IsDirty());

	FEditorLevel Limited(256, 1);
	REQUIRE(Limited.LoadDocument(FolderLevelDocument(), {}));
	const auto Before = FolderSnapshot(Limited);
	const auto Entities = Limited.GetWorld().SnapshotEntities();
	const auto Generation = Limited.GetGeneration();
	CHECK_FALSE(Limited.CreateFolder());
	CHECK_FALSE(Limited.RenameFolder(First, "Changed"));
	CHECK_FALSE(Limited.DeleteFolder(First));
	CHECK_FALSE(Limited.MoveEntitiesToFolder(std::array{FObjectId{5, 1}}, First));
	CHECK_FALSE(Limited.DeleteSelected());
	CHECK_FALSE(Limited.DuplicateSelected());
	CHECK(FolderSnapshot(Limited) == Before);
	CHECK(Limited.GetWorld().SnapshotEntities() == Entities);
	CHECK(Limited.GetGeneration() == Generation);
	CHECK_FALSE(Limited.IsDirty());
	CHECK_FALSE(Limited.CanUndo());
}
}
