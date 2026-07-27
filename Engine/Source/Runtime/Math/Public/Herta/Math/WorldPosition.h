#pragma once

#include "Herta/Math/Vector.h"

namespace Herta
{
struct FWorldPosition
{
	FVector3d Meters = FVector3d::Zero();

	constexpr FWorldPosition() = default;
	explicit constexpr FWorldPosition(const FVector3d& InMeters)
	    : Meters(InMeters)
	{
	}

	constexpr FWorldPosition(double X, double Y, double Z)
	    : Meters(X, Y, Z)
	{
	}

	[[nodiscard]] constexpr FVector3d RelativeTo(const FWorldPosition& Origin) const
	{
		return Meters - Origin.Meters;
	}

	[[nodiscard]] constexpr FWorldPosition TranslatedBy(const FVector3d& DeltaMeters) const
	{
		return FWorldPosition{Meters + DeltaMeters};
	}

	[[nodiscard]] constexpr bool operator==(const FWorldPosition&) const = default;
};

[[nodiscard]] constexpr FVector3 WorldToOriginRelative(const FWorldPosition& Position, const FWorldPosition& Origin)
{
	return FVector3{Position.RelativeTo(Origin)};
}

[[nodiscard]] constexpr FWorldPosition OriginRelativeToWorld(const FVector3& Position, const FWorldPosition& Origin)
{
	return Origin.TranslatedBy(FVector3d{Position});
}
}
