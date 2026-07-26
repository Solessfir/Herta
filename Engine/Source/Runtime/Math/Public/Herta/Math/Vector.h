#pragma once

#include <cassert>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <limits>
#include <utility>

namespace Herta
{
template <std::floating_point T> struct TVector2
{
	T X = T{0};
	T Y = T{0};

	constexpr TVector2() = default;
	constexpr TVector2(T InX, T InY)
	    : X(InX)
	    , Y(InY)
	{
	}

	template <std::floating_point U>
	explicit constexpr TVector2(const TVector2<U>& Other)
	    : X(static_cast<T>(Other.X))
	    , Y(static_cast<T>(Other.Y))
	{
	}

	[[nodiscard]] static constexpr TVector2 Zero()
	{
		return {};
	}
	[[nodiscard]] static constexpr TVector2 One()
	{
		return {T{1}, T{1}};
	}

	[[nodiscard]] constexpr T& operator[](std::size_t Index)
	{
		assert(Index < 2);
		if (Index >= 2)
		{
			std::unreachable();
		}
		return Index == 0 ? X : Y;
	}
	[[nodiscard]] constexpr const T& operator[](std::size_t Index) const
	{
		assert(Index < 2);
		if (Index >= 2)
		{
			std::unreachable();
		}
		return Index == 0 ? X : Y;
	}

	[[nodiscard]] constexpr TVector2 operator-() const
	{
		return {-X, -Y};
	}
	[[nodiscard]] constexpr TVector2 operator+(const TVector2& Other) const
	{
		return {X + Other.X, Y + Other.Y};
	}
	[[nodiscard]] constexpr TVector2 operator-(const TVector2& Other) const
	{
		return {X - Other.X, Y - Other.Y};
	}
	[[nodiscard]] constexpr TVector2 operator*(T Scalar) const
	{
		return {X * Scalar, Y * Scalar};
	}
	[[nodiscard]] constexpr TVector2 operator/(T Scalar) const
	{
		return {X / Scalar, Y / Scalar};
	}

	constexpr TVector2& operator+=(const TVector2& Other)
	{
		X += Other.X;
		Y += Other.Y;
		return *this;
	}
	constexpr TVector2& operator-=(const TVector2& Other)
	{
		X -= Other.X;
		Y -= Other.Y;
		return *this;
	}
	constexpr TVector2& operator*=(T Scalar)
	{
		X *= Scalar;
		Y *= Scalar;
		return *this;
	}
	constexpr TVector2& operator/=(T Scalar)
	{
		X /= Scalar;
		Y /= Scalar;
		return *this;
	}

	[[nodiscard]] constexpr T Dot(const TVector2& Other) const
	{
		return X * Other.X + Y * Other.Y;
	}
	[[nodiscard]] constexpr T LengthSquared() const
	{
		return Dot(*this);
	}
	[[nodiscard]] T Length() const
	{
		return std::sqrt(LengthSquared());
	}

	[[nodiscard]] TVector2 Normalized() const
	{
		const T Magnitude = Length();
		return Magnitude > std::numeric_limits<T>::epsilon() ? *this / Magnitude : Zero();
	}

	[[nodiscard]] bool IsNearlyEqual(const TVector2& Other, T Tolerance) const
	{
		return std::abs(X - Other.X) <= Tolerance && std::abs(Y - Other.Y) <= Tolerance;
	}

	[[nodiscard]] constexpr bool operator==(const TVector2&) const = default;
};

template <std::floating_point T> [[nodiscard]] constexpr TVector2<T> operator*(T Scalar, const TVector2<T>& Vector)
{
	return Vector * Scalar;
}

template <std::floating_point T> struct TVector3
{
	T X = T{0};
	T Y = T{0};
	T Z = T{0};

	constexpr TVector3() = default;
	constexpr TVector3(T InX, T InY, T InZ)
	    : X(InX)
	    , Y(InY)
	    , Z(InZ)
	{
	}

	template <std::floating_point U>
	explicit constexpr TVector3(const TVector3<U>& Other)
	    : X(static_cast<T>(Other.X))
	    , Y(static_cast<T>(Other.Y))
	    , Z(static_cast<T>(Other.Z))
	{
	}

	[[nodiscard]] static constexpr TVector3 Zero()
	{
		return {};
	}
	[[nodiscard]] static constexpr TVector3 One()
	{
		return {T{1}, T{1}, T{1}};
	}
	[[nodiscard]] static constexpr TVector3 Left()
	{
		return {T{1}, T{0}, T{0}};
	}
	[[nodiscard]] static constexpr TVector3 Right()
	{
		return {T{-1}, T{0}, T{0}};
	}
	[[nodiscard]] static constexpr TVector3 Up()
	{
		return {T{0}, T{1}, T{0}};
	}
	[[nodiscard]] static constexpr TVector3 Down()
	{
		return {T{0}, T{-1}, T{0}};
	}
	[[nodiscard]] static constexpr TVector3 Forward()
	{
		return {T{0}, T{0}, T{1}};
	}
	[[nodiscard]] static constexpr TVector3 Backward()
	{
		return {T{0}, T{0}, T{-1}};
	}

	[[nodiscard]] constexpr T& operator[](std::size_t Index)
	{
		assert(Index < 3);
		if (Index >= 3)
		{
			std::unreachable();
		}
		return Index == 0 ? X : (Index == 1 ? Y : Z);
	}

	[[nodiscard]] constexpr const T& operator[](std::size_t Index) const
	{
		assert(Index < 3);
		if (Index >= 3)
		{
			std::unreachable();
		}
		return Index == 0 ? X : (Index == 1 ? Y : Z);
	}

	[[nodiscard]] constexpr TVector3 operator-() const
	{
		return {-X, -Y, -Z};
	}
	[[nodiscard]] constexpr TVector3 operator+(const TVector3& Other) const
	{
		return {X + Other.X, Y + Other.Y, Z + Other.Z};
	}
	[[nodiscard]] constexpr TVector3 operator-(const TVector3& Other) const
	{
		return {X - Other.X, Y - Other.Y, Z - Other.Z};
	}
	[[nodiscard]] constexpr TVector3 operator*(T Scalar) const
	{
		return {X * Scalar, Y * Scalar, Z * Scalar};
	}
	[[nodiscard]] constexpr TVector3 operator/(T Scalar) const
	{
		return {X / Scalar, Y / Scalar, Z / Scalar};
	}

	constexpr TVector3& operator+=(const TVector3& Other)
	{
		X += Other.X;
		Y += Other.Y;
		Z += Other.Z;
		return *this;
	}
	constexpr TVector3& operator-=(const TVector3& Other)
	{
		X -= Other.X;
		Y -= Other.Y;
		Z -= Other.Z;
		return *this;
	}
	constexpr TVector3& operator*=(T Scalar)
	{
		X *= Scalar;
		Y *= Scalar;
		Z *= Scalar;
		return *this;
	}
	constexpr TVector3& operator/=(T Scalar)
	{
		X /= Scalar;
		Y /= Scalar;
		Z /= Scalar;
		return *this;
	}

	[[nodiscard]] constexpr T Dot(const TVector3& Other) const
	{
		return X * Other.X + Y * Other.Y + Z * Other.Z;
	}

	[[nodiscard]] constexpr TVector3 Cross(const TVector3& Other) const
	{
		return {Y * Other.Z - Z * Other.Y, Z * Other.X - X * Other.Z, X * Other.Y - Y * Other.X};
	}

	[[nodiscard]] constexpr TVector3 ComponentMultiply(const TVector3& Other) const
	{
		return {X * Other.X, Y * Other.Y, Z * Other.Z};
	}

	[[nodiscard]] constexpr T LengthSquared() const
	{
		return Dot(*this);
	}
	[[nodiscard]] T Length() const
	{
		return std::sqrt(LengthSquared());
	}

	[[nodiscard]] TVector3 Normalized() const
	{
		const T Magnitude = Length();
		return Magnitude > std::numeric_limits<T>::epsilon() ? *this / Magnitude : Zero();
	}

	[[nodiscard]] bool IsNearlyEqual(const TVector3& Other, T Tolerance) const
	{
		return std::abs(X - Other.X) <= Tolerance && std::abs(Y - Other.Y) <= Tolerance &&
		       std::abs(Z - Other.Z) <= Tolerance;
	}

	[[nodiscard]] constexpr bool operator==(const TVector3&) const = default;
};

template <std::floating_point T> [[nodiscard]] constexpr TVector3<T> operator*(T Scalar, const TVector3<T>& Vector)
{
	return Vector * Scalar;
}

template <std::floating_point T> struct TVector4
{
	T X = T{0};
	T Y = T{0};
	T Z = T{0};
	T W = T{0};

	constexpr TVector4() = default;
	constexpr TVector4(T InX, T InY, T InZ, T InW)
	    : X(InX)
	    , Y(InY)
	    , Z(InZ)
	    , W(InW)
	{
	}

	constexpr TVector4(const TVector3<T>& Vector, T InW)
	    : X(Vector.X)
	    , Y(Vector.Y)
	    , Z(Vector.Z)
	    , W(InW)
	{
	}

	template <std::floating_point U>
	explicit constexpr TVector4(const TVector4<U>& Other)
	    : X(static_cast<T>(Other.X))
	    , Y(static_cast<T>(Other.Y))
	    , Z(static_cast<T>(Other.Z))
	    , W(static_cast<T>(Other.W))
	{
	}

	[[nodiscard]] static constexpr TVector4 Zero()
	{
		return {};
	}
	[[nodiscard]] static constexpr TVector4 One()
	{
		return {T{1}, T{1}, T{1}, T{1}};
	}

	[[nodiscard]] constexpr T& operator[](std::size_t Index)
	{
		assert(Index < 4);
		if (Index >= 4)
		{
			std::unreachable();
		}
		if (Index == 0)
			return X;
		if (Index == 1)
			return Y;
		return Index == 2 ? Z : W;
	}

	[[nodiscard]] constexpr const T& operator[](std::size_t Index) const
	{
		assert(Index < 4);
		if (Index >= 4)
		{
			std::unreachable();
		}
		if (Index == 0)
			return X;
		if (Index == 1)
			return Y;
		return Index == 2 ? Z : W;
	}

	[[nodiscard]] constexpr TVector4 operator-() const
	{
		return {-X, -Y, -Z, -W};
	}
	[[nodiscard]] constexpr TVector4 operator+(const TVector4& Other) const
	{
		return {X + Other.X, Y + Other.Y, Z + Other.Z, W + Other.W};
	}
	[[nodiscard]] constexpr TVector4 operator-(const TVector4& Other) const
	{
		return {X - Other.X, Y - Other.Y, Z - Other.Z, W - Other.W};
	}
	[[nodiscard]] constexpr TVector4 operator*(T Scalar) const
	{
		return {X * Scalar, Y * Scalar, Z * Scalar, W * Scalar};
	}
	[[nodiscard]] constexpr TVector4 operator/(T Scalar) const
	{
		return {X / Scalar, Y / Scalar, Z / Scalar, W / Scalar};
	}

	constexpr TVector4& operator+=(const TVector4& Other)
	{
		X += Other.X;
		Y += Other.Y;
		Z += Other.Z;
		W += Other.W;
		return *this;
	}
	constexpr TVector4& operator-=(const TVector4& Other)
	{
		X -= Other.X;
		Y -= Other.Y;
		Z -= Other.Z;
		W -= Other.W;
		return *this;
	}
	constexpr TVector4& operator*=(T Scalar)
	{
		X *= Scalar;
		Y *= Scalar;
		Z *= Scalar;
		W *= Scalar;
		return *this;
	}
	constexpr TVector4& operator/=(T Scalar)
	{
		X /= Scalar;
		Y /= Scalar;
		Z /= Scalar;
		W /= Scalar;
		return *this;
	}

	[[nodiscard]] constexpr T Dot(const TVector4& Other) const
	{
		return X * Other.X + Y * Other.Y + Z * Other.Z + W * Other.W;
	}
	[[nodiscard]] constexpr T LengthSquared() const
	{
		return Dot(*this);
	}
	[[nodiscard]] T Length() const
	{
		return std::sqrt(LengthSquared());
	}

	[[nodiscard]] TVector4 Normalized() const
	{
		const T Magnitude = Length();
		return Magnitude > std::numeric_limits<T>::epsilon() ? *this / Magnitude : Zero();
	}

	[[nodiscard]] bool IsNearlyEqual(const TVector4& Other, T Tolerance) const
	{
		return std::abs(X - Other.X) <= Tolerance && std::abs(Y - Other.Y) <= Tolerance &&
		       std::abs(Z - Other.Z) <= Tolerance && std::abs(W - Other.W) <= Tolerance;
	}

	[[nodiscard]] constexpr bool operator==(const TVector4&) const = default;
};

template <std::floating_point T> [[nodiscard]] constexpr TVector4<T> operator*(T Scalar, const TVector4<T>& Vector)
{
	return Vector * Scalar;
}

using FVector2 = TVector2<float>;
using FVector3 = TVector3<float>;
using FVector4 = TVector4<float>;
using FVector3d = TVector3<double>;
}
