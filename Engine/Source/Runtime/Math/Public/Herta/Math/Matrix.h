#pragma once

#include "Herta/Math/Quaternion.h"

#include <array>
#include <cassert>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <numbers>

namespace Herta
{
template <std::floating_point T> class TMatrix3
{
public:
	constexpr TMatrix3() = default;

	explicit constexpr TMatrix3(const std::array<T, 9>& InElements)
	    : Elements(InElements)
	{
	}

	[[nodiscard]] static constexpr TMatrix3 Identity()
	{
		return {};
	}

	[[nodiscard]] static constexpr TMatrix3 Zero()
	{
		return TMatrix3(std::array<T, 9>{});
	}

	[[nodiscard]] static constexpr TMatrix3 Scale(const TVector3<T>& Scale)
	{
		TMatrix3 Result;
		Result(0, 0) = Scale.X;
		Result(1, 1) = Scale.Y;
		Result(2, 2) = Scale.Z;
		return Result;
	}

	[[nodiscard]] static TMatrix3 Rotation(const TQuaternion<T>& Quaternion)
	{
		const TQuaternion<T> Q = Quaternion.NormalizedOrIdentity();
		const T XX = Q.X * Q.X;
		const T YY = Q.Y * Q.Y;
		const T ZZ = Q.Z * Q.Z;
		const T XY = Q.X * Q.Y;
		const T XZ = Q.X * Q.Z;
		const T YZ = Q.Y * Q.Z;
		const T XW = Q.X * Q.W;
		const T YW = Q.Y * Q.W;
		const T ZW = Q.Z * Q.W;

		TMatrix3 Result;
		Result(0, 0) = T{1} - T{2} * (YY + ZZ);
		Result(0, 1) = T{2} * (XY - ZW);
		Result(0, 2) = T{2} * (XZ + YW);
		Result(1, 0) = T{2} * (XY + ZW);
		Result(1, 1) = T{1} - T{2} * (XX + ZZ);
		Result(1, 2) = T{2} * (YZ - XW);
		Result(2, 0) = T{2} * (XZ - YW);
		Result(2, 1) = T{2} * (YZ + XW);
		Result(2, 2) = T{1} - T{2} * (XX + YY);
		return Result;
	}

	[[nodiscard]] constexpr T& operator()(std::size_t Row, std::size_t Column)
	{
		return Elements[Column * 3 + Row];
	}

	[[nodiscard]] constexpr const T& operator()(std::size_t Row, std::size_t Column) const
	{
		return Elements[Column * 3 + Row];
	}

	[[nodiscard]] constexpr const std::array<T, 9>& Data() const
	{
		return Elements;
	}

	[[nodiscard]] constexpr TMatrix3 operator*(const TMatrix3& Other) const
	{
		TMatrix3 Result = Zero();
		for (std::size_t Column = 0; Column < 3; ++Column)
		{
			for (std::size_t Row = 0; Row < 3; ++Row)
			{
				for (std::size_t Index = 0; Index < 3; ++Index)
				{
					Result(Row, Column) += (*this)(Row, Index) * Other(Index, Column);
				}
			}
		}

		return Result;
	}

	[[nodiscard]] constexpr TVector3<T> operator*(const TVector3<T>& Vector) const
	{
		return {(*this)(0, 0) * Vector.X + (*this)(0, 1) * Vector.Y + (*this)(0, 2) * Vector.Z,
		    (*this)(1, 0) * Vector.X + (*this)(1, 1) * Vector.Y + (*this)(1, 2) * Vector.Z,
		    (*this)(2, 0) * Vector.X + (*this)(2, 1) * Vector.Y + (*this)(2, 2) * Vector.Z};
	}

	[[nodiscard]] constexpr bool operator==(const TMatrix3&) const = default;

private:
	std::array<T, 9> Elements{T{1}, T{0}, T{0}, T{0}, T{1}, T{0}, T{0}, T{0}, T{1}};
};

template <std::floating_point T> class TMatrix4
{
public:
	constexpr TMatrix4() = default;

	explicit constexpr TMatrix4(const std::array<T, 16>& InElements)
	    : Elements(InElements)
	{
	}

	[[nodiscard]] static constexpr TMatrix4 Identity()
	{
		return {};
	}

	[[nodiscard]] static constexpr TMatrix4 Zero()
	{
		return TMatrix4(std::array<T, 16>{});
	}

	[[nodiscard]] static constexpr TMatrix4 Translation(const TVector3<T>& Translation)
	{
		TMatrix4 Result;
		Result(0, 3) = Translation.X;
		Result(1, 3) = Translation.Y;
		Result(2, 3) = Translation.Z;
		return Result;
	}

	[[nodiscard]] static constexpr TMatrix4 Scale(const TVector3<T>& Scale)
	{
		TMatrix4 Result;
		Result(0, 0) = Scale.X;
		Result(1, 1) = Scale.Y;
		Result(2, 2) = Scale.Z;
		return Result;
	}

	[[nodiscard]] static TMatrix4 Rotation(const TQuaternion<T>& Quaternion)
	{
		const TMatrix3<T> Rotation3 = TMatrix3<T>::Rotation(Quaternion);
		TMatrix4 Result;
		for (std::size_t Column = 0; Column < 3; ++Column)
		{
			for (std::size_t Row = 0; Row < 3; ++Row)
			{
				Result(Row, Column) = Rotation3(Row, Column);
			}
		}

		return Result;
	}

	[[nodiscard]] static TMatrix4 Transform(const TVector3<T>& InTranslation,
	    const TQuaternion<T>& InRotation,
	    const TVector3<T>& InScale)
	{
		return TMatrix4::Translation(InTranslation) * TMatrix4::Rotation(InRotation) * TMatrix4::Scale(InScale);
	}

	[[nodiscard]] static TMatrix4 PerspectiveReversedInfinite(T VerticalFieldOfViewRadians, T AspectRatio, T NearPlane)
	{
		assert(std::isfinite(VerticalFieldOfViewRadians));
		assert(std::isfinite(AspectRatio));
		assert(std::isfinite(NearPlane));
		assert(VerticalFieldOfViewRadians > T{0} && VerticalFieldOfViewRadians < std::numbers::pi_v<T>);
		assert(AspectRatio > T{0});
		assert(NearPlane > T{0});

		const T FocalLength = T{1} / std::tan(VerticalFieldOfViewRadians * T{0.5});
		TMatrix4 Result = Zero();
		Result(0, 0) = -FocalLength / AspectRatio;
		Result(1, 1) = FocalLength;
		Result(2, 3) = NearPlane;
		Result(3, 2) = T{1};
		return Result;
	}

	[[nodiscard]] constexpr T& operator()(std::size_t Row, std::size_t Column)
	{
		return Elements[Column * 4 + Row];
	}

	[[nodiscard]] constexpr const T& operator()(std::size_t Row, std::size_t Column) const
	{
		return Elements[Column * 4 + Row];
	}

	[[nodiscard]] constexpr const std::array<T, 16>& Data() const
	{
		return Elements;
	}

	[[nodiscard]] constexpr TMatrix4 operator*(const TMatrix4& Other) const
	{
		TMatrix4 Result = Zero();
		for (std::size_t Column = 0; Column < 4; ++Column)
		{
			for (std::size_t Row = 0; Row < 4; ++Row)
			{
				for (std::size_t Index = 0; Index < 4; ++Index)
				{
					Result(Row, Column) += (*this)(Row, Index) * Other(Index, Column);
				}
			}
		}

		return Result;
	}

	[[nodiscard]] constexpr TVector4<T> operator*(const TVector4<T>& Vector) const
	{
		return {
		    (*this)(0, 0) * Vector.X + (*this)(0, 1) * Vector.Y + (*this)(0, 2) * Vector.Z + (*this)(0, 3) * Vector.W,
		    (*this)(1, 0) * Vector.X + (*this)(1, 1) * Vector.Y + (*this)(1, 2) * Vector.Z + (*this)(1, 3) * Vector.W,
		    (*this)(2, 0) * Vector.X + (*this)(2, 1) * Vector.Y + (*this)(2, 2) * Vector.Z + (*this)(2, 3) * Vector.W,
		    (*this)(3, 0) * Vector.X + (*this)(3, 1) * Vector.Y + (*this)(3, 2) * Vector.Z + (*this)(3, 3) * Vector.W,
		};
	}

	[[nodiscard]] constexpr TVector3<T> TransformPosition(const TVector3<T>& Position) const
	{
		const TVector4<T> Result = *this * TVector4<T>{Position, T{1}};
		return {Result.X, Result.Y, Result.Z};
	}

	[[nodiscard]] constexpr TVector3<T> TransformVector(const TVector3<T>& Vector) const
	{
		const TVector4<T> Result = *this * TVector4<T>{Vector, T{0}};
		return {Result.X, Result.Y, Result.Z};
	}

	[[nodiscard]] constexpr bool operator==(const TMatrix4&) const = default;

private:
	// The public matrix contract is column-major even though element access stays row-column.
	std::array<T, 16> Elements{
	    T{1},
	    T{0},
	    T{0},
	    T{0},
	    T{0},
	    T{1},
	    T{0},
	    T{0},
	    T{0},
	    T{0},
	    T{1},
	    T{0},
	    T{0},
	    T{0},
	    T{0},
	    T{1},
	};
};

using FMatrix3 = TMatrix3<float>;
using FMatrix4 = TMatrix4<float>;
}
