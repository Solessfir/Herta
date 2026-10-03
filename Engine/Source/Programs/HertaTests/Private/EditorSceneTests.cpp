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
	CHECK(Scene.FindBodies(ESceneBodyType::Static) == std::vector<std::size_t>{0});
	CHECK(Scene.FindBodies(ESceneBodyType::Dynamic) == std::vector<std::size_t>{2});
	Document.Entities[2].BodyType = ESceneBodyType::Dynamic;
	REQUIRE(SaveScene(Path, Document));
	REQUIRE(Scene.Load(Path));
	CHECK(Scene.FindBodies(ESceneBodyType::Dynamic) == std::vector<std::size_t>{1, 2});
	Document.Entities[2].BodyType = ESceneBodyType::Static;
	REQUIRE(SaveScene(Path, Document));
	REQUIRE(Scene.Load(Path));
	CHECK(Scene.FindBodies(ESceneBodyType::Static) == std::vector<std::size_t>{0, 1});
	Document.Entities.clear();
	REQUIRE(SaveScene(Path, Document));
	REQUIRE(Scene.Load(Path));
	CHECK(Scene.GetObjects().empty());
	CHECK(Scene.FindBodies(ESceneBodyType::Dynamic).empty());
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

TEST_CASE("Command duplication translates every selected copy by the same world offset")
{
	FEditorScene Scene;
	const auto Before = Scene.GetWorld().SnapshotEntities();
	const std::array Selected{Before[0].Id, Before[1].Id};
	Scene.SetSelection(Selected, Selected.front());
	const FVector3d Offset{0.125, 0., 0.125};
	REQUIRE(Scene.DuplicateSelected(false, Offset));
	const auto Copies = SelectedEditorObjects(Scene);
	REQUIRE(Copies.size() == Selected.size());

	for (std::size_t Index = 0; Index < Copies.size(); ++Index)
	{
		const FSceneEntity Copy = *Scene.GetWorld().GetEntity(*Scene.GetWorld().FindEntity(Copies[Index]));
		CHECK(Copy.Id != Before[Index].Id);
		CHECK(Copy.Transform.Translation == Before[Index].Transform.Translation.TranslatedBy(Offset));
		CHECK(Copy.Transform.Rotation == Before[Index].Transform.Rotation);
		CHECK(Copy.Transform.Scale == Before[Index].Transform.Scale);
		CHECK(Copy.Mesh == Before[Index].Mesh);
		CHECK(Copy.BodyType == Before[Index].BodyType);
	}

	const auto After = Scene.GetWorld().SnapshotEntities();
	CHECK(Scene.GetUndoLabel() == "Duplicate objects");
	REQUIRE(Scene.Undo());
	CHECK(Scene.GetWorld().SnapshotEntities() == Before);
	CHECK(SelectedEditorObjects(Scene) == std::vector<FObjectId>(Selected.begin(), Selected.end()));
	CHECK(Scene.GetActiveObject() == Selected.front());
	CHECK_FALSE(Scene.CanUndo());
	REQUIRE(Scene.Redo());
	CHECK(Scene.GetWorld().SnapshotEntities() == After);
	CHECK(SelectedEditorObjects(Scene) == Copies);
}

TEST_CASE("Invalid duplication offsets preserve authored objects selection and existing history")
{
	const auto InvalidOffsets = std::array{
	    FVector3d{std::numeric_limits<double>::quiet_NaN(), 0., 0.},
	    FVector3d{0., std::numeric_limits<double>::infinity(), 0.},
	    FVector3d{0., 0., -std::numeric_limits<double>::infinity()},
	    FVector3d{0., -1.e7, 0.},
	    FVector3d{0., 0., 10000001.},
	};

	for (const bool bRedo : std::array{false, true})
	{
		for (const bool bWithinActiveEdit : std::array{false, true})
		{
			FEditorScene Scene;
			Scene.GetObjects()[0].Translation.y = 8.f;
			REQUIRE(Scene.CommitEdits("Raise cube"));
			if (bRedo)
			{
				REQUIRE(Scene.Undo());
			}

			const auto Before = Scene.GetWorld().SnapshotEntities();
			const std::array Selected{Before[0].Id, Before[1].Id};
			Scene.SetSelection(Selected, Selected.front());
			const auto Generation = Scene.GetGeneration();
			std::vector<FEntityId> Handles;

			for (const FSceneEntity& Entity : Before)
			{
				Handles.push_back(*Scene.GetWorld().FindEntity(Entity.Id));
			}

			if (bWithinActiveEdit)
			{
				REQUIRE(Scene.BeginEdit("Duplicate objects"));
			}

			for (const FVector3d& Offset : InvalidOffsets)
			{
				CHECK_FALSE(Scene.DuplicateSelected(bWithinActiveEdit, Offset));
				CHECK(Scene.GetWorld().SnapshotEntities() == Before);
				CHECK(SelectedEditorObjects(Scene) == std::vector<FObjectId>(Selected.begin(), Selected.end()));
				CHECK(Scene.GetActiveObject() == Selected.front());
				CHECK(Scene.GetGeneration() == Generation);
				CHECK(Scene.HasActiveEdit() == bWithinActiveEdit);
				CHECK(Scene.IsDirty() == !bRedo);
				CHECK(Scene.GetUndoLabel() == (bRedo ? "" : "Raise cube"));
				CHECK(Scene.GetRedoLabel() == (bRedo ? "Raise cube" : ""));

				for (std::size_t Index = 0; Index < Before.size(); ++Index)
				{
					CHECK(Scene.GetWorld().GetEntity(Handles[Index]) == Before[Index]);
				}
			}

			if (bWithinActiveEdit)
			{
				REQUIRE(Scene.EndEdit());
			}

			CHECK(Scene.CanUndo() == !bRedo);
			CHECK(Scene.CanRedo() == bRedo);
		}
	}
}

TEST_CASE("Gesture duplication and transforms form one undo step for single and multiple selections")
{
	for (const bool bMultiple : std::array{false, true})
	{
		CAPTURE(bMultiple);
		FEditorScene Scene;
		const auto Before = Scene.GetWorld().SnapshotEntities();
		std::vector<FObjectId> Originals{Before[0].Id};
		if (bMultiple)
		{
			Originals.push_back(Before[1].Id);
		}

		Scene.SetSelection(Originals, Originals.front());
		std::vector<FEntityId> OriginalHandles;

		for (const FSceneEntity& Entity : Before)
		{
			OriginalHandles.push_back(*Scene.GetWorld().FindEntity(Entity.Id));
		}

		REQUIRE(Scene.BeginEdit("Duplicate objects"));
		REQUIRE(Scene.DuplicateSelected(true));
		const auto Copies = SelectedEditorObjects(Scene);
		REQUIRE(Copies.size() == Originals.size());
		CHECK(Scene.GetObjects().size() == Before.size() + Originals.size());
		CHECK(Scene.GetActiveObject() == Copies.front());
		CHECK_FALSE(Scene.CanUndo());

		for (std::size_t Index = 0; Index < Copies.size(); ++Index)
		{
			CHECK(Scene.GetWorld().GetEntity(*Scene.GetWorld().FindEntity(Copies[Index]))->Transform == Before[Index].Transform);
		}

		const auto Generation = Scene.GetGeneration();
		const auto Inserted = Scene.GetWorld().SnapshotEntities();
		REQUIRE(Scene.DuplicateSelected(true));
		CHECK(Scene.GetWorld().SnapshotEntities() == Inserted);
		CHECK(SelectedEditorObjects(Scene) == Copies);
		CHECK(Scene.GetGeneration() == Generation);
		CHECK_FALSE(Scene.DuplicateSelected());
		std::vector<FEntityId> CopyHandles;

		for (const FObjectId Copy : Copies)
		{
			CopyHandles.push_back(*Scene.GetWorld().FindEntity(Copy));
			FPreviewObject& Object = FindEditorObject(Scene, Copy);
			Object.Translation.x += 2.f;
			Object.Translation.y += 1.f;
			Object.Rotation = FromPreviewEulerXYZ({0.25f, -0.4f, 0.1f});
			Object.Scale = {2.f, 3.f, 4.f};
		}

		REQUIRE(Scene.CommitEdits());
		REQUIRE(Scene.DuplicateSelected(true));
		CHECK(Scene.GetObjects().size() == Before.size() + Originals.size());
		REQUIRE(Scene.EndEdit());
		CHECK(Scene.GetUndoLabel() == "Duplicate objects");
		CHECK(Scene.GetGeneration() == Generation);
		const auto After = Scene.GetWorld().SnapshotEntities();
		REQUIRE(Scene.Undo());
		CHECK(Scene.GetWorld().SnapshotEntities() == Before);
		CHECK(SelectedEditorObjects(Scene) == Originals);
		CHECK(Scene.GetActiveObject() == Originals.front());
		CHECK_FALSE(Scene.CanUndo());
		CHECK_FALSE(Scene.IsDirty());

		for (std::size_t Index = 0; Index < Before.size(); ++Index)
		{
			CHECK(Scene.GetWorld().GetEntity(OriginalHandles[Index]) == Before[Index]);
		}

		REQUIRE(Scene.Redo());
		CHECK(Scene.GetWorld().SnapshotEntities() == After);
		CHECK(SelectedEditorObjects(Scene) == Copies);
		CHECK(Scene.GetActiveObject() == Copies.front());

		for (const FEntityId Handle : CopyHandles)
		{
			CHECK_FALSE(Scene.GetWorld().GetEntity(Handle));
		}
	}
}

TEST_CASE("Canceling gesture duplication restores original objects handles selection and redo")
{
	FEditorScene Scene;
	const FObjectId Original = Scene.GetObjects()[0].Id;
	FindEditorObject(Scene, Original).Translation.y = 8.f;
	REQUIRE(Scene.CommitEdits("Raise cube"));
	REQUIRE(Scene.Undo());
	const auto Before = Scene.GetWorld().SnapshotEntities();
	const auto Selection = SelectedEditorObjects(Scene);
	const auto Active = Scene.GetActiveObject();
	const FEntityId Handle = *Scene.GetWorld().FindEntity(Original);
	REQUIRE(Scene.BeginEdit("Duplicate objects"));
	REQUIRE(Scene.DuplicateSelected(true));
	const FObjectId Copy = *Scene.GetActiveObject();
	FindEditorObject(Scene, Copy).Translation.x = 5.f;
	REQUIRE(Scene.CommitEdits());
	REQUIRE(Scene.CancelEdit());
	CHECK(Scene.GetWorld().SnapshotEntities() == Before);
	CHECK(Scene.GetWorld().GetEntity(Handle) == Before[0]);
	CHECK_FALSE(Scene.GetWorld().FindEntity(Copy));
	CHECK(SelectedEditorObjects(Scene) == Selection);
	CHECK(Scene.GetActiveObject() == Active);
	CHECK_FALSE(Scene.HasActiveEdit());
	CHECK_FALSE(Scene.IsDirty());
	CHECK(Scene.CanRedo());
	CHECK(Scene.GetRedoLabel() == "Raise cube");
	REQUIRE(Scene.Redo());
	CHECK(FindEditorObject(Scene, Original).Translation.y == 8.f);
}

TEST_CASE("Rejected gesture duplication history removes copies without invalidating original handles")
{
	FEditorScene Scene(256, 1);
	const auto Before = Scene.GetWorld().SnapshotEntities();
	const auto Selection = SelectedEditorObjects(Scene);
	const auto Active = Scene.GetActiveObject();
	std::vector<FEntityId> Handles;

	for (const FSceneEntity& Entity : Before)
	{
		Handles.push_back(*Scene.GetWorld().FindEntity(Entity.Id));
	}

	REQUIRE(Scene.BeginEdit("Duplicate objects"));
	REQUIRE(Scene.DuplicateSelected(true));
	const FObjectId Copy = *Scene.GetActiveObject();
	FindEditorObject(Scene, Copy).Translation.x = 3.f;
	REQUIRE(Scene.CommitEdits());
	CHECK_FALSE(Scene.EndEdit());
	CHECK(Scene.GetWorld().SnapshotEntities() == Before);
	CHECK(SelectedEditorObjects(Scene) == Selection);
	CHECK(Scene.GetActiveObject() == Active);
	CHECK_FALSE(Scene.GetWorld().FindEntity(Copy));
	CHECK_FALSE(Scene.HasActiveEdit());
	CHECK_FALSE(Scene.IsDirty());
	CHECK_FALSE(Scene.CanUndo());
	CHECK_FALSE(Scene.CanRedo());

	for (std::size_t Index = 0; Index < Before.size(); ++Index)
	{
		CHECK(Scene.GetWorld().GetEntity(Handles[Index]) == Before[Index]);
		CHECK(Scene.GetWorld().FindEntity(Before[Index].Id) == Handles[Index]);
	}
}

TEST_CASE("Clicking without a duplication drag leaves the scene and history unchanged")
{
	FEditorScene Scene;
	const auto Before = Scene.GetWorld().SnapshotEntities();
	const auto Selection = SelectedEditorObjects(Scene);
	const auto Generation = Scene.GetGeneration();
	CHECK_FALSE(Scene.DuplicateSelected(true));
	REQUIRE(Scene.BeginEdit("Duplicate objects"));
	REQUIRE(Scene.EndEdit());
	CHECK(Scene.GetWorld().SnapshotEntities() == Before);
	CHECK(SelectedEditorObjects(Scene) == Selection);
	CHECK(Scene.GetGeneration() == Generation);
	CHECK_FALSE(Scene.IsDirty());
	CHECK_FALSE(Scene.CanUndo());
	CHECK_FALSE(Scene.CanRedo());
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

TEST_CASE("Empty entities retain absent components through editing history and scene files")
{
	Tests::FScratchDirectory Scratch("HertaEmptyEntity");
	FEditorScene Scene;
	const auto Before = Scene.GetWorld().SnapshotEntities();
	const auto Created = Scene.CreateEmptyEntity(FWorldPosition{2., 3., 4.});
	REQUIRE(Created);
	const auto Handle = *Scene.GetWorld().FindEntity(*Created);
	CHECK(FindEditorObject(Scene, *Created).Label == "Entity");
	CHECK_FALSE(FindEditorObject(Scene, *Created).Mesh.IsValid());
	CHECK_FALSE(Scene.GetWorld().GetEntity(Handle)->Mesh);
	CHECK(Scene.GetWorld().GetEntity(Handle)->BodyType == ESceneBodyType::None);
	CHECK(Scene.GetActiveObject() == *Created);
	REQUIRE(Scene.BeginEdit("Edit empty entity"));
	FPreviewObject& Object = FindEditorObject(Scene, *Created);
	Object.Label = "Anchor";
	Object.Translation = {9.f, 8.f, 7.f};
	Object.Rotation = FromPreviewEulerXYZ({0.2f, -0.3f, 0.4f});
	Object.Scale = {0.5f, 2.f, 3.f};
	REQUIRE(Scene.EndEdit());
	const auto Edited = Scene.GetWorld().SnapshotEntities();
	CHECK(Scene.GetWorld().FindEntity(*Created) == Handle);
	CHECK_FALSE(Scene.GetWorld().GetEntity(Handle)->Mesh);
	REQUIRE(Scene.Undo());
	CHECK(FindEditorObject(Scene, *Created).Label == "Entity");
	CHECK(FindEditorObject(Scene, *Created).Translation.x == 2.f);
	CHECK_FALSE(FindEditorObject(Scene, *Created).Mesh.IsValid());
	REQUIRE(Scene.Redo());
	CHECK(Scene.GetWorld().SnapshotEntities() == Edited);
	const auto Path = Scratch.GetPath() / "Anchor.hscene";
	REQUIRE(Scene.Save(Path));
	FEditorScene Restored;
	REQUIRE(Restored.Load(Path));
	CHECK(Restored.GetWorld().SnapshotEntities() == Edited);
	CHECK_FALSE(FindEditorObject(Restored, *Created).Mesh.IsValid());
	CHECK_FALSE(Restored.IsDirty());
	REQUIRE(Scene.Undo());
	REQUIRE(Scene.Undo());
	CHECK(Scene.GetWorld().SnapshotEntities() == Before);
	CHECK_FALSE(Scene.GetWorld().GetEntity(Handle));
	REQUIRE(Scene.Redo());
	REQUIRE(Scene.Redo());
	CHECK(Scene.GetWorld().SnapshotEntities() == Edited);
	CHECK(Scene.GetWorld().FindEntity(*Created) != Handle);
	CHECK_FALSE(Scene.IsDirty());
}

TEST_CASE("Empty entity clipboard and duplication preserve optional components and remap identities")
{
	FEditorScene Source;
	const auto Created = Source.CreateEmptyEntity(FWorldPosition{2., 4., 6.});
	REQUIRE(Created);
	const auto Second = Source.CreateEmptyEntity();
	REQUIRE(Second);
	CHECK(FindEditorObject(Source, *Second).Label == "Entity 2");
	Source.SetSelection(std::array{*Created}, *Created);
	REQUIRE(Source.AddRigidBodyToSelected());
	FindEditorObject(Source, *Created).Label = "Body anchor";
	REQUIRE(Source.CommitEdits());
	const auto Original = *Source.GetWorld().GetEntity(*Source.GetWorld().FindEntity(*Created));
	REQUIRE(Source.DuplicateSelected());
	const auto CopyId = *Source.GetActiveObject();
	const auto Copy = *Source.GetWorld().GetEntity(*Source.GetWorld().FindEntity(CopyId));
	CHECK_FALSE(Copy.Mesh);
	CHECK(Copy.BodyType == Original.BodyType);
	CHECK(Copy.Transform == Original.Transform);
	REQUIRE(Source.Undo());
	CHECK(Source.GetActiveObject() == *Created);
	const auto Clipboard = Source.CopySelected();
	REQUIRE(Clipboard);
	FEditorScene Target;
	const auto Before = Target.GetWorld().SnapshotEntities();
	REQUIRE(Target.PasteEntities(*Clipboard));
	const auto PastedId = *Target.GetActiveObject();
	CHECK(PastedId != *Created);
	const auto Pasted = *Target.GetWorld().GetEntity(*Target.GetWorld().FindEntity(PastedId));
	CHECK_FALSE(Pasted.Mesh);
	CHECK(Pasted.BodyType == Original.BodyType);
	CHECK(Pasted.Name == Original.Name);
	CHECK(Pasted.Transform == Original.Transform);
	CHECK_FALSE(FindEditorObject(Target, PastedId).Mesh.IsValid());
	const auto After = Target.GetWorld().SnapshotEntities();
	REQUIRE(Target.Undo());
	CHECK(Target.GetWorld().SnapshotEntities() == Before);
	REQUIRE(Target.Redo());
	CHECK(Target.GetWorld().SnapshotEntities() == After);
}

TEST_CASE("Static mesh authoring preserves handles active selection and no-op redo branches")
{
	Tests::FScratchDirectory Scratch("HertaMeshComponents");
	FEditorScene Scene;
	const FObjectId Cube = Scene.GetObjects()[0].Id;
	const auto Empty = Scene.CreateEmptyEntity();
	REQUIRE(Empty);
	const auto EmptyHandle = *Scene.GetWorld().FindEntity(*Empty);
	const auto CubeHandle = *Scene.GetWorld().FindEntity(Cube);
	const std::array Selected{*Empty, Cube};
	Scene.SetSelection(Selected, *Empty);
	REQUIRE(Scene.Save(Scratch.GetPath() / "BeforeMesh.hscene"));
	const auto Before = Scene.GetWorld().SnapshotEntities();
	REQUIRE(Scene.AddStaticMeshToSelected(EngineCubeAsset));
	CHECK(Scene.GetWorld().GetEntity(EmptyHandle)->Mesh->Asset == EngineCubeAsset);
	CHECK(Scene.GetWorld().GetEntity(CubeHandle)->Mesh->Asset == EngineCubeAsset);
	CHECK(SelectedEditorObjects(Scene) == std::vector<FObjectId>(Selected.begin(), Selected.end()));
	CHECK(Scene.GetActiveObject() == *Empty);
	const auto Added = Scene.GetWorld().SnapshotEntities();
	const auto Generation = Scene.GetGeneration();
	REQUIRE(Scene.AddStaticMeshToSelected(FAssetId{1, 2}));
	CHECK(Scene.GetWorld().SnapshotEntities() == Added);
	CHECK(Scene.GetGeneration() == Generation);
	REQUIRE(Scene.Undo());
	CHECK(Scene.GetWorld().SnapshotEntities() == Before);
	CHECK_FALSE(Scene.IsDirty());
	Scene.SetSelection(std::array{*Empty}, *Empty);
	REQUIRE(Scene.RemoveStaticMeshFromSelected());
	CHECK(Scene.CanRedo());
	CHECK_FALSE(Scene.IsDirty());
	REQUIRE(Scene.Redo());
	CHECK(Scene.GetWorld().SnapshotEntities() == Added);
	CHECK(Scene.GetActiveObject() == *Empty);
	REQUIRE(Scene.RemoveStaticMeshFromSelected());
	CHECK_FALSE(Scene.GetWorld().GetEntity(EmptyHandle)->Mesh);
	CHECK_FALSE(Scene.GetWorld().GetEntity(CubeHandle)->Mesh);
	CHECK(Scene.GetWorld().GetEntity(CubeHandle)->BodyType == ESceneBodyType::Dynamic);
	CHECK(Scene.FindBodies(ESceneBodyType::Dynamic).empty());
	REQUIRE(Scene.AddStaticMeshToSelected(FAssetId{1, 2}));
	CHECK(Scene.GetWorld().GetEntity(EmptyHandle)->Mesh->Asset == FAssetId{1, 2});
	CHECK(Scene.GetWorld().GetEntity(CubeHandle)->Mesh->Asset == FAssetId{1, 2});
	CHECK(Scene.GetWorld().FindEntity(*Empty) == EmptyHandle);
	CHECK(Scene.GetWorld().FindEntity(Cube) == CubeHandle);
	CHECK(Scene.GetActiveObject() == *Empty);
	REQUIRE(Scene.Undo());
	CHECK_FALSE(Scene.GetWorld().GetEntity(EmptyHandle)->Mesh);
	REQUIRE(Scene.Undo());
	CHECK(Scene.GetWorld().SnapshotEntities() == Added);
}

TEST_CASE("Rigid body authoring is independent from meshes and addition preserves existing motion")
{
	FEditorScene Scene;
	const FObjectId Cube = Scene.GetObjects()[0].Id;
	const FObjectId Floor = Scene.GetObjects()[1].Id;
	const auto Empty = Scene.CreateEmptyEntity();
	REQUIRE(Empty);
	const auto EmptyHandle = *Scene.GetWorld().FindEntity(*Empty);
	const auto CubeHandle = *Scene.GetWorld().FindEntity(Cube);
	const auto FloorHandle = *Scene.GetWorld().FindEntity(Floor);
	const std::array Selected{*Empty, Cube, Floor};
	Scene.SetSelection(Selected, *Empty);
	const auto Before = Scene.GetWorld().SnapshotEntities();
	REQUIRE(Scene.AddRigidBodyToSelected(ESceneBodyType::Static));
	CHECK(Scene.GetWorld().GetEntity(EmptyHandle)->BodyType == ESceneBodyType::Static);
	CHECK_FALSE(Scene.GetWorld().GetEntity(EmptyHandle)->Mesh);
	CHECK(Scene.GetWorld().GetEntity(CubeHandle)->BodyType == ESceneBodyType::Dynamic);
	CHECK(Scene.GetWorld().GetEntity(FloorHandle)->BodyType == ESceneBodyType::Static);
	const auto Added = Scene.GetWorld().SnapshotEntities();
	const auto Generation = Scene.GetGeneration();
	REQUIRE(Scene.AddRigidBodyToSelected());
	CHECK(Scene.GetWorld().SnapshotEntities() == Added);
	CHECK(Scene.GetGeneration() == Generation);
	REQUIRE(Scene.SetSelectedBodyType(ESceneBodyType::Dynamic));
	CHECK(Scene.GetWorld().GetEntity(EmptyHandle)->BodyType == ESceneBodyType::Dynamic);
	CHECK(Scene.GetWorld().GetEntity(FloorHandle)->BodyType == ESceneBodyType::Dynamic);
	CHECK(Scene.FindBodies(ESceneBodyType::Static).empty());
	REQUIRE(Scene.SetSelectedBodyType(ESceneBodyType::None));
	CHECK(Scene.GetWorld().GetEntity(EmptyHandle)->BodyType == ESceneBodyType::None);
	CHECK(Scene.GetWorld().GetEntity(CubeHandle)->BodyType == ESceneBodyType::None);
	CHECK(Scene.GetWorld().GetEntity(FloorHandle)->BodyType == ESceneBodyType::None);
	REQUIRE(Scene.Undo());
	REQUIRE(Scene.Undo());
	CHECK(Scene.GetWorld().SnapshotEntities() == Added);
	REQUIRE(Scene.Undo());
	CHECK(Scene.GetWorld().SnapshotEntities() == Before);
	CHECK(Scene.GetWorld().FindEntity(*Empty) == EmptyHandle);
	CHECK(Scene.GetWorld().FindEntity(Cube) == CubeHandle);
	CHECK(Scene.GetWorld().FindEntity(Floor) == FloorHandle);
	CHECK(Scene.GetActiveObject() == *Empty);
	CHECK(SelectedEditorObjects(Scene) == std::vector<FObjectId>(Selected.begin(), Selected.end()));
}

TEST_CASE("Mixed rigid body edits never insert missing components or consume no-op redo branches")
{
	FEditorScene Scene;
	const FObjectId Cube = Scene.GetObjects()[0].Id;
	const FObjectId Floor = Scene.GetObjects()[1].Id;
	const auto Empty = Scene.CreateEmptyEntity();
	REQUIRE(Empty);
	const auto EmptyHandle = *Scene.GetWorld().FindEntity(*Empty);
	const auto CubeHandle = *Scene.GetWorld().FindEntity(Cube);
	const auto FloorHandle = *Scene.GetWorld().FindEntity(Floor);
	const std::array Selected{*Empty, Cube, Floor};
	Scene.SetSelection(Selected, *Empty);
	const auto Before = Scene.GetWorld().SnapshotEntities();
	REQUIRE(Scene.SetSelectedBodyType(ESceneBodyType::Static));
	CHECK(Scene.GetWorld().GetEntity(EmptyHandle)->BodyType == ESceneBodyType::None);
	CHECK(Scene.GetWorld().GetEntity(CubeHandle)->BodyType == ESceneBodyType::Static);
	CHECK(Scene.GetWorld().GetEntity(FloorHandle)->BodyType == ESceneBodyType::Static);
	CHECK(Scene.GetActiveObject() == *Empty);
	REQUIRE(Scene.Undo());
	CHECK(Scene.GetWorld().SnapshotEntities() == Before);
	Scene.SetSelection(std::array{*Empty}, *Empty);
	const auto Generation = Scene.GetGeneration();
	REQUIRE(Scene.SetSelectedBodyType(ESceneBodyType::Dynamic));
	REQUIRE(Scene.SetSelectedBodyType(ESceneBodyType::None));
	CHECK(Scene.GetWorld().SnapshotEntities() == Before);
	CHECK(Scene.GetGeneration() == Generation);
	CHECK(Scene.CanRedo());
	Scene.SetSelection(std::array{Floor}, Floor);
	REQUIRE(Scene.AddRigidBodyToSelected());
	REQUIRE(Scene.SetSelectedBodyType(ESceneBodyType::Static));
	CHECK(Scene.CanRedo());
	REQUIRE(Scene.Redo());
	CHECK(Scene.GetWorld().GetEntity(EmptyHandle)->BodyType == ESceneBodyType::None);
	CHECK(Scene.GetWorld().GetEntity(CubeHandle)->BodyType == ESceneBodyType::Static);
	CHECK(Scene.GetActiveObject() == *Empty);
	CHECK(Scene.GetWorld().FindEntity(*Empty) == EmptyHandle);
	CHECK(Scene.GetWorld().FindEntity(Cube) == CubeHandle);
	CHECK(Scene.GetWorld().FindEntity(Floor) == FloorHandle);
}

TEST_CASE("Meshless rigid bodies load and save but cannot be selected for preview simulation")
{
	Tests::FScratchDirectory Scratch("HertaMeshlessBodies");
	const std::array Entities{
	    FSceneEntity{.Id = FObjectId{1, 1}, .Name = "Empty", .BodyType = ESceneBodyType::Dynamic},
	    FSceneEntity{.Id = FObjectId{2, 2}, .Name = "Floor", .Mesh = FStaticMeshComponent{EngineCubeAsset}, .BodyType = ESceneBodyType::Static},
	    FSceneEntity{.Id = FObjectId{3, 3}, .Name = "Cube", .Mesh = FStaticMeshComponent{EngineCubeAsset}, .BodyType = ESceneBodyType::Dynamic},
	};

	const auto Path = Scratch.GetPath() / "Bodies.hscene";
	REQUIRE(SaveScene(Path, {.Id = FObjectId::Generate(), .Name = "Bodies", .Entities = {Entities.begin(), Entities.end()}}));
	FEditorScene Scene;
	REQUIRE(Scene.Load(Path));
	CHECK(Scene.FindBodies(ESceneBodyType::Static) == std::vector<std::size_t>{1});
	CHECK(Scene.FindBodies(ESceneBodyType::Dynamic) == std::vector<std::size_t>{2});
	CHECK_FALSE(Scene.GetObjects()[0].Mesh.IsValid());
	REQUIRE(Scene.CommitEdits());
	CHECK_FALSE(Scene.IsDirty());
	REQUIRE(Scene.Save(Path));
	const auto Saved = LoadScene(Path);
	REQUIRE(Saved);
	CHECK_FALSE(Saved->Entities[0].Mesh);
	CHECK(Saved->Entities[0].BodyType == ESceneBodyType::Dynamic);
	REQUIRE(Scene.AddStaticMeshToSelected(EngineCubeAsset));
	CHECK(Scene.FindBodies(ESceneBodyType::Dynamic) == std::vector<std::size_t>{0, 2});
	REQUIRE(Scene.RemoveStaticMeshFromSelected());
	CHECK(Scene.FindBodies(ESceneBodyType::Dynamic) == std::vector<std::size_t>{2});
}

TEST_CASE("Component authoring failures no-op selections and history admission preserve authored state")
{
	FEditorScene Scene;
	const auto Before = Scene.GetWorld().SnapshotEntities();
	const auto Selection = SelectedEditorObjects(Scene);
	const auto Active = Scene.GetActiveObject();
	const auto Generation = Scene.GetGeneration();
	CHECK_FALSE(Scene.AddStaticMeshToSelected({}));
	CHECK_FALSE(Scene.AddRigidBodyToSelected(ESceneBodyType::None));
	CHECK_FALSE(Scene.AddRigidBodyToSelected(static_cast<ESceneBodyType>(255)));
	CHECK_FALSE(Scene.SetSelectedBodyType(static_cast<ESceneBodyType>(255)));
	CHECK_FALSE(Scene.CreateEmptyEntity(FWorldPosition{std::numeric_limits<double>::infinity(), 0., 0.}));
	CHECK(Scene.GetWorld().SnapshotEntities() == Before);
	CHECK(SelectedEditorObjects(Scene) == Selection);
	CHECK(Scene.GetActiveObject() == Active);
	CHECK(Scene.GetGeneration() == Generation);
	CHECK_FALSE(Scene.IsDirty());
	Scene.SetSelection({});
	REQUIRE(Scene.AddStaticMeshToSelected(EngineCubeAsset));
	REQUIRE(Scene.RemoveStaticMeshFromSelected());
	REQUIRE(Scene.AddRigidBodyToSelected());
	REQUIRE(Scene.SetSelectedBodyType(ESceneBodyType::None));
	CHECK_FALSE(Scene.CanUndo());
	CHECK_FALSE(Scene.IsDirty());

	for (const bool bSimulation : {false, true})
	{
		CAPTURE(bSimulation);
		FEditorScene Blocked;
		if (bSimulation)
		{
			Blocked.SetSimulationRunning(true);
		}
		else
		{
			REQUIRE(Blocked.BeginEdit("Active gesture"));
		}

		CHECK_FALSE(Blocked.CreateEmptyEntity());
		CHECK_FALSE(Blocked.AddStaticMeshToSelected(EngineCubeAsset));
		CHECK_FALSE(Blocked.RemoveStaticMeshFromSelected());
		CHECK_FALSE(Blocked.AddRigidBodyToSelected());
		CHECK_FALSE(Blocked.SetSelectedBodyType(ESceneBodyType::None));
		CHECK(Blocked.GetWorld().SnapshotEntities() == Before);
		CHECK_FALSE(Blocked.IsDirty());
	}

	FEditorScene Limited(0);
	const auto Handle = *Limited.GetWorld().FindEntity(Before.front().Id);
	const auto LimitedGeneration = Limited.GetGeneration();
	CHECK_FALSE(Limited.RemoveStaticMeshFromSelected());
	CHECK_FALSE(Limited.SetSelectedBodyType(ESceneBodyType::None));
	CHECK_FALSE(Limited.CreateEmptyEntity());
	CHECK(Limited.GetWorld().SnapshotEntities() == Before);
	CHECK(Limited.GetWorld().FindEntity(Before.front().Id) == Handle);
	CHECK(Limited.GetGeneration() == LimitedGeneration);
	CHECK_FALSE(Limited.CanUndo());
	CHECK_FALSE(Limited.IsDirty());
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
