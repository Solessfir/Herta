#include "EditorLevel.h"
#include "Herta/EditorFramework/ScalingStatistics.h"
#include "Herta/Level/ScalingLevel.h"
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
		const auto Path = Scratch.GetPath() / "Scaling.hlevel";
		const auto Fixture = GenerateScalingLevel({.CubeCount = Count, .Workload = EScalingLevelWorkload::Rendering});
		REQUIRE(Fixture);
		REQUIRE(SaveLevel(Path, *Fixture));
		FEditorLevel Level;
		REQUIRE(Level.Load(Path));
		const auto Before = Level.GetWorld().SnapshotEntities();
		const auto OriginalHandle = Level.GetWorld().FindEntity(Level.GetObjects()[0].Id);
		std::vector<FObjectId> Selected;

		for (const auto& Object : Level.GetObjects())
		{
			Selected.push_back(Object.Id);
		}

		const auto Start = std::chrono::steady_clock::now();
		auto RequestedSelection = Selected;
		RequestedSelection.push_back(Selected.front());
		RequestedSelection.push_back(FObjectId{1, 1});
		Level.SetSelection(RequestedSelection, Selected.front());
		const double SelectionMilliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - Start).count();
		CHECK(Level.GetSelection().size() == Count + 1);
		CHECK(Level.GetActiveObject() == Selected.front());
		REQUIRE(Level.BeginEdit("Scaling transform"));

		for (auto& Object : Level.GetObjects())
		{
			Object.Translation.x += 1.f;
		}

		REQUIRE(Level.EndEdit());
		const auto Transformed = Level.GetWorld().SnapshotEntities();
		CHECK(Transformed != Before);
		REQUIRE(Level.Undo());
		CHECK(Level.GetWorld().SnapshotEntities() == Before);
		REQUIRE(Level.Redo());
		CHECK(Level.GetWorld().SnapshotEntities() == Transformed);
		REQUIRE(Level.RemoveStaticMeshFromSelected());
		CHECK_FALSE(Level.GetObjects()[0].Mesh.IsValid());
		REQUIRE(Level.Undo());
		CHECK(Level.GetWorld().SnapshotEntities() == Transformed);
		REQUIRE(Level.AddRigidBodyToSelected());
		REQUIRE(Level.SetSelectedBodyProperty(&FLevelRigidBodySettings::MassKg, 2.f));
		REQUIRE(Level.SetSelectedBodyType(ELevelBodyType::None));
		REQUIRE(Level.Undo());

		for (const auto& Entity : Level.GetWorld().SnapshotEntities())
		{
			CHECK(Entity.BodyType != ELevelBodyType::None);
		}

		REQUIRE(Level.Save());
		const auto Saved = LoadLevel(Path);
		REQUIRE(Saved);
		CHECK(Saved->Entities == Level.GetWorld().SnapshotEntities());
		const auto Copy = Level.CopySelected();
		REQUIRE(Copy);
		const auto ParsedCopy = ParseLevel(*Copy);
		REQUIRE(ParsedCopy);
		CHECK(ParsedCopy->Entities.size() == Count + 1);
		REQUIRE(Level.DuplicateSelected(false, {1., 0., 0.}));
		CHECK(Level.GetObjects().size() == (Count + 1) * 2);
		CHECK(Level.GetSelection().size() == Count + 1);
		REQUIRE(Level.Undo());
		CHECK(Level.GetObjects().size() == Count + 1);
		CHECK(Level.GetSelection().size() == Selected.size());
		REQUIRE(Level.DeleteSelected());
		CHECK(Level.GetObjects().empty());
		REQUIRE(Level.Undo());
		CHECK(Level.GetObjects().size() == Count + 1);
		CHECK(Level.GetSelection().size() == Selected.size());
		CHECK_FALSE(Level.GetWorld().GetEntity(*OriginalHandle));
		const double Milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - Start).count();
		std::vector<double> SnapshotSamples;
		for (std::size_t Index = 0; Index < 68; ++Index)
		{
			const auto SnapshotStart = std::chrono::steady_clock::now();
			const auto Snapshot = Level.GetWorld().SnapshotEntities();
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
