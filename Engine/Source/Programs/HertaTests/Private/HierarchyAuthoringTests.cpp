#include "EditorLevel.h"
#include "TestFiles.h"

#include <doctest/doctest.h>

#include <array>
#include <limits>

namespace Herta
{
namespace
{
constexpr FObjectId HierarchyRoot{30, 1};
constexpr FObjectId HierarchyChild{20, 1};
constexpr FObjectId HierarchyGrandchild{10, 1};
constexpr FObjectId HierarchyTarget{40, 1};

FLevelDocument MakeHierarchyDocument()
{
	return {
	    .Id = {1, 1},
	    .Name = "Hierarchy",
	    .Entities = {
	        {.Id = HierarchyGrandchild, .Name = "Grandchild", .Parent = HierarchyChild, .Transform = {.Translation = FWorldPosition{1., 0., 0.}}},
	        {.Id = HierarchyChild, .Name = "Child", .Parent = HierarchyRoot, .Transform = {.Translation = FWorldPosition{1., 3., 2.}, .Rotation = FQuaternion::FromAxisAngle({0.f, 0.f, 1.f}, 0.25f), .Scale = {0.8f, 1.2f, 1.f}}},
	        {.Id = HierarchyRoot, .Name = "Root", .Transform = {.Translation = FWorldPosition{10., 2., -4.}, .Rotation = FQuaternion::FromAxisAngle({0.f, 1.f, 0.f}, 0.3f), .Scale = {2.f, 2.f, 2.f}}},
	        {.Id = HierarchyTarget, .Name = "Target", .Transform = {.Translation = FWorldPosition{-4., 0., 1.}, .Rotation = FQuaternion::FromAxisAngle({0.f, 1.f, 0.f}, -0.2f), .Scale = {0.5f, 0.5f, 0.5f}}},
	    },
	};
}

void LoadHierarchy(FEditorLevel& Level, const std::filesystem::path& Directory)
{
	const auto Path = Directory / "Hierarchy.hlevel";
	REQUIRE(SaveLevel(Path, MakeHierarchyDocument()));
	REQUIRE(Level.Load(Path));
}

FLevelEntity ReadHierarchyEntity(const FEditorLevel& Level, const FObjectId Id)
{
	const auto Handle = Level.GetWorld().FindEntity(Id);
	REQUIRE(Handle);
	const auto Entity = Level.GetWorld().GetEntity(*Handle);
	REQUIRE(Entity);
	return *Entity;
}

TMatrix4<double> ReadHierarchyWorld(const FEditorLevel& Level, const FObjectId Id)
{
	const auto Handle = Level.GetWorld().FindEntity(Id);
	REQUIRE(Handle);
	const auto Matrix = Level.GetWorld().GetWorldMatrix(*Handle);
	REQUIRE(Matrix);
	return *Matrix;
}

FPreviewObject& ReadHierarchyPreview(FEditorLevel& Level, const FObjectId Id)
{
	const auto Object = std::ranges::find(Level.GetObjects(), Id, &FPreviewObject::Id);
	REQUIRE(Object != Level.GetObjects().end());
	return *Object;
}

void CheckHierarchyMatrix(const TMatrix4<double>& Actual, const TMatrix4<double>& Expected)
{
	for (std::size_t Index = 0; Index < Actual.Data().size(); ++Index)
	{
		CHECK(Actual.Data()[Index] == doctest::Approx(Expected.Data()[Index]).epsilon(0.00001));
	}
}

void CheckHierarchyPreview(FEditorLevel& Level, const FObjectId Id)
{
	const auto Expected = ReadHierarchyWorld(Level, Id);
	const auto& Object = ReadHierarchyPreview(Level, Id);
	const Im3d::Mat4 Actual(Object.Translation, Object.Rotation, Object.Scale);
	for (int Row = 0; Row < 4; ++Row)
	{
		for (int Column = 0; Column < 4; ++Column)
		{
			CHECK(Actual(Row, Column) == doctest::Approx(Expected(static_cast<std::size_t>(Row), static_cast<std::size_t>(Column))).epsilon(0.00001));
		}
	}
}
}

TEST_CASE("Hierarchy authoring previews world poses and reparents selected roots as one transaction")
{
	Tests::FScratchDirectory Scratch("HertaHierarchyReparent");
	FEditorLevel Level;
	LoadHierarchy(Level, Scratch.GetPath());
	CHECK_FALSE(Level.IsDirty());
	CheckHierarchyPreview(Level, HierarchyChild);
	CheckHierarchyPreview(Level, HierarchyGrandchild);
	CHECK(ReadHierarchyPreview(Level, HierarchyChild).Parent == HierarchyRoot);
	const auto Before = Level.GetWorld().SnapshotEntities();
	const auto RootWorld = ReadHierarchyWorld(Level, HierarchyRoot);
	const auto ChildWorld = ReadHierarchyWorld(Level, HierarchyChild);
	const auto GrandchildWorld = ReadHierarchyWorld(Level, HierarchyGrandchild);
	const auto RootHandle = Level.GetWorld().FindEntity(HierarchyRoot);
	std::array Selected{HierarchyChild, HierarchyRoot};
	SUBCASE("Child before parent")
	{
	}

	SUBCASE("Parent before child")
	{
		Selected = {HierarchyRoot, HierarchyChild};
	}

	Level.SetSelection(Selected, HierarchyChild);
	REQUIRE(Level.ReparentEntities(Selected, HierarchyTarget));
	CHECK(ReadHierarchyEntity(Level, HierarchyRoot).Parent == HierarchyTarget);
	CHECK(ReadHierarchyEntity(Level, HierarchyChild).Parent == HierarchyRoot);
	CHECK(Level.GetWorld().FindEntity(HierarchyRoot) == RootHandle);
	CHECK(Level.GetActiveObject() == HierarchyChild);
	CHECK(Level.IsDirty());
	CheckHierarchyMatrix(ReadHierarchyWorld(Level, HierarchyRoot), RootWorld);
	CheckHierarchyMatrix(ReadHierarchyWorld(Level, HierarchyChild), ChildWorld);
	CheckHierarchyMatrix(ReadHierarchyWorld(Level, HierarchyGrandchild), GrandchildWorld);
	const auto After = Level.GetWorld().SnapshotEntities();
	REQUIRE(Level.Undo());
	CHECK(Level.GetWorld().SnapshotEntities() == Before);
	CHECK_FALSE(Level.IsDirty());
	REQUIRE(Level.Redo());
	CHECK(Level.GetWorld().SnapshotEntities() == After);
	REQUIRE(Level.Save());
	FEditorLevel Loaded;
	REQUIRE(Loaded.Load(Level.GetPath()));
	CHECK(Loaded.GetWorld().SnapshotEntities() == After);
	CheckHierarchyPreview(Loaded, HierarchyGrandchild);
}

TEST_CASE("Hierarchy reparent rejects cycles self and stale IDs without changing state")
{
	Tests::FScratchDirectory Scratch("HertaHierarchyFailures");
	FEditorLevel Level;
	LoadHierarchy(Level, Scratch.GetPath());
	const auto Before = Level.GetWorld().SnapshotEntities();
	const auto Selection = std::vector<FObjectId>(Level.GetSelection().begin(), Level.GetSelection().end());
	const auto Generation = Level.GetGeneration();
	const std::array Root{HierarchyRoot};
	CHECK_FALSE(Level.ReparentEntities(Root, HierarchyRoot));
	CHECK_FALSE(Level.ReparentEntities(Root, HierarchyGrandchild));
	CHECK_FALSE(Level.ReparentEntities(Root, FObjectId{99, 99}));
	const std::array Stale{FObjectId{99, 99}};
	CHECK_FALSE(Level.ReparentEntities(Stale, HierarchyTarget));
	CHECK(Level.GetWorld().SnapshotEntities() == Before);
	CHECK(std::vector<FObjectId>(Level.GetSelection().begin(), Level.GetSelection().end()) == Selection);
	CHECK(Level.GetGeneration() == Generation);
	CHECK_FALSE(Level.CanUndo());
	CHECK_FALSE(Level.IsDirty());
	const std::array Child{HierarchyChild};
	REQUIRE(Level.ReparentEntities(Child, HierarchyRoot));
	CHECK_FALSE(Level.CanUndo());
	Level.SetSimulationRunning(true);
	CHECK_FALSE(Level.ReparentEntities(Child, std::nullopt));
	CHECK(Level.GetWorld().SnapshotEntities() == Before);
}

TEST_CASE("Hierarchy edits move unselected descendants and preserve locals during grouped world edits")
{
	Tests::FScratchDirectory Scratch("HertaHierarchyEdits");
	FEditorLevel Level;
	LoadHierarchy(Level, Scratch.GetPath());
	const auto Before = Level.GetWorld().SnapshotEntities();
	const auto ChildLocal = ReadHierarchyEntity(Level, HierarchyChild).Transform;
	const auto GrandchildLocal = ReadHierarchyEntity(Level, HierarchyGrandchild).Transform;
	const auto ChildWorld = ReadHierarchyWorld(Level, HierarchyChild);
	const auto GrandchildWorld = ReadHierarchyWorld(Level, HierarchyGrandchild);
	const auto Generation = Level.GetGeneration();
	const auto* ObjectsData = Level.GetObjects().data();
	REQUIRE(Level.BeginEdit("Move hierarchy"));
	ReadHierarchyPreview(Level, HierarchyRoot).Translation.x += 3.f;
	REQUIRE(Level.CommitEdits());
	CHECK(Level.GetGeneration() == Generation);
	CHECK(Level.GetObjects().data() == ObjectsData);
	CHECK(ReadHierarchyEntity(Level, HierarchyChild).Transform == ChildLocal);
	CHECK(ReadHierarchyEntity(Level, HierarchyGrandchild).Transform == GrandchildLocal);
	CheckHierarchyMatrix(ReadHierarchyWorld(Level, HierarchyChild), TMatrix4<double>::Translation({3., 0., 0.}) * ChildWorld);
	CheckHierarchyPreview(Level, HierarchyGrandchild);
	ReadHierarchyPreview(Level, HierarchyRoot).Translation.y += 2.f;
	ReadHierarchyPreview(Level, HierarchyChild).Translation.y += 2.f;
	REQUIRE(Level.EndEdit());
	CHECK(ReadHierarchyEntity(Level, HierarchyChild).Transform.Translation.Meters.IsNearlyEqual(ChildLocal.Translation.Meters, 0.00001));
	CheckHierarchyMatrix(ReadHierarchyWorld(Level, HierarchyGrandchild), TMatrix4<double>::Translation({3., 2., 0.}) * GrandchildWorld);
	REQUIRE(Level.Undo());
	CHECK(Level.GetWorld().SnapshotEntities() == Before);
	CHECK_FALSE(Level.CanUndo());
}

TEST_CASE("Hierarchy child world edits and unparenting preserve sibling poses")
{
	Tests::FScratchDirectory Scratch("HertaHierarchyChildEdit");
	FEditorLevel Level;
	LoadHierarchy(Level, Scratch.GetPath());
	const auto RootBefore = ReadHierarchyEntity(Level, HierarchyRoot);
	const auto ChildWorld = ReadHierarchyWorld(Level, HierarchyChild);
	ReadHierarchyPreview(Level, HierarchyChild).Translation.z += 4.f;
	REQUIRE(Level.CommitEdits());
	CHECK(ReadHierarchyEntity(Level, HierarchyRoot) == RootBefore);
	const auto Expected = TMatrix4<double>::Translation({0., 0., 4.}) * ChildWorld;
	CheckHierarchyMatrix(ReadHierarchyWorld(Level, HierarchyChild), Expected);
	const std::array Selected{HierarchyChild};
	REQUIRE(Level.ReparentEntities(Selected, std::nullopt));
	CHECK_FALSE(ReadHierarchyEntity(Level, HierarchyChild).Parent.IsValid());
	CHECK(ReadHierarchyEntity(Level, HierarchyGrandchild).Parent == HierarchyChild);
	CheckHierarchyMatrix(ReadHierarchyWorld(Level, HierarchyChild), Expected);
	CheckHierarchyPreview(Level, HierarchyGrandchild);
}

TEST_CASE("Deleting hierarchy parents detaches surviving children without deleting descendants")
{
	Tests::FScratchDirectory Scratch("HertaHierarchyDelete");
	FEditorLevel Level;
	LoadHierarchy(Level, Scratch.GetPath());
	const auto Before = Level.GetWorld().SnapshotEntities();
	const auto ChildWorld = ReadHierarchyWorld(Level, HierarchyChild);
	const auto GrandchildWorld = ReadHierarchyWorld(Level, HierarchyGrandchild);
	std::vector Selected{HierarchyRoot};
	SUBCASE("Delete only the parent")
	{
	}

	SUBCASE("Also delete the child but retain its descendant")
	{
		Selected.push_back(HierarchyChild);
	}

	Level.SetSelection(Selected, HierarchyRoot);
	REQUIRE(Level.DeleteSelected());
	CHECK_FALSE(Level.GetWorld().FindEntity(HierarchyRoot));
	if (Selected.size() == 1)
	{
		CHECK_FALSE(ReadHierarchyEntity(Level, HierarchyChild).Parent.IsValid());
		CHECK(ReadHierarchyEntity(Level, HierarchyGrandchild).Parent == HierarchyChild);
		CheckHierarchyMatrix(ReadHierarchyWorld(Level, HierarchyChild), ChildWorld);
	}
	else
	{
		CHECK_FALSE(Level.GetWorld().FindEntity(HierarchyChild));
		CHECK_FALSE(ReadHierarchyEntity(Level, HierarchyGrandchild).Parent.IsValid());
	}

	CheckHierarchyMatrix(ReadHierarchyWorld(Level, HierarchyGrandchild), GrandchildWorld);
	const auto After = Level.GetWorld().SnapshotEntities();
	REQUIRE(Level.Undo());
	CHECK(Level.GetWorld().SnapshotEntities() == Before);
	REQUIRE(Level.Redo());
	CHECK(Level.GetWorld().SnapshotEntities() == After);
}

TEST_CASE("Hierarchical duplication remaps selected parents and applies world offsets once")
{
	Tests::FScratchDirectory Scratch("HertaHierarchyDuplicate");
	FEditorLevel Level;
	LoadHierarchy(Level, Scratch.GetPath());
	const auto Before = Level.GetWorld().SnapshotEntities();
	const auto RootWorld = ReadHierarchyWorld(Level, HierarchyRoot);
	const auto ChildWorld = ReadHierarchyWorld(Level, HierarchyChild);
	std::vector Selected{HierarchyRoot, HierarchyChild};
	SUBCASE("Duplicate parent and child together")
	{
	}

	SUBCASE("Duplicate a child under its existing parent")
	{
		Selected = {HierarchyChild};
	}

	Level.SetSelection(Selected, HierarchyChild);
	REQUIRE(Level.DuplicateSelected(false, {3., 1., -2.}));
	FObjectId RootCopy;
	FObjectId ChildCopy;
	for (const auto& Entity : Level.GetWorld().SnapshotEntities())
	{
		if (Entity.Name == "Root Copy")
		{
			RootCopy = Entity.Id;
		}

		if (Entity.Name == "Child Copy")
		{
			ChildCopy = Entity.Id;
		}
	}

	REQUIRE(ChildCopy.IsValid());
	CHECK(ReadHierarchyEntity(Level, ChildCopy).Parent == (RootCopy.IsValid() ? RootCopy : HierarchyRoot));
	CheckHierarchyMatrix(ReadHierarchyWorld(Level, ChildCopy), TMatrix4<double>::Translation({3., 1., -2.}) * ChildWorld);
	if (RootCopy.IsValid())
	{
		CheckHierarchyMatrix(ReadHierarchyWorld(Level, RootCopy), TMatrix4<double>::Translation({3., 1., -2.}) * RootWorld);
	}

	REQUIRE(Level.Undo());
	CHECK(Level.GetWorld().SnapshotEntities() == Before);
}

TEST_CASE("Hierarchy clipboard remaps internal links and roots fragments copied without parents")
{
	Tests::FScratchDirectory Scratch("HertaHierarchyClipboard");
	FEditorLevel Source;
	LoadHierarchy(Source, Scratch.GetPath());
	const auto ChildWorld = ReadHierarchyWorld(Source, HierarchyChild);
	std::vector Selected{HierarchyRoot, HierarchyChild};
	SUBCASE("Copy parent and child together")
	{
	}

	SUBCASE("Copy child without its parent")
	{
		Selected = {HierarchyChild};
	}

	Source.SetSelection(Selected, HierarchyChild);
	const auto Clipboard = Source.CopySelected();
	REQUIRE(Clipboard);
	FEditorLevel Target;
	const auto Before = Target.GetWorld().SnapshotEntities();
	REQUIRE(Target.PasteEntities(*Clipboard));
	FObjectId ChildCopy;
	FObjectId RootCopy;
	for (const FObjectId Id : Target.GetSelection())
	{
		CHECK(Id != HierarchyChild);
		CHECK(Id != HierarchyRoot);
		const auto Entity = ReadHierarchyEntity(Target, Id);
		if (Entity.Name == "Child")
		{
			ChildCopy = Id;
		}

		if (Entity.Name == "Root")
		{
			RootCopy = Id;
		}
	}

	REQUIRE(ChildCopy.IsValid());
	CHECK(ReadHierarchyEntity(Target, ChildCopy).Parent == RootCopy);
	CheckHierarchyMatrix(ReadHierarchyWorld(Target, ChildCopy), ChildWorld);
	REQUIRE(Target.Undo());
	CHECK(Target.GetWorld().SnapshotEntities() == Before);
	REQUIRE(Target.Redo());
	CheckHierarchyPreview(Target, ChildCopy);
}

TEST_CASE("Hierarchy rejects sheared poses before loading or reparenting")
{
	Tests::FScratchDirectory Scratch("HertaHierarchyShear");
	FEditorLevel Level;
	const auto Before = Level.GetWorld().SnapshotEntities();
	auto Document = MakeHierarchyDocument();
	Document.Entities[2].Transform.Scale = {2.f, 1.f, 1.f};
	const auto Path = Scratch.GetPath() / "Sheared.hlevel";
	REQUIRE(SaveLevel(Path, Document));
	CHECK_FALSE(Level.Load(Path));
	CHECK(Level.GetWorld().SnapshotEntities() == Before);
	const auto Child = Level.GetObjects()[0].Id;
	const auto Floor = Level.GetObjects()[1].Id;
	const std::array Selected{Child};
	CHECK_FALSE(Level.ReparentEntities(Selected, Floor));
	CHECK(Level.GetWorld().SnapshotEntities() == Before);
	CHECK_FALSE(Level.CanUndo());
}

TEST_CASE("Hierarchy rejected world edits restore preview and leave transaction history untouched")
{
	Tests::FScratchDirectory Scratch("HertaHierarchyRejectedEdit");
	FEditorLevel Level;
	LoadHierarchy(Level, Scratch.GetPath());
	const auto Before = Level.GetWorld().SnapshotEntities();
	const auto Generation = Level.GetGeneration();
	const auto ChildBefore = ReadHierarchyPreview(Level, HierarchyChild);
	const auto* ObjectsData = Level.GetObjects().data();

	SUBCASE("Non-finite world translation")
	{
		ReadHierarchyPreview(Level, HierarchyChild).Translation.x = std::numeric_limits<float>::quiet_NaN();
	}

	SUBCASE("Parent nonuniform scale creates descendant shear")
	{
		ReadHierarchyPreview(Level, HierarchyRoot).Scale.x = 3.f;
	}

	CHECK_FALSE(Level.CommitEdits());
	CHECK(Level.GetWorld().SnapshotEntities() == Before);
	CHECK(Level.GetGeneration() == Generation);
	CHECK(Level.GetObjects().data() == ObjectsData);
	const auto& ChildAfter = ReadHierarchyPreview(Level, HierarchyChild);
	CHECK(ChildAfter.Translation.x == ChildBefore.Translation.x);
	CHECK(ChildAfter.Translation.y == ChildBefore.Translation.y);
	CHECK(ChildAfter.Translation.z == ChildBefore.Translation.z);
	CheckHierarchyPreview(Level, HierarchyRoot);
	CHECK_FALSE(Level.CanUndo());
	CHECK_FALSE(Level.IsDirty());
	REQUIRE(Level.CommitEdits());
}

TEST_CASE("Hierarchy simulation world overrides are independent and propagate to non-body descendants")
{
	Tests::FScratchDirectory Scratch("HertaHierarchySimulation");
	FEditorLevel Level;
	LoadHierarchy(Level, Scratch.GetPath());
	const auto Before = Level.GetWorld().SnapshotEntities();
	const auto GrandchildWorld = ReadHierarchyWorld(Level, HierarchyGrandchild);
	const auto ChildWorld = ReadHierarchyWorld(Level, HierarchyChild);
	const auto AuthoredPreview = Level.GetObjects();
	Level.SetSimulationRunning(true);
	ReadHierarchyPreview(Level, HierarchyRoot).Translation.y -= 2.f;
	std::vector Overrides{HierarchyRoot};
	FVector3d ExpectedDelta{0., -2., 0.};

	SUBCASE("Non-body children follow simulated parent")
	{
	}

	SUBCASE("Simulated child keeps its independent world pose")
	{
		ReadHierarchyPreview(Level, HierarchyChild).Translation.y -= 1.f;
		Overrides.push_back(HierarchyChild);
		ExpectedDelta = {0., -1., 0.};
	}

	REQUIRE(Level.UpdatePreviewHierarchy(Overrides));
	const auto CheckTransient = [&](const FObjectId Id, const TMatrix4<double>& Authored)
	{
		const auto& Object = ReadHierarchyPreview(Level, Id);
		const Im3d::Mat4 Preview(Object.Translation, Object.Rotation, Object.Scale);
		const auto Expected = TMatrix4<double>::Translation(ExpectedDelta) * Authored;

		for (int Row = 0; Row < 4; ++Row)
		{
			for (int Column = 0; Column < 4; ++Column)
			{
				CHECK(Preview(Row, Column) == doctest::Approx(Expected(static_cast<std::size_t>(Row), static_cast<std::size_t>(Column))).epsilon(0.00001));
			}
		}
	};

	CheckTransient(HierarchyChild, ChildWorld);
	CheckTransient(HierarchyGrandchild, GrandchildWorld);
	CHECK(Level.GetWorld().SnapshotEntities() == Before);
	CHECK_FALSE(Level.IsDirty());
	CHECK_FALSE(Level.CanUndo());
	Level.GetObjects() = AuthoredPreview;
	Level.SetSimulationRunning(false);
	REQUIRE(Level.CommitEdits());
	CHECK(Level.GetWorld().SnapshotEntities() == Before);
}
}
