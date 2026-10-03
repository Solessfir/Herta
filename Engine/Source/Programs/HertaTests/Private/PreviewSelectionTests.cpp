#include "Herta/EditorCore/PreviewSelection.h"

#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <limits>
#include <numbers>

namespace Herta
{
namespace
{
void CheckPreviewSilhouetteFaces(const std::vector<std::pair<FVector3, FVector3>>& Edges, const FVector3& CameraPosition)
{
	for (const auto& [First, Second] : Edges)
	{
		int FrontFaces = 0;
		int AdjacentFaces = 0;
		for (std::size_t Axis = 0; Axis < 3; ++Axis)
		{
			if (First[Axis] == Second[Axis])
			{
				CHECK(std::abs(First[Axis]) == 1.f);
				++AdjacentFaces;
				FrontFaces += First[Axis] > 0.f ? CameraPosition[Axis] > 1.f : CameraPosition[Axis] < -1.f;
			}
		}

		CHECK(AdjacentFaces == 2);
		CHECK(FrontFaces == 1);
	}
}
}

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

TEST_CASE("Preview silhouette contains four front-facing outline edges")
{
	const FVector3 CameraPosition{0.f, 0.f, -5.f};
	const auto Edges = GetPreviewCubeSilhouette(CameraPosition, FMatrix4::Identity());
	REQUIRE(Edges.size() == 4);
	for (const auto& [First, Second] : Edges)
	{
		CHECK(First.Z == -1.f);
		CHECK(Second.Z == -1.f);
	}

	CheckPreviewSilhouetteFaces(Edges, CameraPosition);
}

TEST_CASE("Preview silhouette contains six oblique outline edges and no inner edges")
{
	for (const FVector3 CameraPosition : std::array{FVector3{3.f, 4.f, -5.f}, FVector3{-3.f, -4.f, 5.f}, FVector3{-3.f, 4.f, -5.f}})
	{
		const auto Edges = GetPreviewCubeSilhouette(CameraPosition, FMatrix4::Identity());
		REQUIRE(Edges.size() == 6);
		CheckPreviewSilhouetteFaces(Edges, CameraPosition);
	}

	CHECK(GetPreviewCubeSilhouette(FVector3::Zero(), FMatrix4::Identity()).empty());
}

TEST_CASE("Preview silhouette transforms rotated nonuniformly scaled endpoints into world space")
{
	const FMatrix4 Model = FMatrix4::Transform({2.f, 3.f, 4.f}, FQuaternion::FromAxisAngle(FVector3::Up(), std::numbers::pi_v<float> * 0.5f), {2.f, 3.f, 4.f});
	const FVector3 LocalCamera{3.f, 4.f, -5.f};
	const auto LocalEdges = GetPreviewCubeSilhouette(LocalCamera, FMatrix4::Identity());
	const auto WorldEdges = GetPreviewCubeSilhouette(Model.TransformPosition(LocalCamera), Model);
	REQUIRE(WorldEdges.size() == LocalEdges.size());
	for (std::size_t Index = 0; Index < LocalEdges.size(); ++Index)
	{
		CHECK(WorldEdges[Index].first.IsNearlyEqual(Model.TransformPosition(LocalEdges[Index].first), 0.0001f));
		CHECK(WorldEdges[Index].second.IsNearlyEqual(Model.TransformPosition(LocalEdges[Index].second), 0.0001f));
	}
}

TEST_CASE("Preview selection rejects singular invalid and non-TRS inputs")
{
	const FViewportPickingRay Ray{.Origin = {0.f, 0.f, -5.f}, .Direction = FVector3::Forward()};
	const FMatrix4 Singular = FMatrix4::Scale({0.f, 1.f, 1.f});
	CHECK_FALSE(HitTestPreviewCube(Ray, Singular));
	CHECK(GetPreviewCubeSilhouette(Ray.Origin, Singular).empty());
	FMatrix4 Sheared;
	Sheared(0, 1) = 0.5f;
	CHECK_FALSE(HitTestPreviewCube(Ray, Sheared));
	CHECK(GetPreviewCubeSilhouette(Ray.Origin, Sheared).empty());
	FMatrix4 NonAffine;
	NonAffine(3, 0) = 0.5f;
	CHECK_FALSE(HitTestPreviewCube(Ray, NonAffine));
	CHECK(GetPreviewCubeSilhouette(Ray.Origin, NonAffine).empty());
	const float Invalid = std::numeric_limits<float>::quiet_NaN();
	CHECK_FALSE(HitTestPreviewCube({Ray.Origin, {Invalid, 0.f, 1.f}}, FMatrix4::Identity()));
	CHECK(GetPreviewCubeSilhouette({Invalid, 0.f, -5.f}, FMatrix4::Identity()).empty());
}
}
