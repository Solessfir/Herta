#include "Herta/Math/Math.h"

#include <doctest/doctest.h>

#include <array>
#include <cmath>
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
static_assert(Left.Dot(Up) == 0.f);

constexpr FQuaternion HalfTurnLeft{1.f, 0.f, 0.f, 0.f};
constexpr FQuaternion HalfTurnUp{0.f, 1.f, 0.f, 0.f};
constexpr FQuaternion ComposedHalfTurns = HalfTurnUp * HalfTurnLeft;
static_assert(ComposedHalfTurns == FQuaternion{0.f, 0.f, -1.f, 0.f});

constexpr FMatrix4 ScaleThenTranslate =
    FMatrix4::Translation({10.f, 20.f, 30.f}) * FMatrix4::Scale({2.f, 3.f, 4.f});
static_assert(ScaleThenTranslate.TransformPosition({1.f, 1.f, 1.f}) == FVector3{12.f, 23.f, 34.f});
static_assert(ScaleThenTranslate.Data()[12] == 10.f);
static_assert(FMatrix3::Scale({2.f, 3.f, 4.f}) * FVector3::One() == FVector3{2.f, 3.f, 4.f});

constexpr FWorldPosition DistantPosition{10'000'000.25, -2'000'000.5, 5'000'001.0};
constexpr FWorldPosition NearbyOrigin{10'000'000.0, -2'000'000.0, 5'000'000.0};
static_assert(DistantPosition.RelativeTo(NearbyOrigin) == FVector3d{0.25, -0.5, 1.0});
static_assert(WorldToOriginRelative(DistantPosition, NearbyOrigin) == FVector3{0.25f, -0.5f, 1.f});

void CheckVector(const FVector3& Actual, const FVector3& Expected)
{
	CHECK(std::abs(Actual.X - Expected.X) <= Tolerance);
	CHECK(std::abs(Actual.Y - Expected.Y) <= Tolerance);
	CHECK(std::abs(Actual.Z - Expected.Z) <= Tolerance);
}

FVector3 ProjectToNdc(const FMatrix4& Projection, const FVector3& Position)
{
	const FVector4 ClipPosition = Projection * FVector4{Position, 1.f};
	return {ClipPosition.X / ClipPosition.W, ClipPosition.Y / ClipPosition.W, ClipPosition.Z / ClipPosition.W};
}
}

TEST_CASE("Left-Up-Forward axes form a right-handed basis")
{
	CHECK(FVector3::Left().Cross(FVector3::Up()) == FVector3::Forward());
	CHECK(FVector3::Up().Cross(FVector3::Forward()) == FVector3::Left());
	CHECK(FVector3::Forward().Cross(FVector3::Left()) == FVector3::Up());
	CHECK(FVector3::Left().Dot(FVector3::Up()) == 0.f);
	CHECK(FVector3::Left().Length() == doctest::Approx(1.f));
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
	const FTransform Transform{{10.f, 20.f, 30.f},
	    FQuaternion::FromAxisAngle(FVector3::Up(), std::numbers::pi_v<float> * 0.5f),
	    {2.f, 3.f, 4.f}};

	const FVector3 Expected{14.f, 20.f, 30.f};
	CheckVector(Transform.TransformPosition(FVector3::Forward()), Expected);
	CheckVector(Transform.ToMatrix().TransformPosition(FVector3::Forward()), Expected);
	CheckVector(Transform.TransformVector(FVector3::Forward()), FVector3{4.f, 0.f, 0.f});
}

TEST_CASE("Matrix storage is column-major")
{
	const FMatrix4 Translation = FMatrix4::Translation({3.f, 4.f, 5.f});
	const auto& Data = Translation.Data();

	CHECK(Data[12] == 3.f);
	CHECK(Data[13] == 4.f);
	CHECK(Data[14] == 5.f);
	CHECK(Translation(0, 3) == 3.f);
}

TEST_CASE("Infinite reversed-Z projection follows Herta clip-space conventions")
{
	constexpr float NearPlane = 0.1f;
	const FMatrix4 Projection =
	    FMatrix4::PerspectiveReversedInfinite(std::numbers::pi_v<float> * 0.5f, 1.f, NearPlane);

	const FVector3 NearNdc = ProjectToNdc(Projection, {0.f, 0.f, NearPlane});
	const FVector3 DistantNdc = ProjectToNdc(Projection, {0.f, 0.f, 10'000.f});
	const FVector3 LeftNdc = ProjectToNdc(Projection, FVector3::Left() + FVector3::Forward());
	const FVector3 UpNdc = ProjectToNdc(Projection, FVector3::Up() + FVector3::Forward());

	CHECK(NearNdc.Z == doctest::Approx(1.f));
	CHECK(DistantNdc.Z == doctest::Approx(0.f).epsilon(0.0001));
	CHECK(LeftNdc.X < 0.f);
	CHECK(UpNdc.Y > 0.f);
}

TEST_CASE("Projected front faces are counter-clockwise")
{
	const FMatrix4 Projection = FMatrix4::PerspectiveReversedInfinite(std::numbers::pi_v<float> * 0.5f, 1.f, 0.1f);
	const FVector3 BottomLeft = ProjectToNdc(Projection, {1.f, -1.f, 2.f});
	const FVector3 BottomRight = ProjectToNdc(Projection, {-1.f, -1.f, 2.f});
	const FVector3 Top = ProjectToNdc(Projection, {0.f, 1.f, 2.f});

	const float SignedArea = (BottomRight.X - BottomLeft.X) * (Top.Y - BottomLeft.Y) - (BottomRight.Y - BottomLeft.Y) * (Top.X - BottomLeft.X);
	CHECK(SignedArea > 0.f);
}

TEST_CASE("Quaternion serialized order is XYZW")
{
	constexpr FQuaternion Quaternion{1.f, 2.f, 3.f, 4.f};
	constexpr std::array<float, 4> Serialized = Quaternion.ToXYZW();

	static_assert(Serialized == std::array{1.f, 2.f, 3.f, 4.f});
	static_assert(FQuaternion::FromXYZW(Serialized) == Quaternion);
	CHECK(FQuaternion::FromXYZW(Serialized) == Quaternion);
}

TEST_CASE("World positions become precise origin-relative floats")
{
	const FWorldPosition Position{10'000'000.25, -2'000'000.5, 5'000'001.0};
	const FWorldPosition Origin{10'000'000.0, -2'000'000.0, 5'000'000.0};

	CHECK((Position.RelativeTo(Origin) == FVector3d{0.25, -0.5, 1.0}));
	CHECK((WorldToOriginRelative(Position, Origin) == FVector3{0.25f, -0.5f, 1.f}));
	CHECK(OriginRelativeToWorld(WorldToOriginRelative(Position, Origin), Origin) == Position);
}

TEST_CASE("World rebasing preserves origin-relative positions")
{
	const FWorldPosition Position{-8'000'000.25, 2'000'000.5, -4'000'001.0};
	const FWorldPosition FirstOrigin{-8'000'000.0, 2'000'000.0, -4'000'000.0};
	const FWorldPosition SecondOrigin{-7'999'900.0, 1'999'950.0, -4'000'025.0};

	const FVector3 FirstRelative = WorldToOriginRelative(Position, FirstOrigin);
	const FVector3 SecondRelative =
	    WorldToOriginRelative(OriginRelativeToWorld(FirstRelative, FirstOrigin), SecondOrigin);

	CHECK((FirstRelative == FVector3{-0.25f, 0.5f, -1.f}));
	CHECK((SecondRelative == FVector3{-100.25f, 50.5f, 24.f}));
}
}
