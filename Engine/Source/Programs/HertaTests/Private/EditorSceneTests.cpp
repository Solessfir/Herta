#include "EditorScene.h"
#include "Herta/EditorCore/SceneCommands.h"
#include "TestFiles.h"

#include <doctest/doctest.h>

#include <array>
#include <limits>
#include <numbers>

namespace Herta
{
namespace
{
FPreviewObject& FindEditorObject(FEditorScene& Scene, const FObjectId Id)
{
	const auto Found = std::ranges::find(Scene.GetObjects(), Id, &FPreviewObject::Id);
	REQUIRE(Found != Scene.GetObjects().end());
	return *Found;
}

std::vector<FObjectId> SelectedEditorObjects(const FEditorScene& Scene)
{
	const auto Selected = Scene.GetSelection();
	return {Selected.begin(), Selected.end()};
}
}

TEST_CASE("Editor scene edits and saves Scene-owned transforms and stable identities")
{
	Tests::FScratchDirectory Scratch("HertaEditorScene");
	FEditorScene Scene;
	const FObjectId Id = Scene.GetObjects()[0].Id;
	const FEntityId Handle = *Scene.GetWorld().FindEntity(Id);
	Scene.GetObjects()[0].Label = "Renamed cube";
	Scene.GetObjects()[0].Translation.y = 7.f;
	Scene.GetObjects()[1].Scale.x = 20.f;
	REQUIRE(Scene.CommitEdits());
	CHECK(Scene.GetWorld().FindEntity(Id) == Handle);
	CHECK(Scene.GetWorld().GetEntity(Handle)->Transform.Translation.Meters.Y == 7.);
	const auto Path = Scratch.GetPath() / "Saved.hscene";
	REQUIRE(Scene.Save(Path));
	FEditorScene Restored;
	REQUIRE(Restored.Load(Path));
	CHECK(Restored.GetObjects()[0].Id == Id);
	CHECK(Restored.GetObjects()[0].Label == "Renamed cube");
	CHECK(Restored.GetObjects()[0].Translation.y == 7.f);
	CHECK(Restored.GetObjects()[1].Scale.x == 20.f);
}

TEST_CASE("Editor scene saves authored data while simulation changes the transient view")
{
	Tests::FScratchDirectory Scratch("HertaSceneSimulation");
	FEditorScene Scene;
	Scene.GetObjects()[0].Translation.y = 8.f;
	REQUIRE(Scene.CommitEdits());
	Scene.SetSimulationRunning(true);
	Scene.GetObjects()[0].Translation.y = 0.5f;
	const auto Path = Scratch.GetPath() / "Simulated.hscene";
	REQUIRE(Scene.Save(Path));
	const auto Saved = LoadScene(Path);
	REQUIRE(Saved);
	CHECK(Saved->Entities[0].Transform.Translation.Meters.Y == 8.);
	CHECK_FALSE(Scene.Load(Path));
}

TEST_CASE("Editor scene load failures preserve the world, view, path and generation")
{
	Tests::FScratchDirectory Scratch("HertaSceneFailure");
	FEditorScene Scene;
	const auto Before = Scene.GetWorld().SnapshotEntities();
	const auto Generation = Scene.GetGeneration();
	const auto Path = Scratch.GetPath() / "Broken.hscene";
	Tests::WriteText(Path, "{broken");
	CHECK_FALSE(Scene.Load(Path));
	CHECK(Scene.GetWorld().SnapshotEntities() == Before);
	CHECK(Scene.GetGeneration() == Generation);
	CHECK(Scene.GetPath().empty());
	Scene.GetObjects()[0].Translation.x = std::numeric_limits<float>::infinity();
	CHECK_FALSE(Scene.CommitEdits());
	CHECK(Scene.GetWorld().SnapshotEntities() == Before);
}

TEST_CASE("Editor scene loads variable entity counts and identifies physics bodies by components")
{
	Tests::FScratchDirectory Scratch("HertaSceneCounts");
	FEditorScene Scene;
	FSceneDocument Document{.Id = FObjectId::Generate(), .Name = "Reordered", .Entities = Scene.GetWorld().SnapshotEntities()};
	Document.Entities[0].Id = FObjectId{9, 9};
	Document.Entities[1].Id = FObjectId{1, 1};
	FSceneEntity Extra = Document.Entities[0];
	Extra.Id = FObjectId{8, 8};
	Extra.BodyType = ESceneBodyType::None;
	Document.Entities.push_back(Extra);
	const auto Path = Scratch.GetPath() / "Counts.hscene";
	REQUIRE(SaveScene(Path, Document));
	REQUIRE(Scene.Load(Path));
	CHECK(Scene.GetObjects().size() == 3);
	CHECK(Scene.FindBody(ESceneBodyType::Static) == 0);
	CHECK(Scene.FindBody(ESceneBodyType::Dynamic) == 2);
	Document.Entities.clear();
	REQUIRE(SaveScene(Path, Document));
	REQUIRE(Scene.Load(Path));
	CHECK(Scene.GetObjects().empty());
	CHECK_FALSE(Scene.FindBody(ESceneBodyType::Dynamic));
}

TEST_CASE("Editor scene does not lose unedited double coordinates through its float view")
{
	Tests::FScratchDirectory Scratch("HertaScenePrecision");
	FEditorScene Scene;
	FSceneDocument Document{.Id = FObjectId::Generate(), .Name = "Precision", .Entities = Scene.GetWorld().SnapshotEntities()};
	Document.Entities[0].Transform.Translation.Meters.X = 1234567.123456789;
	const auto Path = Scratch.GetPath() / "Precision.hscene";
	REQUIRE(SaveScene(Path, Document));
	REQUIRE(Scene.Load(Path));
	Scene.GetObjects()[0].Label = "Renamed";
	Scene.GetObjects()[0].Translation.y = 9.f;
	REQUIRE(Scene.Save());
	const auto Saved = LoadScene(Path);
	REQUIRE(Saved);
	CHECK(Saved->Entities[0].Transform.Translation.Meters.X == 1234567.123456789);
	CHECK(Saved->Entities[0].Transform.Translation.Meters.Y == 9.);
}

TEST_CASE("Editor scene rejects out-of-range edits before saving or changing authored state")
{
	Tests::FScratchDirectory Scratch("HertaSceneEditorRange");
	FEditorScene Scene;
	const auto Path = Scratch.GetPath() / "Range.hscene";
	FPreviewObject& Object = Scene.GetObjects()[0];
	Object.Translation = {1.e7f, -1.e7f, 0.f};
	Object.Scale = {0.001f, 1000.f, 1.f};
	REQUIRE(Scene.Save(Path));
	FEditorScene Restored;
	REQUIRE(Restored.Load(Path));
	const auto Before = Scene.GetWorld().SnapshotEntities();
	const auto Original = LoadScene(Path);
	REQUIRE(Original);
	const auto InvalidTransforms = std::array{
	    FSceneTransform{.Translation = FWorldPosition{10000001., 0., 0.}},
	    FSceneTransform{.Scale = FVector3{1001.f, 1.f, 1.f}},
	    FSceneTransform{.Scale = FVector3{0.0005f, 1.f, 1.f}},
	};

	for (const FSceneTransform& Transform : InvalidTransforms)
	{
		Object.Translation = {static_cast<float>(Transform.Translation.Meters.X), static_cast<float>(Transform.Translation.Meters.Y), static_cast<float>(Transform.Translation.Meters.Z)};
		Object.Scale = {Transform.Scale.X, Transform.Scale.Y, Transform.Scale.Z};
		CHECK_FALSE(Scene.CommitEdits());
		Object.Translation = {static_cast<float>(Transform.Translation.Meters.X), static_cast<float>(Transform.Translation.Meters.Y), static_cast<float>(Transform.Translation.Meters.Z)};
		Object.Scale = {Transform.Scale.X, Transform.Scale.Y, Transform.Scale.Z};
		CHECK_FALSE(Scene.Save(Path));
		CHECK(Scene.GetWorld().SnapshotEntities() == Before);
		CHECK(Scene.GetPath() == Path);
		const auto Preserved = LoadScene(Path);
		REQUIRE(Preserved);
		CHECK(Preserved->Entities == Original->Entities);
		REQUIRE(Restored.Load(Path));
	}
}

TEST_CASE("Edited scene rotations preserve nontrivial and near-gimbal rotation matrices")
{
	Tests::FScratchDirectory Scratch("HertaSceneRotation");
	const auto Path = Scratch.GetPath() / "Rotation.hscene";
	const auto Angles = std::array{
	    FVector3{0.f, 1.2f, 0.f},
	    FVector3{0.f, 0.f, 0.4f},
	    FVector3{0.37f, 1.2f, -0.61f},
	    FVector3{0.37f, std::numbers::pi_v<float> / 2.f - 0.00001f, -0.61f},
	    FVector3{0.37f, -std::numbers::pi_v<float> / 2.f + 0.00001f, -0.61f},
	    FVector3{0.37f, std::numbers::pi_v<float> / 2.f, -0.61f},
	    FVector3{0.37f, -std::numbers::pi_v<float> / 2.f, -0.61f},
	};

	for (const FVector3& Euler : Angles)
	{
		CAPTURE(Euler.Y);
		FEditorScene Scene;
		const FQuaternion Rotation = FQuaternion::FromAxisAngle({0.f, 0.f, 1.f}, Euler.Z) * FQuaternion::FromAxisAngle({0.f, 1.f, 0.f}, Euler.Y) * FQuaternion::FromAxisAngle({1.f, 0.f, 0.f}, Euler.X);
		const FMatrix3 Matrix = FMatrix3::Rotation(Rotation);
		FPreviewObject& Object = Scene.GetObjects()[0];
		Object.Rotation = FromPreviewEulerXYZ({Euler.X, Euler.Y, Euler.Z});

		for (int Row = 0; Row < 3; ++Row)
		{
			for (int Column = 0; Column < 3; ++Column)
			{
				CHECK(Object.Rotation(Row, Column) == doctest::Approx(Matrix(static_cast<std::size_t>(Row), static_cast<std::size_t>(Column))).epsilon(0.000001).scale(1.));
			}
		}

		REQUIRE(Scene.Save(Path));
		FEditorScene Restored;
		REQUIRE(Restored.Load(Path));
		const Im3d::Mat3& Actual = Restored.GetObjects()[0].Rotation;

		for (int Row = 0; Row < 3; ++Row)
		{
			for (int Column = 0; Column < 3; ++Column)
			{
				CHECK(Actual(Row, Column) == doctest::Approx(Matrix(static_cast<std::size_t>(Row), static_cast<std::size_t>(Column))).epsilon(0.0002).scale(1.));
			}
		}
	}
}

TEST_CASE("Editor gestures group live changes into one reversible transaction")
{
	FEditorScene Scene;
	const auto Before = Scene.GetWorld().SnapshotEntities();
	const FObjectId Cube = Scene.GetObjects()[0].Id;
	const FObjectId Floor = Scene.GetObjects()[1].Id;
	const std::array Selected{Cube, Floor};
	Scene.SetSelection(Selected, Cube);
	const auto Generation = Scene.GetGeneration();
	REQUIRE(Scene.BeginEdit("Move objects"));
	CHECK(Scene.HasActiveEdit());
	CHECK_FALSE(Scene.CanUndo());
	FindEditorObject(Scene, Cube).Translation.y = 5.f;
	FindEditorObject(Scene, Floor).Translation.x = 1.f;
	REQUIRE(Scene.CommitEdits());
	CHECK(Scene.IsDirty());
	CHECK_FALSE(Scene.CanUndo());
	FindEditorObject(Scene, Cube).Translation.y = 8.f;
	REQUIRE(Scene.CommitEdits());
	REQUIRE(Scene.EndEdit());
	CHECK_FALSE(Scene.HasActiveEdit());
	CHECK(Scene.GetGeneration() == Generation);
	CHECK(Scene.GetUndoLabel() == "Move objects");
	const auto After = Scene.GetWorld().SnapshotEntities();
	REQUIRE(Scene.Undo());
	CHECK(Scene.GetWorld().SnapshotEntities() == Before);
	CHECK(SelectedEditorObjects(Scene) == std::vector<FObjectId>(Selected.begin(), Selected.end()));
	CHECK(Scene.GetActiveObject() == Cube);
	CHECK_FALSE(Scene.CanUndo());
	CHECK_FALSE(Scene.IsDirty());
	REQUIRE(Scene.Redo());
	CHECK(Scene.GetWorld().SnapshotEntities() == After);
	CHECK(Scene.IsDirty());
}

TEST_CASE("No-op and canceled editor gestures preserve redo and the saved state")
{
	FEditorScene Scene;
	const FObjectId Cube = Scene.GetObjects()[0].Id;
	FindEditorObject(Scene, Cube).Translation.y = 8.f;
	REQUIRE(Scene.CommitEdits("Raise cube"));
	REQUIRE(Scene.Undo());
	const auto Before = Scene.GetWorld().SnapshotEntities();
	REQUIRE(Scene.BeginEdit("No-op"));
	FindEditorObject(Scene, Cube).Translation.y = 9.f;
	REQUIRE(Scene.CommitEdits());
	FindEditorObject(Scene, Cube).Translation.y = 4.f;
	REQUIRE(Scene.EndEdit());
	CHECK_FALSE(Scene.IsDirty());
	CHECK(Scene.CanRedo());
	CHECK(Scene.GetRedoLabel() == "Raise cube");
	CHECK(Scene.GetWorld().SnapshotEntities() == Before);
	REQUIRE(Scene.BeginEdit("Canceled move"));
	FindEditorObject(Scene, Cube).Translation.y = 12.f;
	REQUIRE(Scene.CommitEdits());
	CHECK(Scene.IsDirty());
	CHECK_FALSE(Scene.Save());
	CHECK_FALSE(Scene.Load("Unused.hscene"));
	CHECK_FALSE(Scene.Undo());
	CHECK_FALSE(Scene.DeleteSelected());
	REQUIRE(Scene.CancelEdit());
	CHECK(Scene.GetWorld().SnapshotEntities() == Before);
	CHECK(FindEditorObject(Scene, Cube).Translation.y == 4.f);
	CHECK_FALSE(Scene.IsDirty());
	CHECK(Scene.CanRedo());
	REQUIRE(Scene.Redo());
	CHECK(FindEditorObject(Scene, Cube).Translation.y == 8.f);
}

TEST_CASE("Rejected grouped edits cancel completely while preserving widget references")
{
	FEditorScene Scene;
	const auto Before = Scene.GetWorld().SnapshotEntities();
	FPreviewObject* const Object = &Scene.GetObjects()[0];
	REQUIRE(Scene.BeginEdit("Rejected move"));
	Object->Translation.y = 8.f;
	REQUIRE(Scene.CommitEdits());
	Object->Translation.x = 10000001.f;
	CHECK_FALSE(Scene.EndEdit());
	CHECK_FALSE(Scene.HasActiveEdit());
	CHECK(&Scene.GetObjects()[0] == Object);
	CHECK(Scene.GetWorld().SnapshotEntities() == Before);
	CHECK(Object->Translation.y == 4.f);
	CHECK_FALSE(Scene.IsDirty());
	REQUIRE(Scene.BeginEdit("Rejected rename"));
	Object->Label = std::string(1025, 'x');
	CHECK_FALSE(Scene.EndEdit());
	CHECK_FALSE(Scene.HasActiveEdit());
	CHECK(&Scene.GetObjects()[0] == Object);
	CHECK(Scene.GetWorld().SnapshotEntities() == Before);
	CHECK(Object->Label == "Preview Cube");
	Object->Translation.y = 8.f;
	CHECK_FALSE(Scene.CommitEdits(""));
	CHECK(Scene.GetWorld().SnapshotEntities() == Before);
	CHECK(Object->Translation.y == 4.f);
	CHECK_FALSE(Scene.CanUndo());
}

TEST_CASE("Editor history branches and tracks successful saves and loads")
{
	Tests::FScratchDirectory Scratch("HertaSceneHistory");
	const auto Path = Scratch.GetPath() / "Saved.hscene";
	FEditorScene Scene;
	const FObjectId Cube = Scene.GetObjects()[0].Id;
	FindEditorObject(Scene, Cube).Label = "First";
	REQUIRE(Scene.CommitEdits("Rename object"));
	CHECK(Scene.IsDirty());
	REQUIRE(Scene.Save(Path));
	CHECK_FALSE(Scene.IsDirty());
	REQUIRE(Scene.Undo());
	CHECK(Scene.IsDirty());
	REQUIRE(Scene.Redo());
	CHECK_FALSE(Scene.IsDirty());
	REQUIRE(Scene.Undo());
	FindEditorObject(Scene, Cube).Label = "Branched";
	REQUIRE(Scene.CommitEdits("Rename object"));
	CHECK_FALSE(Scene.CanRedo());
	CHECK(Scene.IsDirty());
	CHECK_FALSE(Scene.Save(Scratch.GetPath() / "Missing" / "Failed.hscene"));
	CHECK(Scene.IsDirty());
	CHECK(Scene.GetPath() == Path);
	REQUIRE(Scene.Save());
	CHECK_FALSE(Scene.IsDirty());
	FindEditorObject(Scene, Cube).Translation.y = 10.f;
	REQUIRE(Scene.CommitEdits());
	REQUIRE(Scene.Load(Path));
	CHECK_FALSE(Scene.IsDirty());
	CHECK_FALSE(Scene.CanUndo());
	CHECK_FALSE(Scene.CanRedo());
	CHECK(Scene.GetActiveObject() == Scene.GetObjects()[0].Id);
}

TEST_CASE("Editor history preserves unedited double coordinates through grouped edits")
{
	Tests::FScratchDirectory Scratch("HertaSceneHistoryPrecision");
	const auto Path = Scratch.GetPath() / "Precision.hscene";
	FEditorScene Scene;
	FSceneDocument Document{.Id = FObjectId::Generate(), .Name = "Precision", .Entities = Scene.GetWorld().SnapshotEntities()};
	Document.Entities[0].Transform.Translation.Meters.X = 1234567.123456789;
	REQUIRE(SaveScene(Path, Document));
	REQUIRE(Scene.Load(Path));
	const FObjectId Cube = Scene.GetObjects()[0].Id;
	REQUIRE(Scene.BeginEdit("Move vertically"));
	FindEditorObject(Scene, Cube).Translation.y = 8.f;
	REQUIRE(Scene.CommitEdits());
	REQUIRE(Scene.EndEdit());
	CHECK(Scene.GetWorld().SnapshotEntities()[0].Transform.Translation.Meters.X == 1234567.123456789);
	REQUIRE(Scene.Undo());
	CHECK(Scene.GetWorld().SnapshotEntities() == Document.Entities);
	REQUIRE(Scene.Redo());
	CHECK(Scene.GetWorld().SnapshotEntities()[0].Transform.Translation.Meters.X == 1234567.123456789);
}

TEST_CASE("Structural editor transactions restore stable selection and reject old runtime handles")
{
	FEditorScene Scene;
	const std::vector<FObjectId> InitialSelection = SelectedEditorObjects(Scene);
	const auto InitialActive = Scene.GetActiveObject();
	const FObjectId Floor = Scene.GetObjects()[1].Id;
	const FEntityId FloorHandle = *Scene.GetWorld().FindEntity(Floor);
	const auto Created = Scene.CreateEntity(FWorldPosition{2., 3., 4.});
	REQUIRE(Created);
	CHECK(Scene.GetWorld().GetEntity(FloorHandle).has_value());
	CHECK(Scene.GetActiveObject() == *Created);
	CHECK(SelectedEditorObjects(Scene) == std::vector<FObjectId>{*Created});
	CHECK(FindEditorObject(Scene, *Created).Label == "Cube");
	const FEntityId CreatedHandle = *Scene.GetWorld().FindEntity(*Created);
	REQUIRE(Scene.DeleteSelected());
	CHECK_FALSE(Scene.GetWorld().GetEntity(CreatedHandle).has_value());
	CHECK(Scene.GetSelection().empty());
	REQUIRE(Scene.Undo());
	CHECK(Scene.GetActiveObject() == *Created);
	CHECK(Scene.GetWorld().FindEntity(*Created).has_value());
	CHECK_FALSE(Scene.GetWorld().GetEntity(CreatedHandle).has_value());
	REQUIRE(Scene.Undo());
	CHECK_FALSE(Scene.GetWorld().FindEntity(*Created).has_value());
	CHECK(SelectedEditorObjects(Scene) == InitialSelection);
	CHECK(Scene.GetActiveObject() == InitialActive);
	REQUIRE(Scene.Redo());
	CHECK(Scene.GetActiveObject() == *Created);
	CHECK(Scene.GetWorld().GetEntity(FloorHandle).has_value());
}

TEST_CASE("Rejected structural history admission preserves entities handles and selection")
{
	FEditorScene Scene(256, 1);
	const auto Before = Scene.GetWorld().SnapshotEntities();
	const auto Selection = SelectedEditorObjects(Scene);
	const auto Active = Scene.GetActiveObject();
	const auto Generation = Scene.GetGeneration();
	FPreviewObject* const Object = &Scene.GetObjects()[0];
	std::vector<FEntityId> Handles;

	for (const FSceneEntity& Entity : Before)
	{
		Handles.push_back(*Scene.GetWorld().FindEntity(Entity.Id));
	}

	CHECK_FALSE(Scene.DeleteSelected());
	CHECK(Scene.GetWorld().SnapshotEntities() == Before);
	CHECK(SelectedEditorObjects(Scene) == Selection);
	CHECK(Scene.GetActiveObject() == Active);
	CHECK(Scene.GetGeneration() == Generation);
	CHECK(&Scene.GetObjects()[0] == Object);
	CHECK_FALSE(Scene.IsDirty());
	CHECK_FALSE(Scene.CanUndo());
	CHECK_FALSE(Scene.CanRedo());

	for (std::size_t Index = 0; Index < Before.size(); ++Index)
	{
		CHECK(Scene.GetWorld().GetEntity(Handles[Index]) == Before[Index]);
		CHECK(Scene.GetWorld().FindEntity(Before[Index].Id) == Handles[Index]);
	}
}

TEST_CASE("Editor duplication preserves components and avoids label collisions")
{
	FEditorScene Scene;
	const FObjectId Original = Scene.GetObjects()[0].Id;
	const FSceneEntity Entity = *Scene.GetWorld().GetEntity(*Scene.GetWorld().FindEntity(Original));
	REQUIRE(Scene.DuplicateSelected());
	const FObjectId First = *Scene.GetActiveObject();
	CHECK(First != Original);
	CHECK(FindEditorObject(Scene, First).Label == "Preview Cube Copy");
	const FSceneEntity Duplicate = *Scene.GetWorld().GetEntity(*Scene.GetWorld().FindEntity(First));
	CHECK(Duplicate.Transform == Entity.Transform);
	CHECK(Duplicate.Mesh == Entity.Mesh);
	CHECK(Duplicate.BodyType == Entity.BodyType);
	Scene.SetSelection(std::span(&Original, 1), Original);
	REQUIRE(Scene.DuplicateSelected());
	CHECK(FindEditorObject(Scene, *Scene.GetActiveObject()).Label == "Preview Cube Copy 2");
	REQUIRE(Scene.Undo());
	CHECK(Scene.GetActiveObject() == Original);
	REQUIRE(Scene.Undo());
	CHECK(Scene.GetObjects().size() == 2);
	CHECK(Scene.GetActiveObject() == Original);
}

TEST_CASE("Scene clipboard paste remaps UUIDs and rejects unsupported input atomically")
{
	FEditorScene Source;
	const FObjectId Cube = Source.GetObjects()[0].Id;
	const FObjectId Floor = Source.GetObjects()[1].Id;
	const std::array Selected{Cube, Floor};
	Source.SetSelection(Selected, Cube);
	const auto Clipboard = Source.CopySelected();
	REQUIRE(Clipboard);
	FEditorScene Target;
	const auto Before = Target.GetWorld().SnapshotEntities();
	const auto BeforeSelection = SelectedEditorObjects(Target);
	REQUIRE(Target.PasteEntities(*Clipboard));
	const auto Pasted = SelectedEditorObjects(Target);
	REQUIRE(Pasted.size() == 2);
	CHECK(Target.GetObjects().size() == 4);

	for (std::size_t Index = 0; Index < Pasted.size(); ++Index)
	{
		const FObjectId Object = Pasted[Index];
		CHECK(Object != Cube);
		CHECK(Object != Floor);
		const FSceneEntity Entity = *Target.GetWorld().GetEntity(*Target.GetWorld().FindEntity(Object));
		CHECK(Entity.Mesh->Asset == EngineCubeAsset);
		CHECK(Entity.Transform == Before[Index].Transform);
		CHECK(Entity.BodyType == Before[Index].BodyType);
		CHECK_FALSE(Entity.Parent.IsValid());
	}

	const auto After = Target.GetWorld().SnapshotEntities();
	CHECK_FALSE(Target.PasteEntities("{broken"));
	CHECK(Target.GetWorld().SnapshotEntities() == After);
	CHECK(SelectedEditorObjects(Target) == Pasted);
	FSceneDocument Invalid{.Id = FObjectId::Generate(), .Name = "Invalid", .Entities = Before};
	Invalid.Entities[0].Mesh.reset();
	const auto NonMesh = SerializeScene(Invalid);
	REQUIRE(NonMesh);
	CHECK_FALSE(Target.PasteEntities(*NonMesh));
	CHECK(Target.GetWorld().SnapshotEntities() == After);
	Invalid.Entities = Before;
	Invalid.Entities[1].Parent = Invalid.Entities[0].Id;
	const auto Hierarchy = SerializeScene(Invalid);
	REQUIRE(Hierarchy);
	CHECK_FALSE(Target.PasteEntities(*Hierarchy));
	CHECK(Target.GetWorld().SnapshotEntities() == After);
	REQUIRE(Target.Undo());
	CHECK(Target.GetWorld().SnapshotEntities() == Before);
	CHECK(SelectedEditorObjects(Target) == BeforeSelection);
	REQUIRE(Target.Redo());
	CHECK(Target.GetWorld().SnapshotEntities() == After);
	CHECK(SelectedEditorObjects(Target) == Pasted);
}

TEST_CASE("Empty scenes support authored insertion deletion and history")
{
	Tests::FScratchDirectory Scratch("HertaSceneEmptyAuthoring");
	const auto Path = Scratch.GetPath() / "Empty.hscene";
	REQUIRE(SaveScene(Path, {.Id = FObjectId::Generate(), .Name = "Empty"}));
	FEditorScene Scene;
	REQUIRE(Scene.Load(Path));
	CHECK(Scene.GetSelection().empty());
	CHECK_FALSE(Scene.GetActiveObject());
	const auto Created = Scene.CreateEntity();
	REQUIRE(Created);
	REQUIRE(Scene.DeleteSelected());
	CHECK(Scene.GetObjects().empty());
	REQUIRE(Scene.Undo());
	CHECK(Scene.GetActiveObject() == *Created);
	REQUIRE(Scene.Undo());
	CHECK(Scene.GetObjects().empty());
	CHECK(Scene.GetSelection().empty());
	CHECK_FALSE(Scene.GetActiveObject());
	CHECK_FALSE(Scene.IsDirty());
}

TEST_CASE("Simulation blocks authoring and history while saving only authored poses")
{
	Tests::FScratchDirectory Scratch("HertaSceneBlockedAuthoring");
	const auto Path = Scratch.GetPath() / "Authored.hscene";
	FEditorScene Scene;
	Scene.GetObjects()[0].Translation.y = 8.f;
	REQUIRE(Scene.CommitEdits());
	const auto Before = Scene.GetWorld().SnapshotEntities();
	const auto Clipboard = Scene.CopySelected();
	REQUIRE(Clipboard);
	Scene.SetSimulationRunning(true);
	Scene.GetObjects()[0].Translation.y = 0.5f;
	CHECK_FALSE(Scene.CommitEdits());
	CHECK_FALSE(Scene.BeginEdit("Blocked"));
	CHECK_FALSE(Scene.Undo());
	CHECK_FALSE(Scene.Redo());
	CHECK_FALSE(Scene.CreateEntity());
	CHECK_FALSE(Scene.DuplicateSelected());
	CHECK_FALSE(Scene.DeleteSelected());
	CHECK_FALSE(Scene.PasteEntities(*Clipboard));
	CHECK_FALSE(Scene.CopySelected());
	CHECK_FALSE(Scene.CanUndo());
	CHECK(Scene.GetWorld().SnapshotEntities() == Before);
	REQUIRE(Scene.Save(Path));
	CHECK_FALSE(Scene.IsDirty());
	const auto Saved = LoadScene(Path);
	REQUIRE(Saved);
	CHECK(Saved->Entities == Before);
}

TEST_CASE("Scene commands validate files and retain their editor scene safely")
{
	Tests::FScratchDirectory Scratch("HertaSceneCommands");
	FEditorCommandRegistry Commands;
	auto Scene = std::make_shared<FEditorScene>();
	const auto Path = Scratch.GetPath() / "Commands.hscene";
	Scene->SetPath(Path);
	REQUIRE(RegisterEditorSceneCommands(Commands, Scene));
	REQUIRE(RegisterSceneFileCommands(Commands));
	Scene.reset();
	CHECK(Commands.Execute("scene.save"));
	const auto Text = Path.generic_string();
	CHECK(Commands.Execute("scene.validate \"" + Text + "\""));
	CHECK(Commands.Execute("scene.load \"" + Text + "\""));
	CHECK_FALSE(Commands.Execute("scene.save a b"));
	CHECK_FALSE(Commands.Execute("scene.load"));
	CHECK_FALSE(Commands.Execute("scene.validate"));
	CHECK_FALSE(Commands.Execute("scene.canonicalize"));
}
}
