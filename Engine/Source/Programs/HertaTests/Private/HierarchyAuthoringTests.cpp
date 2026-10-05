#include "EditorScene.h"
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

FSceneDocument MakeHierarchyDocument()
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

void LoadHierarchy(FEditorScene& Scene, const std::filesystem::path& Directory)
{
	const auto Path = Directory / "Hierarchy.hscene";
	REQUIRE(SaveScene(Path, MakeHierarchyDocument()));
	REQUIRE(Scene.Load(Path));
}

FSceneEntity ReadHierarchyEntity(const FEditorScene& Scene, const FObjectId Id)
{
	const auto Handle = Scene.GetWorld().FindEntity(Id);
	REQUIRE(Handle);
	const auto Entity = Scene.GetWorld().GetEntity(*Handle);
	REQUIRE(Entity);
	return *Entity;
}

TMatrix4<double> ReadHierarchyWorld(const FEditorScene& Scene, const FObjectId Id)
{
	const auto Handle = Scene.GetWorld().FindEntity(Id);
	REQUIRE(Handle);
	const auto Matrix = Scene.GetWorld().GetWorldMatrix(*Handle);
	REQUIRE(Matrix);
	return *Matrix;
}

FPreviewObject& ReadHierarchyPreview(FEditorScene& Scene, const FObjectId Id)
{
	const auto Object = std::ranges::find(Scene.GetObjects(), Id, &FPreviewObject::Id);
	REQUIRE(Object != Scene.GetObjects().end());
	return *Object;
}

void CheckHierarchyMatrix(const TMatrix4<double>& Actual, const TMatrix4<double>& Expected)
{
	for (std::size_t Index = 0; Index < Actual.Data().size(); ++Index)
	{
		CHECK(Actual.Data()[Index] == doctest::Approx(Expected.Data()[Index]).epsilon(0.00001));
	}
}

void CheckHierarchyPreview(FEditorScene& Scene, const FObjectId Id)
{
	const auto Expected = ReadHierarchyWorld(Scene, Id);
	const auto& Object = ReadHierarchyPreview(Scene, Id);
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
	FEditorScene Scene;
	LoadHierarchy(Scene, Scratch.GetPath());
	CHECK_FALSE(Scene.IsDirty());
	CheckHierarchyPreview(Scene, HierarchyChild);
	CheckHierarchyPreview(Scene, HierarchyGrandchild);
	CHECK(ReadHierarchyPreview(Scene, HierarchyChild).Parent == HierarchyRoot);
	const auto Before = Scene.GetWorld().SnapshotEntities();
	const auto RootWorld = ReadHierarchyWorld(Scene, HierarchyRoot);
	const auto ChildWorld = ReadHierarchyWorld(Scene, HierarchyChild);
	const auto GrandchildWorld = ReadHierarchyWorld(Scene, HierarchyGrandchild);
	const auto RootHandle = Scene.GetWorld().FindEntity(HierarchyRoot);
	std::array Selected{HierarchyChild, HierarchyRoot};
	SUBCASE("Child before parent")
	{
	}

	SUBCASE("Parent before child")
	{
		Selected = {HierarchyRoot, HierarchyChild};
	}

	Scene.SetSelection(Selected, HierarchyChild);
	REQUIRE(Scene.ReparentEntities(Selected, HierarchyTarget));
	CHECK(ReadHierarchyEntity(Scene, HierarchyRoot).Parent == HierarchyTarget);
	CHECK(ReadHierarchyEntity(Scene, HierarchyChild).Parent == HierarchyRoot);
	CHECK(Scene.GetWorld().FindEntity(HierarchyRoot) == RootHandle);
	CHECK(Scene.GetActiveObject() == HierarchyChild);
	CHECK(Scene.IsDirty());
	CheckHierarchyMatrix(ReadHierarchyWorld(Scene, HierarchyRoot), RootWorld);
	CheckHierarchyMatrix(ReadHierarchyWorld(Scene, HierarchyChild), ChildWorld);
	CheckHierarchyMatrix(ReadHierarchyWorld(Scene, HierarchyGrandchild), GrandchildWorld);
	const auto After = Scene.GetWorld().SnapshotEntities();
	REQUIRE(Scene.Undo());
	CHECK(Scene.GetWorld().SnapshotEntities() == Before);
	CHECK_FALSE(Scene.IsDirty());
	REQUIRE(Scene.Redo());
	CHECK(Scene.GetWorld().SnapshotEntities() == After);
	REQUIRE(Scene.Save());
	FEditorScene Loaded;
	REQUIRE(Loaded.Load(Scene.GetPath()));
	CHECK(Loaded.GetWorld().SnapshotEntities() == After);
	CheckHierarchyPreview(Loaded, HierarchyGrandchild);
}

TEST_CASE("Hierarchy reparent rejects cycles self and stale IDs without changing state")
{
	Tests::FScratchDirectory Scratch("HertaHierarchyFailures");
	FEditorScene Scene;
	LoadHierarchy(Scene, Scratch.GetPath());
	const auto Before = Scene.GetWorld().SnapshotEntities();
	const auto Selection = std::vector<FObjectId>(Scene.GetSelection().begin(), Scene.GetSelection().end());
	const auto Generation = Scene.GetGeneration();
	const std::array Root{HierarchyRoot};
	CHECK_FALSE(Scene.ReparentEntities(Root, HierarchyRoot));
	CHECK_FALSE(Scene.ReparentEntities(Root, HierarchyGrandchild));
	CHECK_FALSE(Scene.ReparentEntities(Root, FObjectId{99, 99}));
	const std::array Stale{FObjectId{99, 99}};
	CHECK_FALSE(Scene.ReparentEntities(Stale, HierarchyTarget));
	CHECK(Scene.GetWorld().SnapshotEntities() == Before);
	CHECK(std::vector<FObjectId>(Scene.GetSelection().begin(), Scene.GetSelection().end()) == Selection);
	CHECK(Scene.GetGeneration() == Generation);
	CHECK_FALSE(Scene.CanUndo());
	CHECK_FALSE(Scene.IsDirty());
	const std::array Child{HierarchyChild};
	REQUIRE(Scene.ReparentEntities(Child, HierarchyRoot));
	CHECK_FALSE(Scene.CanUndo());
	Scene.SetSimulationRunning(true);
	CHECK_FALSE(Scene.ReparentEntities(Child, std::nullopt));
	CHECK(Scene.GetWorld().SnapshotEntities() == Before);
}

TEST_CASE("Hierarchy edits move unselected descendants and preserve locals during grouped world edits")
{
	Tests::FScratchDirectory Scratch("HertaHierarchyEdits");
	FEditorScene Scene;
	LoadHierarchy(Scene, Scratch.GetPath());
	const auto Before = Scene.GetWorld().SnapshotEntities();
	const auto ChildLocal = ReadHierarchyEntity(Scene, HierarchyChild).Transform;
	const auto GrandchildLocal = ReadHierarchyEntity(Scene, HierarchyGrandchild).Transform;
	const auto ChildWorld = ReadHierarchyWorld(Scene, HierarchyChild);
	const auto GrandchildWorld = ReadHierarchyWorld(Scene, HierarchyGrandchild);
	const auto Generation = Scene.GetGeneration();
	const auto* ObjectsData = Scene.GetObjects().data();
	REQUIRE(Scene.BeginEdit("Move hierarchy"));
	ReadHierarchyPreview(Scene, HierarchyRoot).Translation.x += 3.f;
	REQUIRE(Scene.CommitEdits());
	CHECK(Scene.GetGeneration() == Generation);
	CHECK(Scene.GetObjects().data() == ObjectsData);
	CHECK(ReadHierarchyEntity(Scene, HierarchyChild).Transform == ChildLocal);
	CHECK(ReadHierarchyEntity(Scene, HierarchyGrandchild).Transform == GrandchildLocal);
	CheckHierarchyMatrix(ReadHierarchyWorld(Scene, HierarchyChild), TMatrix4<double>::Translation({3., 0., 0.}) * ChildWorld);
	CheckHierarchyPreview(Scene, HierarchyGrandchild);
	ReadHierarchyPreview(Scene, HierarchyRoot).Translation.y += 2.f;
	ReadHierarchyPreview(Scene, HierarchyChild).Translation.y += 2.f;
	REQUIRE(Scene.EndEdit());
	CHECK(ReadHierarchyEntity(Scene, HierarchyChild).Transform.Translation.Meters.IsNearlyEqual(ChildLocal.Translation.Meters, 0.00001));
	CheckHierarchyMatrix(ReadHierarchyWorld(Scene, HierarchyGrandchild), TMatrix4<double>::Translation({3., 2., 0.}) * GrandchildWorld);
	REQUIRE(Scene.Undo());
	CHECK(Scene.GetWorld().SnapshotEntities() == Before);
	CHECK_FALSE(Scene.CanUndo());
}

TEST_CASE("Hierarchy child world edits and unparenting preserve sibling poses")
{
	Tests::FScratchDirectory Scratch("HertaHierarchyChildEdit");
	FEditorScene Scene;
	LoadHierarchy(Scene, Scratch.GetPath());
	const auto RootBefore = ReadHierarchyEntity(Scene, HierarchyRoot);
	const auto ChildWorld = ReadHierarchyWorld(Scene, HierarchyChild);
	ReadHierarchyPreview(Scene, HierarchyChild).Translation.z += 4.f;
	REQUIRE(Scene.CommitEdits());
	CHECK(ReadHierarchyEntity(Scene, HierarchyRoot) == RootBefore);
	const auto Expected = TMatrix4<double>::Translation({0., 0., 4.}) * ChildWorld;
	CheckHierarchyMatrix(ReadHierarchyWorld(Scene, HierarchyChild), Expected);
	const std::array Selected{HierarchyChild};
	REQUIRE(Scene.ReparentEntities(Selected, std::nullopt));
	CHECK_FALSE(ReadHierarchyEntity(Scene, HierarchyChild).Parent.IsValid());
	CHECK(ReadHierarchyEntity(Scene, HierarchyGrandchild).Parent == HierarchyChild);
	CheckHierarchyMatrix(ReadHierarchyWorld(Scene, HierarchyChild), Expected);
	CheckHierarchyPreview(Scene, HierarchyGrandchild);
}

TEST_CASE("Deleting hierarchy parents detaches surviving children without deleting descendants")
{
	Tests::FScratchDirectory Scratch("HertaHierarchyDelete");
	FEditorScene Scene;
	LoadHierarchy(Scene, Scratch.GetPath());
	const auto Before = Scene.GetWorld().SnapshotEntities();
	const auto ChildWorld = ReadHierarchyWorld(Scene, HierarchyChild);
	const auto GrandchildWorld = ReadHierarchyWorld(Scene, HierarchyGrandchild);
	std::vector Selected{HierarchyRoot};
	SUBCASE("Delete only the parent")
	{
	}

	SUBCASE("Also delete the child but retain its descendant")
	{
		Selected.push_back(HierarchyChild);
	}

	Scene.SetSelection(Selected, HierarchyRoot);
	REQUIRE(Scene.DeleteSelected());
	CHECK_FALSE(Scene.GetWorld().FindEntity(HierarchyRoot));
	if (Selected.size() == 1)
	{
		CHECK_FALSE(ReadHierarchyEntity(Scene, HierarchyChild).Parent.IsValid());
		CHECK(ReadHierarchyEntity(Scene, HierarchyGrandchild).Parent == HierarchyChild);
		CheckHierarchyMatrix(ReadHierarchyWorld(Scene, HierarchyChild), ChildWorld);
	}
	else
	{
		CHECK_FALSE(Scene.GetWorld().FindEntity(HierarchyChild));
		CHECK_FALSE(ReadHierarchyEntity(Scene, HierarchyGrandchild).Parent.IsValid());
	}

	CheckHierarchyMatrix(ReadHierarchyWorld(Scene, HierarchyGrandchild), GrandchildWorld);
	const auto After = Scene.GetWorld().SnapshotEntities();
	REQUIRE(Scene.Undo());
	CHECK(Scene.GetWorld().SnapshotEntities() == Before);
	REQUIRE(Scene.Redo());
	CHECK(Scene.GetWorld().SnapshotEntities() == After);
}

TEST_CASE("Hierarchical duplication remaps selected parents and applies world offsets once")
{
	Tests::FScratchDirectory Scratch("HertaHierarchyDuplicate");
	FEditorScene Scene;
	LoadHierarchy(Scene, Scratch.GetPath());
	const auto Before = Scene.GetWorld().SnapshotEntities();
	const auto RootWorld = ReadHierarchyWorld(Scene, HierarchyRoot);
	const auto ChildWorld = ReadHierarchyWorld(Scene, HierarchyChild);
	std::vector Selected{HierarchyRoot, HierarchyChild};
	SUBCASE("Duplicate parent and child together")
	{
	}

	SUBCASE("Duplicate a child under its existing parent")
	{
		Selected = {HierarchyChild};
	}

	Scene.SetSelection(Selected, HierarchyChild);
	REQUIRE(Scene.DuplicateSelected(false, {3., 1., -2.}));
	FObjectId RootCopy;
	FObjectId ChildCopy;
	for (const auto& Entity : Scene.GetWorld().SnapshotEntities())
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
	CHECK(ReadHierarchyEntity(Scene, ChildCopy).Parent == (RootCopy.IsValid() ? RootCopy : HierarchyRoot));
	CheckHierarchyMatrix(ReadHierarchyWorld(Scene, ChildCopy), TMatrix4<double>::Translation({3., 1., -2.}) * ChildWorld);
	if (RootCopy.IsValid())
	{
		CheckHierarchyMatrix(ReadHierarchyWorld(Scene, RootCopy), TMatrix4<double>::Translation({3., 1., -2.}) * RootWorld);
	}

	REQUIRE(Scene.Undo());
	CHECK(Scene.GetWorld().SnapshotEntities() == Before);
}

TEST_CASE("Hierarchy clipboard remaps internal links and roots fragments copied without parents")
{
	Tests::FScratchDirectory Scratch("HertaHierarchyClipboard");
	FEditorScene Source;
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
	FEditorScene Target;
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
	FEditorScene Scene;
	const auto Before = Scene.GetWorld().SnapshotEntities();
	auto Document = MakeHierarchyDocument();
	Document.Entities[2].Transform.Scale = {2.f, 1.f, 1.f};
	const auto Path = Scratch.GetPath() / "Sheared.hscene";
	REQUIRE(SaveScene(Path, Document));
	CHECK_FALSE(Scene.Load(Path));
	CHECK(Scene.GetWorld().SnapshotEntities() == Before);
	const auto Child = Scene.GetObjects()[0].Id;
	const auto Floor = Scene.GetObjects()[1].Id;
	const std::array Selected{Child};
	CHECK_FALSE(Scene.ReparentEntities(Selected, Floor));
	CHECK(Scene.GetWorld().SnapshotEntities() == Before);
	CHECK_FALSE(Scene.CanUndo());
}

TEST_CASE("Hierarchy rejected world edits restore preview and leave transaction history untouched")
{
	Tests::FScratchDirectory Scratch("HertaHierarchyRejectedEdit");
	FEditorScene Scene;
	LoadHierarchy(Scene, Scratch.GetPath());
	const auto Before = Scene.GetWorld().SnapshotEntities();
	const auto Generation = Scene.GetGeneration();
	const auto ChildBefore = ReadHierarchyPreview(Scene, HierarchyChild);
	const auto* ObjectsData = Scene.GetObjects().data();

	SUBCASE("Non-finite world translation")
	{
		ReadHierarchyPreview(Scene, HierarchyChild).Translation.x = std::numeric_limits<float>::quiet_NaN();
	}

	SUBCASE("Parent nonuniform scale creates descendant shear")
	{
		ReadHierarchyPreview(Scene, HierarchyRoot).Scale.x = 3.f;
	}

	CHECK_FALSE(Scene.CommitEdits());
	CHECK(Scene.GetWorld().SnapshotEntities() == Before);
	CHECK(Scene.GetGeneration() == Generation);
	CHECK(Scene.GetObjects().data() == ObjectsData);
	const auto& ChildAfter = ReadHierarchyPreview(Scene, HierarchyChild);
	CHECK(ChildAfter.Translation.x == ChildBefore.Translation.x);
	CHECK(ChildAfter.Translation.y == ChildBefore.Translation.y);
	CHECK(ChildAfter.Translation.z == ChildBefore.Translation.z);
	CheckHierarchyPreview(Scene, HierarchyRoot);
	CHECK_FALSE(Scene.CanUndo());
	CHECK_FALSE(Scene.IsDirty());
	REQUIRE(Scene.CommitEdits());
}

TEST_CASE("Hierarchy simulation world overrides are independent and propagate to non-body descendants")
{
	Tests::FScratchDirectory Scratch("HertaHierarchySimulation");
	FEditorScene Scene;
	LoadHierarchy(Scene, Scratch.GetPath());
	const auto Before = Scene.GetWorld().SnapshotEntities();
	const auto GrandchildWorld = ReadHierarchyWorld(Scene, HierarchyGrandchild);
	const auto ChildWorld = ReadHierarchyWorld(Scene, HierarchyChild);
	const auto AuthoredPreview = Scene.GetObjects();
	Scene.SetSimulationRunning(true);
	ReadHierarchyPreview(Scene, HierarchyRoot).Translation.y -= 2.f;
	std::vector Overrides{HierarchyRoot};
	FVector3d ExpectedDelta{0., -2., 0.};

	SUBCASE("Non-body children follow simulated parent")
	{
	}

	SUBCASE("Simulated child keeps its independent world pose")
	{
		ReadHierarchyPreview(Scene, HierarchyChild).Translation.y -= 1.f;
		Overrides.push_back(HierarchyChild);
		ExpectedDelta = {0., -1., 0.};
	}

	REQUIRE(Scene.UpdatePreviewHierarchy(Overrides));
	const auto CheckTransient = [&](const FObjectId Id, const TMatrix4<double>& Authored)
	{
		const auto& Object = ReadHierarchyPreview(Scene, Id);
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
	CHECK(Scene.GetWorld().SnapshotEntities() == Before);
	CHECK_FALSE(Scene.IsDirty());
	CHECK_FALSE(Scene.CanUndo());
	Scene.GetObjects() = AuthoredPreview;
	Scene.SetSimulationRunning(false);
	REQUIRE(Scene.CommitEdits());
	CHECK(Scene.GetWorld().SnapshotEntities() == Before);
}
}
