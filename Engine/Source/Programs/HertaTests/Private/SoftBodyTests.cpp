#include "PreviewSimulation.h"
#include "SoftBodyGeometry.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <vector>

namespace Herta
{
namespace
{
[[nodiscard]] FVector3 ToVector(const std::array<float, 3>& Value)
{
	return {Value[0], Value[1], Value[2]};
}

// Every rendered triangle must face the same way as the normals used to shade it.
void CheckWinding(const FCookedModel& Model)
{
	REQUIRE(Model.Indices.size() % 3 == 0);
	for (std::size_t Index = 0; Index < Model.Indices.size(); Index += 3)
	{
		const auto& A = Model.Vertices[Model.Indices[Index]];
		const auto& B = Model.Vertices[Model.Indices[Index + 1]];
		const auto& C = Model.Vertices[Model.Indices[Index + 2]];
		const FVector3 Face = (ToVector(B.Position) - ToVector(A.Position)).Cross(ToVector(C.Position) - ToVector(A.Position));
		CHECK(Face.Dot(ToVector(A.Normal) + ToVector(B.Normal) + ToVector(C.Normal)) >= 0.f);
	}
}
}

TEST_CASE("Soft body ropes hang from a pinned origin at 10 cm spacing")
{
	const FSoftBodyComponent Rope{.Shape = ESoftBodyShape::Rope, .Length = 3.f};
	const FSoftBodyTopology Topology = BuildSoftBodyTopology(Rope);
	REQUIRE(Topology.Vertices.size() == 31);
	CHECK(Topology.StretchEdges.size() == 30);
	CHECK(Topology.BendEdges.size() == 29);
	CHECK(Topology.Faces.empty());
	CHECK(Topology.Pinned == std::vector<std::uint32_t>{0});
	CHECK(Topology.Vertices.back().Y == doctest::Approx(-3.f));
	CHECK(BuildSoftBodyTopology({.Shape = ESoftBodyShape::Rope, .bPinned = false}).Pinned.empty());

	const FCookedModel Model = BuildSoftBodyModel(Rope, Topology, Topology.Vertices);
	CHECK(Model.Vertices.size() == 31 * 9);
	CHECK(Model.Indices.size() == 30 * 8 * 6);
	REQUIRE(Model.Sections.size() == 1);
	CHECK(Model.Sections.front().IndexCount == Model.Indices.size());
	CheckWinding(Model);
}

TEST_CASE("Soft body cloth is a pinned, double-sided grid and balls are outward shells")
{
	const FSoftBodyComponent Cloth{.Shape = ESoftBodyShape::Cloth, .Length = 2.f, .Height = 1.5f};
	const FSoftBodyTopology Sheet = BuildSoftBodyTopology(Cloth);
	CHECK(Sheet.Vertices.size() == 21 * 16);
	CHECK(Sheet.Faces.size() == 2 * 20 * 15);
	CHECK(Sheet.Pinned == std::vector<std::uint32_t>{0, 20});
	for (const auto& [A, B, C] : Sheet.Faces)
	{
		CHECK((Sheet.Vertices[B] - Sheet.Vertices[A]).Cross(Sheet.Vertices[C] - Sheet.Vertices[A]).Z > 0.f);
	}

	const FCookedModel SheetModel = BuildSoftBodyModel(Cloth, Sheet, Sheet.Vertices);
	CHECK(SheetModel.Indices.size() == Sheet.Faces.size() * 6);
	CheckWinding(SheetModel);

	const FSoftBodyComponent Ball{.Shape = ESoftBodyShape::Ball, .Length = 1.f};
	const FSoftBodyTopology Shell = BuildSoftBodyTopology(Ball);
	CHECK(Shell.Vertices.size() == 162);
	CHECK(Shell.Faces.size() == 320);
	CHECK(Shell.Pinned.empty());
	for (const auto& [A, B, C] : Shell.Faces)
	{
		CHECK((Shell.Vertices[B] - Shell.Vertices[A]).Cross(Shell.Vertices[C] - Shell.Vertices[A]).Dot(Shell.Vertices[A]) > 0.f);
	}

	CheckWinding(BuildSoftBodyModel(Ball, Shell, Shell.Vertices));
}

TEST_CASE("Preview simulation steps soft bodies and reports their world vertices")
{
	const FSoftBodyComponent Rope{.Shape = ESoftBodyShape::Rope, .Length = 1.f, .MassKg = 0.2f};
	const FSoftBodyTopology Topology = BuildSoftBodyTopology(Rope);
	// Lay the rope horizontally so gravity swings it down from its pin.
	std::vector<FVector3> World;
	for (const FVector3& Vertex : Topology.Vertices)
	{
		World.push_back({-Vertex.Y, 2.f, 0.f});
	}

	const std::array SoftBodies{FPreviewSimulationSoftBody{.ObjectIndex = 4, .Settings = MakeSoftBodyPhysicsSettings(Rope, Topology, World)}};
	FPreviewSimulation Simulation;
	REQUIRE(Simulation.Start({}, SoftBodies));
	REQUIRE(Simulation.GetSoftBodyPositions(4).size() == World.size());
	CHECK(Simulation.GetSoftBodyPositions(3).empty());
	const std::uint64_t Revision = Simulation.GetSoftBodyRevision();
	for (int Frame = 0; Frame < 30; ++Frame)
	{
		REQUIRE(Simulation.Update(1.f / 60.f));
	}

	CHECK(Simulation.GetSoftBodyRevision() > Revision);
	const auto Positions = Simulation.GetSoftBodyPositions(4);
	CHECK(Positions.front().Y == doctest::Approx(2.f).epsilon(1e-3));
	CHECK(Positions.back().Y < 1.9f);
	Simulation.Stop();
	CHECK(Simulation.GetSoftBodyPositions(4).empty());
}
}
