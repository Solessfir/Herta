#include "Herta/Animation/MeshSlice.h"

#include <mapbox/earcut.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <format>
#include <map>
#include <optional>
#include <utility>

namespace Herta
{
namespace
{
using FPositionKey = std::array<std::uint32_t, 3>;

[[nodiscard]] FVector3 ToVector(const std::array<float, 3>& Value)
{
	return {Value[0], Value[1], Value[2]};
}

[[nodiscard]] std::array<float, 3> ToArray(const FVector3& Value)
{
	return {Value.X, Value.Y, Value.Z};
}

[[nodiscard]] FPositionKey KeyOf(const std::array<float, 3>& Position)
{
	return {std::bit_cast<std::uint32_t>(Position[0]), std::bit_cast<std::uint32_t>(Position[1]), std::bit_cast<std::uint32_t>(Position[2])};
}

[[nodiscard]] FVector3 NormalizedOr(const FVector3& Value, const FVector3& Fallback)
{
	const float Length = Value.Length();
	return Length > 1.e-12f ? Value * (1.f / Length) : Fallback;
}

// Blends two influences at T, keeping the four heaviest joints, ties broken by joint index, and renormalizing so the weights still sum exactly.
[[nodiscard]] FCookedSkinInfluence BlendInfluences(const FCookedSkinInfluence& A, const FCookedSkinInfluence& B, const float T)
{
	std::array<std::pair<std::uint16_t, float>, 8> Joints{};
	std::size_t Count = 0;
	const auto Add = [&](const std::uint16_t Joint, const float Weight)
	{
		if (Weight <= 0.f)
		{
			return;
		}

		const auto Found = std::find_if(Joints.begin(), Joints.begin() + static_cast<std::ptrdiff_t>(Count), [Joint](const auto& Entry)
		{
			return Entry.first == Joint;
		});

		if (Found != Joints.begin() + static_cast<std::ptrdiff_t>(Count))
		{
			Found->second += Weight;
		}
		else
		{
			Joints[Count++] = {Joint, Weight};
		}
	};

	for (std::size_t Slot = 0; Slot < 4; ++Slot)
	{
		Add(A.Joints[Slot], static_cast<float>(A.Weights[Slot]) * (1.f - T));
		Add(B.Joints[Slot], static_cast<float>(B.Weights[Slot]) * T);
	}

	// Unused slots weigh zero and real ones more, so sorting the whole array keeps them last; GCC misreads a sort bounded by Count as overrunning.
	std::ranges::sort(Joints, [](const auto& Left, const auto& Right)
	{
		return Left.second != Right.second ? Left.second > Right.second : Left.first < Right.first;
	});

	FCookedSkinInfluence Result{.Joints = {}, .Weights = {}};
	const std::size_t Kept = std::min<std::size_t>(Count, 4);
	float Total = 0.f;
	for (std::size_t Slot = 0; Slot < Kept; ++Slot)
	{
		Total += Joints[Slot].second;
	}

	if (Kept == 0 || Total <= 0.f)
	{
		return A;
	}

	std::uint32_t Sum = 0;
	for (std::size_t Slot = 0; Slot < Kept; ++Slot)
	{
		Result.Joints[Slot] = Joints[Slot].first;
		Result.Weights[Slot] = static_cast<std::uint16_t>(std::lround(Joints[Slot].second / Total * static_cast<float>(CookedSkinWeightTotal)));
		Sum += Result.Weights[Slot];
	}

	// Rounding can miss the total by a unit or two; the heaviest joint absorbs it.
	Result.Weights[0] = static_cast<std::uint16_t>(static_cast<std::int32_t>(Result.Weights[0]) + static_cast<std::int32_t>(CookedSkinWeightTotal) - static_cast<std::int32_t>(Sum));
	return Result;
}

void OrthonormalizeTangentImpl(FCookedVertex& Vertex)
{
	const FVector3 Normal = NormalizedOr(ToVector(Vertex.Normal), FVector3{0.f, 1.f, 0.f});
	FVector3 Tangent{Vertex.Tangent[0], Vertex.Tangent[1], Vertex.Tangent[2]};
	Tangent = Tangent - Normal * Normal.Dot(Tangent);
	// A tangent along the normal has no direction left; any perpendicular serves a surface that has no texture detail to orient.
	Tangent = NormalizedOr(Tangent, NormalizedOr(Normal.Cross(std::abs(Normal.Y) < 0.9f ? FVector3{0.f, 1.f, 0.f} : FVector3{1.f, 0.f, 0.f}), FVector3{1.f, 0.f, 0.f}));
	Vertex.Normal = ToArray(Normal);
	Vertex.Tangent = {Tangent.X, Tangent.Y, Tangent.Z, Vertex.Tangent[3] < 0.f ? -1.f : 1.f};
}

[[nodiscard]] FCookedVertex BlendVertices(const FCookedVertex& A, const FCookedVertex& B, const float T)
{
	FCookedVertex Result;
	for (std::size_t Axis = 0; Axis < 3; ++Axis)
	{
		Result.Position[Axis] = A.Position[Axis] + (B.Position[Axis] - A.Position[Axis]) * T;
	}

	Result.UV = {A.UV[0] + (B.UV[0] - A.UV[0]) * T, A.UV[1] + (B.UV[1] - A.UV[1]) * T};
	Result.Normal = ToArray(NormalizedOr(ToVector(A.Normal) * (1.f - T) + ToVector(B.Normal) * T, ToVector(A.Normal)));
	const FVector3 Tangent = NormalizedOr(FVector3{A.Tangent[0], A.Tangent[1], A.Tangent[2]} * (1.f - T) + FVector3{B.Tangent[0], B.Tangent[1], B.Tangent[2]} * T, FVector3{A.Tangent[0], A.Tangent[1], A.Tangent[2]});
	Result.Tangent = {Tangent.X, Tangent.Y, Tangent.Z, A.Tangent[3]};
	OrthonormalizeTangentImpl(Result);
	return Result;
}

// A blended skinning matrix's rotation, transposed, turns a posed direction back into bind space; skinning is rigid enough for that to hold.
[[nodiscard]] FVector3 ToBindDirection(const FCookedSkinInfluence& Influence, const std::span<const FMatrix4> Matrices, const FVector3& Posed)
{
	FVector3 Result{};
	for (std::size_t Slot = 0; Slot < 4; ++Slot)
	{
		if (Influence.Weights[Slot] == 0 || Influence.Joints[Slot] >= Matrices.size())
		{
			continue;
		}

		const FMatrix4& Matrix = Matrices[Influence.Joints[Slot]];
		const float Weight = static_cast<float>(Influence.Weights[Slot]) / static_cast<float>(CookedSkinWeightTotal);
		Result = Result + FVector3{Matrix(0, 0) * Posed.X + Matrix(1, 0) * Posed.Y + Matrix(2, 0) * Posed.Z, Matrix(0, 1) * Posed.X + Matrix(1, 1) * Posed.Y + Matrix(2, 1) * Posed.Z, Matrix(0, 2) * Posed.X + Matrix(1, 2) * Posed.Y + Matrix(2, 2) * Posed.Z} * Weight;
	}

	return NormalizedOr(Result, Posed);
}

// One half under construction: the original vertices it uses, remapped on first use, and the cut vertices it shares with the other half.
struct FHalfBuilder
{
	FSkinnedMeshPiece Piece;
	std::vector<std::uint32_t> Remap;

	std::uint32_t Use(const FSkinnedMeshPiece& Mesh, const std::uint32_t Vertex)
	{
		if (Remap[Vertex] == UINT32_MAX)
		{
			Remap[Vertex] = static_cast<std::uint32_t>(Piece.Vertices.size());
			Piece.Vertices.push_back(Mesh.Vertices[Vertex]);
			Piece.Influences.push_back(Mesh.Influences[Vertex]);
		}

		return Remap[Vertex];
	}

	std::uint32_t Add(const FCookedVertex& Vertex, const FCookedSkinInfluence& Influence)
	{
		Piece.Vertices.push_back(Vertex);
		Piece.Influences.push_back(Influence);
		return static_cast<std::uint32_t>(Piece.Vertices.size() - 1);
	}
};

// A point where the plane crosses an edge, shared by every triangle on that edge, welded by position so UV seams do not split the outline.
struct FCutPoint
{
	FCookedVertex Bind;
	FCookedSkinInfluence Influence;
	FVector3 Posed;
};

[[nodiscard]] bool Inside(const std::vector<std::array<double, 2>>& Polygon, const std::array<double, 2>& Point)
{
	bool bInside = false;
	for (std::size_t Index = 0, Previous = Polygon.size() - 1; Index < Polygon.size(); Previous = Index++)
	{
		const auto& A = Polygon[Index];
		const auto& B = Polygon[Previous];
		if ((A[1] > Point[1]) != (B[1] > Point[1]) && Point[0] < (B[0] - A[0]) * (Point[1] - A[1]) / (B[1] - A[1]) + A[0])
		{
			bInside = !bInside;
		}
	}

	return bInside;
}
}

std::expected<FMeshSliceResult, FAnimationError> SliceSkinnedMesh(const FSkinnedMeshPiece& Mesh, const std::span<const FCookedVertex> Posed, const std::span<const FMatrix4> SkinningMatrices, const FMeshSlicePlane& Plane, const std::uint32_t WoundMaterial)
{
	std::vector<float> Sides(Posed.size());
	for (std::size_t Vertex = 0; Vertex < Posed.size(); ++Vertex)
	{
		Sides[Vertex] = Plane.Normal.Dot(ToVector(Posed[Vertex].Position)) - Plane.Distance;
	}

	return SplitSkinnedMesh(Mesh, Posed, SkinningMatrices, Sides, Plane, WoundMaterial);
}

std::expected<FMeshSliceResult, FAnimationError> SplitSkinnedMesh(const FSkinnedMeshPiece& Mesh, const std::span<const FCookedVertex> Posed, const std::span<const FMatrix4> SkinningMatrices, const std::span<const float> Sides, const FMeshSlicePlane& Plane, const std::uint32_t WoundMaterial)
{
	const std::size_t VertexCount = Mesh.Vertices.size();
	if (Posed.size() != VertexCount || Mesh.Influences.size() != VertexCount || Sides.size() != VertexCount || std::abs(Plane.Normal.Length() - 1.f) > 1.e-3f || !std::isfinite(Plane.Distance)
	    || std::ranges::any_of(Mesh.Indices, [VertexCount](const std::uint32_t Index)
	{
		return Index >= VertexCount;
	}) || !std::ranges::all_of(Sides, [](const float Value)
	{
		return std::isfinite(Value);
	}))
	{
		return std::unexpected(FAnimationError{"Slicing needs one posed vertex, influence, and finite side per bind vertex, in-range indices, and a unit plane normal"});
	}

	// Zero counts as in front, so every crossing edge has a strict sign change and a cut point strictly inside it.
	const std::span<const float> Side = Sides;

	const auto Front = [&Side](const std::uint32_t Vertex)
	{
		return Side[Vertex] >= 0.f;
	};

	FHalfBuilder Back{.Piece = {}, .Remap = std::vector<std::uint32_t>(VertexCount, UINT32_MAX)};
	FHalfBuilder Forth{.Piece = {}, .Remap = std::vector<std::uint32_t>(VertexCount, UINT32_MAX)};
	std::map<std::pair<FPositionKey, FPositionKey>, std::size_t> PointsByEdge;
	std::vector<FCutPoint> Points;
	// Each half's own copy of a cut vertex, keyed by the exact vertex pair, so seam duplicates keep their own UVs.
	std::map<std::pair<std::uint32_t, std::uint32_t>, std::pair<std::uint32_t, std::uint32_t>> CutVertices;
	std::map<std::size_t, std::vector<std::size_t>> Neighbours;

	const auto Cut = [&](const std::uint32_t A, const std::uint32_t B) -> std::pair<std::size_t, std::pair<std::uint32_t, std::uint32_t>>
	{
		// Interpolating from the lower key keeps the arithmetic, and so the point, identical on every triangle that shares the edge.
		const bool bSwap = KeyOf(Mesh.Vertices[B].Position) < KeyOf(Mesh.Vertices[A].Position);
		const std::uint32_t From = bSwap ? B : A;
		const std::uint32_t To = bSwap ? A : B;
		const float T = Side[From] / (Side[From] - Side[To]);
		const auto EdgeKey = std::pair{KeyOf(Mesh.Vertices[From].Position), KeyOf(Mesh.Vertices[To].Position)};
		auto [Point, bNewPoint] = PointsByEdge.try_emplace(EdgeKey, Points.size());
		if (bNewPoint)
		{
			const FVector3 PosedFrom = ToVector(Posed[From].Position);
			Points.push_back({.Bind = BlendVertices(Mesh.Vertices[From], Mesh.Vertices[To], T), .Influence = BlendInfluences(Mesh.Influences[From], Mesh.Influences[To], T), .Posed = PosedFrom + (ToVector(Posed[To].Position) - PosedFrom) * T});
		}

		auto [Vertex, bNewVertex] = CutVertices.try_emplace({From, To});
		if (bNewVertex)
		{
			const FCookedVertex Bind = BlendVertices(Mesh.Vertices[From], Mesh.Vertices[To], T);
			const FCookedSkinInfluence& Influence = Points[Point->second].Influence;
			Vertex->second = {Back.Add(Bind, Influence), Forth.Add(Bind, Influence)};
		}

		return {Point->second, Vertex->second};
	};

	for (const FCookedMeshSection& Section : Mesh.Sections)
	{
		const auto BackFirst = static_cast<std::uint32_t>(Back.Piece.Indices.size());
		const auto FrontFirst = static_cast<std::uint32_t>(Forth.Piece.Indices.size());
		for (std::uint32_t Triangle = Section.FirstIndex; Triangle + 2 < Section.FirstIndex + Section.IndexCount && Triangle + 2 < Mesh.Indices.size(); Triangle += 3)
		{
			const std::array<std::uint32_t, 3> Corner{Mesh.Indices[Triangle], Mesh.Indices[Triangle + 1], Mesh.Indices[Triangle + 2]};
			const std::array<bool, 3> bFront{Front(Corner[0]), Front(Corner[1]), Front(Corner[2])};
			if (bFront[0] == bFront[1] && bFront[1] == bFront[2])
			{
				FHalfBuilder& Half = bFront[0] ? Forth : Back;
				for (const std::uint32_t Vertex : Corner)
				{
					Half.Piece.Indices.push_back(Half.Use(Mesh, Vertex));
				}

				continue;
			}

			// Rotate so the corner alone on its side comes first, keeping the winding.
			const std::size_t Lone = bFront[0] != bFront[1] && bFront[0] != bFront[2] ? 0 : bFront[1] != bFront[0] && bFront[1] != bFront[2] ? 1 : 2;
			const std::uint32_t L = Corner[Lone];
			const std::uint32_t B = Corner[(Lone + 1) % 3];
			const std::uint32_t C = Corner[(Lone + 2) % 3];
			const auto [PointLB, VertexLB] = Cut(L, B);
			const auto [PointLC, VertexLC] = Cut(L, C);
			FHalfBuilder& LoneHalf = bFront[Lone] ? Forth : Back;
			FHalfBuilder& PairHalf = bFront[Lone] ? Back : Forth;
			const auto Own = [&](const FHalfBuilder& Half, const std::pair<std::uint32_t, std::uint32_t>& Vertex)
			{
				return &Half == &Back ? Vertex.first : Vertex.second;
			};

			LoneHalf.Piece.Indices.insert(LoneHalf.Piece.Indices.end(), {LoneHalf.Use(Mesh, L), Own(LoneHalf, VertexLB), Own(LoneHalf, VertexLC)});
			const std::uint32_t PairB = PairHalf.Use(Mesh, B);
			const std::uint32_t PairC = PairHalf.Use(Mesh, C);
			PairHalf.Piece.Indices.insert(PairHalf.Piece.Indices.end(), {Own(PairHalf, VertexLB), PairB, PairC, Own(PairHalf, VertexLB), PairC, Own(PairHalf, VertexLC)});
			if (PointLB != PointLC)
			{
				Neighbours[PointLB].push_back(PointLC);
				Neighbours[PointLC].push_back(PointLB);
			}
		}

		for (auto [Half, First] : {std::pair{&Back, BackFirst}, std::pair{&Forth, FrontFirst}})
		{
			const auto Count = static_cast<std::uint32_t>(Half->Piece.Indices.size()) - First;
			if (Count > 0)
			{
				Half->Piece.Sections.push_back({.FirstIndex = First, .IndexCount = Count, .Material = Section.Material});
			}
		}
	}

	// Trace the cut outline into loops. A closed mesh gives every point two neighbours; an open one leaves chains, which close end to end.
	std::vector<std::vector<std::size_t>> Loops;
	std::vector<bool> Visited(Points.size(), false);
	for (const auto& [Start, Unused] : Neighbours)
	{
		if (Visited[Start])
		{
			continue;
		}

		std::vector<std::size_t> Loop{Start};
		Visited[Start] = true;
		std::size_t Current = Start;
		for (;;)
		{
			const auto Next = std::ranges::find_if(Neighbours[Current], [&Visited](const std::size_t Point)
			{
				return !Visited[Point];
			});

			if (Next == Neighbours[Current].end())
			{
				break;
			}

			Current = *Next;
			Visited[Current] = true;
			Loop.push_back(Current);
		}

		if (Loop.size() >= 3)
		{
			Loops.push_back(std::move(Loop));
		}
	}

	FMeshSliceResult Result;
	Result.CapLoops = Loops.size();
	if (!Loops.empty())
	{
		// Caps lie in the plane, in a basis whose U and V also give the cut its planar UVs.
		const FVector3 U = NormalizedOr(Plane.Normal.Cross(std::abs(Plane.Normal.Y) < 0.9f ? FVector3{0.f, 1.f, 0.f} : FVector3{1.f, 0.f, 0.f}), FVector3{1.f, 0.f, 0.f});
		const FVector3 V = Plane.Normal.Cross(U);
		std::vector<std::vector<std::array<double, 2>>> Polygons;
		for (const std::vector<std::size_t>& Loop : Loops)
		{
			auto& Polygon = Polygons.emplace_back();
			for (const std::size_t Point : Loop)
			{
				Polygon.push_back({Points[Point].Posed.Dot(U), Points[Point].Posed.Dot(V)});
			}
		}

		// Loops nest: one inside an odd number of others is a hole in the smallest of them, such as the inside of a hollow limb.
		std::vector<std::size_t> Depth(Loops.size(), 0);
		std::vector<std::optional<std::size_t>> Container(Loops.size());
		const auto Area = [&Polygons](const std::size_t Loop)
		{
			double Twice = 0.;
			const auto& Polygon = Polygons[Loop];
			for (std::size_t Index = 0, Previous = Polygon.size() - 1; Index < Polygon.size(); Previous = Index++)
			{
				Twice += Polygon[Previous][0] * Polygon[Index][1] - Polygon[Index][0] * Polygon[Previous][1];
			}

			return std::abs(Twice) * 0.5;
		};

		for (std::size_t Loop = 0; Loop < Loops.size(); ++Loop)
		{
			for (std::size_t Other = 0; Other < Loops.size(); ++Other)
			{
				if (Other != Loop && Area(Other) > Area(Loop) && Inside(Polygons[Other], Polygons[Loop].front()))
				{
					++Depth[Loop];
					if (!Container[Loop] || Area(Other) < Area(*Container[Loop]))
					{
						Container[Loop] = Other;
					}
				}
			}
		}

		for (FHalfBuilder* Half : {&Back, &Forth})
		{
			const auto First = static_cast<std::uint32_t>(Half->Piece.Indices.size());
			// The back half's cap faces forward, out of it towards the removed front, and the front half's faces back.
			const FVector3 Facing = Half == &Back ? Plane.Normal : -Plane.Normal;
			for (std::size_t Outer = 0; Outer < Loops.size(); ++Outer)
			{
				if (Depth[Outer] % 2 != 0)
				{
					continue;
				}

				std::vector<std::size_t> Rings{Outer};
				for (std::size_t Hole = 0; Hole < Loops.size(); ++Hole)
				{
					if (Depth[Hole] % 2 != 0 && Container[Hole] == Outer)
					{
						Rings.push_back(Hole);
					}
				}

				std::vector<std::vector<std::array<double, 2>>> Shape;
				std::vector<std::uint32_t> CapVertices;
				for (const std::size_t Ring : Rings)
				{
					Shape.push_back(Polygons[Ring]);
					for (const std::size_t Point : Loops[Ring])
					{
						const FCutPoint& CutPoint = Points[Point];
						FCookedVertex Vertex = CutPoint.Bind;
						Vertex.Normal = ToArray(ToBindDirection(CutPoint.Influence, SkinningMatrices, Facing));
						const FVector3 Tangent = ToBindDirection(CutPoint.Influence, SkinningMatrices, U);
						Vertex.Tangent = {Tangent.X, Tangent.Y, Tangent.Z, 1.f};
						Vertex.UV = {CutPoint.Posed.Dot(U), CutPoint.Posed.Dot(V)};
						OrthonormalizeTangentImpl(Vertex);
						CapVertices.push_back(Half->Add(Vertex, CutPoint.Influence));
					}
				}

				const std::vector<std::uint32_t> Triangles = mapbox::earcut<std::uint32_t>(Shape);
				for (std::size_t Corner = 0; Corner + 2 < Triangles.size(); Corner += 3)
				{
					std::array<std::uint32_t, 3> Triangle{CapVertices[Triangles[Corner]], CapVertices[Triangles[Corner + 1]], CapVertices[Triangles[Corner + 2]]};
					// Earcut winds by the outline's own direction, so each triangle is turned to face out of its half.
					const auto Corner2D = [&](const std::size_t Index)
					{
						std::size_t Remaining = Triangles[Corner + Index];
						for (const auto& Ring : Shape)
						{
							if (Remaining < Ring.size())
							{
								return Ring[Remaining];
							}

							Remaining -= Ring.size();
						}

						return std::array<double, 2>{};
					};

					const auto A = Corner2D(0);
					const auto B = Corner2D(1);
					const auto C = Corner2D(2);
					// Counter-clockwise in the U, V basis faces along U x V, which is the plane normal.
					const double Winding = (B[0] - A[0]) * (C[1] - A[1]) - (B[1] - A[1]) * (C[0] - A[0]);
					if ((Winding > 0.) != (Half == &Back))
					{
						std::swap(Triangle[1], Triangle[2]);
					}

					Half->Piece.Indices.insert(Half->Piece.Indices.end(), Triangle.begin(), Triangle.end());
				}
			}

			const auto Count = static_cast<std::uint32_t>(Half->Piece.Indices.size()) - First;
			if (Count > 0)
			{
				Half->Piece.Sections.push_back({.FirstIndex = First, .IndexCount = Count, .Material = WoundMaterial});
			}
		}
	}

	Result.Back = std::move(Back.Piece);
	Result.Front = std::move(Forth.Piece);
	return Result;
}

void OrthonormalizeTangent(FCookedVertex& Vertex)
{
	OrthonormalizeTangentImpl(Vertex);
}
}
