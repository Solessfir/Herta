#include "Herta/Math/Math.h"

#include <cmath>
#include <doctest/doctest.h>
#include <numbers>

namespace Herta
{
namespace
{
constexpr float Tolerance = 1.0e-5f;

constexpr FVector3 Left = FVector3::Left();
constexpr FVector3 Up = FVector3::Up();
constexpr FVector3 Forward = FVector3::Forward();
static_assert(Left.Cross(Up) == Forward);
static_assert(Up.Cross(Forward) == Left);
static_assert(Forward.Cross(Left) == Up);
static_assert(Left.Dot(Up) == 0.0f);

constexpr FQuaternion HalfTurnLeft{1.0f, 0.0f, 0.0f, 0.0f};
constexpr FQuaternion HalfTurnUp{0.0f, 1.0f, 0.0f, 0.0f};
constexpr FQuaternion ComposedHalfTurns = HalfTurnUp * HalfTurnLeft;
static_assert(ComposedHalfTurns == FQuaternion{0.0f, 0.0f, -1.0f, 0.0f});

constexpr FMatrix4 ScaleThenTranslate =
    FMatrix4::Translation({10.0f, 20.0f, 30.0f}) * FMatrix4::Scale({2.0f, 3.0f, 4.0f});
static_assert(ScaleThenTranslate.TransformPosition({1.0f, 1.0f, 1.0f}) == FVector3{12.0f, 23.0f, 34.0f});
static_assert(ScaleThenTranslate.Data()[12] == 10.0f);
static_assert(FMatrix3::Scale({2.0f, 3.0f, 4.0f}) * FVector3::One() == FVector3{2.0f, 3.0f, 4.0f});

constexpr FWorldPosition DistantPosition{10'000'000.25, -2'000'000.5, 5'000'001.0};
constexpr FWorldPosition NearbyOrigin{10'000'000.0, -2'000'000.0, 5'000'000.0};
static_assert(DistantPosition.RelativeTo(NearbyOrigin) == FVector3d{0.25, -0.5, 1.0});
static_assert(WorldToOriginRelative(DistantPosition, NearbyOrigin) == FVector3{0.25f, -0.5f, 1.0f});

void CheckVector(const FVector3& Actual, const FVector3& Expected)
{
	CHECK(std::abs(Actual.X - Expected.X) <= Tolerance);
	CHECK(std::abs(Actual.Y - Expected.Y) <= Tolerance);
	CHECK(std::abs(Actual.Z - Expected.Z) <= Tolerance);
}
}

TEST_CASE("Left-Up-Forward axes form a right-handed basis")
{
	CHECK(FVector3::Left().Cross(FVector3::Up()) == FVector3::Forward());
	CHECK(FVector3::Up().Cross(FVector3::Forward()) == FVector3::Left());
	CHECK(FVector3::Forward().Cross(FVector3::Left()) == FVector3::Up());
	CHECK(FVector3::Left().Dot(FVector3::Up()) == 0.0f);
	CHECK(FVector3::Left().Length() == doctest::Approx(1.0f));
}

TEST_CASE("Quaternion multiplication applies the right operand first")
{
	const FQuaternion RotateAroundUp = FQuaternion::FromAxisAngle(FVector3::Up(), std::numbers::pi_v<float> * 0.5f);
	const FQuaternion RotateAroundForward =
	    FQuaternion::FromAxisAngle(FVector3::Forward(), std::numbers::pi_v<float> * 0.5f);

	const FVector3 Sequential = RotateAroundForward.RotateVector(RotateAroundUp.RotateVector(FVector3::Forward()));
	const FVector3 Composed = (RotateAroundForward * RotateAroundUp).RotateVector(FVector3::Forward());

	CheckVector(RotateAroundUp.RotateVector(FVector3::Forward()), FVector3::Left());
	CheckVector(Sequential, FVector3::Up());
	CheckVector(Composed, Sequential);
}

TEST_CASE("Transform uses Translation Rotation Scale order")
{
	const FTransform Transform{{10.0f, 20.0f, 30.0f},
	                           FQuaternion::FromAxisAngle(FVector3::Up(), std::numbers::pi_v<float> * 0.5f),
	                           {2.0f, 3.0f, 4.0f}};

	const FVector3 Expected{14.0f, 20.0f, 30.0f};
	CheckVector(Transform.TransformPosition(FVector3::Forward()), Expected);
	CheckVector(Transform.ToMatrix().TransformPosition(FVector3::Forward()), Expected);
	CheckVector(Transform.TransformVector(FVector3::Forward()), FVector3{4.0f, 0.0f, 0.0f});
}

TEST_CASE("Matrix storage is column-major")
{
	const FMatrix4 Translation = FMatrix4::Translation({3.0f, 4.0f, 5.0f});
	const auto& Data = Translation.Data();

	CHECK(Data[12] == 3.0f);
	CHECK(Data[13] == 4.0f);
	CHECK(Data[14] == 5.0f);
	CHECK(Translation(0, 3) == 3.0f);
}

TEST_CASE("World positions become precise origin-relative floats")
{
	const FWorldPosition Position{10'000'000.25, -2'000'000.5, 5'000'001.0};
	const FWorldPosition Origin{10'000'000.0, -2'000'000.0, 5'000'000.0};

	CHECK((Position.RelativeTo(Origin) == FVector3d{0.25, -0.5, 1.0}));
	CHECK((WorldToOriginRelative(Position, Origin) == FVector3{0.25f, -0.5f, 1.0f}));
}
}
