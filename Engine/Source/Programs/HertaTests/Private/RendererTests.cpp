#include "Herta/Math/Matrix.h"
#include "Herta/Renderer/MeshRenderer.h"
#include "TestGraphicsDevice.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <numbers>

namespace
{
using Herta::Tests::FTestGraphicsDevice;
using Herta::Tests::FTestPipeline;

Herta::FVector3 Position(const Herta::FCookedVertex& Vertex)
{
	return {Vertex.Position[0], Vertex.Position[1], Vertex.Position[2]};
}

// The renderer owns no content, so tests draw the same 1 m cube that texture previews use.
std::shared_ptr<const Herta::FRenderMesh> CreateTestCube(FTestGraphicsDevice& Device)
{
	Herta::FCookedTexture White{.ColorSpace = Herta::ETextureColorSpace::Srgb, .Mips = {{.Width = 1, .Height = 1, .Pixels = std::vector<std::byte>(4, std::byte{255})}}};
	auto Mesh = Herta::FRenderMesh::Create(Device, Herta::CreateTexturedCubeModel(std::move(White)), "Test cube");
	REQUIRE_MESSAGE(Mesh, (Mesh ? "" : Mesh.error().Message));
	return std::move(*Mesh);
}

// One cube five meters in front of the camera.
struct FCubeScene
{
	explicit FCubeScene(FTestGraphicsDevice& Device)
	    : Cube(CreateTestCube(Device))
	    , Mesh(Cube.get())
	{
		View.Models = std::span{&Model, 1};
		View.Meshes = std::span{&Mesh, 1};
	}

	FCubeScene(const FCubeScene&) = delete;
	FCubeScene& operator=(const FCubeScene&) = delete;
	FCubeScene(FCubeScene&&) = delete;
	FCubeScene& operator=(FCubeScene&&) = delete;

	std::shared_ptr<const Herta::FRenderMesh> Cube;
	const Herta::FRenderMesh* Mesh = nullptr;
	Herta::FMatrix4 Model;
	Herta::FMeshRenderView View{.View = Herta::FMatrix4::Translation({0, 0, 5}), .Projection = Herta::FMatrix4::PerspectiveReversedInfinite(std::numbers::pi_v<float> / 3.f, 4.f / 3.f, 0.1f), .Models = {}};
};

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

TEST_CASE("Mesh renderer skips zero extents and rejects nonfinite models before recording")
{
	FTestGraphicsDevice Device;
	FCubeScene Scene(Device);
	const auto Renderer = Herta::FMeshRenderer::Create(Device, {}, {});
	REQUIRE(Renderer);
	const std::uint64_t UploadSubmission = Device.Submissions;
	Device.Events.clear();
	CHECK((*Renderer)->Render({0, 100}, Scene.View));
	CHECK((*Renderer)->Render({100, 0}, Scene.View));
	Scene.Model(0, 3) = std::numeric_limits<float>::infinity();
	CHECK_FALSE((*Renderer)->Render({100, 100}, Scene.View));
	CHECK(Device.Events.empty());
	CHECK(Device.Submissions == UploadSubmission);
	CHECK_FALSE((*Renderer)->GetColorTarget());
}

TEST_CASE("Mesh renderer preserves previous targets when resize allocation fails")
{
	FTestGraphicsDevice Device;
	FCubeScene Scene(Device);
	const auto Renderer = Herta::FMeshRenderer::Create(Device, {}, {});
	REQUIRE(Renderer);
	REQUIRE((*Renderer)->Render({320, 240}, Scene.View));
	const Herta::FTextureHandle Previous = (*Renderer)->GetColorTarget();
	const std::uint64_t Submission = Device.Submissions;
	Device.bFailDepth = true;
	Device.Events.clear();
	CHECK_FALSE((*Renderer)->Render({640, 480}, Scene.View));
	CHECK((*Renderer)->GetColorTarget() == Previous);
	CHECK(Previous->GetDescriptor().Extent == Herta::FExtent2D{320, 240});
	CHECK(Device.Submissions == Submission);
	CHECK(Device.Events.empty());
	Device.bFailDepth = false;
	REQUIRE((*Renderer)->Render({640, 480}, Scene.View));
	CHECK((*Renderer)->GetColorTarget() != Previous);
}

TEST_CASE("Mesh renderer records clear before indexed reversed-Z geometry")
{
	FTestGraphicsDevice Device;
	FCubeScene Scene(Device);
	const auto Renderer = Herta::FMeshRenderer::Create(Device, {}, {});
	REQUIRE(Renderer);
	Device.Events.clear();
	REQUIRE((*Renderer)->Render({640, 480}, Scene.View));
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
	CHECK(Near.Z / Near.W < 1.f);
}

TEST_CASE("Textured cube models are 1 m with outward counter-clockwise faces and unmirrored UVs")
{
	const Herta::FCookedModel Cube = Herta::CreateTexturedCubeModel({.ColorSpace = Herta::ETextureColorSpace::Srgb, .Mips = {{.Width = 1, .Height = 1, .Pixels = std::vector<std::byte>(4)}}});
	REQUIRE(Herta::ValidateCookedModel(Cube));
	REQUIRE(Cube.Vertices.size() == 24);
	REQUIRE(Cube.Indices.size() == 36);
	for (const Herta::FCookedVertex& Vertex : Cube.Vertices)
	{
		for (const float Coordinate : Vertex.Position)
		{
			CHECK(std::abs(Coordinate) == 0.5f);
		}

		CHECK(Vertex.UV[0] >= 0.f);
		CHECK(Vertex.UV[0] <= 1.f);
		CHECK(Vertex.UV[1] >= 0.f);
		CHECK(Vertex.UV[1] <= 1.f);
	}

	for (std::size_t Index = 0; Index < Cube.Indices.size(); Index += 3)
	{
		const Herta::FCookedVertex& A = Cube.Vertices[Cube.Indices[Index]];
		const Herta::FCookedVertex& B = Cube.Vertices[Cube.Indices[Index + 1]];
		const Herta::FCookedVertex& C = Cube.Vertices[Cube.Indices[Index + 2]];
		const Herta::FVector3 Normal = (Position(B) - Position(A)).Cross(Position(C) - Position(A));
		CHECK(Normal.Dot(Position(A) + Position(B) + Position(C)) > 0);
		// V grows downward, so an unmirrored texture winds clockwise in UV space on a counter-clockwise face.
		const float UVArea = (B.UV[0] - A.UV[0]) * (C.UV[1] - A.UV[1]) - (B.UV[1] - A.UV[1]) * (C.UV[0] - A.UV[0]);
		CHECK(UVArea < 0.f);
	}
}

TEST_CASE("Mesh renderer cancels recording after a failed graph pass")
{
	FTestGraphicsDevice Device;
	FCubeScene Scene(Device);
	const auto Renderer = Herta::FMeshRenderer::Create(Device, {}, {});
	REQUIRE(Renderer);
	Device.Events.clear();
	Device.bFailDraw = true;
	const std::uint64_t Submission = Device.Submissions;
	const auto Result = (*Renderer)->Render({.Width = 320, .Height = 240}, Scene.View);
	REQUIRE_FALSE(Result);
	CHECK(Result.error().Message == "Draw failed");
	CHECK(Device.Events == std::vector<std::string>{"Begin", "Clear", "Draw", "Cancel"});
	CHECK(Device.Submissions == Submission);
}

TEST_CASE("Mesh renderer uses caller matrices and rejects nonfinite views before recording")
{
	FTestGraphicsDevice Device;
	FCubeScene Scene(Device);
	const auto Renderer = Herta::FMeshRenderer::Create(Device, {}, {});
	REQUIRE(Renderer);
	Scene.Model = Herta::FMatrix4::Translation({1, 2, 3});
	Scene.View.View = Herta::FMatrix4::Translation({0, 0, 4});
	Scene.View.Projection = Herta::FMatrix4::Scale({2, 3, 4});
	REQUIRE((*Renderer)->Render({320, 240}, Scene.View));
	CHECK(Device.LastDraw.WorldToClip == (Scene.View.Projection * Scene.View.View * Scene.Model).Data());
	CHECK(Device.LastDraw.ObjectToView == (Scene.View.View * Scene.Model).Data());
	Device.Events.clear();
	Scene.Model(1, 1) = std::numeric_limits<float>::infinity();
	CHECK_FALSE((*Renderer)->Render({320, 240}, Scene.View));
	CHECK(Device.Events.empty());
}

TEST_CASE("Mesh renderer records distinct transforms for every model and accepts an empty scene")
{
	FTestGraphicsDevice Device;
	const auto Cube = CreateTestCube(Device);
	const auto Renderer = Herta::FMeshRenderer::Create(Device, {}, {});
	REQUIRE(Renderer);
	std::array Models{Herta::FMatrix4::Translation({2, 0, 0}), Herta::FMatrix4::Translation({-2, 0, 0}) * Herta::FMatrix4::Scale({2, 0.25f, 3})};
	const std::array<const Herta::FRenderMesh*, 2> Meshes{Cube.get(), Cube.get()};
	Herta::FMeshRenderView View{.View = Herta::FMatrix4::Translation({0, 0, 5}), .Projection = Herta::FMatrix4{}, .Models = Models};
	View.Meshes = Meshes;
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
	View.Meshes = {};
	REQUIRE((*Renderer)->Render({320, 240}, View));
	CHECK(Device.Events == std::vector<std::string>{"Begin", "Clear", "Submit"});
}

TEST_CASE("Debug renderer expands portable pixel sizes and orders depth-tested draws before overlays")
{
	FTestGraphicsDevice Device;
	const auto Cube = CreateTestCube(Device);
	const Herta::FRenderMesh* const CubeMesh = Cube.get();
	const auto Renderer = CreateDebugRenderer(Device);
	REQUIRE(Renderer);
	const std::array<Herta::FDebugDrawVertex, 1> Points{{{.Position = {0, 0, 0.5f}, .Size = 10, .Color = {1, 0, 0, 1}}}};
	const std::array<Herta::FDebugDrawVertex, 2> Lines{{{.Position = {-0.5f, 0, 0.5f}, .Size = 4, .Color = {0, 1, 0, 1}}, {.Position = {0.5f, 0, 0.5f}, .Size = 4, .Color = {0, 1, 0, 1}}}};
	const std::array<Herta::FDebugDrawVertex, 3> Triangles{{{.Position = {0, 0, 0.5f}}, {.Position = {0, 0.5f, 0.5f}}, {.Position = {0.5f, 0, 0.5f}}}};
	const std::array Lists{Herta::FDebugDrawList{.Primitive = Herta::EDebugPrimitive::Lines, .Vertices = Lines, .bDepthTest = false}, Herta::FDebugDrawList{.Primitive = Herta::EDebugPrimitive::Points, .Vertices = Points}, Herta::FDebugDrawList{.Primitive = Herta::EDebugPrimitive::Lines, .Vertices = Lines}, Herta::FDebugDrawList{.Primitive = Herta::EDebugPrimitive::Triangles, .Vertices = Triangles}};
	Device.Events.clear();
	const Herta::FMatrix4 Model;
	Herta::FMeshRenderView View;
	View.Models = std::span{&Model, 1};
	View.Meshes = std::span{&CubeMesh, 1};
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
	const Herta::FMeshRenderView View{.View = Herta::FMatrix4{}, .Projection = Herta::FMatrix4::PerspectiveReversedInfinite(1.f, 1, 0.1f), .Models = {}};
	std::array<Herta::FDebugDrawVertex, 2> Vertices{{{.Position = {0, 0, -1}, .Size = 4}, {.Position = {0.5f, 0, 1}, .Size = 4}}};
	Herta::FDebugDrawList List{.Primitive = Herta::EDebugPrimitive::Lines, .Vertices = Vertices};
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
	// Created first so the grid's index upload stays at a fixed position in IndexUploads.
	const auto Cube = CreateTestCube(Device);
	const Herta::FRenderMesh* const CubeMesh = Cube.get();
	const auto Renderer = CreateGridRenderer(Device);
	REQUIRE(Renderer);
	Herta::FMeshRenderView View;
	const Herta::FMatrix4 Model;
	View.Models = std::span{&Model, 1};
	View.Meshes = std::span{&CubeMesh, 1};
	View.View = Herta::FMatrix4::Translation({-3, 0, -7});
	View.Projection = Herta::FMatrix4::PerspectiveReversedInfinite(1.f, 1.5f, 0.1f);
	View.bDrawGrid = true;
	View.GridCenter = {3, 9, 7};
	const std::array<Herta::FDebugDrawVertex, 2> Overlay{{{.Position = {2.5f, 0, 8.f}, .Size = 4}, {.Position = {3.5f, 0, 8.f}, .Size = 4}}};
	const Herta::FDebugDrawList Debug{.Primitive = Herta::EDebugPrimitive::Lines, .Vertices = Overlay, .bDepthTest = false};
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

	float CenterX = 0.f;
	float CenterZ = 0.f;
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
	const auto Cube = CreateTestCube(Device);
	const Herta::FRenderMesh* const CubeMesh = Cube.get();
	const auto Renderer = CreateDebugRenderer(Device);
	REQUIRE(Renderer);
	Herta::FMeshRenderView View;
	const Herta::FMatrix4 Model;
	View.Models = std::span{&Model, 1};
	View.Meshes = std::span{&CubeMesh, 1};
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

	Herta::FCookedTexture Small{.ColorSpace = Herta::ETextureColorSpace::Srgb, .Mips = {{.Width = 2, .Height = 2, .Pixels = std::vector<std::byte>(16)}, {.Width = 1, .Height = 1, .Pixels = std::vector<std::byte>(4)}}};
	Herta::FCookedTexture Large{.ColorSpace = Herta::ETextureColorSpace::Linear, .Mips = {}};
	for (std::uint32_t Size = Herta::MaximumCookedTextureDimension; Size > 0; Size /= 2)
	{
		const auto Height = std::max(1u, Size / 2);
		Large.Mips.push_back({.Width = Size, .Height = Height, .Pixels = std::vector<std::byte>(std::size_t{Size} * Height * 4)});
	}

	Herta::FCookedModel Model;
	Model.Vertices = {{.Position = {-1, 0, 2}, .UV = {0, 0}}, {.Position = {3, 0, 2}, .UV = {1, 0}}, {.Position = {0, 5, -4}, .UV = {0, 1}}};
	Model.Indices = {0, 1, 2, 0, 2, 1};
	Model.Sections = {{.FirstIndex = 0, .IndexCount = 3, .Material = 0}, {.FirstIndex = 3, .IndexCount = 3, .Material = 1}};
	Model.Materials = {{.Name = "Near"}, {.Name = "Far"}};
	Model.Materials[0].Textures[0] = 0;
	Model.Materials[1].Textures[0] = 1;
	Model.Textures = {Small, Large, Large};

	Device.Events.clear();
	Device.TextureWrites.clear();
	Device.MaximumRecordingBytes = 0;
	const auto Mesh = Herta::FRenderMesh::Create(Device, Model, "Test model");
	REQUIRE_MESSAGE(Mesh, (Mesh ? "" : Mesh.error().Message));
	CHECK(Device.TextureWrites.size() == Small.Mips.size() + Large.Mips.size() * 2);
	CHECK(Device.MaximumRecordingBytes <= Herta::MaximumUploadBytesPerRecording);
	CHECK(std::ranges::count(Device.Events, std::string("Submit")) == 2);
	CHECK(Device.Events.back() == "Submit");
	CHECK((*Mesh)->GetBoundsMinimum() == Herta::FVector3{-1, 0, -4});
	CHECK((*Mesh)->GetBoundsMaximum() == Herta::FVector3{3, 5, 2});

	const std::array<Herta::FMatrix4, 2> Models{Herta::FMatrix4{}, Herta::FMatrix4::Translation({0, 1, 0})};
	const std::array<const Herta::FRenderMesh*, 2> Meshes{Mesh->get(), nullptr};
	Herta::FMeshRenderView View{.View = Herta::FMatrix4::Translation({0, 0, 5}), .Projection = Herta::FMatrix4{}, .Models = Models};
	View.Meshes = Meshes;
	Device.Draws.clear();
	REQUIRE((*Renderer)->Render({64, 64}, View));
	// The null entry is a mesh that is still loading, so only the two sections draw.
	REQUIRE(Device.Draws.size() == 2);
	CHECK(Device.Draws[0].FirstIndex == 0);
	CHECK(Device.Draws[1].FirstIndex == 3);
	CHECK(Device.Draws[1].IndexCount == 3);
	REQUIRE(Device.Draws[0].Textures.size() == 9);
	REQUIRE(Device.Draws[1].Textures.size() == 9);
	CHECK(Device.Draws[0].Textures[0] != Device.Draws[1].Textures[0]);
	CHECK(Device.Draws[1].Textures[0]->GetDescriptor().MipLevels == Large.Mips.size());
	CHECK(Device.Draws[1].Textures[0]->GetDescriptor().Format == Herta::ETextureFormat::Rgba8);

	View.Meshes = std::span(Meshes).first(1);
	CHECK_FALSE((*Renderer)->Render({64, 64}, View));
	Model.Indices[5] = 7;
	CHECK_FALSE(Herta::FRenderMesh::Create(Device, Model, "Broken model"));
}

TEST_CASE("Render material packed maps share uploads while parameter edits keep GPU textures")
{
	FTestGraphicsDevice Device;
	Herta::FMaterialAsset Material;
	const Herta::FAssetId PackedId{0x0000000000004000, 0x8000000000000001};
	Material.Textures[1].Texture = PackedId;
	Material.Textures[1].Channel = Herta::EMaterialChannel::Blue;
	Material.Textures[2].Texture = PackedId;
	Material.Textures[2].Channel = Herta::EMaterialChannel::Green;
	std::array<std::optional<Herta::FCookedTexture>, Herta::MaterialTextureSlotCount> Sources{};
	Sources[1] = Herta::FCookedTexture{.ColorSpace = Herta::ETextureColorSpace::Linear, .Mips = {{.Width = 1, .Height = 1, .Pixels = std::vector<std::byte>(4, std::byte{128})}}};
	Sources[2] = Sources[1];
	const auto Created = Herta::FRenderMaterial::Create(Device, Material, Sources, "Packed material");
	REQUIRE(Created);
	CHECK(Device.TextureDescriptors.size() == 1);
	CHECK(Device.TextureWrites.size() == 1);
	CHECK(Device.Submissions == 1);
	Material.Parameters.Roughness = 0.75f;
	const auto Updated = (*Created)->WithParameters(Material.Parameters);
	REQUIRE(Updated);
	CHECK((*Updated)->GetParameters() == Material.Parameters);
	CHECK((*Created)->GetParameters().Roughness == 0.5f);
	CHECK(Device.TextureDescriptors.size() == 1);
	CHECK(Device.Submissions == 1);
	Material.Parameters.Roughness = -1.f;
	CHECK_FALSE((*Created)->WithParameters(Material.Parameters));
	Material.Parameters.Roughness = 0.5f;
	Material.Textures[0].Texture = PackedId;
	Sources[0] = Sources[1];
	Sources[0]->ColorSpace = Herta::ETextureColorSpace::Srgb;
	const auto Separate = Herta::FRenderMaterial::Create(Device, Material, Sources, "Separate material");
	REQUIRE(Separate);
	REQUIRE(Device.TextureDescriptors.size() == 3);
	CHECK(Device.TextureDescriptors[1].Format == Herta::ETextureFormat::Rgba8Srgb);
	CHECK(Device.TextureDescriptors[2].Format == Herta::ETextureFormat::Rgba8);
}

TEST_CASE("Mesh renderer submits ten thousand shared cubes as one indexed instanced draw")
{
	FTestGraphicsDevice Device;
	const auto Cube = CreateTestCube(Device);
	Herta::FShaderAsset InstancedVertex;
	InstancedVertex.Bytecode = {1};
	const auto Renderer = Herta::FMeshRenderer::Create(Device, {}, {}, {}, {}, {}, {}, InstancedVertex);
	REQUIRE(Renderer);
	std::vector<Herta::FMatrix4> Models;
	for (std::size_t Index = 0; Index < 10'000; ++Index)
	{
		Models.push_back(Herta::FMatrix4::Translation({static_cast<float>(Index % 100), 0, static_cast<float>(Index / 100)}));
	}

	const std::vector<const Herta::FRenderMesh*> Meshes(Models.size(), Cube.get());
	const Herta::FMeshRenderView View{.View = Herta::FMatrix4::Translation({0, 0, 5}), .Projection = Herta::FMatrix4::PerspectiveReversedInfinite(1.f, 1.f, 0.1f), .Models = Models, .Meshes = Meshes};
	Device.Events.clear();
	REQUIRE((*Renderer)->Render({64, 64}, View));
	CHECK(Device.Events == std::vector<std::string>{"Begin", "Clear", "Draw", "Submit"});
	CHECK((*Renderer)->GetLastDrawCount() == 1);
	REQUIRE(Device.Draws.size() == 1);
	const auto& Draw = Device.Draws[0];
	CHECK(std::static_pointer_cast<FTestPipeline>(Draw.Pipeline)->Descriptor.bInstanced);
	CHECK(Draw.IndexCount == 36);
	CHECK(Draw.InstanceCount == 10'000);
	CHECK(Draw.FirstInstance == 0);
	REQUIRE(Draw.Instances);
	CHECK(Draw.Instances->GetDescriptor().VertexFormat == Herta::EGraphicsVertexFormat::MeshInstance);
	CHECK(Draw.Instances->GetDescriptor().Size == Models.size() * sizeof(Herta::FMeshInstance));
	REQUIRE(Device.InstanceUpload.size() == Models.size());
	for (const std::size_t Index : {std::size_t{0}, std::size_t{99}, std::size_t{9'999}})
	{
		CHECK(Device.InstanceUpload[Index].WorldToClip == (View.Projection * View.View * Models[Index]).Data());
		CHECK(Device.InstanceUpload[Index].ObjectToView == (View.View * Models[Index]).Data());
	}

	const Herta::FBufferHandle PreviousInstances = Draw.Instances;
	Models[99] = Herta::FMatrix4::Translation({1, 2, 3}) * Herta::FMatrix4::Scale({-2, 3, 4});
	Device.Draws.clear();
	REQUIRE((*Renderer)->Render({64, 64}, View));
	CHECK(Device.Draws[0].Instances == PreviousInstances);
	CHECK(Device.InstanceUpload[99].WorldToClip == (View.Projection * View.View * Models[99]).Data());
	CHECK(Device.InstanceUpload[99].ObjectToView == (View.View * Models[99]).Data());
	CHECK(Device.MaximumRecordingBytes <= Herta::MaximumUploadBytesPerRecording);

	Device.Events.clear();
	Device.bFailDraw = true;
	const std::uint64_t Submissions = Device.Submissions;
	CHECK_FALSE((*Renderer)->Render({64, 64}, View));
	CHECK(Device.Events == std::vector<std::string>{"Begin", "Clear", "Draw", "Cancel"});
	CHECK(Device.Submissions == Submissions);
	CHECK((*Renderer)->GetLastDrawCount() == 0);
}

TEST_CASE("Mesh instancing groups shared sections and skips null meshes without changing transforms or textures")
{
	FTestGraphicsDevice Device;
	Herta::FCookedTexture White{.ColorSpace = Herta::ETextureColorSpace::Srgb, .Mips = {{.Width = 1, .Height = 1, .Pixels = std::vector<std::byte>(4, std::byte{255})}}};
	Herta::FCookedModel Model = Herta::CreateTexturedCubeModel(White);
	Model.Textures.push_back(White);
	Model.Materials.push_back({.Name = "Second section"});
	Model.Materials.back().Textures[0] = 1;
	Model.Sections = {{.FirstIndex = 0, .IndexCount = 18, .Material = 0}, {.FirstIndex = 18, .IndexCount = 18, .Material = 1}};
	const auto First = Herta::FRenderMesh::Create(Device, Model, "First mesh");
	const auto Second = CreateTestCube(Device);
	REQUIRE(First);
	Herta::FShaderAsset InstancedVertex;
	InstancedVertex.Bytecode = {1};
	const auto Renderer = Herta::FMeshRenderer::Create(Device, {}, {}, {}, {}, {}, {}, InstancedVertex);
	REQUIRE(Renderer);
	const std::array Models{Herta::FMatrix4::Translation({1, 0, 0}), Herta::FMatrix4::Translation({2, 0, 0}), Herta::FMatrix4::Translation({3, 0, 0}), Herta::FMatrix4::Translation({4, 0, 0})};
	const std::array<const Herta::FRenderMesh*, 4> Meshes{First->get(), Second.get(), nullptr, First->get()};
	Herta::FMeshRenderView View{.View = Herta::FMatrix4{}, .Projection = Herta::FMatrix4{}, .Models = Models, .Meshes = Meshes};
	Device.Draws.clear();
	REQUIRE((*Renderer)->Render({64, 64}, View));
	REQUIRE(Device.Draws.size() == 3);
	CHECK((*Renderer)->GetLastDrawCount() == 3);
	CHECK(Device.Draws[0].FirstInstance == 0);
	CHECK(Device.Draws[0].InstanceCount == 2);
	CHECK(Device.Draws[1].FirstInstance == 0);
	CHECK(Device.Draws[1].InstanceCount == 2);
	CHECK(Device.Draws[1].FirstIndex == 18);
	REQUIRE(Device.Draws[0].Textures.size() == 9);
	REQUIRE(Device.Draws[1].Textures.size() == 9);
	CHECK(Device.Draws[0].Textures[0] != Device.Draws[1].Textures[0]);
	CHECK(Device.Draws[2].FirstInstance == 2);
	CHECK(Device.Draws[2].InstanceCount == 1);
	REQUIRE(Device.InstanceUpload.size() == 3);
	CHECK(Device.InstanceUpload[0].WorldToClip == Models[0].Data());
	CHECK(Device.InstanceUpload[1].WorldToClip == Models[3].Data());
	CHECK(Device.InstanceUpload[2].WorldToClip == Models[1].Data());

	View.Models = {};
	View.Meshes = {};
	Device.Events.clear();
	REQUIRE((*Renderer)->Render({64, 64}, View));
	CHECK(Device.Events == std::vector<std::string>{"Begin", "Clear", "Submit"});
	CHECK((*Renderer)->GetLastDrawCount() == 0);
}

TEST_CASE("Material shader sharing reuses compatible pipelines and counts only successful publications")
{
	using namespace Herta;
	FTestGraphicsDevice Device;
	FTestGraphicsDevice OtherDevice;
	FCubeScene Scene(Device);
	FVisualShaderSet Visuals;
	Visuals.FullscreenVertex.Bytecode = {1};
	const auto Source = FMeshRenderer::Create(Device, {}, {}, {}, {}, {}, {}, {}, Visuals);
	const auto Preview = FMeshRenderer::Create(Device, {}, {}, {}, {}, {}, {}, {}, Visuals);
	const auto Other = FMeshRenderer::Create(OtherDevice, {}, {}, {}, {}, {}, {}, {}, Visuals);
	const auto Legacy = FMeshRenderer::Create(Device, {}, {});
	REQUIRE(Source);
	REQUIRE(Preview);
	REQUIRE(Other);
	REQUIRE(Legacy);
	CHECK((*Source)->GetMaterialShaderGeneration() == 0);
	CHECK_FALSE((*Preview)->ShareMaterialShaders(**Other));
	CHECK_FALSE((*Preview)->ShareMaterialShaders(**Legacy));

	const auto Shader = [](const EShaderStage Stage, const std::string_view EntryPoint)
	{
		FShaderAsset Result;
		Result.Stage = Stage;
		Result.EntryPoint = EntryPoint;
		Result.Bindings = {{.Name = "Visual", .Type = EShaderBindingType::ConstantBuffer, .Binding = 64, .ByteSize = sizeof(FVisualUniforms)}};
		Result.Bytecode = {1};
		return Result;
	};

	const auto Vertex = Shader(EShaderStage::Vertex, "vertexMain");
	const auto Instanced = Shader(EShaderStage::Vertex, "instancedVertexMain");
	const auto Fragment = Shader(EShaderStage::Fragment, "fragmentMain");
	const auto Material = FRenderMaterial::Create(Device, FMaterialAsset{.ShaderPath = "Game/Shared.slang"}, {}, "Shared");
	REQUIRE(Material);
	const FRenderMaterial* const MaterialPointer = Material->get();
	const std::span<const FRenderMaterial* const> Slots{&MaterialPointer, 1};
	Scene.View.Materials = {&Slots, 1};
	const auto Draw = [&](FMeshRenderer& Renderer)
	{
		Device.Draws.clear();
		REQUIRE(Renderer.Render({64, 64}, Scene.View));
		const auto MeshDraw = std::ranges::find_if(Device.Draws, [](const FIndexedDraw& Item)
		{
			return std::static_pointer_cast<FTestPipeline>(Item.Pipeline)->Descriptor.VertexFormat == EGraphicsVertexFormat::Mesh;
		});

		REQUIRE(MeshDraw != Device.Draws.end());
		return MeshDraw->Pipeline;
	};

	const auto Original = Draw(**Preview);
	REQUIRE((*Source)->PublishMaterialShader("Game/Shared.slang", Vertex, Instanced, Fragment));
	CHECK((*Source)->GetMaterialShaderGeneration() == 1);
	const auto Published = Draw(**Source);
	REQUIRE(Published != Original);
	const std::uint64_t Submissions = Device.Submissions;
	bool bPipelineCreated = false;
	Device.ValidatePipeline = [&](const FGraphicsPipelineDescriptor&) -> std::expected<void, FPresentationError>
	{
		bPipelineCreated = true;
		return {};
	};

	REQUIRE((*Preview)->ShareMaterialShaders(**Source));
	CHECK_FALSE(bPipelineCreated);
	CHECK(Device.Submissions == Submissions);
	CHECK((*Preview)->GetMaterialShaderGeneration() == 0);
	CHECK(Draw(**Preview) == Published);
	CHECK_FALSE((*Preview)->ShareMaterialShaders(**Other));
	CHECK(Draw(**Preview) == Published);
	Device.ValidatePipeline = [](const FGraphicsPipelineDescriptor& Descriptor) -> std::expected<void, FPresentationError>
	{
		if (Descriptor.bInstanced)
		{
			return std::unexpected(FPresentationError{.Code = EPresentationErrorCode::InvalidDescriptor, .Message = "Rejected instanced material"});
		}

		return {};
	};

	CHECK_FALSE((*Source)->PublishMaterialShader("Game/Shared.slang", Vertex, Instanced, Fragment));
	CHECK((*Source)->GetMaterialShaderGeneration() == 1);
	REQUIRE((*Preview)->ShareMaterialShaders(**Source));
	CHECK(Draw(**Preview) == Published);
	FShaderAsset Invalid = Fragment;
	Invalid.Bindings.clear();
	CHECK_FALSE((*Source)->PublishMaterialShader("Game/Shared.slang", Vertex, Instanced, Invalid));
	CHECK((*Source)->GetMaterialShaderGeneration() == 1);
	Device.ValidatePipeline = {};
	REQUIRE((*Source)->PublishMaterialShader("Game/Shared.slang", Vertex, Instanced, Fragment));
	CHECK((*Source)->GetMaterialShaderGeneration() == 2);
	const auto Updated = Draw(**Source);
	CHECK(Updated != Published);
	CHECK(Draw(**Preview) == Published);
	REQUIRE((*Preview)->ShareMaterialShaders(**Source));
	CHECK(Draw(**Preview) == Updated);

	Scene.View.Materials = {};
	REQUIRE((*Source)->PublishMaterialShader("", Vertex, Instanced, Fragment));
	CHECK((*Source)->GetMaterialShaderGeneration() == 3);
	const auto Default = Draw(**Source);
	REQUIRE((*Preview)->ShareMaterialShaders(**Source));
	CHECK(Draw(**Preview) == Default);
	CHECK((*Preview)->GetMaterialShaderGeneration() == 0);
}

TEST_CASE("Mesh vertex updates upload inside the frame's recording and must replace every vertex")
{
	FTestGraphicsDevice Device;
	FCubeScene Scene(Device);
	const auto Renderer = Herta::FMeshRenderer::Create(Device, {}, {});
	REQUIRE(Renderer);

	std::vector<Herta::FCookedVertex> Moved(Device.Vertices.size());
	Moved.front().Position = {7.f, 8.f, 9.f};
	const std::array Updates{Herta::FRenderMeshVertexUpdate{.Mesh = Scene.Mesh, .Vertices = Moved}};
	Scene.View.VertexUpdates = Updates;
	Device.Events.clear();
	REQUIRE((*Renderer)->Render({64, 64}, Scene.View));
	CHECK(std::ranges::count(Device.Events, std::string{"Begin"}) == 1);
	REQUIRE(Device.Vertices.size() == Moved.size());
	CHECK(Device.Vertices.front().Position == std::array{7.f, 8.f, 9.f});

	const std::array Partial{Herta::FRenderMeshVertexUpdate{.Mesh = Scene.Mesh, .Vertices = std::span{Moved}.first(1)}};
	Scene.View.VertexUpdates = Partial;
	CHECK_FALSE((*Renderer)->Render({64, 64}, Scene.View));
}
