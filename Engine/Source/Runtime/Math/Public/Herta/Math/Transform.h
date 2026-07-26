#pragma once

#include "Herta/Math/Matrix.h"

#include <concepts>

namespace Herta
{
template <std::floating_point T> struct TTransform
{
	TVector3<T> Translation = TVector3<T>::Zero();
	TQuaternion<T> Rotation = TQuaternion<T>::Identity();
	TVector3<T> Scale3D = TVector3<T>::One();

	constexpr TTransform() = default;

	constexpr TTransform(const TVector3<T>& InTranslation,
	                     const TQuaternion<T>& InRotation,
	                     const TVector3<T>& InScale3D)
	    : Translation(InTranslation)
	    , Rotation(InRotation)
	    , Scale3D(InScale3D)
	{
	}

	[[nodiscard]] static constexpr TTransform Identity()
	{
		return {};
	}

	[[nodiscard]] TMatrix4<T> ToMatrix() const
	{
		return TMatrix4<T>::Transform(Translation, Rotation, Scale3D);
	}

	[[nodiscard]] TVector3<T> TransformPosition(const TVector3<T>& Position) const
	{
		return Translation + Rotation.RotateVector(Scale3D.ComponentMultiply(Position));
	}

	[[nodiscard]] TVector3<T> TransformVector(const TVector3<T>& Vector) const
	{
		return Rotation.RotateVector(Scale3D.ComponentMultiply(Vector));
	}

	[[nodiscard]] constexpr bool operator==(const TTransform&) const = default;
};

using FTransform = TTransform<float>;
}
