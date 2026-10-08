#include "EditorLevel.h"
#include "Herta/EditorCore/LevelCommands.h"
#include "MaterialPanel.h"
#include "PreviewVisuals.h"
#include "TestFiles.h"

#include <doctest/doctest.h>
#include <im3d.h>

#include <array>
#include <limits>
#include <numbers>

namespace Herta
{
namespace
{
struct FVisualPreviewTestContext
{
	FVisualPreviewTestContext();
	~FVisualPreviewTestContext();
	FVisualPreviewTestContext(const FVisualPreviewTestContext&) = delete;
	FVisualPreviewTestContext& operator=(const FVisualPreviewTestContext&) = delete;
	FVisualPreviewTestContext(FVisualPreviewTestContext&&) = delete;
	FVisualPreviewTestContext& operator=(FVisualPreviewTestContext&&) = delete;

	Im3d::Context Context;
	Im3d::Context& Previous = Im3d::GetContext();
};

FVisualPreviewTestContext::FVisualPreviewTestContext()
{
	Im3d::SetContext(Context);
	auto& AppData = Context.getAppData();
	AppData.m_viewOrigin = {0.f, 0.f, -10.f};
	AppData.m_viewDirection = {0.f, 0.f, 1.f};
	AppData.m_viewportSize = {960.f, 540.f};
	AppData.m_projScaleY = 1.1547005f;
}

FVisualPreviewTestContext::~FVisualPreviewTestContext()
{
	Im3d::SetContext(Previous);
}

FPreviewObject& FindEditorObject(FEditorLevel& Level, const FObjectId Id)
{
	const auto Found = std::ranges::find(Level.GetObjects(), Id, &FPreviewObject::Id);
	REQUIRE(Found != Level.GetObjects().end());
	return *Found;
}

std::vector<FObjectId> SelectedEditorObjects(const FEditorLevel& Level)
{
	const auto Selected = Level.GetSelection();
	return {Selected.begin(), Selected.end()};
}
}

TEST_CASE("Visual preview ranges use physical meters and game view hides all guides")
{
	FVisualPreviewTestContext Context;
	FWorld World;
	FLightComponent Light;
	Light.Range = 8.f;
	const auto Id = World.QueueCreateEntity(FLevelEntity{.Name = "Point", .Light = Light});
	REQUIRE(Id);
	REQUIRE(World.FlushStructuralChanges());
	const std::array<FPreviewObject, 1> Objects{{{.Label = "Point", .Translation = {0.f}, .Scale = {100.f}, .Id = *Id, .Kind = EPreviewObjectKind::PointLight}}};
	FPreviewSelection Selection;
	Im3d::NewFrame();
	DrawPreviewVisuals(World, Objects, Selection, false);
	Im3d::EndFrame();
	REQUIRE(Im3d::GetDrawListCount() > 0);
	float MaximumDistance = 0.f;

	for (const Im3d::DrawList& List : std::span{Im3d::GetDrawLists(), Im3d::GetDrawListCount()})
	{
		for (const Im3d::VertexData& Vertex : std::span{List.m_vertexData, List.m_vertexCount})
		{
			const auto& Position = Vertex.m_positionSize;
			MaximumDistance = std::max(MaximumDistance, Im3d::Length(Im3d::Vec3{Position.x, Position.y, Position.z}));
		}
	}

	CHECK(MaximumDistance == doctest::Approx(8.f));
	Im3d::NewFrame();
	DrawPreviewVisuals(World, Objects, Selection, true);
	Im3d::EndFrame();
	CHECK(Im3d::GetDrawListCount() == 0);
}

TEST_CASE("Material drafts keep gesture history separate from level history and preserve failed saves")
{
	FMaterialPanel Panel;
	const FAssetId Asset{5, 5};
	FMaterialAsset Original;
	Panel.Open(Asset, Original, "Game/Test.hmat");
	CHECK_FALSE(Panel.IsDirty());
	Panel.BeginEdit("Roughness gesture");
	FMaterialAsset Changed = Original;
	Changed.Parameters.Roughness = 0.1f;
	REQUIRE(Panel.SetDraft(Changed));
	Changed.Parameters.Roughness = 0.3f;
	REQUIRE(Panel.SetDraft(Changed));
	REQUIRE(Panel.EndEdit());
	CHECK(Panel.IsDirty());
	REQUIRE(Panel.Undo());
	CHECK(Panel.GetDraft() == Original);
	CHECK_FALSE(Panel.IsDirty());
	REQUIRE(Panel.Redo());
	CHECK(Panel.GetDraft() == Changed);
	FMaterialPanelContext Context{
	    .Save = [](FAssetId, const FMaterialAsset&) -> std::expected<void, FAssetError>
	{
		return std::unexpected(FAssetError{.Message = "Read-only directory"});
	},
	};

	CHECK_FALSE(Panel.Save(Context));
	CHECK(Panel.IsDirty());
	CHECK(Panel.GetDraft() == Changed);
	Context.Save = [Asset](const FAssetId SavedId, const FMaterialAsset&) -> std::expected<void, FAssetError>
	{
		CHECK(SavedId == Asset);
		return {};
	};
	REQUIRE(Panel.Save(Context));
	CHECK_FALSE(Panel.IsDirty());
	REQUIRE(Panel.Undo());
	CHECK(Panel.IsDirty());
	Panel.BeginEdit("Cancel tint");
	FMaterialAsset Tint = Panel.GetDraft();
	Tint.Parameters.BaseColor = {0.2f, 0.3f, 0.4f, 1.f};
	REQUIRE(Panel.SetDraft(Tint));
	Panel.CancelEdit();
	CHECK(Panel.GetDraft() == Original);
	FMaterialAsset Invalid = Panel.GetDraft();
	Invalid.Parameters.Roughness = -1.f;
	CHECK_FALSE(Panel.SetDraft(Invalid));
	CHECK(Panel.GetDraft() == Original);
	Panel.Open(FAssetId{6, 6}, FMaterialAsset{}, "Game/Other.hmat");
	CHECK(Panel.GetAsset() == Asset);
	FMaterialPanel Engine;
	Engine.Open(Asset, Original, "Engine/Test.hmat", true);
	CHECK_FALSE(Engine.SetDraft(Changed));
	CHECK_FALSE(Engine.Save(Context));
}

TEST_CASE("Mixed visual property edits preserve unrelated component values")
{
	FEditorLevel Level;
	const auto First = Level.CreateLightEntity(ELightType::Point);
	REQUIRE(First);
	REQUIRE(Level.SetSelectedVisualProperty(ELevelComponentType::Light, "intensity", 2000.f));
	const auto Second = Level.CreateLightEntity(ELightType::Point);
	REQUIRE(Second);
	Level.SetSelection(std::array{*First, *Second});
	const auto Before = Level.GetWorld().SnapshotEntities();
	REQUIRE(Level.SetSelectedVisualProperty(ELevelComponentType::Light, "color", FVector3{0.3f, 0.5f, 0.7f}));
	const auto A = Level.GetWorld().GetEntity(*Level.GetWorld().FindEntity(*First));
	const auto B = Level.GetWorld().GetEntity(*Level.GetWorld().FindEntity(*Second));
	CHECK(A->Light->Intensity == 2000.f);
	CHECK(B->Light->Intensity == 1000.f);
	CHECK(A->Light->Color == B->Light->Color);
	REQUIRE(Level.Undo());
	CHECK(Level.GetWorld().SnapshotEntities() == Before);
	CHECK_FALSE(Level.SetSelectedVisualProperty(ELevelComponentType::Light, "intensity", true));
	CHECK(Level.GetWorld().SnapshotEntities() == Before);
}

TEST_CASE("Visual authoring and material overrides use grouped reversible transactions")
{
	FEditorLevel Level;
	const auto Created = Level.CreateLightEntity(ELightType::Directional, FWorldPosition{1., 2., 3.});
	REQUIRE(Created);
	CHECK(Level.GetWorld().GetEntity(*Level.GetWorld().FindEntity(*Created))->Light->Intensity == 128'000.f);
	const auto Before = Level.GetWorld().SnapshotEntities();
	REQUIRE(Level.BeginEdit("Tune sun"));
	FLightComponent Light = *Level.GetWorld().GetEntity(*Level.GetWorld().FindEntity(*Created))->Light;
	Light.Intensity = 60'000.f;
	REQUIRE(Level.SetSelectedLight(Light));
	Light.Intensity = 80'000.f;
	REQUIRE(Level.SetSelectedLight(Light));
	REQUIRE(Level.EndEdit());
	CHECK(Level.GetUndoLabel() == "Tune sun");
	REQUIRE(Level.Undo());
	CHECK(Level.GetWorld().SnapshotEntities() == Before);
	REQUIRE(Level.Redo());
	CHECK(Level.GetWorld().GetEntity(*Level.GetWorld().FindEntity(*Created))->Light->Intensity == 80'000.f);
	REQUIRE(Level.SetSelectedLight(std::nullopt));
	CHECK_FALSE(Level.GetWorld().GetEntity(*Level.GetWorld().FindEntity(*Created))->Light);
	REQUIRE(Level.Undo());
	REQUIRE(Level.AddSkyAtmosphereToSelected());
	REQUIRE(Level.AddHeightFogToSelected());
	const auto Visual = *Level.GetWorld().GetEntity(*Level.GetWorld().FindEntity(*Created));
	REQUIRE(Visual.SkyAtmosphere);
	REQUIRE(Visual.HeightFog);
	FHeightFogComponent Invalid = *Visual.HeightFog;
	Invalid.Density = -1.f;
	CHECK_FALSE(Level.SetSelectedHeightFog(Invalid));
	CHECK(Level.GetWorld().GetEntity(*Level.GetWorld().FindEntity(*Created)) == Visual);
	Level.SetSimulationRunning(true);
	CHECK_FALSE(Level.SetSelectedLight(Light));
	Level.SetSimulationRunning(false);
	const auto Cube = Level.CreateEntity();
	REQUIRE(Cube);
	REQUIRE(Level.SetSelectedMaterial(2, FAssetId{4, 2}));
	const auto Mesh = Level.GetWorld().GetEntity(*Level.GetWorld().FindEntity(*Cube))->Mesh;
	REQUIRE(Mesh);
	REQUIRE(Mesh->Materials.size() == 3);
	CHECK_FALSE(Mesh->Materials[0].IsValid());
	CHECK(Mesh->Materials[2] == FAssetId{4, 2});
	REQUIRE(Level.Undo());
	CHECK(Level.GetWorld().GetEntity(*Level.GetWorld().FindEntity(*Cube))->Mesh->Materials.empty());
	REQUIRE(Level.Redo());
	REQUIRE(Level.SetSelectedMaterial(2, {}));
	CHECK(Level.GetWorld().GetEntity(*Level.GetWorld().FindEntity(*Cube))->Mesh->Materials.empty());
	CHECK_FALSE(Level.SetSelectedMaterial(256, FAssetId{4, 3}));
}

TEST_CASE("Editor level edits and saves Level-owned transforms and stable identities")
{
	Tests::FScratchDirectory Scratch("HertaEditorLevel");
	FEditorLevel Level;
	const FObjectId Id = Level.GetObjects()[0].Id;
	const FEntityId Handle = *Level.GetWorld().FindEntity(Id);
	Level.GetObjects()[0].Label = "Renamed cube";
	Level.GetObjects()[0].Translation.y = 7.f;
	Level.GetObjects()[1].Scale.x = 20.f;
	REQUIRE(Level.CommitEdits());
	CHECK(Level.GetWorld().FindEntity(Id) == Handle);
	CHECK(Level.GetWorld().GetEntity(Handle)->Transform.Translation.Meters.Y == 7.);
	const auto Path = Scratch.GetPath() / "Saved.hlevel";
	REQUIRE(Level.Save(Path));
	FEditorLevel Restored;
	REQUIRE(Restored.Load(Path));
	CHECK(Restored.GetObjects()[0].Id == Id);
	CHECK(Restored.GetObjects()[0].Label == "Renamed cube");
	CHECK(Restored.GetObjects()[0].Translation.y == 7.f);
	CHECK(Restored.GetObjects()[1].Scale.x == 20.f);
}

TEST_CASE("Moving one object keeps every entity's material slots through commit and save")
{
	Tests::FScratchDirectory Scratch("HertaEditorLevelMaterials");
	FEditorLevel Level;
	const FObjectId Cube = Level.GetObjects()[0].Id;
	const FObjectId Floor = Level.GetObjects()[1].Id;
	Level.SetSelection(std::array{Floor}, Floor);
	REQUIRE(Level.SetSelectedMaterial(0, FAssetId{7, 7}));
	Level.GetObjects()[0].Translation.y = 9.f;
	REQUIRE(Level.CommitEdits());
	const auto Materials = [&](const FEditorLevel& Source, const FObjectId Id)
	{
		return Source.GetWorld().GetEntity(*Source.GetWorld().FindEntity(Id))->Mesh->Materials;
	};

	CHECK(Materials(Level, Floor) == std::vector<FAssetId>{FAssetId{7, 7}});
	const auto Path = Scratch.GetPath() / "Materials.hlevel";
	REQUIRE(Level.Save(Path));
	FEditorLevel Restored;
	REQUIRE(Restored.Load(Path));
	CHECK(Materials(Restored, Floor) == std::vector<FAssetId>{FAssetId{7, 7}});
	CHECK(Restored.GetWorld().GetEntity(*Restored.GetWorld().FindEntity(Cube))->Transform.Translation.Meters.Y == 9.);
}

TEST_CASE("Editor level saves authored data while simulation changes the transient view")
{
	Tests::FScratchDirectory Scratch("HertaLevelSimulation");
	FEditorLevel Level;
	Level.GetObjects()[0].Translation.y = 8.f;
	REQUIRE(Level.CommitEdits());
	Level.SetSimulationRunning(true);
	Level.GetObjects()[0].Translation.y = 0.5f;
	const auto Path = Scratch.GetPath() / "Simulated.hlevel";
	REQUIRE(Level.Save(Path));
	const auto Saved = LoadLevel(Path);
	REQUIRE(Saved);
	CHECK(Saved->Entities[0].Transform.Translation.Meters.Y == 8.);
	CHECK_FALSE(Level.Load(Path));
}

TEST_CASE("Editor level load failures preserve the world, view, path and generation")
{
	Tests::FScratchDirectory Scratch("HertaLevelFailure");
	FEditorLevel Level;
	const auto Before = Level.GetWorld().SnapshotEntities();
	const auto Generation = Level.GetGeneration();
	const auto Path = Scratch.GetPath() / "Broken.hlevel";
	Tests::WriteText(Path, "{broken");
	CHECK_FALSE(Level.Load(Path));
	CHECK(Level.GetWorld().SnapshotEntities() == Before);
	CHECK(Level.GetGeneration() == Generation);
	CHECK(Level.GetPath().empty());
	Level.GetObjects()[0].Translation.x = std::numeric_limits<float>::infinity();
	CHECK_FALSE(Level.CommitEdits());
	CHECK(Level.GetWorld().SnapshotEntities() == Before);
}

TEST_CASE("Editor level loads variable entity counts and identifies physics bodies by components")
{
	Tests::FScratchDirectory Scratch("HertaLevelCounts");
	FEditorLevel Level;
	FLevelDocument Document{.Id = FObjectId::Generate(), .Name = "Reordered", .Entities = Level.GetWorld().SnapshotEntities()};
	Document.Entities[0].Id = FObjectId{9, 9};
	Document.Entities[1].Id = FObjectId{1, 1};
	FLevelEntity Extra = Document.Entities[0];
	Extra.Id = FObjectId{8, 8};
	Extra.BodyType = ELevelBodyType::None;
	Document.Entities.push_back(Extra);
	const auto Path = Scratch.GetPath() / "Counts.hlevel";
	REQUIRE(SaveLevel(Path, Document));
	REQUIRE(Level.Load(Path));
	CHECK(Level.GetObjects().size() == 3);
	CHECK(Level.FindBodies(ELevelBodyType::Static) == std::vector<std::size_t>{0});
	CHECK(Level.FindBodies(ELevelBodyType::Dynamic) == std::vector<std::size_t>{2});
	Document.Entities[2].BodyType = ELevelBodyType::Dynamic;
	REQUIRE(SaveLevel(Path, Document));
	REQUIRE(Level.Load(Path));
	CHECK(Level.FindBodies(ELevelBodyType::Dynamic) == std::vector<std::size_t>{1, 2});
	Document.Entities[2].BodyType = ELevelBodyType::Static;
	REQUIRE(SaveLevel(Path, Document));
	REQUIRE(Level.Load(Path));
	CHECK(Level.FindBodies(ELevelBodyType::Static) == std::vector<std::size_t>{0, 1});
	Document.Entities.clear();
	REQUIRE(SaveLevel(Path, Document));
	REQUIRE(Level.Load(Path));
	CHECK(Level.GetObjects().empty());
	CHECK(Level.FindBodies(ELevelBodyType::Dynamic).empty());
}

TEST_CASE("Editor level does not lose unedited double coordinates through its float view")
{
	Tests::FScratchDirectory Scratch("HertaLevelPrecision");
	FEditorLevel Level;
	FLevelDocument Document{.Id = FObjectId::Generate(), .Name = "Precision", .Entities = Level.GetWorld().SnapshotEntities()};
	Document.Entities[0].Transform.Translation.Meters.X = 1234567.123456789;
	const auto Path = Scratch.GetPath() / "Precision.hlevel";
	REQUIRE(SaveLevel(Path, Document));
	REQUIRE(Level.Load(Path));
	Level.GetObjects()[0].Label = "Renamed";
	Level.GetObjects()[0].Translation.y = 9.f;
	REQUIRE(Level.Save());
	const auto Saved = LoadLevel(Path);
	REQUIRE(Saved);
	CHECK(Saved->Entities[0].Transform.Translation.Meters.X == 1234567.123456789);
	CHECK(Saved->Entities[0].Transform.Translation.Meters.Y == 9.);
}

TEST_CASE("Editor level rejects out-of-range edits before saving or changing authored state")
{
	Tests::FScratchDirectory Scratch("HertaLevelEditorRange");
	FEditorLevel Level;
	const auto Path = Scratch.GetPath() / "Range.hlevel";
	FPreviewObject& Object = Level.GetObjects()[0];
	Object.Translation = {1.e7f, -1.e7f, 0.f};
	Object.Scale = {0.001f, 1000.f, 1.f};
	REQUIRE(Level.Save(Path));
	FEditorLevel Restored;
	REQUIRE(Restored.Load(Path));
	const auto Before = Level.GetWorld().SnapshotEntities();
	const auto Original = LoadLevel(Path);
	REQUIRE(Original);
	const auto InvalidTransforms = std::array{
	    FLevelTransform{.Translation = FWorldPosition{10000001., 0., 0.}},
	    FLevelTransform{.Scale = FVector3{1001.f, 1.f, 1.f}},
	    FLevelTransform{.Scale = FVector3{0.0005f, 1.f, 1.f}},
	};

	for (const FLevelTransform& Transform : InvalidTransforms)
	{
		Object.Translation = {static_cast<float>(Transform.Translation.Meters.X), static_cast<float>(Transform.Translation.Meters.Y), static_cast<float>(Transform.Translation.Meters.Z)};
		Object.Scale = {Transform.Scale.X, Transform.Scale.Y, Transform.Scale.Z};
		CHECK_FALSE(Level.CommitEdits());
		Object.Translation = {static_cast<float>(Transform.Translation.Meters.X), static_cast<float>(Transform.Translation.Meters.Y), static_cast<float>(Transform.Translation.Meters.Z)};
		Object.Scale = {Transform.Scale.X, Transform.Scale.Y, Transform.Scale.Z};
		CHECK_FALSE(Level.Save(Path));
		CHECK(Level.GetWorld().SnapshotEntities() == Before);
		CHECK(Level.GetPath() == Path);
		const auto Preserved = LoadLevel(Path);
		REQUIRE(Preserved);
		CHECK(Preserved->Entities == Original->Entities);
		REQUIRE(Restored.Load(Path));
	}
}

TEST_CASE("Edited level rotations preserve nontrivial and near-gimbal rotation matrices")
{
	Tests::FScratchDirectory Scratch("HertaLevelRotation");
	const auto Path = Scratch.GetPath() / "Rotation.hlevel";
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
		FEditorLevel Level;
		const FQuaternion Rotation = FQuaternion::FromAxisAngle({0.f, 0.f, 1.f}, Euler.Z) * FQuaternion::FromAxisAngle({0.f, 1.f, 0.f}, Euler.Y) * FQuaternion::FromAxisAngle({1.f, 0.f, 0.f}, Euler.X);
		const FMatrix3 Matrix = FMatrix3::Rotation(Rotation);
		FPreviewObject& Object = Level.GetObjects()[0];
		Object.Rotation = FromPreviewEulerXYZ({Euler.X, Euler.Y, Euler.Z});

		for (int Row = 0; Row < 3; ++Row)
		{
			for (int Column = 0; Column < 3; ++Column)
			{
				CHECK(Object.Rotation(Row, Column) == doctest::Approx(Matrix(static_cast<std::size_t>(Row), static_cast<std::size_t>(Column))).epsilon(0.000001).scale(1.));
			}
		}

		REQUIRE(Level.Save(Path));
		FEditorLevel Restored;
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
	FEditorLevel Level;
	const auto Before = Level.GetWorld().SnapshotEntities();
	const FObjectId Cube = Level.GetObjects()[0].Id;
	const FObjectId Floor = Level.GetObjects()[1].Id;
	const std::array Selected{Cube, Floor};
	Level.SetSelection(Selected, Cube);
	const auto Generation = Level.GetGeneration();
	REQUIRE(Level.BeginEdit("Move objects"));
	CHECK(Level.HasActiveEdit());
	CHECK_FALSE(Level.CanUndo());
	FindEditorObject(Level, Cube).Translation.y = 5.f;
	FindEditorObject(Level, Floor).Translation.x = 1.f;
	REQUIRE(Level.CommitEdits());
	CHECK(Level.IsDirty());
	CHECK_FALSE(Level.CanUndo());
	FindEditorObject(Level, Cube).Translation.y = 8.f;
	REQUIRE(Level.CommitEdits());
	REQUIRE(Level.EndEdit());
	CHECK_FALSE(Level.HasActiveEdit());
	CHECK(Level.GetGeneration() == Generation);
	CHECK(Level.GetUndoLabel() == "Move objects");
	const auto After = Level.GetWorld().SnapshotEntities();
	REQUIRE(Level.Undo());
	CHECK(Level.GetWorld().SnapshotEntities() == Before);
	CHECK(SelectedEditorObjects(Level) == std::vector<FObjectId>(Selected.begin(), Selected.end()));
	CHECK(Level.GetActiveObject() == Cube);
	CHECK_FALSE(Level.CanUndo());
	CHECK_FALSE(Level.IsDirty());
	REQUIRE(Level.Redo());
	CHECK(Level.GetWorld().SnapshotEntities() == After);
	CHECK(Level.IsDirty());
}

TEST_CASE("No-op and canceled editor gestures preserve redo and the saved state")
{
	FEditorLevel Level;
	const FObjectId Cube = Level.GetObjects()[0].Id;
	FindEditorObject(Level, Cube).Translation.y = 8.f;
	REQUIRE(Level.CommitEdits("Raise cube"));
	REQUIRE(Level.Undo());
	const auto Before = Level.GetWorld().SnapshotEntities();
	REQUIRE(Level.BeginEdit("No-op"));
	FindEditorObject(Level, Cube).Translation.y = 9.f;
	REQUIRE(Level.CommitEdits());
	FindEditorObject(Level, Cube).Translation.y = 4.f;
	REQUIRE(Level.EndEdit());
	CHECK_FALSE(Level.IsDirty());
	CHECK(Level.CanRedo());
	CHECK(Level.GetRedoLabel() == "Raise cube");
	CHECK(Level.GetWorld().SnapshotEntities() == Before);
	REQUIRE(Level.BeginEdit("Canceled move"));
	FindEditorObject(Level, Cube).Translation.y = 12.f;
	REQUIRE(Level.CommitEdits());
	CHECK(Level.IsDirty());
	CHECK_FALSE(Level.Save());
	CHECK_FALSE(Level.Load("Unused.hlevel"));
	CHECK_FALSE(Level.Undo());
	CHECK_FALSE(Level.DeleteSelected());
	REQUIRE(Level.CancelEdit());
	CHECK(Level.GetWorld().SnapshotEntities() == Before);
	CHECK(FindEditorObject(Level, Cube).Translation.y == 4.f);
	CHECK_FALSE(Level.IsDirty());
	CHECK(Level.CanRedo());
	REQUIRE(Level.Redo());
	CHECK(FindEditorObject(Level, Cube).Translation.y == 8.f);
}

TEST_CASE("Rejected grouped edits cancel completely while preserving widget references")
{
	FEditorLevel Level;
	const auto Before = Level.GetWorld().SnapshotEntities();
	FPreviewObject* const Object = &Level.GetObjects()[0];
	REQUIRE(Level.BeginEdit("Rejected move"));
	Object->Translation.y = 8.f;
	REQUIRE(Level.CommitEdits());
	Object->Translation.x = 10000001.f;
	CHECK_FALSE(Level.EndEdit());
	CHECK_FALSE(Level.HasActiveEdit());
	CHECK(&Level.GetObjects()[0] == Object);
	CHECK(Level.GetWorld().SnapshotEntities() == Before);
	CHECK(Object->Translation.y == 4.f);
	CHECK_FALSE(Level.IsDirty());
	REQUIRE(Level.BeginEdit("Rejected rename"));
	Object->Label = std::string(1025, 'x');
	CHECK_FALSE(Level.EndEdit());
	CHECK_FALSE(Level.HasActiveEdit());
	CHECK(&Level.GetObjects()[0] == Object);
	CHECK(Level.GetWorld().SnapshotEntities() == Before);
	CHECK(Object->Label == "Preview Cube");
	Object->Translation.y = 8.f;
	CHECK_FALSE(Level.CommitEdits(""));
	CHECK(Level.GetWorld().SnapshotEntities() == Before);
	CHECK(Object->Translation.y == 4.f);
	CHECK_FALSE(Level.CanUndo());
}

TEST_CASE("Editor history branches and tracks successful saves and loads")
{
	Tests::FScratchDirectory Scratch("HertaLevelHistory");
	const auto Path = Scratch.GetPath() / "Saved.hlevel";
	FEditorLevel Level;
	const FObjectId Cube = Level.GetObjects()[0].Id;
	FindEditorObject(Level, Cube).Label = "First";
	REQUIRE(Level.CommitEdits("Rename object"));
	CHECK(Level.IsDirty());
	REQUIRE(Level.Save(Path));
	CHECK_FALSE(Level.IsDirty());
	REQUIRE(Level.Undo());
	CHECK(Level.IsDirty());
	REQUIRE(Level.Redo());
	CHECK_FALSE(Level.IsDirty());
	REQUIRE(Level.Undo());
	FindEditorObject(Level, Cube).Label = "Branched";
	REQUIRE(Level.CommitEdits("Rename object"));
	CHECK_FALSE(Level.CanRedo());
	CHECK(Level.IsDirty());
	CHECK_FALSE(Level.Save(Scratch.GetPath() / "Missing" / "Failed.hlevel"));
	CHECK(Level.IsDirty());
	CHECK(Level.GetPath() == Path);
	REQUIRE(Level.Save());
	CHECK_FALSE(Level.IsDirty());
	FindEditorObject(Level, Cube).Translation.y = 10.f;
	REQUIRE(Level.CommitEdits());
	REQUIRE(Level.Load(Path));
	CHECK_FALSE(Level.IsDirty());
	CHECK_FALSE(Level.CanUndo());
	CHECK_FALSE(Level.CanRedo());
	CHECK(Level.GetActiveObject() == Level.GetObjects()[0].Id);
}

TEST_CASE("Editor history preserves unedited double coordinates through grouped edits")
{
	Tests::FScratchDirectory Scratch("HertaLevelHistoryPrecision");
	const auto Path = Scratch.GetPath() / "Precision.hlevel";
	FEditorLevel Level;
	FLevelDocument Document{.Id = FObjectId::Generate(), .Name = "Precision", .Entities = Level.GetWorld().SnapshotEntities()};
	Document.Entities[0].Transform.Translation.Meters.X = 1234567.123456789;
	REQUIRE(SaveLevel(Path, Document));
	REQUIRE(Level.Load(Path));
	const FObjectId Cube = Level.GetObjects()[0].Id;
	REQUIRE(Level.BeginEdit("Move vertically"));
	FindEditorObject(Level, Cube).Translation.y = 8.f;
	REQUIRE(Level.CommitEdits());
	REQUIRE(Level.EndEdit());
	CHECK(Level.GetWorld().SnapshotEntities()[0].Transform.Translation.Meters.X == 1234567.123456789);
	REQUIRE(Level.Undo());
	CHECK(Level.GetWorld().SnapshotEntities() == Document.Entities);
	REQUIRE(Level.Redo());
	CHECK(Level.GetWorld().SnapshotEntities()[0].Transform.Translation.Meters.X == 1234567.123456789);
}

TEST_CASE("Structural editor transactions restore stable selection and reject old runtime handles")
{
	FEditorLevel Level;
	const std::vector<FObjectId> InitialSelection = SelectedEditorObjects(Level);
	const auto InitialActive = Level.GetActiveObject();
	const FObjectId Floor = Level.GetObjects()[1].Id;
	const FEntityId FloorHandle = *Level.GetWorld().FindEntity(Floor);
	const auto Created = Level.CreateEntity(FWorldPosition{2., 3., 4.});
	REQUIRE(Created);
	CHECK(Level.GetWorld().GetEntity(FloorHandle).has_value());
	CHECK(Level.GetActiveObject() == *Created);
	CHECK(SelectedEditorObjects(Level) == std::vector<FObjectId>{*Created});
	CHECK(FindEditorObject(Level, *Created).Label == "Cube");
	const FEntityId CreatedHandle = *Level.GetWorld().FindEntity(*Created);
	REQUIRE(Level.DeleteSelected());
	CHECK_FALSE(Level.GetWorld().GetEntity(CreatedHandle).has_value());
	CHECK(Level.GetSelection().empty());
	REQUIRE(Level.Undo());
	CHECK(Level.GetActiveObject() == *Created);
	CHECK(Level.GetWorld().FindEntity(*Created).has_value());
	CHECK_FALSE(Level.GetWorld().GetEntity(CreatedHandle).has_value());
	REQUIRE(Level.Undo());
	CHECK_FALSE(Level.GetWorld().FindEntity(*Created).has_value());
	CHECK(SelectedEditorObjects(Level) == InitialSelection);
	CHECK(Level.GetActiveObject() == InitialActive);
	REQUIRE(Level.Redo());
	CHECK(Level.GetActiveObject() == *Created);
	CHECK(Level.GetWorld().GetEntity(FloorHandle).has_value());
}

TEST_CASE("Rejected structural history admission preserves entities handles and selection")
{
	FEditorLevel Level(256, 1);
	const auto Before = Level.GetWorld().SnapshotEntities();
	const auto Selection = SelectedEditorObjects(Level);
	const auto Active = Level.GetActiveObject();
	const auto Generation = Level.GetGeneration();
	FPreviewObject* const Object = &Level.GetObjects()[0];
	std::vector<FEntityId> Handles;

	for (const FLevelEntity& Entity : Before)
	{
		Handles.push_back(*Level.GetWorld().FindEntity(Entity.Id));
	}

	CHECK_FALSE(Level.DeleteSelected());
	CHECK(Level.GetWorld().SnapshotEntities() == Before);
	CHECK(SelectedEditorObjects(Level) == Selection);
	CHECK(Level.GetActiveObject() == Active);
	CHECK(Level.GetGeneration() == Generation);
	CHECK(&Level.GetObjects()[0] == Object);
	CHECK_FALSE(Level.IsDirty());
	CHECK_FALSE(Level.CanUndo());
	CHECK_FALSE(Level.CanRedo());

	for (std::size_t Index = 0; Index < Before.size(); ++Index)
	{
		CHECK(Level.GetWorld().GetEntity(Handles[Index]) == Before[Index]);
		CHECK(Level.GetWorld().FindEntity(Before[Index].Id) == Handles[Index]);
	}
}

TEST_CASE("Editor duplication preserves components and avoids label collisions")
{
	FEditorLevel Level;
	const FObjectId Original = Level.GetObjects()[0].Id;
	const FLevelEntity Entity = *Level.GetWorld().GetEntity(*Level.GetWorld().FindEntity(Original));
	REQUIRE(Level.DuplicateSelected());
	const FObjectId First = *Level.GetActiveObject();
	CHECK(First != Original);
	CHECK(FindEditorObject(Level, First).Label == "Preview Cube Copy");
	const FLevelEntity Duplicate = *Level.GetWorld().GetEntity(*Level.GetWorld().FindEntity(First));
	CHECK(Duplicate.Transform == Entity.Transform);
	CHECK(Duplicate.Mesh == Entity.Mesh);
	CHECK(Duplicate.BodyType == Entity.BodyType);
	Level.SetSelection(std::span(&Original, 1), Original);
	REQUIRE(Level.DuplicateSelected());
	CHECK(FindEditorObject(Level, *Level.GetActiveObject()).Label == "Preview Cube Copy 2");
	REQUIRE(Level.Undo());
	CHECK(Level.GetActiveObject() == Original);
	REQUIRE(Level.Undo());
	CHECK(Level.GetObjects().size() == 2);
	CHECK(Level.GetActiveObject() == Original);
}

TEST_CASE("Command duplication translates every selected copy by the same world offset")
{
	FEditorLevel Level;
	const auto Before = Level.GetWorld().SnapshotEntities();
	const std::array Selected{Before[0].Id, Before[1].Id};
	Level.SetSelection(Selected, Selected.front());
	const FVector3d Offset{0.125, 0., 0.125};
	REQUIRE(Level.DuplicateSelected(false, Offset));
	const auto Copies = SelectedEditorObjects(Level);
	REQUIRE(Copies.size() == Selected.size());

	for (std::size_t Index = 0; Index < Copies.size(); ++Index)
	{
		const FLevelEntity Copy = *Level.GetWorld().GetEntity(*Level.GetWorld().FindEntity(Copies[Index]));
		CHECK(Copy.Id != Before[Index].Id);
		CHECK(Copy.Transform.Translation == Before[Index].Transform.Translation.TranslatedBy(Offset));
		CHECK(Copy.Transform.Rotation == Before[Index].Transform.Rotation);
		CHECK(Copy.Transform.Scale == Before[Index].Transform.Scale);
		CHECK(Copy.Mesh == Before[Index].Mesh);
		CHECK(Copy.BodyType == Before[Index].BodyType);
	}

	const auto After = Level.GetWorld().SnapshotEntities();
	CHECK(Level.GetUndoLabel() == "Duplicate objects");
	REQUIRE(Level.Undo());
	CHECK(Level.GetWorld().SnapshotEntities() == Before);
	CHECK(SelectedEditorObjects(Level) == std::vector<FObjectId>(Selected.begin(), Selected.end()));
	CHECK(Level.GetActiveObject() == Selected.front());
	CHECK_FALSE(Level.CanUndo());
	REQUIRE(Level.Redo());
	CHECK(Level.GetWorld().SnapshotEntities() == After);
	CHECK(SelectedEditorObjects(Level) == Copies);
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
			FEditorLevel Level;
			Level.GetObjects()[0].Translation.y = 8.f;
			REQUIRE(Level.CommitEdits("Raise cube"));
			if (bRedo)
			{
				REQUIRE(Level.Undo());
			}

			const auto Before = Level.GetWorld().SnapshotEntities();
			const std::array Selected{Before[0].Id, Before[1].Id};
			Level.SetSelection(Selected, Selected.front());
			const auto Generation = Level.GetGeneration();
			std::vector<FEntityId> Handles;

			for (const FLevelEntity& Entity : Before)
			{
				Handles.push_back(*Level.GetWorld().FindEntity(Entity.Id));
			}

			if (bWithinActiveEdit)
			{
				REQUIRE(Level.BeginEdit("Duplicate objects"));
			}

			for (const FVector3d& Offset : InvalidOffsets)
			{
				CHECK_FALSE(Level.DuplicateSelected(bWithinActiveEdit, Offset));
				CHECK(Level.GetWorld().SnapshotEntities() == Before);
				CHECK(SelectedEditorObjects(Level) == std::vector<FObjectId>(Selected.begin(), Selected.end()));
				CHECK(Level.GetActiveObject() == Selected.front());
				CHECK(Level.GetGeneration() == Generation);
				CHECK(Level.HasActiveEdit() == bWithinActiveEdit);
				CHECK(Level.IsDirty() == !bRedo);
				CHECK(Level.GetUndoLabel() == (bRedo ? "" : "Raise cube"));
				CHECK(Level.GetRedoLabel() == (bRedo ? "Raise cube" : ""));

				for (std::size_t Index = 0; Index < Before.size(); ++Index)
				{
					CHECK(Level.GetWorld().GetEntity(Handles[Index]) == Before[Index]);
				}
			}

			if (bWithinActiveEdit)
			{
				REQUIRE(Level.EndEdit());
			}

			CHECK(Level.CanUndo() == !bRedo);
			CHECK(Level.CanRedo() == bRedo);
		}
	}
}

TEST_CASE("Gesture duplication and transforms form one undo step for single and multiple selections")
{
	for (const bool bMultiple : std::array{false, true})
	{
		CAPTURE(bMultiple);
		FEditorLevel Level;
		const auto Before = Level.GetWorld().SnapshotEntities();
		std::vector<FObjectId> Originals{Before[0].Id};
		if (bMultiple)
		{
			Originals.push_back(Before[1].Id);
		}

		Level.SetSelection(Originals, Originals.front());
		std::vector<FEntityId> OriginalHandles;

		for (const FLevelEntity& Entity : Before)
		{
			OriginalHandles.push_back(*Level.GetWorld().FindEntity(Entity.Id));
		}

		REQUIRE(Level.BeginEdit("Duplicate objects"));
		REQUIRE(Level.DuplicateSelected(true));
		const auto Copies = SelectedEditorObjects(Level);
		REQUIRE(Copies.size() == Originals.size());
		CHECK(Level.GetObjects().size() == Before.size() + Originals.size());
		CHECK(Level.GetActiveObject() == Copies.front());
		CHECK_FALSE(Level.CanUndo());

		for (std::size_t Index = 0; Index < Copies.size(); ++Index)
		{
			CHECK(Level.GetWorld().GetEntity(*Level.GetWorld().FindEntity(Copies[Index]))->Transform == Before[Index].Transform);
		}

		const auto Generation = Level.GetGeneration();
		const auto Inserted = Level.GetWorld().SnapshotEntities();
		REQUIRE(Level.DuplicateSelected(true));
		CHECK(Level.GetWorld().SnapshotEntities() == Inserted);
		CHECK(SelectedEditorObjects(Level) == Copies);
		CHECK(Level.GetGeneration() == Generation);
		CHECK_FALSE(Level.DuplicateSelected());
		std::vector<FEntityId> CopyHandles;

		for (const FObjectId Copy : Copies)
		{
			CopyHandles.push_back(*Level.GetWorld().FindEntity(Copy));
			FPreviewObject& Object = FindEditorObject(Level, Copy);
			Object.Translation.x += 2.f;
			Object.Translation.y += 1.f;
			Object.Rotation = FromPreviewEulerXYZ({0.25f, -0.4f, 0.1f});
			Object.Scale = {2.f, 3.f, 4.f};
		}

		REQUIRE(Level.CommitEdits());
		REQUIRE(Level.DuplicateSelected(true));
		CHECK(Level.GetObjects().size() == Before.size() + Originals.size());
		REQUIRE(Level.EndEdit());
		CHECK(Level.GetUndoLabel() == "Duplicate objects");
		CHECK(Level.GetGeneration() == Generation);
		const auto After = Level.GetWorld().SnapshotEntities();
		REQUIRE(Level.Undo());
		CHECK(Level.GetWorld().SnapshotEntities() == Before);
		CHECK(SelectedEditorObjects(Level) == Originals);
		CHECK(Level.GetActiveObject() == Originals.front());
		CHECK_FALSE(Level.CanUndo());
		CHECK_FALSE(Level.IsDirty());

		for (std::size_t Index = 0; Index < Before.size(); ++Index)
		{
			CHECK(Level.GetWorld().GetEntity(OriginalHandles[Index]) == Before[Index]);
		}

		REQUIRE(Level.Redo());
		CHECK(Level.GetWorld().SnapshotEntities() == After);
		CHECK(SelectedEditorObjects(Level) == Copies);
		CHECK(Level.GetActiveObject() == Copies.front());

		for (const FEntityId Handle : CopyHandles)
		{
			CHECK_FALSE(Level.GetWorld().GetEntity(Handle));
		}
	}
}

TEST_CASE("Canceling gesture duplication restores original objects handles selection and redo")
{
	FEditorLevel Level;
	const FObjectId Original = Level.GetObjects()[0].Id;
	FindEditorObject(Level, Original).Translation.y = 8.f;
	REQUIRE(Level.CommitEdits("Raise cube"));
	REQUIRE(Level.Undo());
	const auto Before = Level.GetWorld().SnapshotEntities();
	const auto Selection = SelectedEditorObjects(Level);
	const auto Active = Level.GetActiveObject();
	const FEntityId Handle = *Level.GetWorld().FindEntity(Original);
	REQUIRE(Level.BeginEdit("Duplicate objects"));
	REQUIRE(Level.DuplicateSelected(true));
	const FObjectId Copy = *Level.GetActiveObject();
	FindEditorObject(Level, Copy).Translation.x = 5.f;
	REQUIRE(Level.CommitEdits());
	REQUIRE(Level.CancelEdit());
	CHECK(Level.GetWorld().SnapshotEntities() == Before);
	CHECK(Level.GetWorld().GetEntity(Handle) == Before[0]);
	CHECK_FALSE(Level.GetWorld().FindEntity(Copy));
	CHECK(SelectedEditorObjects(Level) == Selection);
	CHECK(Level.GetActiveObject() == Active);
	CHECK_FALSE(Level.HasActiveEdit());
	CHECK_FALSE(Level.IsDirty());
	CHECK(Level.CanRedo());
	CHECK(Level.GetRedoLabel() == "Raise cube");
	REQUIRE(Level.Redo());
	CHECK(FindEditorObject(Level, Original).Translation.y == 8.f);
}

TEST_CASE("Rejected gesture duplication history removes copies without invalidating original handles")
{
	FEditorLevel Level(256, 1);
	const auto Before = Level.GetWorld().SnapshotEntities();
	const auto Selection = SelectedEditorObjects(Level);
	const auto Active = Level.GetActiveObject();
	std::vector<FEntityId> Handles;

	for (const FLevelEntity& Entity : Before)
	{
		Handles.push_back(*Level.GetWorld().FindEntity(Entity.Id));
	}

	REQUIRE(Level.BeginEdit("Duplicate objects"));
	REQUIRE(Level.DuplicateSelected(true));
	const FObjectId Copy = *Level.GetActiveObject();
	FindEditorObject(Level, Copy).Translation.x = 3.f;
	REQUIRE(Level.CommitEdits());
	CHECK_FALSE(Level.EndEdit());
	CHECK(Level.GetWorld().SnapshotEntities() == Before);
	CHECK(SelectedEditorObjects(Level) == Selection);
	CHECK(Level.GetActiveObject() == Active);
	CHECK_FALSE(Level.GetWorld().FindEntity(Copy));
	CHECK_FALSE(Level.HasActiveEdit());
	CHECK_FALSE(Level.IsDirty());
	CHECK_FALSE(Level.CanUndo());
	CHECK_FALSE(Level.CanRedo());

	for (std::size_t Index = 0; Index < Before.size(); ++Index)
	{
		CHECK(Level.GetWorld().GetEntity(Handles[Index]) == Before[Index]);
		CHECK(Level.GetWorld().FindEntity(Before[Index].Id) == Handles[Index]);
	}
}

TEST_CASE("Clicking without a duplication drag leaves the level and history unchanged")
{
	FEditorLevel Level;
	const auto Before = Level.GetWorld().SnapshotEntities();
	const auto Selection = SelectedEditorObjects(Level);
	const auto Generation = Level.GetGeneration();
	CHECK_FALSE(Level.DuplicateSelected(true));
	REQUIRE(Level.BeginEdit("Duplicate objects"));
	REQUIRE(Level.EndEdit());
	CHECK(Level.GetWorld().SnapshotEntities() == Before);
	CHECK(SelectedEditorObjects(Level) == Selection);
	CHECK(Level.GetGeneration() == Generation);
	CHECK_FALSE(Level.IsDirty());
	CHECK_FALSE(Level.CanUndo());
	CHECK_FALSE(Level.CanRedo());
}

TEST_CASE("Level clipboard paste remaps UUIDs and rejects unsupported input atomically")
{
	FEditorLevel Source;
	const FObjectId Cube = Source.GetObjects()[0].Id;
	const FObjectId Floor = Source.GetObjects()[1].Id;
	const std::array Selected{Cube, Floor};
	Source.SetSelection(Selected, Cube);
	const auto Clipboard = Source.CopySelected();
	REQUIRE(Clipboard);
	FEditorLevel Target;
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
		const FLevelEntity Entity = *Target.GetWorld().GetEntity(*Target.GetWorld().FindEntity(Object));
		CHECK(Entity.Mesh->Asset == EngineCubeAsset);
		CHECK(Entity.Transform == Before[Index].Transform);
		CHECK(Entity.BodyType == Before[Index].BodyType);
		CHECK_FALSE(Entity.Parent.IsValid());
	}

	const auto After = Target.GetWorld().SnapshotEntities();
	CHECK_FALSE(Target.PasteEntities("{broken"));
	CHECK(Target.GetWorld().SnapshotEntities() == After);
	CHECK(SelectedEditorObjects(Target) == Pasted);
	FLevelDocument Invalid{.Id = FObjectId::Generate(), .Name = "Invalid", .Entities = Before};
	Invalid.Entities[0].Transform.Scale = {2.f, 1.f, 1.f};
	Invalid.Entities[1].Parent = Invalid.Entities[0].Id;
	Invalid.Entities[1].Transform.Rotation = FQuaternion::FromAxisAngle({0.f, 0.f, 1.f}, 0.4f);
	const auto Hierarchy = SerializeLevel(Invalid);
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

TEST_CASE("Empty levels support authored insertion deletion and history")
{
	Tests::FScratchDirectory Scratch("HertaLevelEmptyAuthoring");
	const auto Path = Scratch.GetPath() / "Empty.hlevel";
	REQUIRE(SaveLevel(Path, {.Id = FObjectId::Generate(), .Name = "Empty"}));
	FEditorLevel Level;
	REQUIRE(Level.Load(Path));
	CHECK(Level.GetSelection().empty());
	CHECK_FALSE(Level.GetActiveObject());
	const auto Created = Level.CreateEntity();
	REQUIRE(Created);
	REQUIRE(Level.DeleteSelected());
	CHECK(Level.GetObjects().empty());
	REQUIRE(Level.Undo());
	CHECK(Level.GetActiveObject() == *Created);
	REQUIRE(Level.Undo());
	CHECK(Level.GetObjects().empty());
	CHECK(Level.GetSelection().empty());
	CHECK_FALSE(Level.GetActiveObject());
	CHECK_FALSE(Level.IsDirty());
}

TEST_CASE("Empty entities retain absent components through editing history and level files")
{
	Tests::FScratchDirectory Scratch("HertaEmptyEntity");
	FEditorLevel Level;
	const auto Before = Level.GetWorld().SnapshotEntities();
	const auto Created = Level.CreateEmptyEntity(FWorldPosition{2., 3., 4.});
	REQUIRE(Created);
	const auto Handle = *Level.GetWorld().FindEntity(*Created);
	CHECK(FindEditorObject(Level, *Created).Label == "Entity");
	CHECK_FALSE(FindEditorObject(Level, *Created).Mesh.IsValid());
	CHECK_FALSE(Level.GetWorld().GetEntity(Handle)->Mesh);
	CHECK(Level.GetWorld().GetEntity(Handle)->BodyType == ELevelBodyType::None);
	CHECK(Level.GetActiveObject() == *Created);
	REQUIRE(Level.BeginEdit("Edit empty entity"));
	FPreviewObject& Object = FindEditorObject(Level, *Created);
	Object.Label = "Anchor";
	Object.Translation = {9.f, 8.f, 7.f};
	Object.Rotation = FromPreviewEulerXYZ({0.2f, -0.3f, 0.4f});
	Object.Scale = {0.5f, 2.f, 3.f};
	REQUIRE(Level.EndEdit());
	const auto Edited = Level.GetWorld().SnapshotEntities();
	CHECK(Level.GetWorld().FindEntity(*Created) == Handle);
	CHECK_FALSE(Level.GetWorld().GetEntity(Handle)->Mesh);
	REQUIRE(Level.Undo());
	CHECK(FindEditorObject(Level, *Created).Label == "Entity");
	CHECK(FindEditorObject(Level, *Created).Translation.x == 2.f);
	CHECK_FALSE(FindEditorObject(Level, *Created).Mesh.IsValid());
	REQUIRE(Level.Redo());
	CHECK(Level.GetWorld().SnapshotEntities() == Edited);
	const auto Path = Scratch.GetPath() / "Anchor.hlevel";
	REQUIRE(Level.Save(Path));
	FEditorLevel Restored;
	REQUIRE(Restored.Load(Path));
	CHECK(Restored.GetWorld().SnapshotEntities() == Edited);
	CHECK_FALSE(FindEditorObject(Restored, *Created).Mesh.IsValid());
	CHECK_FALSE(Restored.IsDirty());
	REQUIRE(Level.Undo());
	REQUIRE(Level.Undo());
	CHECK(Level.GetWorld().SnapshotEntities() == Before);
	CHECK_FALSE(Level.GetWorld().GetEntity(Handle));
	REQUIRE(Level.Redo());
	REQUIRE(Level.Redo());
	CHECK(Level.GetWorld().SnapshotEntities() == Edited);
	CHECK(Level.GetWorld().FindEntity(*Created) != Handle);
	CHECK_FALSE(Level.IsDirty());
}

TEST_CASE("Empty entity clipboard and duplication preserve optional components and remap identities")
{
	FEditorLevel Source;
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
	FEditorLevel Target;
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
	FEditorLevel Level;
	const FObjectId Cube = Level.GetObjects()[0].Id;
	const auto Empty = Level.CreateEmptyEntity();
	REQUIRE(Empty);
	const auto EmptyHandle = *Level.GetWorld().FindEntity(*Empty);
	const auto CubeHandle = *Level.GetWorld().FindEntity(Cube);
	const std::array Selected{*Empty, Cube};
	Level.SetSelection(Selected, *Empty);
	REQUIRE(Level.Save(Scratch.GetPath() / "BeforeMesh.hlevel"));
	const auto Before = Level.GetWorld().SnapshotEntities();
	REQUIRE(Level.AddStaticMeshToSelected(EngineCubeAsset));
	CHECK(Level.GetWorld().GetEntity(EmptyHandle)->Mesh->Asset == EngineCubeAsset);
	CHECK(Level.GetWorld().GetEntity(CubeHandle)->Mesh->Asset == EngineCubeAsset);
	CHECK(SelectedEditorObjects(Level) == std::vector<FObjectId>(Selected.begin(), Selected.end()));
	CHECK(Level.GetActiveObject() == *Empty);
	const auto Added = Level.GetWorld().SnapshotEntities();
	const auto Generation = Level.GetGeneration();
	REQUIRE(Level.AddStaticMeshToSelected(FAssetId{1, 2}));
	CHECK(Level.GetWorld().SnapshotEntities() == Added);
	CHECK(Level.GetGeneration() == Generation);
	REQUIRE(Level.Undo());
	CHECK(Level.GetWorld().SnapshotEntities() == Before);
	CHECK_FALSE(Level.IsDirty());
	Level.SetSelection(std::array{*Empty}, *Empty);
	REQUIRE(Level.RemoveStaticMeshFromSelected());
	CHECK(Level.CanRedo());
	CHECK_FALSE(Level.IsDirty());
	REQUIRE(Level.Redo());
	CHECK(Level.GetWorld().SnapshotEntities() == Added);
	CHECK(Level.GetActiveObject() == *Empty);
	REQUIRE(Level.RemoveStaticMeshFromSelected());
	CHECK_FALSE(Level.GetWorld().GetEntity(EmptyHandle)->Mesh);
	CHECK_FALSE(Level.GetWorld().GetEntity(CubeHandle)->Mesh);
	CHECK(Level.GetWorld().GetEntity(CubeHandle)->BodyType == ELevelBodyType::Dynamic);
	CHECK(Level.FindBodies(ELevelBodyType::Dynamic).empty());
	REQUIRE(Level.AddStaticMeshToSelected(FAssetId{1, 2}));
	CHECK(Level.GetWorld().GetEntity(EmptyHandle)->Mesh->Asset == FAssetId{1, 2});
	CHECK(Level.GetWorld().GetEntity(CubeHandle)->Mesh->Asset == FAssetId{1, 2});
	CHECK(Level.GetWorld().FindEntity(*Empty) == EmptyHandle);
	CHECK(Level.GetWorld().FindEntity(Cube) == CubeHandle);
	CHECK(Level.GetActiveObject() == *Empty);
	REQUIRE(Level.Undo());
	CHECK_FALSE(Level.GetWorld().GetEntity(EmptyHandle)->Mesh);
	REQUIRE(Level.Undo());
	CHECK(Level.GetWorld().SnapshotEntities() == Added);
}

TEST_CASE("Rigid body authoring is independent from meshes and addition preserves existing motion")
{
	FEditorLevel Level;
	const FObjectId Cube = Level.GetObjects()[0].Id;
	const FObjectId Floor = Level.GetObjects()[1].Id;
	const auto Empty = Level.CreateEmptyEntity();
	REQUIRE(Empty);
	const auto EmptyHandle = *Level.GetWorld().FindEntity(*Empty);
	const auto CubeHandle = *Level.GetWorld().FindEntity(Cube);
	const auto FloorHandle = *Level.GetWorld().FindEntity(Floor);
	const std::array Selected{*Empty, Cube, Floor};
	Level.SetSelection(Selected, *Empty);
	const auto Before = Level.GetWorld().SnapshotEntities();
	REQUIRE(Level.AddRigidBodyToSelected(ELevelBodyType::Static));
	CHECK(Level.GetWorld().GetEntity(EmptyHandle)->BodyType == ELevelBodyType::Static);
	CHECK_FALSE(Level.GetWorld().GetEntity(EmptyHandle)->Mesh);
	CHECK(Level.GetWorld().GetEntity(CubeHandle)->BodyType == ELevelBodyType::Dynamic);
	CHECK(Level.GetWorld().GetEntity(FloorHandle)->BodyType == ELevelBodyType::Static);
	const auto Added = Level.GetWorld().SnapshotEntities();
	const auto Generation = Level.GetGeneration();
	REQUIRE(Level.AddRigidBodyToSelected());
	CHECK(Level.GetWorld().SnapshotEntities() == Added);
	CHECK(Level.GetGeneration() == Generation);
	REQUIRE(Level.SetSelectedBodyType(ELevelBodyType::Dynamic));
	CHECK(Level.GetWorld().GetEntity(EmptyHandle)->BodyType == ELevelBodyType::Dynamic);
	CHECK(Level.GetWorld().GetEntity(FloorHandle)->BodyType == ELevelBodyType::Dynamic);
	CHECK(Level.FindBodies(ELevelBodyType::Static).empty());
	REQUIRE(Level.SetSelectedBodyType(ELevelBodyType::None));
	CHECK(Level.GetWorld().GetEntity(EmptyHandle)->BodyType == ELevelBodyType::None);
	CHECK(Level.GetWorld().GetEntity(CubeHandle)->BodyType == ELevelBodyType::None);
	CHECK(Level.GetWorld().GetEntity(FloorHandle)->BodyType == ELevelBodyType::None);
	REQUIRE(Level.Undo());
	REQUIRE(Level.Undo());
	CHECK(Level.GetWorld().SnapshotEntities() == Added);
	REQUIRE(Level.Undo());
	CHECK(Level.GetWorld().SnapshotEntities() == Before);
	CHECK(Level.GetWorld().FindEntity(*Empty) == EmptyHandle);
	CHECK(Level.GetWorld().FindEntity(Cube) == CubeHandle);
	CHECK(Level.GetWorld().FindEntity(Floor) == FloorHandle);
	CHECK(Level.GetActiveObject() == *Empty);
	CHECK(SelectedEditorObjects(Level) == std::vector<FObjectId>(Selected.begin(), Selected.end()));
}

TEST_CASE("Rigid body collision shapes change only bodies and undo as one edit")
{
	FEditorLevel Level;
	const FObjectId Cube = Level.GetObjects()[0].Id;
	const FObjectId Floor = Level.GetObjects()[1].Id;
	const auto Empty = Level.CreateEmptyEntity();
	REQUIRE(Empty);
	Level.SetSelection(std::array{Cube, Floor, *Empty}, Cube);
	const auto Before = Level.GetWorld().SnapshotEntities();
	REQUIRE(Level.SetSelectedCollisionShape(ELevelCollisionShape::Capsule));
	CHECK(Level.GetUndoLabel() == "Change collision shape");
	CHECK(Level.GetWorld().GetEntity(*Level.GetWorld().FindEntity(Cube))->BodySettings.Collision == ELevelCollisionShape::Capsule);
	CHECK(Level.GetWorld().GetEntity(*Level.GetWorld().FindEntity(Floor))->BodySettings.Collision == ELevelCollisionShape::Capsule);
	CHECK(Level.GetWorld().GetEntity(*Level.GetWorld().FindEntity(*Empty))->BodySettings == FLevelRigidBodySettings{});
	CHECK_FALSE(Level.SetSelectedCollisionShape(static_cast<ELevelCollisionShape>(9)));
	REQUIRE(Level.Undo());
	CHECK(Level.GetWorld().SnapshotEntities() == Before);
}

TEST_CASE("Rigid body property gestures preserve mixed values and undo as one edit")
{
	FEditorLevel Level;
	const FObjectId Cube = Level.GetObjects()[0].Id;
	const FObjectId Floor = Level.GetObjects()[1].Id;
	const auto Empty = Level.CreateEmptyEntity();
	REQUIRE(Empty);
	Level.SetSelection(std::array{Cube}, Cube);
	REQUIRE(Level.SetSelectedBodyProperty(&FLevelRigidBodySettings::MassKg, 12.f));
	Level.SetSelection(std::array{Floor}, Floor);
	REQUIRE(Level.SetSelectedBodyProperty(&FLevelRigidBodySettings::MassKg, 30.f));
	Level.SetSelection(std::array{Cube, Floor, *Empty}, Cube);
	const auto Before = Level.GetWorld().SnapshotEntities();
	REQUIRE(Level.BeginEdit("Edit friction"));
	REQUIRE(Level.SetSelectedBodyProperty(&FLevelRigidBodySettings::Friction, 0.4f));
	REQUIRE(Level.SetSelectedBodyProperty(&FLevelRigidBodySettings::Friction, 0.8f));
	REQUIRE(Level.EndEdit());
	const auto After = Level.GetWorld().SnapshotEntities();
	CHECK(Level.GetWorld().GetEntity(*Level.GetWorld().FindEntity(Cube))->BodySettings.MassKg == 12.f);
	CHECK(Level.GetWorld().GetEntity(*Level.GetWorld().FindEntity(Floor))->BodySettings.MassKg == 30.f);
	CHECK(Level.GetWorld().GetEntity(*Level.GetWorld().FindEntity(*Empty))->BodySettings == FLevelRigidBodySettings{});
	CHECK(Level.GetUndoLabel() == "Edit friction");
	REQUIRE(Level.Undo());
	CHECK(Level.GetWorld().SnapshotEntities() == Before);
	REQUIRE(Level.Redo());
	CHECK(Level.GetWorld().SnapshotEntities() == After);
	REQUIRE(Level.BeginEdit("Cancel damping"));
	REQUIRE(Level.SetSelectedBodyProperty(&FLevelRigidBodySettings::AngularDamping, 0.5f));
	REQUIRE(Level.CancelEdit());
	CHECK(Level.GetWorld().SnapshotEntities() == After);
	REQUIRE(Level.SetSelectedBodyType(ELevelBodyType::None));
	CHECK(Level.GetWorld().GetEntity(*Level.GetWorld().FindEntity(Cube))->BodySettings == FLevelRigidBodySettings{});
	REQUIRE(Level.Undo());
	CHECK(Level.GetWorld().SnapshotEntities() == After);
}

TEST_CASE("Rigid body property admission rejects invalid edits atomically")
{
	FEditorLevel Level;
	Level.SetSelection(std::array{Level.GetObjects()[0].Id, Level.GetObjects()[1].Id});
	const auto Before = Level.GetWorld().SnapshotEntities();
	CHECK_FALSE(Level.SetSelectedBodyProperty(nullptr, 1.f));
	CHECK_FALSE(Level.SetSelectedBodyProperty(&FLevelRigidBodySettings::MassKg, 0.f));
	CHECK_FALSE(Level.SetSelectedBodyProperty(&FLevelRigidBodySettings::Friction, 2.f));
	CHECK_FALSE(Level.SetSelectedBodyProperty(&FLevelRigidBodySettings::GravityScale, std::numeric_limits<float>::quiet_NaN()));
	CHECK(Level.GetWorld().SnapshotEntities() == Before);
	CHECK_FALSE(Level.CanUndo());
	Level.SetSimulationRunning(true);
	CHECK_FALSE(Level.SetSelectedBodyProperty(&FLevelRigidBodySettings::MassKg, 2.f));
	CHECK(Level.GetWorld().SnapshotEntities() == Before);
}

TEST_CASE("Authored rigid body properties survive level and clipboard round trips")
{
	Tests::FScratchDirectory Scratch("HertaBodyProperties");
	FEditorLevel Level;
	const FObjectId Cube = Level.GetObjects()[0].Id;
	Level.SetSelection(std::array{Cube}, Cube);
	REQUIRE(Level.SetSelectedBodyProperty(&FLevelRigidBodySettings::MassKg, 25.f));
	REQUIRE(Level.SetSelectedBodyProperty(&FLevelRigidBodySettings::GravityScale, 0.5f));
	REQUIRE(Level.SetSelectedBodyProperty(&FLevelRigidBodySettings::Restitution, 0.75f));
	const auto Expected = Level.GetWorld().GetEntity(*Level.GetWorld().FindEntity(Cube))->BodySettings;
	const auto Clipboard = Level.CopySelected();
	REQUIRE(Clipboard);
	REQUIRE(Level.PasteEntities(*Clipboard));
	CHECK(Level.GetWorld().GetEntity(*Level.GetWorld().FindEntity(*Level.GetActiveObject()))->BodySettings == Expected);
	REQUIRE(Level.DuplicateSelected());
	CHECK(Level.GetWorld().GetEntity(*Level.GetWorld().FindEntity(*Level.GetActiveObject()))->BodySettings == Expected);
	const auto Path = Scratch.GetPath() / "Bodies.hlevel";
	REQUIRE(Level.Save(Path));
	FEditorLevel Loaded;
	REQUIRE(Loaded.Load(Path));
	CHECK(Loaded.GetWorld().SnapshotEntities() == Level.GetWorld().SnapshotEntities());
}

TEST_CASE("Mixed rigid body edits never insert missing components or consume no-op redo branches")
{
	FEditorLevel Level;
	const FObjectId Cube = Level.GetObjects()[0].Id;
	const FObjectId Floor = Level.GetObjects()[1].Id;
	const auto Empty = Level.CreateEmptyEntity();
	REQUIRE(Empty);
	const auto EmptyHandle = *Level.GetWorld().FindEntity(*Empty);
	const auto CubeHandle = *Level.GetWorld().FindEntity(Cube);
	const auto FloorHandle = *Level.GetWorld().FindEntity(Floor);
	const std::array Selected{*Empty, Cube, Floor};
	Level.SetSelection(Selected, *Empty);
	const auto Before = Level.GetWorld().SnapshotEntities();
	REQUIRE(Level.SetSelectedBodyType(ELevelBodyType::Static));
	CHECK(Level.GetWorld().GetEntity(EmptyHandle)->BodyType == ELevelBodyType::None);
	CHECK(Level.GetWorld().GetEntity(CubeHandle)->BodyType == ELevelBodyType::Static);
	CHECK(Level.GetWorld().GetEntity(FloorHandle)->BodyType == ELevelBodyType::Static);
	CHECK(Level.GetActiveObject() == *Empty);
	REQUIRE(Level.Undo());
	CHECK(Level.GetWorld().SnapshotEntities() == Before);
	Level.SetSelection(std::array{*Empty}, *Empty);
	const auto Generation = Level.GetGeneration();
	REQUIRE(Level.SetSelectedBodyType(ELevelBodyType::Dynamic));
	REQUIRE(Level.SetSelectedBodyType(ELevelBodyType::None));
	CHECK(Level.GetWorld().SnapshotEntities() == Before);
	CHECK(Level.GetGeneration() == Generation);
	CHECK(Level.CanRedo());
	Level.SetSelection(std::array{Floor}, Floor);
	REQUIRE(Level.AddRigidBodyToSelected());
	REQUIRE(Level.SetSelectedBodyType(ELevelBodyType::Static));
	CHECK(Level.CanRedo());
	REQUIRE(Level.Redo());
	CHECK(Level.GetWorld().GetEntity(EmptyHandle)->BodyType == ELevelBodyType::None);
	CHECK(Level.GetWorld().GetEntity(CubeHandle)->BodyType == ELevelBodyType::Static);
	CHECK(Level.GetActiveObject() == *Empty);
	CHECK(Level.GetWorld().FindEntity(*Empty) == EmptyHandle);
	CHECK(Level.GetWorld().FindEntity(Cube) == CubeHandle);
	CHECK(Level.GetWorld().FindEntity(Floor) == FloorHandle);
}

TEST_CASE("Meshless rigid bodies load and save but cannot be selected for preview simulation")
{
	Tests::FScratchDirectory Scratch("HertaMeshlessBodies");
	const std::array Entities{
	    FLevelEntity{.Id = FObjectId{1, 1}, .Name = "Empty", .BodyType = ELevelBodyType::Dynamic},
	    FLevelEntity{.Id = FObjectId{2, 2}, .Name = "Floor", .Mesh = FStaticMeshComponent{EngineCubeAsset}, .BodyType = ELevelBodyType::Static},
	    FLevelEntity{.Id = FObjectId{3, 3}, .Name = "Cube", .Mesh = FStaticMeshComponent{EngineCubeAsset}, .BodyType = ELevelBodyType::Dynamic},
	};

	const auto Path = Scratch.GetPath() / "Bodies.hlevel";
	REQUIRE(SaveLevel(Path, {.Id = FObjectId::Generate(), .Name = "Bodies", .Entities = {Entities.begin(), Entities.end()}}));
	FEditorLevel Level;
	REQUIRE(Level.Load(Path));
	CHECK(Level.FindBodies(ELevelBodyType::Static) == std::vector<std::size_t>{1});
	CHECK(Level.FindBodies(ELevelBodyType::Dynamic) == std::vector<std::size_t>{2});
	CHECK_FALSE(Level.GetObjects()[0].Mesh.IsValid());
	REQUIRE(Level.CommitEdits());
	CHECK_FALSE(Level.IsDirty());
	REQUIRE(Level.Save(Path));
	const auto Saved = LoadLevel(Path);
	REQUIRE(Saved);
	CHECK_FALSE(Saved->Entities[0].Mesh);
	CHECK(Saved->Entities[0].BodyType == ELevelBodyType::Dynamic);
	REQUIRE(Level.AddStaticMeshToSelected(EngineCubeAsset));
	CHECK(Level.FindBodies(ELevelBodyType::Dynamic) == std::vector<std::size_t>{0, 2});
	REQUIRE(Level.RemoveStaticMeshFromSelected());
	CHECK(Level.FindBodies(ELevelBodyType::Dynamic) == std::vector<std::size_t>{2});
}

TEST_CASE("Component authoring failures no-op selections and history admission preserve authored state")
{
	FEditorLevel Level;
	const auto Before = Level.GetWorld().SnapshotEntities();
	const auto Selection = SelectedEditorObjects(Level);
	const auto Active = Level.GetActiveObject();
	const auto Generation = Level.GetGeneration();
	CHECK_FALSE(Level.AddStaticMeshToSelected({}));
	CHECK_FALSE(Level.AddRigidBodyToSelected(ELevelBodyType::None));
	CHECK_FALSE(Level.AddRigidBodyToSelected(static_cast<ELevelBodyType>(255)));
	CHECK_FALSE(Level.SetSelectedBodyType(static_cast<ELevelBodyType>(255)));
	CHECK_FALSE(Level.CreateEmptyEntity(FWorldPosition{std::numeric_limits<double>::infinity(), 0., 0.}));
	CHECK(Level.GetWorld().SnapshotEntities() == Before);
	CHECK(SelectedEditorObjects(Level) == Selection);
	CHECK(Level.GetActiveObject() == Active);
	CHECK(Level.GetGeneration() == Generation);
	CHECK_FALSE(Level.IsDirty());
	Level.SetSelection({});
	REQUIRE(Level.AddStaticMeshToSelected(EngineCubeAsset));
	REQUIRE(Level.RemoveStaticMeshFromSelected());
	REQUIRE(Level.AddRigidBodyToSelected());
	REQUIRE(Level.SetSelectedBodyType(ELevelBodyType::None));
	CHECK_FALSE(Level.CanUndo());
	CHECK_FALSE(Level.IsDirty());

	for (const bool bSimulation : {false, true})
	{
		CAPTURE(bSimulation);
		FEditorLevel Blocked;
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
		CHECK_FALSE(Blocked.SetSelectedBodyType(ELevelBodyType::None));
		CHECK(Blocked.GetWorld().SnapshotEntities() == Before);
		CHECK_FALSE(Blocked.IsDirty());
	}

	FEditorLevel Limited(0);
	const auto Handle = *Limited.GetWorld().FindEntity(Before.front().Id);
	const auto LimitedGeneration = Limited.GetGeneration();
	CHECK_FALSE(Limited.RemoveStaticMeshFromSelected());
	CHECK_FALSE(Limited.SetSelectedBodyType(ELevelBodyType::None));
	CHECK_FALSE(Limited.CreateEmptyEntity());
	CHECK(Limited.GetWorld().SnapshotEntities() == Before);
	CHECK(Limited.GetWorld().FindEntity(Before.front().Id) == Handle);
	CHECK(Limited.GetGeneration() == LimitedGeneration);
	CHECK_FALSE(Limited.CanUndo());
	CHECK_FALSE(Limited.IsDirty());
}

TEST_CASE("Simulation blocks authoring and history while saving only authored poses")
{
	Tests::FScratchDirectory Scratch("HertaLevelBlockedAuthoring");
	const auto Path = Scratch.GetPath() / "Authored.hlevel";
	FEditorLevel Level;
	Level.GetObjects()[0].Translation.y = 8.f;
	REQUIRE(Level.CommitEdits());
	const auto Before = Level.GetWorld().SnapshotEntities();
	const auto Clipboard = Level.CopySelected();
	REQUIRE(Clipboard);
	Level.SetSimulationRunning(true);
	Level.GetObjects()[0].Translation.y = 0.5f;
	CHECK_FALSE(Level.CommitEdits());
	CHECK_FALSE(Level.BeginEdit("Blocked"));
	CHECK_FALSE(Level.Undo());
	CHECK_FALSE(Level.Redo());
	CHECK_FALSE(Level.CreateEntity());
	CHECK_FALSE(Level.DuplicateSelected());
	CHECK_FALSE(Level.DeleteSelected());
	CHECK_FALSE(Level.PasteEntities(*Clipboard));
	CHECK_FALSE(Level.CopySelected());
	CHECK_FALSE(Level.CanUndo());
	CHECK(Level.GetWorld().SnapshotEntities() == Before);
	REQUIRE(Level.Save(Path));
	CHECK_FALSE(Level.IsDirty());
	const auto Saved = LoadLevel(Path);
	REQUIRE(Saved);
	CHECK(Saved->Entities == Before);
}

TEST_CASE("Level commands validate files and retain their editor level safely")
{
	Tests::FScratchDirectory Scratch("HertaLevelCommands");
	FEditorCommandRegistry Commands;
	auto Level = std::make_shared<FEditorLevel>();
	const auto Path = Scratch.GetPath() / "Commands.hlevel";
	Level->SetPath(Path);
	REQUIRE(RegisterEditorLevelCommands(Commands, Level));
	REQUIRE(RegisterLevelFileCommands(Commands));
	Level.reset();
	CHECK(Commands.Execute("level.save"));
	const auto Text = Path.generic_string();
	CHECK(Commands.Execute("level.validate \"" + Text + "\""));
	CHECK(Commands.Execute("level.load \"" + Text + "\""));
	CHECK_FALSE(Commands.Execute("scene.save"));
	CHECK_FALSE(Commands.Execute("scene.load \"" + Text + "\""));
	CHECK_FALSE(Commands.Execute("level.save a b"));
	CHECK_FALSE(Commands.Execute("level.load"));
	CHECK_FALSE(Commands.Execute("level.validate"));
	CHECK_FALSE(Commands.Execute("level.canonicalize"));
}
}
