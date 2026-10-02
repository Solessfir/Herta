#include "Herta/Math/Matrix.h"
#include "Herta/Renderer/MeshRenderer.h"
#include "TestGraphicsDevice.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <doctest/doctest.h>
#include <limits>

namespace
{
using Herta::Tests::FTestGraphicsDevice;
using Herta::Tests::FTestPipeline;

Herta::FVector3 Position(const Herta::FMeshVertex& Vertex)
{
	return {Vertex.Position[0], Vertex.Position[1], Vertex.Position[2]};
}

std::expected<std::unique_ptr<Herta::FMeshRenderer>, Herta::FPresentationError> CreateDebugRenderer(FTestGraphicsDevice& Device)
{
	Herta::FShaderAsset Vertex;
	Vertex.Bytecode = {1};
	Herta::FShaderAsset Fragment = Vertex;
	Fragment.Stage = Herta::EShaderStage::Fragment;
	return Herta::FMeshRenderer::Create(Device, {}, {}, Vertex, Fragment);
}

std::expected<std::unique_ptr<Herta::FMeshRenderer>, Herta::FPresentationError> CreateGridRenderer(FTestGraphicsDevice& Device, const bool bOnlyGridVertexShader = false)
{
	Herta::FShaderAsset Vertex;
	Vertex.Bytecode = {1};
	Herta::FShaderAsset Fragment = Vertex;
	Fragment.Stage = Herta::EShaderStage::Fragment;
	Herta::FShaderAsset DebugVertex = Vertex;
	Herta::FShaderAsset DebugFragment = Fragment;
	Herta::FShaderAsset GridVertex = Vertex;
	Herta::FShaderAsset GridFragment = Fragment;
	if (bOnlyGridVertexShader)
	{
		GridFragment.Bytecode.clear();
	}
	return Herta::FMeshRenderer::Create(Device, {}, {}, DebugVertex, DebugFragment, GridVertex, GridFragment);
}
}

TEST_CASE("Mesh renderer skips zero extents and rejects nonfinite rotation before recording")
{
	FTestGraphicsDevice Device;
	const auto Renderer = Herta::FMeshRenderer::Create(Device, {}, {});
	REQUIRE(Renderer);
	const std::uint64_t UploadSubmission = Device.Submissions;
	Device.Events.clear();
	CHECK((*Renderer)->Render({0, 100}));
	CHECK((*Renderer)->Render({100, 0}));
	CHECK_FALSE((*Renderer)->Render({100, 100}, std::numeric_limits<float>::infinity()));
	CHECK(Device.Events.empty());
	CHECK(Device.Submissions == UploadSubmission);
	CHECK_FALSE((*Renderer)->GetColorTarget());
}

TEST_CASE("Mesh renderer preserves previous targets when resize allocation fails")
{
	FTestGraphicsDevice Device;
	const auto Renderer = Herta::FMeshRenderer::Create(Device, {}, {});
	REQUIRE(Renderer);
	REQUIRE((*Renderer)->Render({320, 240}));
	const Herta::FTextureHandle Previous = (*Renderer)->GetColorTarget();
	const std::uint64_t Submission = Device.Submissions;
	Device.bFailDepth = true;
	Device.Events.clear();
	CHECK_FALSE((*Renderer)->Render({640, 480}));
	CHECK((*Renderer)->GetColorTarget() == Previous);
	CHECK(Previous->GetDescriptor().Extent == Herta::FExtent2D{320, 240});
	CHECK(Device.Submissions == Submission);
	CHECK(Device.Events.empty());
	Device.bFailDepth = false;
	REQUIRE((*Renderer)->Render({640, 480}));
	CHECK((*Renderer)->GetColorTarget() != Previous);
}

TEST_CASE("Mesh renderer records clear before indexed reversed-Z geometry")
{
	FTestGraphicsDevice Device;
	const auto Renderer = Herta::FMeshRenderer::Create(Device, {}, {});
	REQUIRE(Renderer);
	Device.Events.clear();
	REQUIRE((*Renderer)->Render({640, 480}, 0));
	CHECK(Device.Events == std::vector<std::string>{"Begin", "Clear", "Draw", "Submit"});
	CHECK(Device.LastDraw.IndexCount == 36);
	REQUIRE(Device.LastDraw.DepthTarget);
	CHECK(Device.LastDraw.DepthTarget->GetDescriptor().Format == Herta::ETextureFormat::Depth32);
	const Herta::FMatrix4 Transform(Device.LastDraw.WorldToClip);
	const Herta::FVector4 Near = Transform * Herta::FVector4{0, 0, -1, 1};
	const Herta::FVector4 Far = Transform * Herta::FVector4{0, 0, 1, 1};
	CHECK(Near.Z == doctest::Approx(0.1f));
	CHECK(Far.Z == doctest::Approx(0.1f));
	CHECK(Near.W > 0);
	CHECK(Far.W > Near.W);
	CHECK(Near.Z / Near.W > Far.Z / Far.W);
	CHECK(Near.Z / Near.W < 1.0f);
}

TEST_CASE("Mesh renderer uploads counter-clockwise cube triangles with valid UVs")
{
	FTestGraphicsDevice Device;
	const auto Renderer = Herta::FMeshRenderer::Create(Device, {}, {});
	REQUIRE(Renderer);
	REQUIRE(Device.Vertices.size() == 24);
	REQUIRE(Device.Indices.size() == 36);
	for (std::size_t Index = 0; Index < Device.Indices.size(); Index += 3)
	{
		REQUIRE(Device.Indices[Index] < Device.Vertices.size());
		REQUIRE(Device.Indices[Index + 1] < Device.Vertices.size());
		REQUIRE(Device.Indices[Index + 2] < Device.Vertices.size());
		const Herta::FVector3 A = Position(Device.Vertices[Device.Indices[Index]]);
		const Herta::FVector3 B = Position(Device.Vertices[Device.Indices[Index + 1]]);
		const Herta::FVector3 C = Position(Device.Vertices[Device.Indices[Index + 2]]);
		const Herta::FVector3 Normal = (B - A).Cross(C - A);
		CHECK(Normal.Dot(A + B + C) > 0);
	}

	for (const Herta::FMeshVertex& Vertex : Device.Vertices)
	{
		CHECK(Vertex.UV[0] >= 0.0f);
		CHECK(Vertex.UV[0] <= 1.0f);
		CHECK(Vertex.UV[1] >= 0.0f);
		CHECK(Vertex.UV[1] <= 1.0f);
	}
}

TEST_CASE("Mesh renderer cancels recording after a failed graph pass")
{
	FTestGraphicsDevice Device;
	const auto Renderer = Herta::FMeshRenderer::Create(Device, {}, {});
	REQUIRE(Renderer);
	Device.Events.clear();
	Device.bFailDraw = true;
	const std::uint64_t Submission = Device.Submissions;
	const auto Result = (*Renderer)->Render({320, 240});
	REQUIRE_FALSE(Result);
	CHECK(Result.error().Message == "Draw failed");
	CHECK(Device.Events == std::vector<std::string>{"Begin", "Clear", "Draw", "Cancel"});
	CHECK(Device.Submissions == Submission);
}

TEST_CASE("Mesh renderer uses caller matrices and rejects nonfinite views before recording")
{
	FTestGraphicsDevice Device;
	const auto Renderer = Herta::FMeshRenderer::Create(Device, {}, {});
	REQUIRE(Renderer);
	Herta::FMatrix4 Model = Herta::FMatrix4::Translation({1, 2, 3});
	Herta::FMeshRenderView View{Herta::FMatrix4::Translation({0, 0, 4}), Herta::FMatrix4::Scale({2, 3, 4}), std::span{&Model, 1}};
	REQUIRE((*Renderer)->Render({320, 240}, View));
	CHECK(Device.LastDraw.WorldToClip == (View.Projection * View.View * Model).Data());
	Device.Events.clear();
	Model(1, 1) = std::numeric_limits<float>::infinity();
	CHECK_FALSE((*Renderer)->Render({320, 240}, View));
	CHECK(Device.Events.empty());
}

TEST_CASE("Mesh renderer records distinct transforms for every model and accepts an empty scene")
{
	FTestGraphicsDevice Device;
	const auto Renderer = Herta::FMeshRenderer::Create(Device, {}, {});
	REQUIRE(Renderer);
	std::array Models{Herta::FMatrix4::Translation({2, 0, 0}), Herta::FMatrix4::Translation({-2, 0, 0}) * Herta::FMatrix4::Scale({2, 0.25f, 3})};
	Herta::FMeshRenderView View{Herta::FMatrix4::Translation({0, 0, 5}), Herta::FMatrix4{}, Models};
	Device.Events.clear();
	REQUIRE((*Renderer)->Render({320, 240}, View));
	REQUIRE(Device.Draws.size() == 2);
	CHECK(Device.Events == std::vector<std::string>{"Begin", "Clear", "Draw", "Draw", "Submit"});
	for (std::size_t Index = 0; Index < Models.size(); ++Index)
	{
		CHECK(Device.Draws[Index].WorldToClip == (View.Projection * View.View * Models[Index]).Data());
		CHECK(Device.Draws[Index].IndexCount == 36);
	}
	Device.Events.clear();
	Models[1](0, 0) = std::numeric_limits<float>::infinity();
	CHECK_FALSE((*Renderer)->Render({320, 240}, View));
	CHECK(Device.Events.empty());
	View.Models = {};
	REQUIRE((*Renderer)->Render({320, 240}, View));
	CHECK(Device.Events == std::vector<std::string>{"Begin", "Clear", "Submit"});
}

TEST_CASE("Debug renderer expands portable pixel sizes and orders depth-tested draws before overlays")
{
	FTestGraphicsDevice Device;
	const auto Renderer = CreateDebugRenderer(Device);
	REQUIRE(Renderer);
	const std::array<Herta::FDebugDrawVertex, 1> Points{{{{0, 0, 0.5f}, 10, {1, 0, 0, 1}}}};
	const std::array<Herta::FDebugDrawVertex, 2> Lines{{{{-0.5f, 0, 0.5f}, 4, {0, 1, 0, 1}}, {{0.5f, 0, 0.5f}, 4, {0, 1, 0, 1}}}};
	const std::array<Herta::FDebugDrawVertex, 3> Triangles{{{{0, 0, 0.5f}}, {{0, 0.5f, 0.5f}}, {{0.5f, 0, 0.5f}}}};
	const std::array Lists{Herta::FDebugDrawList{Herta::EDebugPrimitive::Lines, Lines, false}, Herta::FDebugDrawList{Herta::EDebugPrimitive::Points, Points}, Herta::FDebugDrawList{Herta::EDebugPrimitive::Lines, Lines}, Herta::FDebugDrawList{Herta::EDebugPrimitive::Triangles, Triangles}};
	Device.Events.clear();
	const Herta::FMatrix4 Model;
	Herta::FMeshRenderView View;
	View.Models = std::span{&Model, 1};
	REQUIRE((*Renderer)->Render({200, 100}, View, Lists));
	CHECK(Device.Events == std::vector<std::string>{"Begin", "Clear", "Draw", "Draw", "Draw", "Submit"});
	REQUIRE(Device.Draws.size() == 3);
	CHECK(Device.Draws[1].IndexCount == 15);
	CHECK(Device.Draws[2].IndexCount == 6);
	CHECK(std::static_pointer_cast<FTestPipeline>(Device.Draws[1].Pipeline)->Descriptor.bDepthTest);
	CHECK_FALSE(std::static_pointer_cast<FTestPipeline>(Device.Draws[2].Pipeline)->Descriptor.bDepthTest);
	CHECK_FALSE(Device.Draws[1].Texture);
	REQUIRE(Device.DebugUploads.size() == 2);
	const auto& Vertices = Device.DebugUploads[0];
	CHECK(Vertices[0].Position[0] == doctest::Approx(0.05f));
	CHECK(Vertices[0].Position[1] == doctest::Approx(-0.1f));
	CHECK(Vertices[2].Position[1] == doctest::Approx(0.1f));
	CHECK(Vertices[6].Position[1] == doctest::Approx(0.04f));
	CHECK(Vertices[7].Position[1] == doctest::Approx(-0.04f));
}

TEST_CASE("Debug renderer clips camera-crossing lines and rejects incomplete or nonfinite primitives")
{
	FTestGraphicsDevice Device;
	const auto Renderer = CreateDebugRenderer(Device);
	REQUIRE(Renderer);
	const Herta::FMeshRenderView View{Herta::FMatrix4{}, Herta::FMatrix4::PerspectiveReversedInfinite(1.0f, 1, 0.1f), {}};
	std::array<Herta::FDebugDrawVertex, 2> Vertices{{{{0, 0, -1}, 4}, {{0.5f, 0, 1}, 4}}};
	Herta::FDebugDrawList List{Herta::EDebugPrimitive::Lines, Vertices};
	REQUIRE((*Renderer)->Render({100, 100}, View, std::span{&List, 1}));
	REQUIRE(Device.DebugUploads.size() == 1);
	for (const auto& Vertex : Device.DebugUploads[0])
	{
		CHECK(Vertex.Position[3] >= 0.1f);
		CHECK(Vertex.Position[2] <= Vertex.Position[3]);
		for (const float Value : Vertex.Position)
		{
			CHECK(std::isfinite(Value));
		}
	}
	Device.Events.clear();
	List.Vertices = std::span{Vertices}.first(1);
	CHECK_FALSE((*Renderer)->Render({100, 100}, View, std::span{&List, 1}));
	List.Vertices = Vertices;
	Vertices[0].Size = std::numeric_limits<float>::quiet_NaN();
	CHECK_FALSE((*Renderer)->Render({100, 100}, View, std::span{&List, 1}));
	Vertices[0].Size = 1;
	Vertices[0].Color[0] = std::numeric_limits<float>::infinity();
	CHECK_FALSE((*Renderer)->Render({100, 100}, View, std::span{&List, 1}));
	CHECK(Device.Events.empty());
}

TEST_CASE("Mesh renderer draws the optional world grid between mesh and debug overlays")
{
	FTestGraphicsDevice Device;
	const auto Renderer = CreateGridRenderer(Device);
	REQUIRE(Renderer);
	Herta::FMeshRenderView View;
	const Herta::FMatrix4 Model;
	View.Models = std::span{&Model, 1};
	View.View = Herta::FMatrix4::Translation({-3, 0, -7});
	View.Projection = Herta::FMatrix4::PerspectiveReversedInfinite(1.0f, 1.5f, 0.1f);
	View.bDrawGrid = true;
	View.GridCenter = {3, 9, 7};
	const std::array<Herta::FDebugDrawVertex, 2> Overlay{{{{2.5f, 0, 8.0f}, 4}, {{3.5f, 0, 8.0f}, 4}}};
	const Herta::FDebugDrawList Debug{Herta::EDebugPrimitive::Lines, Overlay, false};
	Device.Events.clear();
	Device.DebugUploads.clear();
	Device.Draws.clear();
	REQUIRE((*Renderer)->Render({200, 100}, View, std::span{&Debug, 1}));
	CHECK(Device.Events == std::vector<std::string>{"Begin", "Clear", "Draw", "Draw", "Draw", "Submit"});
	REQUIRE(Device.Draws.size() == 3);
	CHECK(Device.Draws[0].IndexCount == 36);
	CHECK(Device.Draws[1].IndexCount == 6);
	CHECK(Device.Draws[2].IndexCount == 6);
	CHECK(std::static_pointer_cast<FTestPipeline>(Device.Draws[1].Pipeline)->Descriptor.bDepthTest);
	CHECK_FALSE(std::static_pointer_cast<FTestPipeline>(Device.Draws[2].Pipeline)->Descriptor.bDepthTest);
	CHECK(std::static_pointer_cast<FTestPipeline>(Device.Draws[1].Pipeline)->Descriptor.VertexFormat == Herta::EGraphicsVertexFormat::ColoredClipPosition);
	REQUIRE(Device.DebugUploads.size() == 2);
	REQUIRE(Device.DebugUploads[0].size() == 4);
	REQUIRE(Device.IndexUploads.size() == 3);
	CHECK(Device.IndexUploads[1] == std::vector<std::uint32_t>{0, 1, 2, 0, 2, 3});
	CHECK(std::all_of(Device.IndexUploads[1].begin(), Device.IndexUploads[1].end(), [](const std::uint32_t Index)
	                   {
		                   return Index < 4;
	                   }));
	float CenterX = 0.0f;
	float CenterZ = 0.0f;
	for (const Herta::FColoredClipVertex& Vertex : Device.DebugUploads[0])
	{
		CHECK(Vertex.Color[2] == doctest::Approx(View.GridCenter.X));
		CHECK(Vertex.Color[3] == doctest::Approx(View.GridCenter.Z));
		CenterX += Vertex.Color[0] * 0.25f;
		CenterZ += Vertex.Color[1] * 0.25f;
		const Herta::FVector4 Expected = View.Projection * View.View * Herta::FVector4{Vertex.Color[0], 0, Vertex.Color[1], 1};
		const std::array ExpectedClip{Expected.X, Expected.Y, Expected.Z, Expected.W};
		for (std::size_t Index = 0; Index < ExpectedClip.size(); ++Index)
		{
			CHECK(Vertex.Position[Index] == doctest::Approx(ExpectedClip[Index]));
		}
	}
	CHECK(CenterX == doctest::Approx(View.GridCenter.X));
	CHECK(CenterZ == doctest::Approx(View.GridCenter.Z));
}

TEST_CASE("Mesh renderer rejects an enabled grid without both shaders and preserves the disabled path")
{
	FTestGraphicsDevice Device;
	const auto Renderer = CreateDebugRenderer(Device);
	REQUIRE(Renderer);
	Herta::FMeshRenderView View;
	const Herta::FMatrix4 Model;
	View.Models = std::span{&Model, 1};
	View.bDrawGrid = true;
	Device.Events.clear();
	CHECK_FALSE((*Renderer)->Render({200, 100}, View));
	CHECK(Device.Events.empty());
	View.bDrawGrid = false;
	REQUIRE((*Renderer)->Render({200, 100}, View));
	CHECK(Device.Events == std::vector<std::string>{"Begin", "Clear", "Draw", "Submit"});

	const auto PartialGridRenderer = CreateGridRenderer(Device, true);
	CHECK_FALSE(PartialGridRenderer);
}

TEST_CASE("Render meshes draw each section with its texture and upload within the recording budget")
{
	FTestGraphicsDevice Device;
	const auto Renderer = CreateDebugRenderer(Device);
	REQUIRE(Renderer);

	Herta::FCookedTexture Small{Herta::ETextureColorSpace::Srgb, {{2, 2, std::vector<std::byte>(16)}, {1, 1, std::vector<std::byte>(4)}}};
	Herta::FCookedTexture Large{Herta::ETextureColorSpace::Linear, {}};
	for (std::uint32_t Size = Herta::MaximumCookedTextureDimension; Size > 0; Size /= 2)
	{
		Large.Mips.push_back({Size, Size, std::vector<std::byte>(std::size_t{Size} * Size * 4)});
	}
	Herta::FCookedModel Model;
	Model.Vertices = {{{-1, 0, 2}, {0, 0}}, {{3, 0, 2}, {1, 0}}, {{0, 5, -4}, {0, 1}}};
	Model.Indices = {0, 1, 2, 0, 2, 1};
	Model.Sections = {{0, 3, 0}, {3, 3, 1}};
	Model.Materials = {{"Near", 0}, {"Far", 1}};
	Model.Textures = {Small, Large};

	Device.Events.clear();
	Device.TextureWrites.clear();
	Device.MaximumRecordingBytes = 0;
	const auto Mesh = Herta::FRenderMesh::Create(Device, Model, "Test model");
	REQUIRE(Mesh);
	CHECK(Device.TextureWrites.size() == Small.Mips.size() + Large.Mips.size());
	CHECK(Device.MaximumRecordingBytes <= Herta::MaximumUploadBytesPerRecording);
	CHECK(std::ranges::count(Device.Events, std::string("Submit")) == 3);
	CHECK(Device.Events.back() == "Submit");
	CHECK((*Mesh)->GetBoundsMinimum() == Herta::FVector3{-1, 0, -4});
	CHECK((*Mesh)->GetBoundsMaximum() == Herta::FVector3{3, 5, 2});

	const std::array<Herta::FMatrix4, 2> Models{Herta::FMatrix4{}, Herta::FMatrix4::Translation({0, 1, 0})};
	const std::array<const Herta::FRenderMesh*, 2> Meshes{Mesh->get(), nullptr};
	Herta::FMeshRenderView View{Herta::FMatrix4::Translation({0, 0, 5}), Herta::FMatrix4{}, Models};
	View.Meshes = Meshes;
	Device.Draws.clear();
	REQUIRE((*Renderer)->Render({64, 64}, View));
	REQUIRE(Device.Draws.size() == 3);
	CHECK(Device.Draws[0].FirstIndex == 0);
	CHECK(Device.Draws[1].FirstIndex == 3);
	CHECK(Device.Draws[1].IndexCount == 3);
	CHECK(Device.Draws[0].Texture != Device.Draws[1].Texture);
	CHECK(Device.Draws[1].Texture->GetDescriptor().MipLevels == Large.Mips.size());
	CHECK(Device.Draws[1].Texture->GetDescriptor().Format == Herta::ETextureFormat::Rgba8);
	CHECK(Device.Draws[2].IndexCount == 36);

	View.Meshes = std::span(Meshes).first(1);
	CHECK_FALSE((*Renderer)->Render({64, 64}, View));
	Model.Indices[5] = 7;
	CHECK_FALSE(Herta::FRenderMesh::Create(Device, Model, "Broken model"));
}
