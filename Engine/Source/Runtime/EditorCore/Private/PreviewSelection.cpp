#include "Herta/EditorCore/PreviewSelection.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>

namespace Herta
{
namespace
{
constexpr std::array CubeCorners{
    FVector3{-1.0f, -1.0f, -1.0f}, FVector3{1.0f, -1.0f, -1.0f}, FVector3{1.0f, 1.0f, -1.0f}, FVector3{-1.0f, 1.0f, -1.0f}, FVector3{-1.0f, -1.0f, 1.0f}, FVector3{1.0f, -1.0f, 1.0f}, FVector3{1.0f, 1.0f, 1.0f}, FVector3{-1.0f, 1.0f, 1.0f}};

// Edge entries identify two corners and their adjacent faces: -X, +X, -Y, +Y, -Z, +Z.
constexpr std::array<std::array<std::size_t, 4>, 12> CubeEdges{{{0, 1, 2, 4}, {1, 2, 1, 4}, {2, 3, 3, 4}, {3, 0, 0, 4}, {4, 5, 2, 5}, {5, 6, 1, 5}, {6, 7, 3, 5}, {7, 4, 0, 5}, {0, 4, 0, 2}, {1, 5, 1, 2}, {2, 6, 1, 3}, {3, 7, 0, 3}}};

bool IsFinitePreviewVector(const FVector3& Value)
{
	return std::isfinite(Value.X) && std::isfinite(Value.Y) && std::isfinite(Value.Z);
}

std::optional<FMatrix4> GetPreviewWorldToLocal(const FMatrix4& Model)
{
	if (!std::ranges::all_of(Model.Data(), [](const float Element)
	                         {
		                         return std::isfinite(Element);
	                         }) ||
	    Model(3, 0) != 0.0f || Model(3, 1) != 0.0f || Model(3, 2) != 0.0f || Model(3, 3) != 1.0f)
	{
		return std::nullopt;
	}

	FMatrix4 Inverse;
	std::array<FVector3, 3> UnitAxes;
	const FVector3 Translation{Model(0, 3), Model(1, 3), Model(2, 3)};
	for (std::size_t AxisIndex = 0; AxisIndex < 3; ++AxisIndex)
	{
		const FVector3 Axis{Model(0, AxisIndex), Model(1, AxisIndex), Model(2, AxisIndex)};
		const float SquaredScale = Axis.LengthSquared();
		if (!std::isfinite(SquaredScale) || SquaredScale <= std::numeric_limits<float>::min())
		{
			return std::nullopt;
		}
		UnitAxes[AxisIndex] = Axis / std::sqrt(SquaredScale);
		const FVector3 InverseRow = Axis / SquaredScale;
		for (std::size_t Column = 0; Column < 3; ++Column)
		{
			Inverse(AxisIndex, Column) = InverseRow[Column];
		}
		Inverse(AxisIndex, 3) = -InverseRow.Dot(Translation);
	}
	if (std::abs(UnitAxes[0].Dot(UnitAxes[1])) > 0.0001f || std::abs(UnitAxes[0].Dot(UnitAxes[2])) > 0.0001f || std::abs(UnitAxes[1].Dot(UnitAxes[2])) > 0.0001f || !std::ranges::all_of(Inverse.Data(), [](const float Element)
	                                                                                                                                                                                     {
		                                                                                                                                                                                     return std::isfinite(Element);
	                                                                                                                                                                                     }))
	{
		return std::nullopt;
	}
	return Inverse;
}
}

bool HitTestPreviewCube(const FViewportPickingRay& Ray, const FMatrix4& Model)
{
	const auto WorldToLocal = GetPreviewWorldToLocal(Model);
	if (!WorldToLocal || !IsFinitePreviewVector(Ray.Origin) || !IsFinitePreviewVector(Ray.Direction) || Ray.Direction == FVector3::Zero())
	{
		return false;
	}
	const FVector3 Origin = WorldToLocal->TransformPosition(Ray.Origin);
	const FVector3 Direction = WorldToLocal->TransformVector(Ray.Direction);
	if (!IsFinitePreviewVector(Origin) || !IsFinitePreviewVector(Direction) || Direction == FVector3::Zero())
	{
		return false;
	}

	double NearDistance = 0.0;
	double FarDistance = std::numeric_limits<double>::infinity();
	for (std::size_t Axis = 0; Axis < 3; ++Axis)
	{
		if (Direction[Axis] == 0.0f)
		{
			if (Origin[Axis] < -1.0f || Origin[Axis] > 1.0f)
			{
				return false;
			}
			continue;
		}
		double First = (-1.0 - Origin[Axis]) / Direction[Axis];
		double Second = (1.0 - Origin[Axis]) / Direction[Axis];
		if (First > Second)
		{
			std::swap(First, Second);
		}
		NearDistance = std::max(NearDistance, First);
		FarDistance = std::min(FarDistance, Second);
		if (NearDistance > FarDistance)
		{
			return false;
		}
	}
	return true;
}

std::vector<std::pair<FVector3, FVector3>> GetPreviewCubeSilhouette(const FVector3& CameraPosition, const FMatrix4& Model)
{
	const auto WorldToLocal = GetPreviewWorldToLocal(Model);
	if (!WorldToLocal || !IsFinitePreviewVector(CameraPosition))
	{
		return {};
	}
	const FVector3 LocalCamera = WorldToLocal->TransformPosition(CameraPosition);
	if (!IsFinitePreviewVector(LocalCamera))
	{
		return {};
	}
	const std::array FrontFaces{LocalCamera.X < -1.0f, LocalCamera.X > 1.0f, LocalCamera.Y < -1.0f, LocalCamera.Y > 1.0f, LocalCamera.Z < -1.0f, LocalCamera.Z > 1.0f};
	std::vector<std::pair<FVector3, FVector3>> Edges;
	Edges.reserve(6);
	for (const auto& Edge : CubeEdges)
	{
		if (FrontFaces[Edge[2]] == FrontFaces[Edge[3]])
		{
			continue;
		}
		const FVector3 First = Model.TransformPosition(CubeCorners[Edge[0]]);
		const FVector3 Second = Model.TransformPosition(CubeCorners[Edge[1]]);
		if (!IsFinitePreviewVector(First) || !IsFinitePreviewVector(Second))
		{
			return {};
		}
		Edges.emplace_back(First, Second);
	}
	return Edges;
}
}
