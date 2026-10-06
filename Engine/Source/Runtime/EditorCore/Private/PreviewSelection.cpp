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
    FVector3{-1.f, -1.f, -1.f},
    FVector3{1.f, -1.f, -1.f},
    FVector3{1.f, 1.f, -1.f},
    FVector3{-1.f, 1.f, -1.f},
    FVector3{-1.f, -1.f, 1.f},
    FVector3{1.f, -1.f, 1.f},
    FVector3{1.f, 1.f, 1.f},
    FVector3{-1.f, 1.f, 1.f},
};

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
	}) || Model(3, 0) != 0.f
	    || Model(3, 1) != 0.f || Model(3, 2) != 0.f || Model(3, 3) != 1.f)
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

std::optional<double> HitTestPreviewCube(const FViewportPickingRay& Ray, const FMatrix4& Model)
{
	const auto WorldToLocal = GetPreviewWorldToLocal(Model);
	if (!WorldToLocal || !IsFinitePreviewVector(Ray.Origin) || !IsFinitePreviewVector(Ray.Direction) || Ray.Direction == FVector3::Zero())
	{
		return std::nullopt;
	}

	const FVector3 Origin = WorldToLocal->TransformPosition(Ray.Origin);
	const FVector3 Direction = WorldToLocal->TransformVector(Ray.Direction);
	if (!IsFinitePreviewVector(Origin) || !IsFinitePreviewVector(Direction) || Direction == FVector3::Zero())
	{
		return std::nullopt;
	}

	double NearDistance = 0.0;
	double FarDistance = std::numeric_limits<double>::infinity();
	for (std::size_t Axis = 0; Axis < 3; ++Axis)
	{
		if (Direction[Axis] == 0.f)
		{
			if (Origin[Axis] < -1.f || Origin[Axis] > 1.f)
			{
				return std::nullopt;
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
			return std::nullopt;
		}
	}

	return NearDistance;
}

bool IntersectsPreviewCubeSelectionRect(const FMatrix4& ViewProjection, const FMatrix4& Model, const FVector2 First, const FVector2 Second)
{
	if (!std::isfinite(First.X) || !std::isfinite(First.Y) || !std::isfinite(Second.X) || !std::isfinite(Second.Y) || !std::ranges::all_of(ViewProjection.Data(), [](const float Element)
	{
		return std::isfinite(Element);
	}) || !std::ranges::all_of(Model.Data(), [](const float Element)
	{
		return std::isfinite(Element);
	}) || Model(3, 0) != 0.f
	    || Model(3, 1) != 0.f || Model(3, 2) != 0.f || Model(3, 3) != 1.f)
	{
		return false;
	}

	TMatrix4<double> Projection;
	TMatrix4<double> Bounds;

	for (std::size_t Column = 0; Column < 4; ++Column)
	{
		for (std::size_t Row = 0; Row < 4; ++Row)
		{
			Projection(Row, Column) = ViewProjection(Row, Column);
			Bounds(Row, Column) = Model(Row, Column);
		}
	}

	const auto ClipTransform = Projection * Bounds;
	std::array<TVector4<double>, CubeCorners.size()> ClipCorners;

	for (std::size_t Index = 0; Index < CubeCorners.size(); ++Index)
	{
		ClipCorners[Index] = ClipTransform * TVector4<double>{TVector3<double>(CubeCorners[Index]), 1.0};
	}

	constexpr double Infinity = std::numeric_limits<double>::infinity();
	TVector2<double> Minimum{Infinity, Infinity};
	TVector2<double> Maximum{-Infinity, -Infinity};
	bool bProjected = false;

	const auto Include = [&](const TVector4<double>& Clip)
	{
		if (Clip.W <= 0.0)
		{
			return;
		}

		const TVector2<double> Screen{(Clip.X / Clip.W + 1.0) * 0.5, (1.0 - Clip.Y / Clip.W) * 0.5};
		if (!std::isfinite(Screen.X) || !std::isfinite(Screen.Y))
		{
			return;
		}

		Minimum = {std::min(Minimum.X, Screen.X), std::min(Minimum.Y, Screen.Y)};
		Maximum = {std::max(Maximum.X, Screen.X), std::max(Maximum.Y, Screen.Y)};
		bProjected = true;
	};

	for (const auto& Edge : CubeEdges)
	{
		const auto& Start = ClipCorners[Edge[0]];
		const auto& End = ClipCorners[Edge[1]];
		const std::array StartDepth{Start.Z, Start.W - Start.Z};
		const std::array EndDepth{End.Z, End.W - End.Z};
		double Entry = 0.0;
		double Exit = 1.0;

		// Clip before division: corners behind the eye must not mirror or inflate the selection bounds.
		for (std::size_t Plane = 0; Plane < StartDepth.size(); ++Plane)
		{
			if (StartDepth[Plane] < 0.0 && EndDepth[Plane] < 0.0)
			{
				Exit = -1.0;
				break;
			}

			if (StartDepth[Plane] < 0.0)
			{
				Entry = std::max(Entry, StartDepth[Plane] / (StartDepth[Plane] - EndDepth[Plane]));
			}
			else if (EndDepth[Plane] < 0.0)
			{
				Exit = std::min(Exit, StartDepth[Plane] / (StartDepth[Plane] - EndDepth[Plane]));
			}
		}

		if (Entry <= Exit)
		{
			Include(Start + (End - Start) * Entry);
			Include(Start + (End - Start) * Exit);
		}
	}

	return bProjected && Minimum.X <= std::max(First.X, Second.X) && Maximum.X >= std::min(First.X, Second.X) && Minimum.Y <= std::max(First.Y, Second.Y) && Maximum.Y >= std::min(First.Y, Second.Y);
}
}
