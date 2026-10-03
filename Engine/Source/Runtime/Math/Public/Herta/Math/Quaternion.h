#pragma once

#include "Herta/Math/Vector.h"

#include <array>
#include <cmath>
#include <concepts>
#include <limits>

namespace Herta
{
template <std::floating_point T> struct TQuaternion
{
	T X = T{0};
	T Y = T{0};
	T Z = T{0};
	T W = T{1};

	constexpr TQuaternion() = default;

	constexpr TQuaternion(T InX, T InY, T InZ, T InW)
	    : X(InX)
	    , Y(InY)
	    , Z(InZ)
	    , W(InW)
	{
	}

	[[nodiscard]] static constexpr TQuaternion Identity()
	{
		return {};
	}

	[[nodiscard]] static constexpr TQuaternion FromXYZW(const std::array<T, 4>& Elements)
	{
		return {Elements[0], Elements[1], Elements[2], Elements[3]};
	}

	[[nodiscard]] constexpr std::array<T, 4> ToXYZW() const
	{
		return {X, Y, Z, W};
	}

	[[nodiscard]] static TQuaternion FromAxisAngle(const TVector3<T>& Axis, T AngleRadians)
	{
		const T AxisLength = Axis.Length();
		if (AxisLength <= std::numeric_limits<T>::epsilon())
		{
			return Identity();
		}

		const T HalfAngle = AngleRadians * T{0.5};
		const T Sine = std::sin(HalfAngle);
		const TVector3<T> NormalizedAxis = Axis / AxisLength;
		return {NormalizedAxis.X * Sine, NormalizedAxis.Y * Sine, NormalizedAxis.Z * Sine, std::cos(HalfAngle)};
	}

	[[nodiscard]] constexpr TQuaternion Conjugated() const
	{
		return {-X, -Y, -Z, W};
	}

	[[nodiscard]] constexpr T LengthSquared() const
	{
		return X * X + Y * Y + Z * Z + W * W;
	}

	[[nodiscard]] TQuaternion NormalizedOrIdentity() const
	{
		const T Magnitude = std::sqrt(LengthSquared());
		if (Magnitude <= std::numeric_limits<T>::epsilon())
		{
			return Identity();
		}

		return {X / Magnitude, Y / Magnitude, Z / Magnitude, W / Magnitude};
	}

	// For column vectors, A * B applies B first and then A.
	[[nodiscard]] constexpr TQuaternion operator*(const TQuaternion& Other) const
	{
		return {W * Other.X + X * Other.W + Y * Other.Z - Z * Other.Y,
		    W * Other.Y - X * Other.Z + Y * Other.W + Z * Other.X,
		    W * Other.Z + X * Other.Y - Y * Other.X + Z * Other.W,
		    W * Other.W - X * Other.X - Y * Other.Y - Z * Other.Z};
	}

	[[nodiscard]] TVector3<T> RotateVector(const TVector3<T>& Vector) const
	{
		const TQuaternion Rotation = NormalizedOrIdentity();
		const TVector3<T> QuaternionVector{Rotation.X, Rotation.Y, Rotation.Z};
		const TVector3<T> TwiceCross = T{2} * QuaternionVector.Cross(Vector);
		return Vector + Rotation.W * TwiceCross + QuaternionVector.Cross(TwiceCross);
	}

	[[nodiscard]] bool IsNearlyEqual(const TQuaternion& Other, T Tolerance) const
	{
		return std::abs(X - Other.X) <= Tolerance && std::abs(Y - Other.Y) <= Tolerance && std::abs(Z - Other.Z) <= Tolerance && std::abs(W - Other.W) <= Tolerance;
	}

	[[nodiscard]] constexpr bool operator==(const TQuaternion&) const = default;
};

using FQuaternion = TQuaternion<float>;
}
