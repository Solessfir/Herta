#include "Herta/Animation/MeshSlice.h"
#include "Herta/AssetPipeline/AssetCooker.h"
#include "Herta/Math/Quaternion.h"
#include "TestFiles.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <format>
#include <map>
#include <numbers>

namespace Herta
{
namespace
{
constexpr float Pi = std::numbers::pi_v<float>;

// A closed surface from a parametric grid, with a duplicated seam column and, for a sphere, collapsed poles, as exporters write UV seams.
FSkinnedMeshPiece MakeGrid(const std::size_t Columns, const std::size_t Rows, const auto& Point, const bool bTorus)
{
	FSkinnedMeshPiece Mesh;
	for (std::size_t Row = 0; Row <= Rows; ++Row)
	{
		for (std::size_t Column = 0; Column <= Columns; ++Column)
		{
			const float U = static_cast<float>(Column) / static_cast<float>(Columns);
			const float V = static_cast<float>(Row) / static_cast<float>(Rows);
			const FVector3 Position = Point(U == 1.f ? 0.f : U, bTorus && V == 1.f ? 0.f : V);
			// Joint 1 weighs more towards the top, so cut points blend two joints.
			const auto Upper = static_cast<std::uint16_t>(std::clamp(Position.Y * 0.5f + 0.5f, 0.f, 1.f) * CookedSkinWeightTotal);
			Mesh.Vertices.push_back({.Position = {Position.X, Position.Y, Position.Z}, .UV = {U, V}, .Normal = {0.f, 1.f, 0.f}});
			Mesh.Influences.push_back({.Joints = {0, 1, 0, 0}, .Weights = {static_cast<std::uint16_t>(CookedSkinWeightTotal - Upper), Upper, 0, 0}});
		}
	}

	for (std::size_t Row = 0; Row < Rows; ++Row)
	{
		for (std::size_t Column = 0; Column < Columns; ++Column)
		{
			const auto At = [&](const std::size_t R, const std::size_t C)
			{
				return static_cast<std::uint32_t>(R * (Columns + 1) + C);
			};

			Mesh.Indices.insert(Mesh.Indices.end(), {At(Row, Column), At(Row + 1, Column), At(Row + 1, Column + 1), At(Row, Column), At(Row + 1, Column + 1), At(Row, Column + 1)});
		}
	}

	Mesh.Sections.push_back({.FirstIndex = 0, .IndexCount = static_cast<std::uint32_t>(Mesh.Indices.size()), .Material = 0});
	return Mesh;
}

FSkinnedMeshPiece MakeSphere()
{
	return MakeGrid(24, 16, [](const float U, const float V)
	{
		const float Theta = U * 2.f * Pi;
		const float Phi = V * Pi;
		return FVector3{std::sin(Phi) * std::cos(Theta), -std::cos(Phi), std::sin(Phi) * std::sin(Theta)};
	}, false);
}

FSkinnedMeshPiece MakeTorus()
{
	return MakeGrid(32, 16, [](const float U, const float V)
	{
		const float Theta = U * 2.f * Pi;
		const float Phi = V * 2.f * Pi;
		const float Ring = 1.f + 0.4f * std::cos(Phi);
		return FVector3{Ring * std::cos(Theta), 0.4f * std::sin(Phi), Ring * std::sin(Theta)};
	}, true);
}

std::vector<FCookedVertex> Pose(const FSkinnedMeshPiece& Mesh, const std::span<const FMatrix4> Matrices)
{
	std::vector<FCookedVertex> Posed(Mesh.Vertices.size());
	SkinVertices(Matrices, Mesh.Vertices, Mesh.Influences, Posed);
	return Posed;
}

// Every edge, welded by position, must be used once in each direction: no holes, cracks, or flipped triangles.
std::size_t CountOpenEdges(const FSkinnedMeshPiece& Mesh, const std::span<const FCookedVertex> Posed)
{
	std::map<std::array<std::uint32_t, 3>, std::uint32_t> Welded;
	std::vector<std::uint32_t> Id(Posed.size());
	for (std::size_t Vertex = 0; Vertex < Posed.size(); ++Vertex)
	{
		const auto& P = Posed[Vertex].Position;
		// Rounded to 10 um: cap vertices and the cut vertices of the side walls reach the same point through different arithmetic.
		const std::array Key{static_cast<std::uint32_t>(std::lround(P[0] * 1.e5f)), static_cast<std::uint32_t>(std::lround(P[1] * 1.e5f)), static_cast<std::uint32_t>(std::lround(P[2] * 1.e5f))};
		Id[Vertex] = Welded.try_emplace(Key, static_cast<std::uint32_t>(Welded.size())).first->second;
	}

	std::map<std::pair<std::uint32_t, std::uint32_t>, int> Edges;
	for (std::size_t Corner = 0; Corner + 2 < Mesh.Indices.size(); Corner += 3)
	{
		const std::array Triangle{Id[Mesh.Indices[Corner]], Id[Mesh.Indices[Corner + 1]], Id[Mesh.Indices[Corner + 2]]};
		// A sphere's collapsed poles make zero-area triangles, which close nothing.
		if (Triangle[0] == Triangle[1] || Triangle[1] == Triangle[2] || Triangle[0] == Triangle[2])
		{
			continue;
		}

		for (std::size_t Side = 0; Side < 3; ++Side)
		{
			++Edges[{Triangle[Side], Triangle[(Side + 1) % 3]}];
		}
	}

	std::size_t Open = 0;
	for (const auto& [Edge, Count] : Edges)
	{
		const auto Reverse = Edges.find({Edge.second, Edge.first});
		Open += Count != 1 || Reverse == Edges.end() || Reverse->second != 1 ? 1 : 0;
	}

	return Open;
}

float Volume(const FSkinnedMeshPiece& Mesh, const std::span<const FCookedVertex> Posed)
{
	double Sum = 0.;
	for (std::size_t Corner = 0; Corner + 2 < Mesh.Indices.size(); Corner += 3)
	{
		const auto& A = Posed[Mesh.Indices[Corner]].Position;
		const auto& B = Posed[Mesh.Indices[Corner + 1]].Position;
		const auto& C = Posed[Mesh.Indices[Corner + 2]].Position;
		Sum += (FVector3{A[0], A[1], A[2]}.Dot(FVector3{B[0], B[1], B[2]}.Cross(FVector3{C[0], C[1], C[2]})));
	}

	return static_cast<float>(Sum / 6.);
}

void CheckClosedHalves(const FSkinnedMeshPiece& Mesh, const std::span<const FMatrix4> Matrices, const FMeshSlicePlane& Plane, const std::size_t Loops)
{
	const auto Posed = Pose(Mesh, Matrices);
	const auto Sliced = SliceSkinnedMesh(Mesh, Posed, Matrices, Plane, 7);
	REQUIRE_MESSAGE(Sliced, (Sliced ? std::string() : Sliced.error().Message));
	CHECK(Sliced->CapLoops == Loops);
	float Halves = 0.f;
	for (const FSkinnedMeshPiece* Half : {&Sliced->Back, &Sliced->Front})
	{
		REQUIRE_FALSE(Half->Indices.empty());
		CHECK(Half->Sections.back().Material == 7);
		CHECK(std::ranges::all_of(Half->Influences, [](const FCookedSkinInfluence& Influence)
		{
			return Influence.Weights[0] + Influence.Weights[1] + Influence.Weights[2] + Influence.Weights[3] == CookedSkinWeightTotal;
		}));

		// Skinning the half's bind vertices with the same pose puts every vertex on its side of the cut. Cut vertices land within millimeters of
		// the plane rather than on it, because skinning with interpolated weights is not linear along the edge.
		const auto HalfPosed = Pose(*Half, Matrices);
		const float Sign = Half == &Sliced->Back ? -1.f : 1.f;
		CHECK(std::ranges::all_of(HalfPosed, [&](const FCookedVertex& Vertex)
		{
			return Sign * (Plane.Normal.Dot(FVector3{Vertex.Position[0], Vertex.Position[1], Vertex.Position[2]}) - Plane.Distance) > -5.e-3f;
		}));

		CHECK(CountOpenEdges(*Half, HalfPosed) == 0);
		// Cooked models need unit normals and tangents perpendicular to them, which interpolation alone would only approximate.
		CHECK(std::ranges::all_of(Half->Vertices, [](const FCookedVertex& Vertex)
		{
			const FVector3 Normal{Vertex.Normal[0], Vertex.Normal[1], Vertex.Normal[2]};
			const FVector3 Tangent{Vertex.Tangent[0], Vertex.Tangent[1], Vertex.Tangent[2]};
			return std::abs(Normal.Length() - 1.f) < 1.e-3f && std::abs(Tangent.Length() - 1.f) < 1.e-3f && std::abs(Normal.Dot(Tangent)) < 1.e-3f;
		}));
		const float HalfVolume = Volume(*Half, HalfPosed);
		CHECK(HalfVolume > 0.f);
		Halves += HalfVolume;
	}

	CHECK(Halves == doctest::Approx(Volume(Mesh, Posed)).epsilon(1.e-3));
}
}

TEST_CASE("Slicing a seamed sphere at any angle gives two closed halves that hold the whole volume")
{
	const FSkinnedMeshPiece Sphere = MakeSphere();
	const std::array<FMatrix4, 2> Rest{FMatrix4::Identity(), FMatrix4::Identity()};
	for (const FVector3& Direction : {FVector3{0.f, 1.f, 0.f}, FVector3{1.f, 1.f, 0.f}, FVector3{0.3f, -0.4f, 0.87f}, FVector3{1.f, 0.f, 0.f}})
	{
		CheckClosedHalves(Sphere, Rest, {.Normal = Direction.Normalized(), .Distance = 0.137f}, 1);
	}
}

TEST_CASE("A slice through a torus's ring caps it with an outline around a hole")
{
	const std::array<FMatrix4, 2> Rest{FMatrix4::Identity(), FMatrix4::Identity()};
	// Flat through the ring: an outer and an inner circle, which must cap as one annulus per half.
	CheckClosedHalves(MakeTorus(), Rest, {.Normal = FVector3{0.f, 1.f, 0.f}, .Distance = 0.05f}, 2);
	// Upright through both sides of the ring: two separate discs.
	CheckClosedHalves(MakeTorus(), Rest, {.Normal = FVector3{1.f, 0.f, 0.f}, .Distance = 0.2f}, 2);
}

TEST_CASE("Slicing a posed mesh cuts where the pose puts it, and both halves keep animating from the skeleton")
{
	// Joint 1 swings the top of the sphere sideways; the cut follows the posed shape, and its bind-space vertices skin back onto the plane.
	const std::array<FMatrix4, 2> Pose{FMatrix4::Identity(), FMatrix4::Translation({0.3f, 0.1f, 0.f}) * FMatrix4::Rotation(FQuaternion::FromAxisAngle({0.f, 0.f, 1.f}, 0.4f))};
	CheckClosedHalves(MakeSphere(), Pose, {.Normal = FVector3{0.2f, 1.f, 0.1f}.Normalized(), .Distance = 0.2f}, 1);
	// Slicing is deterministic: the same pose and plane give identical halves.
	const FSkinnedMeshPiece Sphere = MakeSphere();
	const auto Posed = ::Herta::Pose(Sphere, Pose);
	const auto First = SliceSkinnedMesh(Sphere, Posed, Pose, {.Normal = FVector3{0.f, 1.f, 0.f}, .Distance = 0.1f}, 1);
	const auto Second = SliceSkinnedMesh(Sphere, Posed, Pose, {.Normal = FVector3{0.f, 1.f, 0.f}, .Distance = 0.1f}, 1);
	REQUIRE(First);
	REQUIRE(Second);
	CHECK(First->Front.Indices == Second->Front.Indices);
	CHECK(std::ranges::equal(First->Front.Vertices, Second->Front.Vertices, [](const FCookedVertex& A, const FCookedVertex& B)
	{
		return A.Position == B.Position && A.UV == B.UV;
	}));
}

TEST_CASE("A plane that misses the mesh leaves it whole on one side")
{
	const FSkinnedMeshPiece Sphere = MakeSphere();
	const std::array<FMatrix4, 2> Rest{FMatrix4::Identity(), FMatrix4::Identity()};
	const auto Sliced = SliceSkinnedMesh(Sphere, Pose(Sphere, Rest), Rest, {.Normal = FVector3{0.f, 1.f, 0.f}, .Distance = 5.f}, 1);
	REQUIRE(Sliced);
	CHECK(Sliced->CapLoops == 0);
	CHECK(Sliced->Front.Indices.empty());
	CHECK(Sliced->Back.Indices.size() == Sphere.Indices.size());
	CHECK_FALSE(SliceSkinnedMesh(Sphere, {}, Rest, {.Normal = FVector3{0.f, 1.f, 0.f}}, 1));
	CHECK_FALSE(SliceSkinnedMesh(Sphere, Pose(Sphere, Rest), Rest, {.Normal = FVector3{0.f, 2.f, 0.f}}, 1));
}
}
