#pragma once

#include "Herta/Math/Transform.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <limits>
#include <optional>

namespace Herta
{
namespace MathPrivate
{
template <std::floating_point T> bool IsFiniteAffine(const TMatrix4<T>& Matrix)
{
	return std::ranges::all_of(Matrix.Data(), [](const T Element)
	{
		return std::isfinite(Element);
	}) && Matrix(3, 0) == T{0}
	       && Matrix(3, 1) == T{0} && Matrix(3, 2) == T{0} && Matrix(3, 3) == T{1};
}
}

template <std::floating_point T> [[nodiscard]] std::optional<TMatrix4<T>> TryInverseAffine(const TMatrix4<T>& Matrix)
{
	if (!MathPrivate::IsFiniteAffine(Matrix))
	{
		return std::nullopt;
	}

	std::array<TVector3<T>, 3> Basis;
	std::array<T, 3> Scales;

	for (std::size_t Column = 0; Column < 3; ++Column)
	{
		const TVector3<T> Vector{Matrix(0, Column), Matrix(1, Column), Matrix(2, Column)};
		const T Scale = std::max({std::abs(Vector.X), std::abs(Vector.Y), std::abs(Vector.Z)});

		if (!std::isfinite(Scale) || Scale <= T{0})
		{
			return std::nullopt;
		}

		Basis[Column] = Vector / Scale;
		Scales[Column] = Scale;
	}

	// Normalize first so tiny or large scales do not underflow or overflow the determinant.
	const std::array<TVector3<T>, 3> Cofactors{
	    Basis[1].Cross(Basis[2]),
	    Basis[2].Cross(Basis[0]),
	    Basis[0].Cross(Basis[1]),
	};

	const T Determinant = Basis[0].Dot(Cofactors[0]);
	const T DeterminantMagnitude = std::abs(Basis[0].X) * (std::abs(Basis[1].Y * Basis[2].Z) + std::abs(Basis[1].Z * Basis[2].Y))
	                               + std::abs(Basis[0].Y) * (std::abs(Basis[1].Z * Basis[2].X) + std::abs(Basis[1].X * Basis[2].Z))
	                               + std::abs(Basis[0].Z) * (std::abs(Basis[1].X * Basis[2].Y) + std::abs(Basis[1].Y * Basis[2].X));

	// Cancellation below scalar precision cannot establish a nonsingular basis.
	if (std::abs(Determinant) <= T{4} * std::numeric_limits<T>::epsilon() * DeterminantMagnitude)
	{
		return std::nullopt;
	}

	TMatrix4<T> Result;

	for (std::size_t Row = 0; Row < 3; ++Row)
	{
		for (std::size_t Column = 0; Column < 3; ++Column)
		{
			Result(Row, Column) = (Cofactors[Row][Column] / Determinant) / Scales[Row];
		}

		Result(Row, 3) = -(Result(Row, 0) * Matrix(0, 3) + Result(Row, 1) * Matrix(1, 3) + Result(Row, 2) * Matrix(2, 3));
	}

	if (!MathPrivate::IsFiniteAffine(Result))
	{
		return std::nullopt;
	}

	return Result;
}

template <std::floating_point T> [[nodiscard]] std::optional<TTransform<T>> TryDecomposeTransform(const TMatrix4<T>& Matrix, const T Tolerance = T{1e-6})
{
	if (!MathPrivate::IsFiniteAffine(Matrix) || !std::isfinite(Tolerance) || Tolerance < T{0})
	{
		return std::nullopt;
	}

	TVector3<T> Scales;
	TMatrix3<T> Rotation;
	std::array<TVector3<T>, 3> Basis;

	for (std::size_t Column = 0; Column < 3; ++Column)
	{
		const TVector3<T> Vector{Matrix(0, Column), Matrix(1, Column), Matrix(2, Column)};
		const T Scale = std::hypot(Vector.X, Vector.Y, Vector.Z);

		if (!std::isfinite(Scale) || Scale <= T{0})
		{
			return std::nullopt;
		}

		Scales[Column] = Scale;
		Basis[Column] = Vector / Scale;

		for (std::size_t Row = 0; Row < 3; ++Row)
		{
			Rotation(Row, Column) = Basis[Column][Row];
		}
	}

	const T Determinant = Basis[0].Dot(Basis[1].Cross(Basis[2]));

	if (Determinant <= T{0} || std::abs(Determinant - T{1}) > Tolerance
	    || std::abs(Basis[0].Dot(Basis[1])) > Tolerance
	    || std::abs(Basis[0].Dot(Basis[2])) > Tolerance
	    || std::abs(Basis[1].Dot(Basis[2])) > Tolerance)
	{
		return std::nullopt;
	}

	TQuaternion<T> Quaternion;
	const T Trace = Rotation(0, 0) + Rotation(1, 1) + Rotation(2, 2);

	if (Trace > T{0})
	{
		const T Factor = T{2} * std::sqrt(Trace + T{1});
		Quaternion = {(Rotation(2, 1) - Rotation(1, 2)) / Factor, (Rotation(0, 2) - Rotation(2, 0)) / Factor, (Rotation(1, 0) - Rotation(0, 1)) / Factor, Factor / T{4}};
	}
	else
	{
		// The largest diagonal branch remains stable around a half turn.
		std::size_t Axis = 0;

		if (Rotation(1, 1) > Rotation(Axis, Axis))
		{
			Axis = 1;
		}

		if (Rotation(2, 2) > Rotation(Axis, Axis))
		{
			Axis = 2;
		}

		const std::size_t Next = (Axis + 1) % 3;
		const std::size_t Last = (Axis + 2) % 3;
		const T Factor = T{2} * std::sqrt(T{1} + Rotation(Axis, Axis) - Rotation(Next, Next) - Rotation(Last, Last));
		std::array<T, 3> Vector;
		Vector[Axis] = Factor / T{4};
		Vector[Next] = (Rotation(Next, Axis) + Rotation(Axis, Next)) / Factor;
		Vector[Last] = (Rotation(Last, Axis) + Rotation(Axis, Last)) / Factor;
		Quaternion = {Vector[0], Vector[1], Vector[2], (Rotation(Last, Next) - Rotation(Next, Last)) / Factor};
	}

	const TTransform<T> Result{{Matrix(0, 3), Matrix(1, 3), Matrix(2, 3)}, Quaternion.NormalizedOrIdentity(), Scales};
	const TMatrix4<T> Reconstructed = Result.ToMatrix();

	if (!MathPrivate::IsFiniteAffine(Reconstructed))
	{
		return std::nullopt;
	}

	for (std::size_t Column = 0; Column < 4; ++Column)
	{
		for (std::size_t Row = 0; Row < 3; ++Row)
		{
			// Basis error is relative to its own scale, never the translation or another axis.
			const T Magnitude = Column < 3 ? Scales[Column] : std::max(T{1}, std::abs(Matrix(Row, Column)));

			if (std::abs(Matrix(Row, Column) / Magnitude - Reconstructed(Row, Column) / Magnitude) > Tolerance)
			{
				return std::nullopt;
			}
		}
	}

	return Result;
}
}
