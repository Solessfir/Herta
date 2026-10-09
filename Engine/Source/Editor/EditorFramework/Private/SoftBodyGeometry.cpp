#include "SoftBodyGeometry.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <numbers>
#include <optional>
#include <utility>

namespace Herta
{
namespace
{
constexpr std::uint32_t RopeSides = 8;
constexpr std::uint32_t BallSubdivisions = 2;

[[nodiscard]] std::uint32_t GetSegmentCount(const float Length, const std::uint32_t Minimum, const std::uint32_t Maximum)
{
	return std::clamp(static_cast<std::uint32_t>(std::lround(Length / SoftBodyVertexSpacing)), Minimum, Maximum);
}

[[nodiscard]] std::pair<std::uint32_t, std::uint32_t> GetClothResolution(const FSoftBodyComponent& SoftBody)
{
	return {GetSegmentCount(SoftBody.Length, 1, 64), GetSegmentCount(SoftBody.Height, 1, 64)};
}

[[nodiscard]] FVector3 SafeNormalized(const FVector3& Vector, const FVector3& Fallback)
{
	const float Length = Vector.Length();
	return Length > 1e-8f ? Vector * (1.f / Length) : Fallback;
}

[[nodiscard]] FVector3 AnyPerpendicular(const FVector3& Direction)
{
	const FVector3 Reference = std::abs(Direction.Y) < 0.9f ? FVector3::Up() : FVector3{1.f, 0.f, 0.f};
	return SafeNormalized(Reference.Cross(Direction), {1.f, 0.f, 0.f});
}

void BuildRope(const FSoftBodyComponent& SoftBody, FSoftBodyTopology& Topology)
{
	const std::uint32_t Segments = GetSegmentCount(SoftBody.Length, 2, 256);
	for (std::uint32_t Index = 0; Index <= Segments; ++Index)
	{
		Topology.Vertices.push_back({0.f, -SoftBody.Length * static_cast<float>(Index) / static_cast<float>(Segments), 0.f});
		if (Index > 0)
		{
			Topology.StretchEdges.push_back({Index - 1, Index});
		}

		if (Index > 1)
		{
			Topology.BendEdges.push_back({Index - 2, Index});
		}
	}

	if (SoftBody.bPinned)
	{
		Topology.Pinned.push_back(0);
	}
}

void BuildCloth(const FSoftBodyComponent& SoftBody, FSoftBodyTopology& Topology)
{
	const auto [Columns, Rows] = GetClothResolution(SoftBody);
	for (std::uint32_t Row = 0; Row <= Rows; ++Row)
	{
		for (std::uint32_t Column = 0; Column <= Columns; ++Column)
		{
			Topology.Vertices.push_back({SoftBody.Length * (static_cast<float>(Column) / static_cast<float>(Columns) - 0.5f), -SoftBody.Height * static_cast<float>(Row) / static_cast<float>(Rows), 0.f});
		}
	}

	const auto Vertex = [Columns](const std::uint32_t Column, const std::uint32_t Row)
	{
		return Row * (Columns + 1) + Column;
	};

	// Both triangles face +Z, the sheet's front side.
	for (std::uint32_t Row = 0; Row < Rows; ++Row)
	{
		for (std::uint32_t Column = 0; Column < Columns; ++Column)
		{
			Topology.Faces.push_back({Vertex(Column, Row), Vertex(Column, Row + 1), Vertex(Column + 1, Row)});
			Topology.Faces.push_back({Vertex(Column + 1, Row), Vertex(Column, Row + 1), Vertex(Column + 1, Row + 1)});
		}
	}

	if (SoftBody.bPinned)
	{
		Topology.Pinned = {Vertex(0, 0), Vertex(Columns, 0)};
	}
}

void BuildBall(const FSoftBodyComponent& SoftBody, FSoftBodyTopology& Topology)
{
	const float Golden = (1.f + std::sqrt(5.f)) * 0.5f;
	std::vector<FVector3> Directions{{-1.f, Golden, 0.f}, {1.f, Golden, 0.f}, {-1.f, -Golden, 0.f}, {1.f, -Golden, 0.f}, {0.f, -1.f, Golden}, {0.f, 1.f, Golden}, {0.f, -1.f, -Golden}, {0.f, 1.f, -Golden}, {Golden, 0.f, -1.f}, {Golden, 0.f, 1.f}, {-Golden, 0.f, -1.f}, {-Golden, 0.f, 1.f}};
	std::vector<std::array<std::uint32_t, 3>> Faces{{0, 11, 5}, {0, 5, 1}, {0, 1, 7}, {0, 7, 10}, {0, 10, 11}, {1, 5, 9}, {5, 11, 4}, {11, 10, 2}, {10, 7, 6}, {7, 1, 8}, {3, 9, 4}, {3, 4, 2}, {3, 2, 6}, {3, 6, 8}, {3, 8, 9}, {4, 9, 5}, {2, 4, 11}, {6, 2, 10}, {8, 6, 7}, {9, 8, 1}};
	for (FVector3& Direction : Directions)
	{
		Direction = Direction.Normalized();
	}

	for (std::uint32_t Level = 0; Level < BallSubdivisions; ++Level)
	{
		std::map<std::pair<std::uint32_t, std::uint32_t>, std::uint32_t> Midpoints;
		const auto Midpoint = [&](const std::uint32_t First, const std::uint32_t Second)
		{
			const auto Key = std::minmax(First, Second);
			const auto [Found, bInserted] = Midpoints.try_emplace(Key, static_cast<std::uint32_t>(Directions.size()));
			if (bInserted)
			{
				Directions.push_back((Directions[First] + Directions[Second]).Normalized());
			}

			return Found->second;
		};

		std::vector<std::array<std::uint32_t, 3>> Refined;
		for (const auto& [A, B, C] : Faces)
		{
			const std::uint32_t AB = Midpoint(A, B);
			const std::uint32_t BC = Midpoint(B, C);
			const std::uint32_t CA = Midpoint(C, A);
			Refined.insert(Refined.end(), {{A, AB, CA}, {B, BC, AB}, {C, CA, BC}, {AB, BC, CA}});
		}

		Faces = std::move(Refined);
	}

	const float Radius = SoftBody.Length * 0.5f;
	for (const FVector3& Direction : Directions)
	{
		Topology.Vertices.push_back(Direction * Radius);
	}

	for (const auto& [A, B, C] : Faces)
	{
		const FVector3 Normal = (Directions[B] - Directions[A]).Cross(Directions[C] - Directions[A]);
		Topology.Faces.push_back(Normal.Dot(Directions[A]) >= 0.f ? std::array{A, B, C} : std::array{A, C, B});
	}
}

struct FModelBuilder
{
	FCookedModel Model;

	std::uint32_t Add(const FVector3& Position, const FVector3& Normal, const float U, const float V)
	{
		const FVector3 Tangent = AnyPerpendicular(Normal);
		Model.Vertices.push_back({.Position = {Position.X, Position.Y, Position.Z}, .UV = {U, V}, .Normal = {Normal.X, Normal.Y, Normal.Z}, .Tangent = {Tangent.X, Tangent.Y, Tangent.Z, 1.f}});
		return static_cast<std::uint32_t>(Model.Vertices.size() - 1);
	}

	// Winds each triangle so its geometric normal agrees with the vertex normals the shading uses.
	void Triangle(const std::uint32_t A, const std::uint32_t B, const std::uint32_t C)
	{
		const auto Position = [this](const std::uint32_t Index)
		{
			const auto& Value = Model.Vertices[Index].Position;
			return FVector3{Value[0], Value[1], Value[2]};
		};

		const auto Normal = [this](const std::uint32_t Index)
		{
			const auto& Value = Model.Vertices[Index].Normal;
			return FVector3{Value[0], Value[1], Value[2]};
		};

		const FVector3 Face = (Position(B) - Position(A)).Cross(Position(C) - Position(A));
		const bool bFlip = Face.Dot(Normal(A) + Normal(B) + Normal(C)) < 0.f;
		Model.Indices.insert(Model.Indices.end(), {A, bFlip ? C : B, bFlip ? B : C});
	}
};

[[nodiscard]] std::vector<FVector3> GetVertexNormals(const FSoftBodyTopology& Topology, const std::span<const FVector3> Positions)
{
	std::vector<FVector3> Normals(Positions.size());
	for (const auto& [A, B, C] : Topology.Faces)
	{
		// The unnormalized cross product weights each face by its area.
		const FVector3 Face = (Positions[B] - Positions[A]).Cross(Positions[C] - Positions[A]);
		Normals[A] = Normals[A] + Face;
		Normals[B] = Normals[B] + Face;
		Normals[C] = Normals[C] + Face;
	}

	for (FVector3& Normal : Normals)
	{
		Normal = SafeNormalized(Normal, FVector3::Up());
	}

	return Normals;
}

void BuildRopeModel(const FSoftBodyComponent& SoftBody, const std::span<const FVector3> Positions, FModelBuilder& Builder)
{
	const auto Count = static_cast<std::uint32_t>(Positions.size());
	FVector3 Normal;
	float Distance = 0.f;
	for (std::uint32_t Index = 0; Index < Count; ++Index)
	{
		const FVector3 Previous = Positions[Index == 0 ? 0 : Index - 1];
		const FVector3 Next = Positions[Index + 1 == Count ? Index : Index + 1];
		const FVector3 Tangent = SafeNormalized(Next - Previous, {0.f, -1.f, 0.f});
		// Parallel transport keeps the tube from twisting between rings.
		Normal = Index == 0 ? AnyPerpendicular(Tangent) : SafeNormalized(Normal - Tangent * Normal.Dot(Tangent), AnyPerpendicular(Tangent));
		const FVector3 Binormal = Tangent.Cross(Normal);
		if (Index > 0)
		{
			Distance += (Positions[Index] - Positions[Index - 1]).Length();
		}

		for (std::uint32_t Side = 0; Side <= RopeSides; ++Side)
		{
			const float Angle = 2.f * std::numbers::pi_v<float> * static_cast<float>(Side) / static_cast<float>(RopeSides);
			const FVector3 Radial = Normal * std::cos(Angle) + Binormal * std::sin(Angle);
			Builder.Add(Positions[Index] + Radial * SoftBody.Thickness, Radial, static_cast<float>(Side) / static_cast<float>(RopeSides), Distance / (2.f * std::numbers::pi_v<float> * SoftBody.Thickness));
		}
	}

	for (std::uint32_t Index = 0; Index + 1 < Count; ++Index)
	{
		for (std::uint32_t Side = 0; Side < RopeSides; ++Side)
		{
			const std::uint32_t A = Index * (RopeSides + 1) + Side;
			const std::uint32_t B = A + RopeSides + 1;
			Builder.Triangle(A, B, A + 1);
			Builder.Triangle(A + 1, B, B + 1);
		}
	}
}
}

FSoftBodyTopology BuildSoftBodyTopology(const FSoftBodyComponent& SoftBody)
{
	FSoftBodyTopology Topology;
	switch (SoftBody.Shape)
	{
		case ESoftBodyShape::Rope:
			BuildRope(SoftBody, Topology);
			break;
		case ESoftBodyShape::Cloth:
			BuildCloth(SoftBody, Topology);
			break;
		case ESoftBodyShape::Ball:
			BuildBall(SoftBody, Topology);
			break;
	}

	return Topology;
}

FPhysicsSoftBodySettings MakeSoftBodyPhysicsSettings(const FSoftBodyComponent& SoftBody, const FSoftBodyTopology& Topology, const std::span<const FVector3> WorldVertices)
{
	// Stiffness maps to XPBD compliance on a squared curve, so most of the slider's travel is in the believable range.
	const float Slack = 1.f - std::clamp(SoftBody.Stiffness, 0.f, 1.f);
	std::optional<FPhysicsSoftBodyAttachment> Attachment;
	if (SoftBody.Shape == ESoftBodyShape::Rope && SoftBody.Attachment.IsValid() && !WorldVertices.empty())
	{
		// The caller resolves the body; a pinned rope also carries its weight.
		Attachment = FPhysicsSoftBodyAttachment{
		    .Vertex = static_cast<std::uint32_t>(WorldVertices.size() - 1),
		    .Point = WorldVertices.back(),
		    .TetherVertex = SoftBody.bPinned ? std::optional<std::uint32_t>{0} : std::nullopt,
		    .TetherLength = SoftBody.Length,
		};
	}

	return {
	    .Vertices = WorldVertices,
	    .PinnedVertices = Topology.Pinned,
	    .StretchEdges = Topology.StretchEdges,
	    .BendEdges = Topology.BendEdges,
	    .Faces = Topology.Faces,
	    .MassKg = SoftBody.MassKg,
	    .StretchCompliance = 0.001f * Slack * Slack,
	    .BendCompliance = 0.05f * Slack * Slack + 0.00001f,
	    .VertexRadius = SoftBody.Shape == ESoftBodyShape::Rope ? SoftBody.Thickness : std::min(SoftBody.Thickness, 0.05f),
	    .Pressure = SoftBody.Shape == ESoftBodyShape::Ball ? SoftBody.Pressure : 0.f,
	    .Friction = SoftBody.Friction,
	    .Iterations = SoftBody.Shape == ESoftBodyShape::Rope ? 16u : 8u,
	    .Attachment = Attachment,
	};
}

FCookedModel BuildSoftBodyModel(const FSoftBodyComponent& SoftBody, const FSoftBodyTopology& Topology, const std::span<const FVector3> Positions)
{
	FModelBuilder Builder;
	if (SoftBody.Shape == ESoftBodyShape::Rope)
	{
		BuildRopeModel(SoftBody, Positions, Builder);
	}
	else
	{
		const std::vector<FVector3> Normals = GetVertexNormals(Topology, Positions);
		const auto [Columns, Rows] = GetClothResolution(SoftBody);
		const auto UV = [&](const std::size_t Index) -> std::pair<float, float>
		{
			if (SoftBody.Shape == ESoftBodyShape::Cloth)
			{
				return {static_cast<float>(Index % (Columns + 1)) / static_cast<float>(Columns), static_cast<float>(Index / (Columns + 1)) / static_cast<float>(Rows)};
			}

			const FVector3 Rest = SafeNormalized(Topology.Vertices[Index], FVector3::Up());
			return {std::atan2(Rest.Z, Rest.X) / (2.f * std::numbers::pi_v<float>)+0.5f, std::acos(std::clamp(Rest.Y, -1.f, 1.f)) / std::numbers::pi_v<float>};
		};

		for (std::size_t Index = 0; Index < Positions.size(); ++Index)
		{
			const auto [U, V] = UV(Index);
			Builder.Add(Positions[Index], Normals[Index], U, V);
		}

		for (const auto& [A, B, C] : Topology.Faces)
		{
			Builder.Triangle(A, B, C);
		}

		// Cloth is visible from behind, so the back gets its own vertices with opposite normals.
		if (SoftBody.Shape == ESoftBodyShape::Cloth)
		{
			const auto Offset = static_cast<std::uint32_t>(Positions.size());
			for (std::size_t Index = 0; Index < Positions.size(); ++Index)
			{
				const auto [U, V] = UV(Index);
				Builder.Add(Positions[Index], -Normals[Index], U, V);
			}

			for (const auto& [A, B, C] : Topology.Faces)
			{
				Builder.Triangle(Offset + A, Offset + C, Offset + B);
			}
		}
	}

	Builder.Model.Sections.push_back({.FirstIndex = 0, .IndexCount = static_cast<std::uint32_t>(Builder.Model.Indices.size()), .Material = 0});
	Builder.Model.Materials.push_back({.Name = "Soft body"});
	return std::move(Builder.Model);
}
}
