#include "EditorScene.h"
#include "Herta/EditorCore/SceneCommands.h"
#include "TestFiles.h"

#include <doctest/doctest.h>

#include <array>
#include <limits>
#include <numbers>

namespace Herta
{
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
	Extra.BodyMotion = ESceneBodyMotion::None;
	Document.Entities.push_back(Extra);
	const auto Path = Scratch.GetPath() / "Counts.hscene";
	REQUIRE(SaveScene(Path, Document));
	REQUIRE(Scene.Load(Path));
	CHECK(Scene.GetObjects().size() == 3);
	CHECK(Scene.FindBody(ESceneBodyMotion::Static) == 0);
	CHECK(Scene.FindBody(ESceneBodyMotion::Dynamic) == 2);
	Document.Entities.clear();
	REQUIRE(SaveScene(Path, Document));
	REQUIRE(Scene.Load(Path));
	CHECK(Scene.GetObjects().empty());
	CHECK_FALSE(Scene.FindBody(ESceneBodyMotion::Dynamic));
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
