#include "Herta/Math/AffineTransform.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numbers>
#include <type_traits>

namespace Herta
{
namespace
{
template <std::floating_point T> void CheckAffineMatrix(const TMatrix4<T>& Actual, const TMatrix4<T>& Expected, const T Tolerance = T{1e-5})
{
	for (std::size_t Column = 0; Column < 4; ++Column)
	{
		const T Magnitude = Column < 3 ? std::hypot(Expected(0, Column), Expected(1, Column), Expected(2, Column)) : T{1};

		for (std::size_t Row = 0; Row < 4; ++Row)
		{
			const T EntryMagnitude = std::max(Magnitude, std::abs(Expected(Row, Column)));
			CHECK(std::abs(Actual(Row, Column) - Expected(Row, Column)) <= Tolerance * EntryMagnitude);
		}
	}
}
}

TEST_CASE_TEMPLATE("Affine identity inverts and decomposes", T, float, double)
{
	const auto Inverse = TryInverseAffine(TMatrix4<T>::Identity());
	const auto Transform = TryDecomposeTransform(TMatrix4<T>::Identity());
	REQUIRE(Inverse.has_value());
	REQUIRE(Transform.has_value());
	CHECK(*Inverse == TMatrix4<T>::Identity());
	CHECK(*Transform == TTransform<T>::Identity());
}

TEST_CASE_TEMPLATE("Affine translated rotated scales round trip", T, float, double)
{
	const auto Rotation = TQuaternion<T>::FromAxisAngle({T{1}, T{2}, T{3}}, T{0.7});

	for (const auto Scale : std::array{TVector3<T>{T{2}, T{3}, T{4}}, TVector3<T>{T{3}, T{3}, T{3}}})
	{
		const TTransform<T> Original{{T{12.25}, T{-30.5}, T{3.75}}, Rotation, Scale};
		const TMatrix4<T> Matrix = Original.ToMatrix();
		const auto Inverse = TryInverseAffine(Matrix);
		const auto Transform = TryDecomposeTransform(Matrix);
		REQUIRE(Inverse.has_value());
		REQUIRE(Transform.has_value());
		CheckAffineMatrix(*Inverse * Matrix, TMatrix4<T>::Identity());
		CheckAffineMatrix(Matrix * *Inverse, TMatrix4<T>::Identity());
		CheckAffineMatrix(Transform->ToMatrix(), Matrix);
		CHECK(Transform->Translation == Original.Translation);
	}
}

TEST_CASE("Affine decomposition preserves double world translation")
{
	const TTransform<double> Original{{1e12 + 0.25, -1e12 - 0.5, 1e12 + 0.75}, TQuaternion<double>::FromAxisAngle({1., 2., 3.}, 0.7), {2., 3., 4.}};
	const auto Transform = TryDecomposeTransform(Original.ToMatrix());
	REQUIRE(Transform.has_value());
	CHECK(Transform->Translation == Original.Translation);
	CheckAffineMatrix(Transform->ToMatrix(), Original.ToMatrix());
}

TEST_CASE_TEMPLATE("Affine parent inverse preserves world pose when reparenting", T, float, double)
{
	const TTransform<T> Parent{{T{10}, T{-20}, T{30}}, TQuaternion<T>::FromAxisAngle({T{1}, T{2}, T{3}}, T{0.4}), {T{2}, T{2}, T{2}}};
	const TTransform<T> World{{T{-15}, T{25}, T{35}}, TQuaternion<T>::FromAxisAngle({T{3}, T{2}, T{1}}, T{0.9}), {T{3}, T{4}, T{5}}};
	const auto ParentInverse = TryInverseAffine(Parent.ToMatrix());
	REQUIRE(ParentInverse.has_value());
	const auto Local = TryDecomposeTransform(*ParentInverse * World.ToMatrix());
	REQUIRE(Local.has_value());
	CheckAffineMatrix(Parent.ToMatrix() * Local->ToMatrix(), World.ToMatrix());
}

TEST_CASE_TEMPLATE("Affine quaternion extraction covers trace and diagonal branches near half turns", T, float, double)
{
	for (const auto Axis : std::array{TVector3<T>{T{1}, T{0}, T{0}}, TVector3<T>{T{0}, T{1}, T{0}}, TVector3<T>{T{0}, T{0}, T{1}}, TVector3<T>{T{1}, T{2}, T{3}}})
	{
		for (const T Angle : std::array{T{0.2}, std::numbers::pi_v<T>, std::numbers::pi_v<T> - T{1e-5}, std::numbers::pi_v<T> + T{1e-5}})
		{
			const auto Matrix = TMatrix4<T>::Rotation(TQuaternion<T>::FromAxisAngle(Axis, Angle));
			const auto Transform = TryDecomposeTransform(Matrix);
			REQUIRE(Transform.has_value());
			CheckAffineMatrix(Transform->ToMatrix(), Matrix);
		}
	}
}

TEST_CASE_TEMPLATE("Affine math supports tiny and large positive scales", T, float, double)
{
	const T Tiny = std::is_same_v<T, float> ? T{1e-20} : static_cast<T>(1e-200);
	const T Large = T{1} / Tiny;

	for (const auto Scale : std::array{TVector3<T>{Tiny, Tiny, Tiny}, TVector3<T>{Large, Large, Large}, TVector3<T>{Tiny, T{1}, Large}})
	{
		const auto Matrix = TMatrix4<T>::Transform({}, TQuaternion<T>::FromAxisAngle({T{1}, T{2}, T{3}}, T{0.7}), Scale);
		const auto Inverse = TryInverseAffine(Matrix);
		const auto Transform = TryDecomposeTransform(Matrix);
		REQUIRE(Inverse.has_value());
		REQUIRE(Transform.has_value());
		CheckAffineMatrix(Matrix * *Inverse, TMatrix4<T>::Identity());
		CheckAffineMatrix(Transform->ToMatrix(), Matrix);
	}
}

TEST_CASE_TEMPLATE("Affine inversion accepts shear and reflections but decomposition rejects them", T, float, double)
{
	TMatrix4<T> Shear;
	Shear(0, 1) = T{0.25};
	Shear(1, 2) = T{-0.5};
	Shear(0, 3) = T{30};
	const auto ShearInverse = TryInverseAffine(Shear);
	REQUIRE(ShearInverse.has_value());
	CheckAffineMatrix(Shear * *ShearInverse, TMatrix4<T>::Identity());
	CHECK_FALSE(TryDecomposeTransform(Shear).has_value());

	const auto Reflection = TMatrix4<T>::Scale({T{-2}, T{3}, T{4}});
	const auto ReflectionInverse = TryInverseAffine(Reflection);
	REQUIRE(ReflectionInverse.has_value());
	CheckAffineMatrix(Reflection * *ReflectionInverse, TMatrix4<T>::Identity());
	CHECK_FALSE(TryDecomposeTransform(Reflection).has_value());
}

TEST_CASE_TEMPLATE("Affine decomposition does not hide shear behind translation or another axis scale", T, float, double)
{
	for (const T Scale : std::array{T{1e-12}, T{1}, T{1e12}})
	{
		auto Matrix = TMatrix4<T>::Scale({Scale, T{1}, T{1e12}});
		Matrix(1, 0) = Scale * T{0.01};
		Matrix(0, 3) = T{1e12};
		CHECK_FALSE(TryDecomposeTransform(Matrix).has_value());
	}
}

TEST_CASE_TEMPLATE("Affine math rejects singular nonfinite and projective matrices", T, float, double)
{
	std::array<TMatrix4<T>, 9> Invalid;
	Invalid[0] = TMatrix4<T>::Scale({T{0}, T{1}, T{1}});
	Invalid[1](0, 1) = T{1};
	Invalid[1](1, 1) = T{0};
	Invalid[2](0, 0) = std::numeric_limits<T>::quiet_NaN();
	Invalid[3](1, 3) = std::numeric_limits<T>::infinity();
	Invalid[4](3, 0) = T{0.1};
	Invalid[5](3, 1) = T{0.1};
	Invalid[6](3, 2) = T{0.1};
	Invalid[7](3, 3) = T{2};
	Invalid[8] = TMatrix4<T>{std::array<T, 16>{T{1}, T{2}, T{3}, T{0}, T{4}, T{5}, T{6}, T{0}, T{5}, T{7}, T{9}, T{0}, T{0}, T{0}, T{0}, T{1}}};

	for (const auto& Matrix : Invalid)
	{
		CHECK_FALSE(TryInverseAffine(Matrix).has_value());
		CHECK_FALSE(TryDecomposeTransform(Matrix).has_value());
	}

	CHECK_FALSE(TryDecomposeTransform(TMatrix4<T>::Identity(), T{-1}).has_value());
	CHECK_FALSE(TryDecomposeTransform(TMatrix4<T>::Identity(), std::numeric_limits<T>::quiet_NaN()).has_value());
	CHECK_FALSE(TryDecomposeTransform(TMatrix4<T>::Identity(), std::numeric_limits<T>::infinity()).has_value());
}

TEST_CASE_TEMPLATE("Affine inversion rejects results beyond the scalar range", T, float, double)
{
	const auto Matrix = TMatrix4<T>::Scale({std::numeric_limits<T>::denorm_min(), T{1}, T{1}});
	CHECK_FALSE(TryInverseAffine(Matrix).has_value());
}
}
