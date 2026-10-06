#include "Herta/Core/Log.h"
#include "Herta/Renderer/MeshRenderer.h"
#include "Herta/Tasks/TaskSystem.h"
#include "MaterialShaders.h"
#include "TestFiles.h"
#include "TestGraphicsDevice.h"

#include <doctest/doctest.h>

#include <array>
#include <fstream>
#include <iterator>
#include <numbers>

namespace Herta
{
struct FMaterialShadersTestAccess
{
	static void Poll(FMaterialShaders& Watcher);
};

void FMaterialShadersTestAccess::Poll(FMaterialShaders& Watcher)
{
	REQUIRE_FALSE(Watcher.bPolling);
	Watcher.Poll();
}

namespace
{
std::filesystem::path FindShaderRoot()
{
	std::error_code Error;
	for (auto Path = std::filesystem::absolute(Tests::GetSiblingExecutable("HertaShaderWorker"), Error).parent_path(); !Path.empty(); Path = Path.parent_path())
	{
		if (std::filesystem::is_regular_file(Path / "Engine/Shaders/TexturedMesh.slang", Error))
		{
			return Path / "Engine/Shaders";
		}

		if (Path == Path.parent_path())
		{
			break;
		}
	}

	return {};
}

std::string ReadShaderText(const std::filesystem::path& Path)
{
	std::ifstream Stream(Path, std::ios::binary);
	REQUIRE(Stream.good());
	return {std::istreambuf_iterator<char>(Stream), std::istreambuf_iterator<char>()};
}

struct FMaterialShadersFixture
{
	FMaterialShadersFixture();
	~FMaterialShadersFixture();
	FMaterialShadersFixture(const FMaterialShadersFixture&) = delete;
	FMaterialShadersFixture& operator=(const FMaterialShadersFixture&) = delete;
	FMaterialShadersFixture(FMaterialShadersFixture&&) = delete;
	FMaterialShadersFixture& operator=(FMaterialShadersFixture&&) = delete;

	void WriteInclude(std::string_view Text);
	void Poll();
	void Compile();
	FGraphicsPipelineHandle Draw();

	Tests::FScratchDirectory Scratch{"HertaMaterialShadersTests"};
	std::unique_ptr<FLogService> Log;
	std::unique_ptr<FTaskSystem> Tasks;
	Tests::FTestGraphicsDevice Device;
	std::unique_ptr<FMeshRenderer> Renderer;
	std::shared_ptr<const FRenderMesh> Mesh;
	std::shared_ptr<const FRenderMaterial> Material;
	std::string Source;
	std::array<std::string, 1> Paths{"Game/Material.slang"};
	std::unique_ptr<FMaterialShaders> Watcher;
};

FMaterialShadersFixture::FMaterialShadersFixture()
{
	const auto ShaderRoot = FindShaderRoot();
	REQUIRE_FALSE(ShaderRoot.empty());
	Source = "#include \"Tint.slangh\"\n" + ReadShaderText(ShaderRoot / "TexturedMesh.slang");
	Tests::WriteText(Scratch.GetPath() / "Shaders/TexturedMesh.slang", ReadShaderText(ShaderRoot / "TexturedMesh.slang"));
	Tests::WriteText(Scratch.GetPath() / "Shaders/VisualShared.slangh", ReadShaderText(ShaderRoot / "VisualShared.slangh"));
	Tests::WriteText(Scratch.GetPath() / "Game/Material.slang", Source);
	Tests::WriteText(Scratch.GetPath() / "Engine/Placeholder.txt", "content");
	WriteInclude("static const float MaterialRevision = 1;\n");
	auto LogResult = FLogService::Create({.bConsoleOutput = false, .bDebuggerOutput = false, .bFileOutput = false});
	REQUIRE(LogResult);
	Log = std::move(*LogResult);
	auto TaskResult = FTaskSystem::Create({.bDeterministic = true, .Log = Log.get()});
	REQUIRE(TaskResult);
	Tasks = std::move(*TaskResult);
	auto Created = FMeshRenderer::Create(Device, {}, {});
	REQUIRE(Created);
	Renderer = std::move(*Created);
	FCookedTexture White{.ColorSpace = ETextureColorSpace::Srgb, .Mips = {{.Width = 1, .Height = 1, .Pixels = std::vector<std::byte>(4, std::byte{255})}}};
	const auto Cube = FRenderMesh::Create(Device, CreateTexturedCubeModel(std::move(White)), "Test cube");
	REQUIRE(Cube);
	Mesh = *Cube;
	const auto Surface = FRenderMaterial::Create(Device, FMaterialAsset{.ShaderPath = "Material.slang"}, {}, "Game/Material.hmat");
	REQUIRE(Surface);
	Material = *Surface;
	Watcher = FMaterialShaders::Create(*Tasks, *Renderer, *Log, Scratch.GetPath() / "Shaders", Tests::GetSiblingExecutable("HertaShaderWorker"), Scratch.GetPath() / "Engine", Scratch.GetPath() / "Game");
	REQUIRE(Watcher);
	Watcher->Tick(Paths);
	Tasks->RunUntilIdle();
}

FMaterialShadersFixture::~FMaterialShadersFixture()
{
	Watcher.reset();
	Tasks->Shutdown();
}

void FMaterialShadersFixture::WriteInclude(const std::string_view Text)
{
	Tests::WriteText(Scratch.GetPath() / "Game/Tint.slangh", Text);
}

void FMaterialShadersFixture::Poll()
{
	FMaterialShadersTestAccess::Poll(*Watcher);
	Tasks->RunUntilIdle();
}

void FMaterialShadersFixture::Compile()
{
	Watcher->Tick(Paths);
	Tasks->RunUntilIdle();
}

FGraphicsPipelineHandle FMaterialShadersFixture::Draw()
{
	const FMatrix4 Model;
	const FRenderMesh* const MeshPointer = Mesh.get();
	const FRenderMaterial* const MaterialPointer = Material.get();
	const std::span<const FRenderMaterial* const> Slots{&MaterialPointer, 1};
	const FMeshRenderView View{
	    .View = FMatrix4::Translation({0, 0, 5}),
	    .Projection = FMatrix4::PerspectiveReversedInfinite(std::numbers::pi_v<float> / 3.f, 4.f / 3.f, 0.1f),
	    .Models = {&Model, 1},
	    .Meshes = {&MeshPointer, 1},
	    .Materials = {&Slots, 1},
	};

	REQUIRE(Renderer->Render({320, 240}, View));
	return Device.LastDraw.Pipeline;
}
}

TEST_CASE("Material shader iteration uses immutable includes and retains the last valid pipeline")
{
	FMaterialShadersFixture Fixture;
	Fixture.WriteInclude("not valid Slang;\n");
	Fixture.Compile();
	const auto* Status = Fixture.Watcher->GetStatus(Fixture.Paths[0]);
	REQUIRE(Status);
	REQUIRE_MESSAGE(Status->bReady, Status->Diagnostics);
	const auto GoodPipeline = Fixture.Draw();
	REQUIRE(GoodPipeline);
	Fixture.Poll();
	Fixture.Compile();
	Status = Fixture.Watcher->GetStatus(Fixture.Paths[0]);
	CHECK(Status->bReady);
	CHECK_FALSE(Status->bCompiling);
	CHECK_FALSE(Status->Diagnostics.empty());
	CHECK(Fixture.Draw() == GoodPipeline);
	const auto FailedGeneration = Status->Generation;
	Fixture.Compile();
	CHECK(Fixture.Watcher->GetStatus(Fixture.Paths[0])->Generation == FailedGeneration);
	CHECK(Fixture.Draw() == GoodPipeline);

	Fixture.WriteInclude("static const float MaterialRevision = 2;\n");
	Fixture.Poll();
	Fixture.Compile();
	CHECK(Fixture.Watcher->GetStatus(Fixture.Paths[0])->bReady);
	CHECK(Fixture.Draw() != GoodPipeline);
}

TEST_CASE("Material shader publication is atomic when the instanced pipeline is rejected")
{
	FMaterialShadersFixture Fixture;
	Fixture.Compile();
	const auto GoodPipeline = Fixture.Draw();
	Fixture.Device.ValidatePipeline = [](const FGraphicsPipelineDescriptor& Descriptor) -> std::expected<void, FPresentationError>
	{
		if (Descriptor.Name == "Instanced material Game/Material.slang")
		{
			return std::unexpected(FPresentationError{.Code = EPresentationErrorCode::InvalidDescriptor, .Message = "Incompatible instanced shader layout"});
		}

		return {};
	};

	Fixture.WriteInclude("static const float MaterialRevision = 3;\n");
	Fixture.Poll();
	Fixture.Compile();
	CHECK(Fixture.Watcher->GetStatus(Fixture.Paths[0])->Diagnostics == "Incompatible instanced shader layout");
	CHECK(Fixture.Draw() == GoodPipeline);
}

TEST_CASE("Material shader changes coalesce and stale snapshots never publish")
{
	FMaterialShadersFixture Fixture;
	Fixture.Compile();
	const auto GoodPipeline = Fixture.Draw();
	Fixture.WriteInclude("static const float MaterialRevision = 4;\n");
	Fixture.Poll();
	Fixture.WriteInclude("static const float MaterialRevision = 5;\n");
	FMaterialShadersTestAccess::Poll(*Fixture.Watcher);
	Fixture.Watcher->Tick(Fixture.Paths);
	Fixture.Tasks->RunUntilIdle();
	CHECK(Fixture.Draw() == GoodPipeline);
	Fixture.Compile();
	CHECK(Fixture.Draw() != GoodPipeline);
}

TEST_CASE("Material shader publication rejects mismatched visual uniforms before replacing pipelines")
{
	FMaterialShadersFixture Fixture;
	Fixture.Compile();
	const auto GoodPipeline = Fixture.Draw();
	const auto* Pipeline = dynamic_cast<const Tests::FTestPipeline*>(GoodPipeline.get());
	REQUIRE(Pipeline);
	auto Vertex = Pipeline->Descriptor.VertexShader;
	auto Instanced = Vertex;
	Instanced.EntryPoint = "instancedVertexMain";
	auto Fragment = Pipeline->Descriptor.FragmentShader;
	bool bCreatedPipeline = false;
	Fixture.Device.ValidatePipeline = [&](const FGraphicsPipelineDescriptor&) -> std::expected<void, FPresentationError>
	{
		bCreatedPipeline = true;
		return {};
	};

	for (auto& Binding : Fragment.Bindings)
	{
		if (Binding.Type == EShaderBindingType::ConstantBuffer)
		{
			Binding.ByteSize = 16;
		}
	}

	CHECK_FALSE(Fixture.Renderer->PublishMaterialShader(Fixture.Paths[0], Vertex, Instanced, Fragment));
	CHECK_FALSE(bCreatedPipeline);
	CHECK(Fixture.Draw() == GoodPipeline);
	Fragment = Pipeline->Descriptor.FragmentShader;
	Fragment.Bindings.clear();
	CHECK_FALSE(Fixture.Renderer->PublishMaterialShader(Fixture.Paths[0], Vertex, Instanced, Fragment));
	CHECK(Fixture.Draw() == GoodPipeline);
}
}
