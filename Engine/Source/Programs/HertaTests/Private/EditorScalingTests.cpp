#include "EditorScene.h"
#include "Herta/EditorFramework/ScalingStatistics.h"
#include "Herta/Scene/ScalingScene.h"
#include "TestFiles.h"

#include <doctest/doctest.h>

#include <array>
#include <chrono>
#include <print>

namespace Herta
{
TEST_CASE("Scaling capture summarizes nearest-rank tails and a conventional median")
{
	CHECK(SummarizeScalingSamples({}).Median == 0.);
	const std::array Values{5., 1., 2., 4., 3., 100.};
	const auto Result = SummarizeScalingSamples(Values);
	CHECK(Result.Median == 3.5);
	CHECK(Result.P95 == 100.);
	CHECK(Result.P99 == 100.);
	const std::array Single{0.5};
	CHECK(SummarizeScalingSamples(Single).P99 == 0.5);
}

TEST_CASE("Scaling editor authoring preserves batch history selection and canonical files")
{
	for (const std::size_t Count : std::array<std::size_t, 3>{1000, 5000, 10000})
	{
		CAPTURE(Count);
		Tests::FScratchDirectory Scratch("HertaScalingAuthoring");
		const auto Path = Scratch.GetPath() / "Scaling.hscene";
		const auto Fixture = GenerateScalingScene({.CubeCount = Count, .Workload = EScalingSceneWorkload::Rendering});
		REQUIRE(Fixture);
		REQUIRE(SaveScene(Path, *Fixture));
		FEditorScene Scene;
		REQUIRE(Scene.Load(Path));
		const auto Before = Scene.GetWorld().SnapshotEntities();
		const auto OriginalHandle = Scene.GetWorld().FindEntity(Scene.GetObjects()[0].Id);
		std::vector<FObjectId> Selected;

		for (const auto& Object : Scene.GetObjects())
		{
			Selected.push_back(Object.Id);
		}

		const auto Start = std::chrono::steady_clock::now();
		auto RequestedSelection = Selected;
		RequestedSelection.push_back(Selected.front());
		RequestedSelection.push_back(FObjectId{1, 1});
		Scene.SetSelection(RequestedSelection, Selected.front());
		const double SelectionMilliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - Start).count();
		CHECK(Scene.GetSelection().size() == Count + 1);
		CHECK(Scene.GetActiveObject() == Selected.front());
		REQUIRE(Scene.BeginEdit("Scaling transform"));

		for (auto& Object : Scene.GetObjects())
		{
			Object.Translation.x += 1.f;
		}

		REQUIRE(Scene.EndEdit());
		const auto Transformed = Scene.GetWorld().SnapshotEntities();
		CHECK(Transformed != Before);
		REQUIRE(Scene.Undo());
		CHECK(Scene.GetWorld().SnapshotEntities() == Before);
		REQUIRE(Scene.Redo());
		CHECK(Scene.GetWorld().SnapshotEntities() == Transformed);
		REQUIRE(Scene.RemoveStaticMeshFromSelected());
		CHECK_FALSE(Scene.GetObjects()[0].Mesh.IsValid());
		REQUIRE(Scene.Undo());
		CHECK(Scene.GetWorld().SnapshotEntities() == Transformed);
		REQUIRE(Scene.AddRigidBodyToSelected());
		REQUIRE(Scene.SetSelectedBodyProperty(&FSceneRigidBodySettings::MassKg, 2.f));
		REQUIRE(Scene.SetSelectedBodyType(ESceneBodyType::None));
		REQUIRE(Scene.Undo());

		for (const auto& Entity : Scene.GetWorld().SnapshotEntities())
		{
			CHECK(Entity.BodyType != ESceneBodyType::None);
		}

		REQUIRE(Scene.Save());
		const auto Saved = LoadScene(Path);
		REQUIRE(Saved);
		CHECK(Saved->Entities == Scene.GetWorld().SnapshotEntities());
		const auto Copy = Scene.CopySelected();
		REQUIRE(Copy);
		const auto ParsedCopy = ParseScene(*Copy);
		REQUIRE(ParsedCopy);
		CHECK(ParsedCopy->Entities.size() == Count + 1);
		REQUIRE(Scene.DuplicateSelected(false, {1., 0., 0.}));
		CHECK(Scene.GetObjects().size() == (Count + 1) * 2);
		CHECK(Scene.GetSelection().size() == Count + 1);
		REQUIRE(Scene.Undo());
		CHECK(Scene.GetObjects().size() == Count + 1);
		CHECK(Scene.GetSelection().size() == Selected.size());
		REQUIRE(Scene.DeleteSelected());
		CHECK(Scene.GetObjects().empty());
		REQUIRE(Scene.Undo());
		CHECK(Scene.GetObjects().size() == Count + 1);
		CHECK(Scene.GetSelection().size() == Selected.size());
		CHECK_FALSE(Scene.GetWorld().GetEntity(*OriginalHandle));
		const double Milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - Start).count();
		std::vector<double> SnapshotSamples;
		for (std::size_t Index = 0; Index < 68; ++Index)
		{
			const auto SnapshotStart = std::chrono::steady_clock::now();
			const auto Snapshot = Scene.GetWorld().SnapshotEntities();
			const double Elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - SnapshotStart).count();
			CHECK(Snapshot.size() == Count + 1);
			if (Index >= 8)
			{
				SnapshotSamples.push_back(Elapsed);
			}
		}

		const auto Snapshot = SummarizeScalingSamples(SnapshotSamples);
		std::println("Scaling authoring count={} selection_ms={:.3f} lifecycle_ms={:.3f} ecs_snapshot_median_ms={:.3f} ecs_snapshot_p95_ms={:.3f} ecs_snapshot_p99_ms={:.3f}", Count, SelectionMilliseconds, Milliseconds, Snapshot.Median, Snapshot.P95, Snapshot.P99);
	}
}
}
