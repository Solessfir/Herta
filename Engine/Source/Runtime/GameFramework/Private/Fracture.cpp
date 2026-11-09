#include "Herta/GameFramework/Fracture.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <numbers>
#include <utility>

namespace Herta
{
namespace
{
// Clipping runs in double so thin slivers between nearby sites keep consistent topology; chunks are converted to float once at the end.
struct FPoint
{
	double X = 0.0;
	double Y = 0.0;
	double Z = 0.0;
};

FPoint operator+(const FPoint& A, const FPoint& B)
{
	return {A.X + B.X, A.Y + B.Y, A.Z + B.Z};
}

FPoint operator-(const FPoint& A, const FPoint& B)
{
	return {A.X - B.X, A.Y - B.Y, A.Z - B.Z};
}

FPoint operator*(const FPoint& A, const double Scale)
{
	return {A.X * Scale, A.Y * Scale, A.Z * Scale};
}

double Dot(const FPoint& A, const FPoint& B)
{
	return A.X * B.X + A.Y * B.Y + A.Z * B.Z;
}

FPoint Cross(const FPoint& A, const FPoint& B)
{
	return {A.Y * B.Z - A.Z * B.Y, A.Z * B.X - A.X * B.Z, A.X * B.Y - A.Y * B.X};
}

FPoint Normalized(const FPoint& A)
{
	const double Length = std::sqrt(Dot(A, A));
	return Length > 0.0 ? A * (1.0 / Length) : FPoint{0.0, 1.0, 0.0};
}

struct FFace
{
	std::vector<std::uint32_t> Loop;
	// The neighbouring cell across this face, or -1 for the box's own faces.
	int Neighbor = -1;
};

struct FPolyhedron
{
	std::vector<FPoint> Points;
	std::vector<FFace> Faces;
};

FPolyhedron MakeBox(const FPoint& Half)
{
	FPolyhedron Box;
	for (int Index = 0; Index < 8; ++Index)
	{
		Box.Points.push_back({(Index & 1) ? Half.X : -Half.X, (Index & 2) ? Half.Y : -Half.Y, (Index & 4) ? Half.Z : -Half.Z});
	}

	// Counter-clockwise seen from outside.
	Box.Faces = {
	    {.Loop = {0, 4, 6, 2}},
	    {.Loop = {1, 3, 7, 5}},
	    {.Loop = {0, 1, 5, 4}},
	    {.Loop = {2, 6, 7, 3}},
	    {.Loop = {0, 2, 3, 1}},
	    {.Loop = {4, 5, 7, 6}},
	};
	return Box;
}

// Keeps the part where Dot(Normal, X) <= Distance; the cut becomes a face across from Neighbor.
void Clip(FPolyhedron& Shape, const FPoint& Normal, const double Distance, const int Neighbor, const double Epsilon)
{
	std::vector<double> Side(Shape.Points.size());
	bool bAnyOutside = false;
	for (std::size_t Index = 0; Index < Shape.Points.size(); ++Index)
	{
		Side[Index] = Dot(Normal, Shape.Points[Index]) - Distance;
		bAnyOutside = bAnyOutside || Side[Index] > Epsilon;
	}

	if (!bAnyOutside)
	{
		return;
	}

	// Each cut edge gets one new point, shared by the two faces that border it.
	std::map<std::pair<std::uint32_t, std::uint32_t>, std::uint32_t> Cuts;
	const auto CutPoint = [&](const std::uint32_t A, const std::uint32_t B)
	{
		const auto Key = std::minmax(A, B);
		if (const auto Found = Cuts.find(Key); Found != Cuts.end())
		{
			return Found->second;
		}

		const double T = Side[A] / (Side[A] - Side[B]);
		Shape.Points.push_back(Shape.Points[A] + (Shape.Points[B] - Shape.Points[A]) * T);
		const auto Created = static_cast<std::uint32_t>(Shape.Points.size() - 1);
		Cuts.emplace(Key, Created);
		return Created;
	};

	std::vector<std::uint32_t> CapPoints;
	std::vector<FFace> Kept;
	for (const FFace& Face : Shape.Faces)
	{
		FFace Result{.Loop = {}, .Neighbor = Face.Neighbor};
		for (std::size_t Index = 0; Index < Face.Loop.size(); ++Index)
		{
			const std::uint32_t Current = Face.Loop[Index];
			const std::uint32_t Next = Face.Loop[(Index + 1) % Face.Loop.size()];
			const bool bCurrentInside = Side[Current] <= Epsilon;
			const bool bNextInside = Side[Next] <= Epsilon;
			if (bCurrentInside)
			{
				Result.Loop.push_back(Current);
				if (std::abs(Side[Current]) <= Epsilon)
				{
					CapPoints.push_back(Current);
				}
			}

			if (bCurrentInside != bNextInside && std::abs(Side[Current]) > Epsilon && std::abs(Side[Next]) > Epsilon)
			{
				const std::uint32_t Cut = CutPoint(Current, Next);
				Result.Loop.push_back(Cut);
				CapPoints.push_back(Cut);
			}
		}

		if (Result.Loop.size() >= 3)
		{
			Kept.push_back(std::move(Result));
		}
	}

	std::ranges::sort(CapPoints);
	CapPoints.erase(std::ranges::unique(CapPoints).begin(), CapPoints.end());
	if (CapPoints.size() >= 3)
	{
		FPoint Center{};
		for (const std::uint32_t Point : CapPoints)
		{
			Center = Center + Shape.Points[Point];
		}

		Center = Center * (1.0 / static_cast<double>(CapPoints.size()));
		const FPoint Axis = std::abs(Normal.X) < 0.9 ? FPoint{1.0, 0.0, 0.0} : FPoint{0.0, 1.0, 0.0};
		const FPoint U = Normalized(Cross(Axis, Normal));
		const FPoint V = Cross(Normal, U);
		// Ascending angle about the outward normal winds the cap counter-clockwise seen from outside.
		std::ranges::sort(CapPoints, {}, [&](const std::uint32_t Point)
		{
			const FPoint Offset = Shape.Points[Point] - Center;
			return std::atan2(Dot(Offset, V), Dot(Offset, U));
		});

		Kept.push_back({.Loop = std::move(CapPoints), .Neighbor = Neighbor});
	}

	Shape.Faces = std::move(Kept);
}

// PCG32 (O'Neill), so every platform draws the same sites; the standard distributions differ between libraries.
struct FRandom
{
	std::uint64_t State = 0;

	std::uint32_t Next()
	{
		const std::uint64_t Old = State;
		State = Old * 6364136223846793005ull + 1442695040888963407ull;
		const auto Shifted = static_cast<std::uint32_t>(((Old >> 18u) ^ Old) >> 27u);
		const auto Rotation = static_cast<std::uint32_t>(Old >> 59u);
		return (Shifted >> Rotation) | (Shifted << ((32u - Rotation) & 31u));
	}

	double Unit()
	{
		return static_cast<double>(Next()) / 4294967296.0;
	}
};

double FaceArea(const FPolyhedron& Shape, const FFace& Face, FPoint& Centroid)
{
	FPoint Sum{};
	double Area = 0.0;
	const FPoint& Origin = Shape.Points[Face.Loop[0]];
	for (std::size_t Index = 1; Index + 1 < Face.Loop.size(); ++Index)
	{
		const FPoint& B = Shape.Points[Face.Loop[Index]];
		const FPoint& C = Shape.Points[Face.Loop[Index + 1]];
		const FPoint Normal = Cross(B - Origin, C - Origin);
		const double Triangle = 0.5 * std::sqrt(Dot(Normal, Normal));
		Sum = Sum + (Origin + B + C) * (Triangle / 3.0);
		Area += Triangle;
	}

	Centroid = Area > 0.0 ? Sum * (1.0 / Area) : Origin;
	return Area;
}

FVector3 ToFloat(const FPoint& Point)
{
	return {static_cast<float>(Point.X), static_cast<float>(Point.Y), static_cast<float>(Point.Z)};
}
}

FFracture FractureBox(const FVector3& HalfExtents, std::uint32_t Cells, const std::uint32_t Seed)
{
	Cells = std::clamp(Cells, MinimumFractureCells, MaximumFractureCells);
	const FPoint Half{HalfExtents.X, HalfExtents.Y, HalfExtents.Z};
	const double Size = std::max({Half.X, Half.Y, Half.Z});
	const double Epsilon = Size * 1e-9;
	FRandom Random{.State = 0x853c49e6748fea9bull ^ (static_cast<std::uint64_t>(Seed) * 0x9e3779b97f4a7c15ull)};
	Random.Next();

	std::vector<FPoint> Sites(Cells);
	for (FPoint& Site : Sites)
	{
		// Sites stay off the faces so no cell degenerates into a sliver against the box.
		Site = {(Random.Unit() * 1.8 - 0.9) * Half.X, (Random.Unit() * 1.8 - 0.9) * Half.Y, (Random.Unit() * 1.8 - 0.9) * Half.Z};
	}

	FFracture Fracture{.HalfExtents = HalfExtents, .Chunks = {}, .Bonds = {}};
	std::vector<FPolyhedron> Shapes(Cells, MakeBox(Half));
	for (std::uint32_t Cell = 0; Cell < Cells; ++Cell)
	{
		for (std::uint32_t Other = 0; Other < Cells; ++Other)
		{
			if (Other == Cell)
			{
				continue;
			}

			const FPoint Normal = Normalized(Sites[Other] - Sites[Cell]);
			Clip(Shapes[Cell], Normal, Dot(Normal, (Sites[Cell] + Sites[Other]) * 0.5), static_cast<int>(Other), Epsilon);
		}
	}

	for (std::uint32_t Cell = 0; Cell < Cells; ++Cell)
	{
		FPolyhedron& Shape = Shapes[Cell];
		FFractureChunk Chunk;
		// Only points that faces still use survive, renumbered in first-use order.
		std::map<std::uint32_t, std::uint32_t> Remap;
		for (const FFace& Face : Shape.Faces)
		{
			std::vector<std::uint32_t> Loop;
			for (const std::uint32_t Point : Face.Loop)
			{
				const auto [Found, bInserted] = Remap.try_emplace(Point, static_cast<std::uint32_t>(Chunk.Points.size()));
				if (bInserted)
				{
					Chunk.Points.push_back(ToFloat(Shape.Points[Point]));
					Chunk.bTouchesBottom = Chunk.bTouchesBottom || Shape.Points[Point].Y <= -Half.Y + Size * 1e-6;
				}

				Loop.push_back(Found->second);
			}

			Chunk.Faces.push_back(std::move(Loop));
		}

		// Volume and centroid from tetrahedra fanned out of the first point.
		const FPoint Apex = Shape.Points[Shape.Faces.front().Loop.front()];
		FPoint Weighted{};
		double Volume = 0.0;
		for (const FFace& Face : Shape.Faces)
		{
			for (std::size_t Index = 1; Index + 1 < Face.Loop.size(); ++Index)
			{
				const FPoint& A = Shape.Points[Face.Loop[0]];
				const FPoint& B = Shape.Points[Face.Loop[Index]];
				const FPoint& C = Shape.Points[Face.Loop[Index + 1]];
				const double Tetrahedron = Dot(A - Apex, Cross(B - Apex, C - Apex)) / 6.0;
				Weighted = Weighted + (Apex + A + B + C) * (Tetrahedron / 4.0);
				Volume += Tetrahedron;
			}
		}

		Chunk.Volume = static_cast<float>(Volume);
		Chunk.Centroid = ToFloat(Volume > 0.0 ? Weighted * (1.0 / Volume) : Sites[Cell]);
		Fracture.Chunks.push_back(std::move(Chunk));

		for (const FFace& Face : Shape.Faces)
		{
			if (Face.Neighbor > static_cast<int>(Cell))
			{
				FPoint Center;
				const double Area = FaceArea(Shape, Face, Center);
				if (Area > Size * Size * 1e-6)
				{
					Fracture.Bonds.push_back({.First = Cell, .Second = static_cast<std::uint32_t>(Face.Neighbor), .Area = static_cast<float>(Area), .Center = ToFloat(Center)});
				}
			}
		}
	}

	return Fracture;
}

FCookedModel BuildFractureChunkModel(const FFracture& Fracture, const std::size_t ChunkIndex)
{
	const FFractureChunk& Chunk = Fracture.Chunks[ChunkIndex];
	FCookedModel Model;
	// Projected along each face's dominant axis at one texel density for the whole box, so a texture runs continuously across neighbouring chunks.
	const float Scale = 0.5f / std::max({Fracture.HalfExtents.X, Fracture.HalfExtents.Y, Fracture.HalfExtents.Z});
	for (const std::vector<std::uint32_t>& Face : Chunk.Faces)
	{
		// Newell's normal uses the whole polygon, so a face whose first corners are nearly collinear still gets a stable direction.
		FVector3 Sum = FVector3::Zero();
		for (std::size_t Index = 0; Index < Face.size(); ++Index)
		{
			const FVector3& Current = Chunk.Points[Face[Index]];
			const FVector3& Next = Chunk.Points[Face[(Index + 1) % Face.size()]];
			Sum = Sum + FVector3{(Current.Y - Next.Y) * (Current.Z + Next.Z), (Current.Z - Next.Z) * (Current.X + Next.X), (Current.X - Next.X) * (Current.Y + Next.Y)};
		}

		const float Length = Sum.Length();
		if (!(Length > 0.f))
		{
			continue;
		}

		const FVector3 Normal = Sum * (1.f / Length);
		const FVector3 Magnitude{std::abs(Normal.X), std::abs(Normal.Y), std::abs(Normal.Z)};
		const int Axis = Magnitude.X >= Magnitude.Y && Magnitude.X >= Magnitude.Z ? 0 : Magnitude.Y >= Magnitude.Z ? 1
		                                                                                                            : 2;
		// The projection's U axis, made perpendicular to the face as normal mapping needs.
		const FVector3 Projected = Axis == 0 ? FVector3{0.f, 0.f, 1.f} : FVector3{1.f, 0.f, 0.f};
		const FVector3 Tangent = (Projected - Normal * Normal.Dot(Projected)).Normalized();
		const auto First = static_cast<std::uint32_t>(Model.Vertices.size());
		for (const std::uint32_t Point : Face)
		{
			const FVector3& Position = Chunk.Points[Point];
			const float U = (Axis == 0 ? Position.Z : Position.X) * Scale + 0.5f;
			const float V = (Axis == 1 ? Position.Z : Position.Y) * -Scale + 0.5f;
			Model.Vertices.push_back({.Position = {Position.X, Position.Y, Position.Z}, .UV = {U, V}, .Normal = {Normal.X, Normal.Y, Normal.Z}, .Tangent = {Tangent.X, Tangent.Y, Tangent.Z, 1.f}});
		}

		for (std::uint32_t Index = 1; Index + 1 < Face.size(); ++Index)
		{
			Model.Indices.insert(Model.Indices.end(), {First, First + Index, First + Index + 1});
		}
	}

	Model.Sections.push_back({.FirstIndex = 0, .IndexCount = static_cast<std::uint32_t>(Model.Indices.size()), .Material = 0});
	Model.Materials.push_back({.Name = "Fracture"});
	return Model;
}
}
