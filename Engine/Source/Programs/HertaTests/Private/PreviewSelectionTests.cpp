#include "Herta/EditorCore/PreviewSelection.h"

#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <limits>
#include <numbers>

namespace Herta
{
TEST_CASE("Preview picking hits only the forward ray including origins inside the cube")
{
	const FMatrix4 Model;
	CHECK(HitTestPreviewCube({{0.f, 0.f, -5.f}, FVector3::Forward()}, Model));
	CHECK(HitTestPreviewCube({{0.f, 0.f, 5.f}, FVector3::Backward()}, Model));
	CHECK_FALSE(HitTestPreviewCube({{0.f, 0.f, -5.f}, FVector3::Backward()}, Model));
	CHECK_FALSE(HitTestPreviewCube({{0.f, 0.f, 5.f}, FVector3::Forward()}, Model));
	CHECK_FALSE(HitTestPreviewCube({{2.f, 0.f, -5.f}, FVector3::Forward()}, Model));
	CHECK(HitTestPreviewCube({{1.f, 1.f, -5.f}, FVector3::Forward()}, Model));
	CHECK(HitTestPreviewCube({FVector3::Zero(), FVector3::Left()}, Model));
	CHECK_FALSE(HitTestPreviewCube({FVector3::Zero(), FVector3::Zero()}, Model));
}

TEST_CASE("Preview picking reports world ray distances for overlapping objects")
{
	const FViewportPickingRay Ray{.Origin = {0.f, 6.f, 0.f}, .Direction = FVector3::Down()};
	const FMatrix4 Cube = FMatrix4::Translation({0.f, 4.f, 0.f});
	const FMatrix4 Floor = FMatrix4::Transform({0.f, -0.25f, 0.f}, FQuaternion::Identity(), {10.f, 0.25f, 10.f});
	const auto CubeDistance = HitTestPreviewCube(Ray, Cube);
	const auto FloorDistance = HitTestPreviewCube(Ray, Floor);
	REQUIRE(CubeDistance);
	REQUIRE(FloorDistance);
	CHECK(*CubeDistance == doctest::Approx(1.0));
	CHECK(*FloorDistance == doctest::Approx(6.0));
	CHECK(*CubeDistance < *FloorDistance);
	CHECK(*HitTestPreviewCube({{0.f, 4.f, 0.f}, FVector3::Down()}, Cube) == doctest::Approx(0.0));
}

TEST_CASE("Preview picking follows translated rotated nonuniformly scaled bounds")
{
	const FQuaternion Rotation = FQuaternion::FromAxisAngle(FVector3::Up(), 0.6f) * FQuaternion::FromAxisAngle(FVector3::Left(), 0.35f);
	const FMatrix4 Model = FMatrix4::Transform({3.f, 2.f, 5.f}, Rotation, {2.f, 0.5f, 3.f});
	const FVector3 Direction = Model.TransformVector(FVector3::Forward()).Normalized();
	CHECK(HitTestPreviewCube({Model.TransformPosition({0.9f, -0.8f, -4.f}), Direction}, Model));
	CHECK_FALSE(HitTestPreviewCube({Model.TransformPosition({1.1f, 0.f, -4.f}), Direction}, Model));
	CHECK_FALSE(HitTestPreviewCube({Model.TransformPosition({0.f, 1.1f, -4.f}), Direction}, Model));
	CHECK_FALSE(HitTestPreviewCube({Model.TransformPosition({0.f, 0.f, -4.f}), -Direction}, Model));
	CHECK(HitTestPreviewCube({Model.TransformPosition({0.f, 0.f, 0.f}), Direction}, Model));
}

TEST_CASE("Preview picking consumes camera rays without changing screen conventions")
{
	const FViewportCameraController Camera;
	constexpr float AspectRatio = 16.f / 9.f;
	const FViewportCameraSnapshot Snapshot = Camera.GetSnapshot(AspectRatio);
	const auto ToScreen = [&Snapshot](const FVector3& Position)
	{
		const FVector4 Clip = Snapshot.Projection * Snapshot.View * FVector4{Position, 1.f};
		return FVector2{(Clip.X / Clip.W + 1.f) * 0.5f, (1.f - Clip.Y / Clip.W) * 0.5f};
	};

	CHECK(HitTestPreviewCube(Camera.MakePickingRay(ToScreen(FVector3::Zero()), AspectRatio), FMatrix4::Identity()));
	CHECK_FALSE(HitTestPreviewCube(Camera.MakePickingRay({0.f, 0.f}, AspectRatio), FMatrix4::Identity()));
	const FMatrix4 LeftCube = FMatrix4::Translation({3.f, 0.f, 0.f});
	const FVector2 LeftCubeScreen = ToScreen(LeftCube.TransformPosition(FVector3::Zero()));
	CHECK(LeftCubeScreen.X < 0.5f);
	CHECK(HitTestPreviewCube(Camera.MakePickingRay(LeftCubeScreen, AspectRatio), LeftCube));
	CHECK_FALSE(HitTestPreviewCube(Camera.MakePickingRay({1.f - LeftCubeScreen.X, LeftCubeScreen.Y}, AspectRatio), LeftCube));
}

TEST_CASE("Preview selection rejects singular invalid and non-TRS inputs")
{
	const FViewportPickingRay Ray{.Origin = {0.f, 0.f, -5.f}, .Direction = FVector3::Forward()};
	const FMatrix4 Singular = FMatrix4::Scale({0.f, 1.f, 1.f});
	CHECK_FALSE(HitTestPreviewCube(Ray, Singular));
	FMatrix4 Sheared;
	Sheared(0, 1) = 0.5f;
	CHECK_FALSE(HitTestPreviewCube(Ray, Sheared));
	FMatrix4 NonAffine;
	NonAffine(3, 0) = 0.5f;
	CHECK_FALSE(HitTestPreviewCube(Ray, NonAffine));
	const float Invalid = std::numeric_limits<float>::quiet_NaN();
	CHECK_FALSE(HitTestPreviewCube({Ray.Origin, {Invalid, 0.f, 1.f}}, FMatrix4::Identity()));
}

TEST_CASE("Preview box selection intersects projected bounds with either drag direction")
{
	const FMatrix4 Projection = FMatrix4::PerspectiveReversedInfinite(std::numbers::pi_v<float> * 0.5f, 1.f, 0.1f);
	const FMatrix4 Model = FMatrix4::Translation({0.f, 0.f, 5.f});
	CHECK(IntersectsPreviewCubeSelectionRect(Projection, Model, {0.49f, 0.49f}, {0.51f, 0.51f}));
	CHECK(IntersectsPreviewCubeSelectionRect(Projection, Model, {0.51f, 0.51f}, {0.49f, 0.49f}));
	CHECK(IntersectsPreviewCubeSelectionRect(Projection, Model, {0.62f, 0.4f}, {0.8f, 0.6f}));
	CHECK_FALSE(IntersectsPreviewCubeSelectionRect(Projection, Model, {0.63f, 0.4f}, {0.8f, 0.6f}));
	CHECK_FALSE(IntersectsPreviewCubeSelectionRect(Projection, Model, {0.f, 0.f}, {0.3f, 0.3f}));
	CHECK(IntersectsPreviewCubeSelectionRect(Projection, Model, {0.5f, 0.5f}, {0.5f, 0.5f}));
}

TEST_CASE("Preview box selection uses conservative rotated nonuniformly scaled mesh bounds")
{
	const FMatrix4 Projection = FMatrix4::PerspectiveReversedInfinite(std::numbers::pi_v<float> * 0.5f, 1.f, 0.1f);
	const FMatrix4 Straight = FMatrix4::Transform({0.f, 0.f, 8.f}, FQuaternion::Identity(), {4.f, 0.5f, 1.f});
	const FMatrix4 Rotated = FMatrix4::Transform({0.f, 0.f, 8.f}, FQuaternion::FromAxisAngle(FVector3::Forward(), std::numbers::pi_v<float> * 0.25f), {4.f, 0.5f, 1.f});
	CHECK_FALSE(IntersectsPreviewCubeSelectionRect(Projection, Straight, {0.28f, 0.28f}, {0.29f, 0.29f}));
	CHECK(IntersectsPreviewCubeSelectionRect(Projection, Rotated, {0.28f, 0.28f}, {0.29f, 0.29f}));
	CHECK_FALSE(IntersectsPreviewCubeSelectionRect(Projection, Rotated, {0.f, 0.f}, {0.2f, 0.2f}));
	const FMatrix4 Flat = FMatrix4::Transform({0.f, 0.f, 5.f}, FQuaternion::Identity(), {0.5f, 0.5f, 0.f});
	CHECK(IntersectsPreviewCubeSelectionRect(Projection, Flat, {0.49f, 0.49f}, {0.51f, 0.51f}));
}

TEST_CASE("Preview box selection clips crossing and behind-eye bounds to the reversed-Z near plane")
{
	const FMatrix4 Projection = FMatrix4::PerspectiveReversedInfinite(std::numbers::pi_v<float> * 0.5f, 1.f, 0.1f);
	const FMatrix4 Crossing = FMatrix4::Transform({0.f, 0.f, 0.05f}, FQuaternion::Identity(), {0.02f, 0.02f, 0.1f});
	CHECK(IntersectsPreviewCubeSelectionRect(Projection, Crossing, {0.39f, 0.49f}, {0.405f, 0.51f}));
	CHECK_FALSE(IntersectsPreviewCubeSelectionRect(Projection, Crossing, {0.2f, 0.4f}, {0.35f, 0.6f}));
	const FMatrix4 BeforeNear = FMatrix4::Transform({0.f, 0.f, 0.05f}, FQuaternion::Identity(), {0.01f, 0.01f, 0.01f});
	CHECK_FALSE(IntersectsPreviewCubeSelectionRect(Projection, BeforeNear, {0.f, 0.f}, {1.f, 1.f}));
	CHECK_FALSE(IntersectsPreviewCubeSelectionRect(Projection, FMatrix4::Translation({0.f, 0.f, -5.f}), {0.f, 0.f}, {1.f, 1.f}));
	CHECK(IntersectsPreviewCubeSelectionRect(Projection, FMatrix4::Identity(), {0.01f, 0.01f}, {0.02f, 0.02f}));
}

TEST_CASE("Preview box selection respects both depth planes for finite reversed-Z projections")
{
	FMatrix4 Projection;
	Projection(2, 2) = -0.1f;
	Projection(2, 3) = 1.f;
	const FMatrix4 Crossing = FMatrix4::Transform({0.f, 0.f, 9.9f}, FQuaternion::Identity(), {0.2f, 0.2f, 0.2f});
	CHECK(IntersectsPreviewCubeSelectionRect(Projection, Crossing, {0.49f, 0.49f}, {0.51f, 0.51f}));
	CHECK_FALSE(IntersectsPreviewCubeSelectionRect(Projection, FMatrix4::Translation({0.f, 0.f, 12.f}), {0.f, 0.f}, {1.f, 1.f}));
	CHECK_FALSE(IntersectsPreviewCubeSelectionRect(Projection, FMatrix4::Translation({0.f, 0.f, -2.f}), {0.f, 0.f}, {1.f, 1.f}));
}

TEST_CASE("Preview box selection follows aspect ratio shifted centers and camera view transforms")
{
	const FMatrix4 Model = FMatrix4::Transform({0.f, 0.f, 5.f}, FQuaternion::Identity(), {0.5f, 0.5f, 0.5f});
	FMatrix4 Projection = FMatrix4::PerspectiveReversedInfinite(std::numbers::pi_v<float> * 0.5f, 2.f, 0.1f);
	Projection(0, 2) = -0.4f;
	Projection(1, 2) = -0.3f;
	CHECK(IntersectsPreviewCubeSelectionRect(Projection, Model, {0.29f, 0.64f}, {0.31f, 0.66f}));
	CHECK_FALSE(IntersectsPreviewCubeSelectionRect(Projection, Model, {0.49f, 0.49f}, {0.51f, 0.51f}));
	CHECK_FALSE(IntersectsPreviewCubeSelectionRect(Projection, Model, {0.22f, 0.64f}, {0.23f, 0.66f}));
	Projection(0, 0) *= 4.f;
	CHECK(IntersectsPreviewCubeSelectionRect(Projection, Model, {0.22f, 0.64f}, {0.23f, 0.66f}));
	const FViewportCameraController Camera;
	const auto Snapshot = Camera.GetSnapshot(16.f / 9.f, {0.3f, 0.7f});
	const FVector4 Clip = Snapshot.Projection * Snapshot.View * FVector4{0.f, 0.f, 0.f, 1.f};
	const FVector2 Screen{(Clip.X / Clip.W + 1.f) * 0.5f, (1.f - Clip.Y / Clip.W) * 0.5f};
	CHECK(IntersectsPreviewCubeSelectionRect(Snapshot.Projection * Snapshot.View, FMatrix4::Identity(), Screen - FVector2{0.01f, 0.01f}, Screen + FVector2{0.01f, 0.01f}));
}

TEST_CASE("Preview box selection rejects invalid inputs and handles finite wide-range transforms")
{
	const FMatrix4 Projection = FMatrix4::PerspectiveReversedInfinite(std::numbers::pi_v<float> * 0.5f, 1.f, 0.1f);
	const FMatrix4 Model = FMatrix4::Translation({0.f, 0.f, 5.f});
	const float Invalid = std::numeric_limits<float>::quiet_NaN();
	const float Infinity = std::numeric_limits<float>::infinity();
	CHECK_FALSE(IntersectsPreviewCubeSelectionRect(Projection, Model, {Invalid, 0.f}, {1.f, 1.f}));
	CHECK_FALSE(IntersectsPreviewCubeSelectionRect(Projection, Model, {0.f, 0.f}, {1.f, Infinity}));
	FMatrix4 InvalidProjection = Projection;
	InvalidProjection(0, 0) = Invalid;
	CHECK_FALSE(IntersectsPreviewCubeSelectionRect(InvalidProjection, Model, {0.f, 0.f}, {1.f, 1.f}));
	FMatrix4 InvalidModel = Model;
	InvalidModel(0, 3) = Infinity;
	CHECK_FALSE(IntersectsPreviewCubeSelectionRect(Projection, InvalidModel, {0.f, 0.f}, {1.f, 1.f}));
	InvalidModel = Model;
	InvalidModel(3, 0) = 0.5f;
	CHECK_FALSE(IntersectsPreviewCubeSelectionRect(Projection, InvalidModel, {0.f, 0.f}, {1.f, 1.f}));
	CHECK_FALSE(IntersectsPreviewCubeSelectionRect(FMatrix4::Zero(), Model, {0.f, 0.f}, {1.f, 1.f}));
	FMatrix4 WideProjection = Projection;
	WideProjection(0, 0) = std::numeric_limits<float>::max();
	const FMatrix4 WideModel = FMatrix4::Transform({0.f, 0.f, 5.f}, FQuaternion::Identity(), {std::numeric_limits<float>::max(), 1.f, 1.f});
	CHECK(IntersectsPreviewCubeSelectionRect(WideProjection, WideModel, {0.49f, 0.49f}, {0.51f, 0.51f}));
}
}
