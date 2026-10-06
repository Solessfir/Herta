#include "Herta/AssetPipeline/AssetCooker.h"
#include "Herta/Core/Log.h"
#include "Herta/Platform/Process.h"
#include "Herta/Tasks/TaskSystem.h"
#include "PreviewAssets.h"
#include "PreviewLevel.h"
#include "TestFiles.h"
#include "TestGraphicsDevice.h"

#include <doctest/doctest.h>

#include <array>
#include <cmath>

namespace Herta
{
struct FPreviewAssetsTestAccess
{
	static void ContentChanged(FPreviewAssets& Assets);
	static std::uint64_t GetRequestGeneration(const FPreviewAssets& Assets);
	static std::size_t GetCachedMeshCount(const FPreviewAssets& Assets);
	static std::size_t GetOutstandingTaskCount(const FPreviewAssets& Assets);
};

void FPreviewAssetsTestAccess::ContentChanged(FPreviewAssets& Assets)
{
	Assets.ContentChanged();
}

std::uint64_t FPreviewAssetsTestAccess::GetRequestGeneration(const FPreviewAssets& Assets)
{
	return Assets.RequestGeneration;
}

std::size_t FPreviewAssetsTestAccess::GetCachedMeshCount(const FPreviewAssets& Assets)
{
	return Assets.MeshCache.size();
}

std::size_t FPreviewAssetsTestAccess::GetOutstandingTaskCount(const FPreviewAssets& Assets)
{
	return Assets.Scope->GetOutstandingTaskCount();
}

namespace
{
constexpr FAssetId StoneId{0x0000000000004000, 0x8000000000000003};
constexpr FAssetId WoodId{0x0000000000004000, 0x8000000000000001};
constexpr FAssetId RobotId{0x0000000000004000, 0x8000000000000002};
constexpr FAssetId PreviewSphereId{0x0000000000004000, 0x8000000000000004};

void CheckEmptyMeshSlot(const FPreviewMeshSlot& Slot)
{
	CHECK_FALSE(Slot.Asset.IsValid());
	CHECK(Slot.Label.empty());
	CHECK_FALSE(Slot.Mesh);
	CHECK_FALSE(Slot.Thumbnail);
	CHECK_FALSE(Slot.Metadata);
	CHECK(Slot.Key == FHash128{});
	CHECK(Slot.ContentGeneration == 0);
	CHECK_FALSE(Slot.bLoading);
	CHECK(Slot.Error.empty());
	CHECK(Slot.Generation == 0);
}

struct FPreviewAssetsFixture
{
	Tests::FScratchDirectory Scratch{"HertaPreviewAssets"};
	std::unique_ptr<FLogService> Log;
	std::unique_ptr<FTaskSystem> Tasks;
	Tests::FTestGraphicsDevice Device;

	FPreviewAssetsFixture()
	{
		std::error_code Error;
		std::filesystem::create_directories(Scratch.GetPath() / "Shaders", Error);
		REQUIRE_FALSE(Error);
		const std::array<std::uint8_t, 4> Pixel{150, 111, 51, 255};
		WriteTexture(Scratch.GetPath() / "Game", "Textures/Wood.png", WoodId, Pixel);
		WriteTexture(Scratch.GetPath() / "Engine", "Textures/Stone.png", StoneId, Pixel);
		Tests::WriteText(Scratch.GetPath() / "Game/Models/Robot.blend", "blend");
		Tests::WriteText(Scratch.GetPath() / "Game/Models/Robot.blend.hmeta", "Format = HertaAssetMetadata\nVersion = 1\nId = " + RobotId.ToString() + "\nImporter = Blender\n");

		auto LogResult = FLogService::Create({.bConsoleOutput = false, .bDebuggerOutput = false, .bFileOutput = false});
		REQUIRE(LogResult);
		Log = std::move(*LogResult);

		auto TaskResult = FTaskSystem::Create({.bDeterministic = true, .Log = Log.get()});
		REQUIRE(TaskResult);
		Tasks = std::move(*TaskResult);
	}

	~FPreviewAssetsFixture()
	{
		Tasks->Shutdown();
	}

	FPreviewAssetsFixture(const FPreviewAssetsFixture&) = delete;
	FPreviewAssetsFixture& operator=(const FPreviewAssetsFixture&) = delete;
	FPreviewAssetsFixture(FPreviewAssetsFixture&&) = delete;
	FPreviewAssetsFixture& operator=(FPreviewAssetsFixture&&) = delete;

	static void WriteTexture(const std::filesystem::path& Root, const std::string& Path, const FAssetId& Id, const std::array<std::uint8_t, 4>& Pixel)
	{
		Tests::WritePng(Root / Path, 1, 1, Pixel);
		Tests::WriteText(Root / (Path + ".hmeta"), "Format = HertaAssetMetadata\nVersion = 1\nId = " + Id.ToString() + "\nImporter = Texture\n");
	}

	void WritePreviewMesh()
	{
		const auto Root = Scratch.GetPath() / "Engine/Shapes";
		const std::array<float, 9> Positions{-1.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f, 0.f};
		Tests::WriteBytes(Root / "Sphere.bin", std::as_bytes(std::span(Positions)));
		Tests::WriteText(Root / "Sphere.gltf", R"({"asset":{"version":"2.0"},"buffers":[{"uri":"Sphere.bin","byteLength":36}],"bufferViews":[{"buffer":0,"byteLength":36}],"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[-1,0,0],"max":[1,1,0]}],"meshes":[{"primitives":[{"attributes":{"POSITION":0}}]}],"nodes":[{"mesh":0}],"scenes":[{"nodes":[0]}],"scene":0})");
		Tests::WriteText(Root / "Sphere.gltf.hmeta", "Format = HertaAssetMetadata\nVersion = 1\nId = " + PreviewSphereId.ToString() + "\nImporter = Gltf\n");
	}

	[[nodiscard]] std::unique_ptr<FPreviewAssets> CreateAssets(FEditorAssetThumbnailRenderer RenderThumbnail = {})
	{
		const std::filesystem::path& Root = Scratch.GetPath();
		return FPreviewAssets::Create(*Tasks, Device, *Log, {.EngineContentRoot = Root / "Engine", .ContentRoot = Root / "Game", .DerivedDataRoot = Root / "DerivedDataCache", .WorkerPath = Tests::GetSiblingExecutable("HertaAssetWorker"), .TargetPlatform = "TestPlatform"}, 2, std::move(RenderThumbnail));
	}

	// FIFO waiting stops before continuations queued by the preceding work.
	void RunToCheckpoint()
	{
		auto Scope = Tasks->CreateScope("Preview asset checkpoint");
		REQUIRE(Scope);
		auto Checkpoint = Tasks->Submit(**Scope, {.Name = "Preview asset checkpoint", .Lane = ETaskLane::BlockingIo}, [](FTaskContext&) {});
		REQUIRE(Checkpoint);
		REQUIRE(Checkpoint->Wait().State == ETaskState::Succeeded);
	}
};
}

TEST_CASE("Preview metadata describes cooked assets rather than their preview geometry")
{
	const FCookedAsset Texture = FCookedTexture{.ColorSpace = ETextureColorSpace::Linear, .Mips = {{.Width = 8, .Height = 4, .Pixels = {}}, {.Width = 4, .Height = 2, .Pixels = {}}, {.Width = 2, .Height = 1, .Pixels = {}}, {.Width = 1, .Height = 1, .Pixels = {}}}};
	const auto TextureMetadata = GetPreviewAssetMetadata(Texture);
	REQUIRE(std::holds_alternative<FPreviewTextureMetadata>(TextureMetadata));
	const auto& Image = std::get<FPreviewTextureMetadata>(TextureMetadata);
	CHECK(Image.Width == 8);
	CHECK(Image.Height == 4);
	CHECK(Image.Mips == 4);
	CHECK(Image.ColorSpace == ETextureColorSpace::Linear);

	const FCookedAsset Model = FCookedModel{
	    .Vertices = {{.Position = {2.f, -8.f, 1.f}, .UV = {}}, {.Position = {5.f, -3.f, 7.f}, .UV = {}}, {.Position = {3.f, -4.f, 2.f}, .UV = {}}},
	    .Indices = {0, 1, 2},
	    .Sections = {},
	    .Materials = {{.Name = "First"}, {.Name = "Second"}},
	    .Textures = {},
	};

	const auto ModelMetadata = GetPreviewAssetMetadata(Model);
	REQUIRE(std::holds_alternative<FPreviewModelMetadata>(ModelMetadata));
	const auto& Mesh = std::get<FPreviewModelMetadata>(ModelMetadata);
	CHECK(Mesh.Vertices == 3);
	CHECK(Mesh.Triangles == 1);
	CHECK(Mesh.Materials == 2);
	CHECK(Mesh.BoundsMinimum == FVector3{2.f, -8.f, 1.f});
	CHECK(Mesh.BoundsMaximum == FVector3{5.f, -3.f, 7.f});

	FMaterialAsset Material{.Name = "Stone"};
	Material.Parameters.BlendMode = EMaterialBlendMode::Masked;
	Material.Textures[0].Texture = StoneId;
	Material.Textures[3].Texture = WoodId;
	const auto MaterialMetadata = GetPreviewAssetMetadata(Material);
	REQUIRE(std::holds_alternative<FPreviewMaterialMetadata>(MaterialMetadata));
	const auto& Surface = std::get<FPreviewMaterialMetadata>(MaterialMetadata);
	CHECK(Surface.Name == "Stone");
	CHECK(Surface.TextureMaps == 2);
	CHECK(Surface.BlendMode == EMaterialBlendMode::Masked);
}

TEST_CASE("Preview material creation and save preserve identity and the last valid GPU material")
{
	FPreviewAssetsFixture Fixture;
	auto Assets = Fixture.CreateAssets();
	REQUIRE(Assets);
	Fixture.Tasks->RunUntilIdle();
	const auto Created = Assets->CreateMaterial("Game", "New Material");
	REQUIRE_MESSAGE(Created.has_value(), (Created ? "" : Created.error().Message));
	Fixture.Tasks->RunUntilIdle();
	Assets->Tick();
	Fixture.Tasks->RunUntilIdle();
	REQUIRE_MESSAGE(Assets->GetMaterial(*Created), (Assets->GetCachedAsset(*Created) ? Assets->GetCachedAsset(*Created)->Error : "Material not cached"));
	REQUIRE(Assets->GetMaterialSource(*Created));
	CHECK(Assets->GetMaterialSource(*Created)->Name == "New Material");
	const auto Path = Assets->GetMaterialPath(*Created);
	REQUIRE(Path);
	const auto Identity = LoadAssetMetadata(*Path);
	REQUIRE(Identity);
	CHECK(Identity->Id == *Created);

	FMaterialAsset Changed = *Assets->GetMaterialSource(*Created);
	Changed.Parameters.BaseColor = {0.25f, 0.5f, 0.75f, 1.f};
	Changed.Parameters.Roughness = 0.75f;
	Changed.Textures[0].Texture = StoneId;
	Changed.Textures[2].Texture = WoodId;
	REQUIRE(Assets->SaveMaterial(*Created, Changed));
	Fixture.Tasks->RunUntilIdle();
	REQUIRE(Assets->GetMaterial(*Created));
	REQUIRE(Assets->GetMaterialSource(*Created));
	CHECK(*Assets->GetMaterialSource(*Created) == Changed);
	const auto SavedIdentity = LoadAssetMetadata(*Path);
	REQUIRE(SavedIdentity);
	CHECK(SavedIdentity->Id == *Created);

	const auto LastValid = Assets->GetMaterial(*Created);
	Tests::WriteText(*Path, "broken material");
	FPreviewAssetsTestAccess::ContentChanged(*Assets);
	Fixture.Tasks->RunUntilIdle();
	CHECK(Assets->GetMaterial(*Created) == LastValid);
	CHECK(*Assets->GetMaterialSource(*Created) == Changed);
	REQUIRE(Assets->GetCachedAsset(*Created));
	CHECK(Assets->GetCachedAsset(*Created)->Error.find("previous version") != std::string::npos);

	FMaterialAsset Invalid = Changed;
	Invalid.Parameters.Roughness = -1.f;
	CHECK_FALSE(Assets->SaveMaterial(*Created, Invalid));
	CHECK(Assets->GetMaterial(*Created) == LastValid);
	REQUIRE(Assets->SaveMaterial(*Created, Changed));
	Fixture.Tasks->RunUntilIdle();
	CHECK(Assets->GetMaterial(*Created) == LastValid);
	CHECK(Assets->GetCachedAsset(*Created)->Error.empty());
}

TEST_CASE("Preview materials remain Game-owned and have portable unique names")
{
	FPreviewAssetsFixture Fixture;
	auto Assets = Fixture.CreateAssets();
	REQUIRE(Assets);
	Fixture.Tasks->RunUntilIdle();
	CHECK_FALSE(Assets->CreateMaterial("Engine", "Material"));
	CHECK_FALSE(Assets->CreateMaterial("Game/..", "Material"));
	CHECK_FALSE(Assets->CreateMaterial("Game", "../Material"));
	CHECK_FALSE(Assets->CreateMaterial("Game", "CON"));
	const auto First = Assets->CreateMaterial("Game", "Material");
	REQUIRE(First);
	const auto Second = Assets->CreateMaterial("Game", "Material");
	REQUIRE(Second);
	CHECK(*First != *Second);
	Fixture.Tasks->RunUntilIdle();
	Assets->Tick();
	Fixture.Tasks->RunUntilIdle();
	const auto FirstPath = Assets->GetMaterialPath(*First);
	const auto SecondPath = Assets->GetMaterialPath(*Second);
	REQUIRE(FirstPath);
	REQUIRE(SecondPath);
	CHECK(FirstPath->filename() == "Material.hmat");
	CHECK(SecondPath->filename() == "Material 1.hmat");
	CHECK_FALSE(Assets->SaveMaterial(StoneId, FMaterialAsset{}));
	CHECK_FALSE(Assets->SaveMaterial(WoodId, FMaterialAsset{}));
	Assets->SetMaterialAssets({});
	CHECK_FALSE(Assets->GetMaterial(*First));
	CHECK_FALSE(Assets->GetMaterial(*Second));
}

TEST_CASE("Material drafts coalesce without saving or uploading unchanged texture bindings")
{
	FPreviewAssetsFixture Fixture;
	Fixture.WritePreviewMesh();
	auto Assets = Fixture.CreateAssets();
	REQUIRE(Assets);
	Fixture.Tasks->RunUntilIdle();
	const auto Created = Assets->CreateMaterial("Game", "Draft");
	REQUIRE(Created);
	Fixture.Tasks->RunUntilIdle();
	Assets->Tick();
	Fixture.Tasks->RunUntilIdle();
	const auto SavedGpu = Assets->GetMaterial(*Created);
	REQUIRE(SavedGpu);
	const FMaterialAsset Saved = *Assets->GetMaterialSource(*Created);
	const auto Path = Assets->GetMaterialPath(*Created);
	REQUIRE(Path);
	const auto ModifiedBefore = std::filesystem::last_write_time(*Path);
	std::uint64_t RenderCount = 0;
	Assets->SetMaterialThumbnailRenderer([&RenderCount, &Assets](const FRenderMesh& Mesh, const FRenderMaterial&) -> std::expected<FEditorAssetThumbnail, FPresentationError>
	{
		REQUIRE(Assets->GetCachedAsset(PreviewSphereId));
		CHECK(Assets->GetCachedAsset(PreviewSphereId)->Mesh.get() == &Mesh);
		return std::make_shared<const std::uint64_t>(++RenderCount);
	});

	CHECK(Assets->AreMaterialThumbnailsReady());
	CHECK_FALSE(Assets->GetCachedAsset(PreviewSphereId));

	FMaterialAsset Draft = Saved;
	Draft.Parameters.Roughness = 0.1f;
	Assets->RequestMaterialPreview(*Created, Draft);
	CHECK(Assets->IsMaterialPreviewLoading());
	CHECK_FALSE(Assets->GetMaterialPreview());
	Draft.Parameters.Roughness = 0.3f;
	Assets->RequestMaterialPreview(*Created, Draft);
	Draft.Parameters.Roughness = 0.8f;
	Assets->RequestMaterialPreview(*Created, Draft);
	Fixture.Tasks->RunUntilIdle();
	CHECK_FALSE(Assets->AreMaterialThumbnailsReady());
	REQUIRE(Assets->GetCachedAsset(PreviewSphereId));
	const auto Sphere = Assets->GetCachedAsset(PreviewSphereId)->Mesh;
	REQUIRE_MESSAGE(Sphere, Assets->GetCachedAsset(PreviewSphereId)->Error);
	const auto WritesBefore = Fixture.Device.TextureWrites.size();
	const auto SubmissionsBefore = Fixture.Device.Submissions;
	Assets->Tick();
	Fixture.Tasks->RunUntilIdle();
	REQUIRE(Assets->GetMaterialPreview());
	CHECK(Assets->GetMaterialPreview()->GetParameters() == Draft.Parameters);
	CHECK(Assets->GetMaterialPreviewAsset() == *Created);
	CHECK_FALSE(Assets->IsMaterialPreviewLoading());
	CHECK(Assets->GetMaterialPreviewError().empty());
	REQUIRE(Assets->GetMaterialPreviewThumbnail());
	CHECK(RenderCount == 1);
	CHECK(Fixture.Device.TextureWrites.size() == WritesBefore);
	CHECK(Fixture.Device.Submissions == SubmissionsBefore);
	CHECK(Assets->GetMaterial(*Created) == SavedGpu);
	CHECK(*Assets->GetMaterialSource(*Created) == Saved);
	CHECK(std::filesystem::last_write_time(*Path) == ModifiedBefore);
	Assets->RequestMaterialPreview(*Created, Draft);
	Fixture.Tasks->RunUntilIdle();
	CHECK(RenderCount == 1);
	Assets->SetThumbnailAssets({});
	REQUIRE(Assets->GetCachedAsset(PreviewSphereId));
	CHECK(Assets->GetCachedAsset(PreviewSphereId)->Mesh == Sphere);
	const auto RequestsBefore = FPreviewAssetsTestAccess::GetRequestGeneration(*Assets);
	Assets->SetMaterialShaderGeneration(1);
	CHECK(Assets->IsMaterialPreviewLoading());
	Assets->Tick();
	CHECK(RenderCount == 2);
	CHECK_FALSE(Assets->IsMaterialPreviewLoading());
	CHECK(FPreviewAssetsTestAccess::GetRequestGeneration(*Assets) == RequestsBefore);
	CHECK(Fixture.Device.TextureWrites.size() == WritesBefore);
	CHECK(Fixture.Device.Submissions == SubmissionsBefore);
	Assets->SetMaterialShaderGeneration(1);
	for (int Frame = 0; Frame < 3; ++Frame)
	{
		Assets->Tick();
		Fixture.Tasks->RunUntilIdle();
	}

	CHECK(RenderCount == 2);
	CHECK(FPreviewAssetsTestAccess::GetRequestGeneration(*Assets) == RequestsBefore);
	CHECK(Fixture.Device.TextureWrites.size() == WritesBefore);
	CHECK(Fixture.Device.Submissions == SubmissionsBefore);
}

TEST_CASE("Material draft failures keep the last valid preview and cancel without resurrection")
{
	FPreviewAssetsFixture Fixture;
	Fixture.WritePreviewMesh();
	auto Assets = Fixture.CreateAssets();
	REQUIRE(Assets);
	Fixture.Tasks->RunUntilIdle();
	const auto Created = Assets->CreateMaterial("Game", "Draft");
	REQUIRE(Created);
	Fixture.Tasks->RunUntilIdle();
	Assets->Tick();
	Fixture.Tasks->RunUntilIdle();
	REQUIRE(Assets->GetMaterialSource(*Created));
	FMaterialAsset Draft = *Assets->GetMaterialSource(*Created);
	Assets->SetMaterialThumbnailRenderer([](const FRenderMesh&, const FRenderMaterial&) -> std::expected<FEditorAssetThumbnail, FPresentationError>
	{
		return std::make_shared<const std::uint64_t>(7);
	});

	Fixture.Tasks->RunUntilIdle();

	Draft.Parameters.Metallic = 0.8f;
	Assets->RequestMaterialPreview(*Created, Draft);
	Fixture.Tasks->RunUntilIdle();
	Assets->Tick();
	Fixture.Tasks->RunUntilIdle();
	const auto LastGood = Assets->GetMaterialPreview();
	const auto LastThumbnail = Assets->GetMaterialPreviewThumbnail();
	REQUIRE_MESSAGE(LastGood, Assets->GetMaterialPreviewError());
	REQUIRE(LastThumbnail);
	Draft.Textures[0].Texture = RobotId;
	Assets->RequestMaterialPreview(*Created, Draft);
	Fixture.Tasks->RunUntilIdle();
	CHECK(Assets->GetMaterialPreview() == LastGood);
	CHECK(Assets->GetMaterialPreviewThumbnail() == LastThumbnail);
	CHECK_FALSE(Assets->GetMaterialPreviewError().empty());
	CHECK_FALSE(Assets->IsMaterialPreviewLoading());
	Draft.Parameters.Roughness = -1.f;
	Assets->RequestMaterialPreview(*Created, Draft);
	FPreviewAssetsTestAccess::ContentChanged(*Assets);
	Fixture.Tasks->RunUntilIdle();
	Assets->Tick();
	Fixture.Tasks->RunUntilIdle();
	CHECK(Assets->GetMaterialPreview() == LastGood);
	CHECK_FALSE(Assets->GetMaterialPreviewError().empty());
	CHECK_FALSE(Assets->IsMaterialPreviewLoading());
	Draft = *Assets->GetMaterialSource(*Created);
	Draft.Parameters.Roughness = 0.1f;
	Assets->RequestMaterialPreview(*Created, Draft);
	Assets->RequestMaterialPreview({}, {});
	Fixture.Tasks->RunUntilIdle();
	CHECK_FALSE(Assets->GetMaterialPreview());
	CHECK_FALSE(Assets->GetMaterialPreviewThumbnail());
	CHECK_FALSE(Assets->GetMaterialPreviewAsset().IsValid());
	CHECK_FALSE(Assets->IsMaterialPreviewLoading());
	CHECK(Assets->GetMaterialPreviewError().empty());
}

TEST_CASE("Material drafts cook cross-mount textures with per-binding color spaces off the UI path")
{
	FPreviewAssetsFixture Fixture;
	auto Assets = Fixture.CreateAssets();
	REQUIRE(Assets);
	Fixture.Tasks->RunUntilIdle();
	const auto Created = Assets->CreateMaterial("Game", "Draft");
	REQUIRE(Created);
	Fixture.Tasks->RunUntilIdle();
	Assets->Tick();
	Fixture.Tasks->RunUntilIdle();
	REQUIRE(Assets->GetMaterialSource(*Created));
	FMaterialAsset Draft = *Assets->GetMaterialSource(*Created);
	Draft.Textures[0].Texture = StoneId;
	Draft.Textures[2].Texture = WoodId;
	const auto WritesBefore = Fixture.Device.TextureWrites.size();
	const auto TexturesBefore = Fixture.Device.TextureDescriptors.size();
	Assets->RequestMaterialPreview(*Created, Draft);
	CHECK(Fixture.Device.TextureWrites.size() == WritesBefore);
	CHECK_FALSE(Assets->GetMaterialPreview());
	Fixture.Tasks->RunUntilIdle();
	REQUIRE(Assets->GetMaterialPreview());
	CHECK(Fixture.Device.TextureWrites.size() > WritesBefore);
	REQUIRE(Fixture.Device.TextureDescriptors.size() == TexturesBefore + 2);
	CHECK(Fixture.Device.TextureDescriptors[TexturesBefore].Format == ETextureFormat::Rgba8Srgb);
	CHECK(Fixture.Device.TextureDescriptors[TexturesBefore + 1].Format == ETextureFormat::Rgba8);
	CHECK(Assets->GetMaterialPreviewError().empty());
	CHECK_FALSE(Assets->GetMaterialSource(*Created)->Textures[0].Texture.IsValid());
	const auto WritesAfter = Fixture.Device.TextureWrites.size();
	Draft.Parameters.BaseColor = {0.1f, 0.2f, 0.3f, 1.f};
	Assets->RequestMaterialPreview(*Created, Draft);
	Fixture.Tasks->RunUntilIdle();
	REQUIRE(Assets->GetMaterialPreview());
	CHECK(Assets->GetMaterialPreview()->GetParameters() == Draft.Parameters);
	CHECK(Fixture.Device.TextureWrites.size() == WritesAfter);
}

TEST_CASE("Preview texture requests retain cooked texture data and release inactive environments")
{
	FPreviewAssetsFixture Fixture;
	auto Assets = Fixture.CreateAssets();
	REQUIRE(Assets);
	Assets->RequestTexture(StoneId);
	Fixture.Tasks->RunUntilIdle();
	REQUIRE(Assets->GetCookedTexture(StoneId));
	REQUIRE(Assets->GetTexture(StoneId));
	CHECK(Assets->GetCookedTexture(StoneId)->Mips.front().Width == 1);
	CHECK(Assets->GetTexture(StoneId)->GetDescriptor().Format == ETextureFormat::Rgba8Srgb);
	Assets->SetTextureAssets(std::array{StoneId});
	Fixture.Tasks->RunUntilIdle();
	REQUIRE(Assets->GetTexture(StoneId));
	Assets->SetTextureAssets({});
	CHECK_FALSE(Assets->GetTexture(StoneId));
	CHECK_FALSE(Assets->GetCookedTexture(StoneId));
}

TEST_CASE("Preview thumbnails load without level bindings and share the existing mesh cache")
{
	FPreviewAssetsFixture Fixture;
	std::uint64_t RenderCount = 0;
	auto Assets = Fixture.CreateAssets([&RenderCount](const FRenderMesh&) -> std::expected<FEditorAssetThumbnail, FPresentationError>
	{
		return std::make_shared<const std::uint64_t>(++RenderCount);
	});

	REQUIRE(Assets);
	Assets->SetThumbnailAssets(std::array{WoodId, WoodId, FAssetId{}});
	CHECK(FPreviewAssetsTestAccess::GetCachedMeshCount(*Assets) == 0);
	CHECK_FALSE(Assets->GetThumbnail(WoodId));
	const auto TasksBeforeLookup = FPreviewAssetsTestAccess::GetOutstandingTaskCount(*Assets);
	CHECK(Assets->GetCachedAsset(WoodId) == nullptr);
	CHECK(FPreviewAssetsTestAccess::GetOutstandingTaskCount(*Assets) == TasksBeforeLookup);
	CHECK(FPreviewAssetsTestAccess::GetCachedMeshCount(*Assets) == 0);
	Assets->Tick();
	CHECK(RenderCount == 0);
	REQUIRE(Assets->GetCachedAsset(WoodId));
	CHECK(Assets->GetCachedAsset(WoodId)->bLoading);
	CHECK_FALSE(Assets->GetCachedAsset(WoodId)->Metadata);
	Fixture.Tasks->RunUntilIdle();
	REQUIRE(Assets->GetThumbnail(WoodId));
	CHECK(*Assets->GetThumbnail(WoodId) == 1);
	CHECK(RenderCount == 1);
	CHECK(Fixture.Device.Submissions == 1);
	const auto* Cached = Assets->GetCachedAsset(WoodId);
	REQUIRE(Cached);
	REQUIRE(Cached->Metadata);
	REQUIRE(std::holds_alternative<FPreviewTextureMetadata>(*Cached->Metadata));
	const auto& Texture = std::get<FPreviewTextureMetadata>(*Cached->Metadata);
	CHECK(Texture.Width == 1);
	CHECK(Texture.Height == 1);
	CHECK(Texture.Mips == 1);
	CHECK(Texture.ColorSpace == ETextureColorSpace::Srgb);
	CheckEmptyMeshSlot(Assets->GetSlot(0));
	CheckEmptyMeshSlot(Assets->GetSlot(1));

	const auto Generation = FPreviewAssetsTestAccess::GetRequestGeneration(*Assets);
	Assets->SetThumbnailAssets(std::array{WoodId});
	Assets->Tick();
	Assets->RebindObjects(std::array{WoodId});
	CHECK(FPreviewAssetsTestAccess::GetRequestGeneration(*Assets) == Generation);
	CHECK(RenderCount == 1);
	REQUIRE(Assets->GetSlot(0).Mesh);
	std::weak_ptr<const std::uint64_t> Released = Assets->GetThumbnail(WoodId);
	Assets->SetThumbnailAssets({});
	Assets->Tick();
	CHECK(Released.expired());
	CHECK_FALSE(Assets->GetThumbnail(WoodId));
	REQUIRE(Assets->GetSlot(0).Mesh);
	CHECK(FPreviewAssetsTestAccess::GetCachedMeshCount(*Assets) == 1);
	CHECK(Fixture.Device.Submissions == 1);
}

TEST_CASE("Preview thumbnail requests are bounded and pruned before their first scan")
{
	FPreviewAssetsFixture Fixture;
	auto Assets = Fixture.CreateAssets([](const FRenderMesh&) -> std::expected<FEditorAssetThumbnail, FPresentationError>
	{
		return std::make_shared<const std::uint64_t>(1);
	});

	REQUIRE(Assets);
	std::vector<FAssetId> Visible;

	for (std::uint64_t Index = 1; Index <= 128; ++Index)
	{
		Visible.emplace_back(0x4000, 0x8000000000000000 + Index);
	}

	Assets->SetThumbnailAssets(Visible);
	Assets->Tick();
	CHECK(FPreviewAssetsTestAccess::GetCachedMeshCount(*Assets) == 64);
	Assets->SetThumbnailAssets({});
	Assets->Tick();
	CHECK(FPreviewAssetsTestAccess::GetCachedMeshCount(*Assets) == 0);
	Fixture.Tasks->RunUntilIdle();
	CHECK(FPreviewAssetsTestAccess::GetRequestGeneration(*Assets) == 0);
	CHECK(Fixture.Device.Submissions == 0);
}

TEST_CASE("Preview thumbnails discard invisible pending results without uploading meshes")
{
	FPreviewAssetsFixture Fixture;
	std::uint64_t RenderCount = 0;
	auto Assets = Fixture.CreateAssets([&RenderCount](const FRenderMesh&) -> std::expected<FEditorAssetThumbnail, FPresentationError>
	{
		return std::make_shared<const std::uint64_t>(++RenderCount);
	});

	REQUIRE(Assets);
	Fixture.Tasks->RunUntilIdle();
	Assets->SetThumbnailAssets(std::array{WoodId});
	Assets->Tick();
	Fixture.RunToCheckpoint();
	Assets->SetThumbnailAssets({});
	Assets->Tick();
	CHECK(FPreviewAssetsTestAccess::GetCachedMeshCount(*Assets) == 0);
	Fixture.Tasks->RunUntilIdle();
	CHECK(RenderCount == 0);
	CHECK(Fixture.Device.Submissions == 0);
	CHECK_FALSE(Assets->GetThumbnail(WoodId));
}

TEST_CASE("Preview thumbnail reimports replace changed keys and preserve the previous image on failure")
{
	FPreviewAssetsFixture Fixture;
	std::uint64_t RenderCount = 0;
	bool bFailRendering = false;
	auto Assets = Fixture.CreateAssets([&RenderCount, &bFailRendering](const FRenderMesh&) -> std::expected<FEditorAssetThumbnail, FPresentationError>
	{
		++RenderCount;
		if (bFailRendering)
		{
			return std::unexpected(FPresentationError{.Message = "Thumbnail render failed"});
		}

		return std::make_shared<const std::uint64_t>(RenderCount);
	});

	REQUIRE(Assets);
	Assets->SetThumbnailAssets(std::array{WoodId});
	Assets->Tick();
	Fixture.Tasks->RunUntilIdle();
	const auto Original = Assets->GetThumbnail(WoodId);
	REQUIRE(Original);

	Tests::WriteText(Fixture.Scratch.GetPath() / "Game/Notes.txt", "unrelated");
	FPreviewAssetsTestAccess::ContentChanged(*Assets);
	Fixture.Tasks->RunUntilIdle();
	CHECK(Assets->GetThumbnail(WoodId) == Original);
	CHECK(RenderCount == 1);

	FPreviewAssetsFixture::WriteTexture(Fixture.Scratch.GetPath() / "Game", "Textures/Wood.png", WoodId, {255, 0, 0, 255});
	FPreviewAssetsTestAccess::ContentChanged(*Assets);
	Fixture.Tasks->RunUntilIdle();
	const auto Changed = Assets->GetThumbnail(WoodId);
	REQUIRE(Changed);
	CHECK(Changed != Original);
	CHECK(RenderCount == 2);

	bFailRendering = true;
	FPreviewAssetsFixture::WriteTexture(Fixture.Scratch.GetPath() / "Game", "Textures/Wood.png", WoodId, {0, 255, 0, 255});
	FPreviewAssetsTestAccess::ContentChanged(*Assets);
	Fixture.Tasks->RunUntilIdle();
	CHECK(Assets->GetThumbnail(WoodId) == Changed);
	CHECK(RenderCount == 3);
	Assets->Tick();
	CHECK(RenderCount == 3);

	Tests::WriteText(Fixture.Scratch.GetPath() / "Game/Textures/Wood.png", "broken");
	FPreviewAssetsTestAccess::ContentChanged(*Assets);
	Fixture.Tasks->RunUntilIdle();
	CHECK(Assets->GetThumbnail(WoodId) == Changed);
	CHECK(RenderCount == 3);
	const auto* Cached = Assets->GetCachedAsset(WoodId);
	REQUIRE(Cached);
	CHECK_FALSE(Cached->bLoading);
	CHECK_FALSE(Cached->Error.empty());
	REQUIRE(Cached->Metadata);
	REQUIRE(std::holds_alternative<FPreviewTextureMetadata>(*Cached->Metadata));
	CHECK(std::get<FPreviewTextureMetadata>(*Cached->Metadata).Width == 1);
}

TEST_CASE("Preview thumbnails do not schedule work without a renderer and release handles on shutdown")
{
	FPreviewAssetsFixture Fixture;
	auto Disabled = Fixture.CreateAssets();
	REQUIRE(Disabled);
	Disabled->SetThumbnailAssets(std::array{WoodId});
	Disabled->Tick();
	Fixture.Tasks->RunUntilIdle();
	CHECK(FPreviewAssetsTestAccess::GetCachedMeshCount(*Disabled) == 0);
	CHECK(Fixture.Device.Submissions == 0);
	Disabled.reset();

	auto Assets = Fixture.CreateAssets([](const FRenderMesh&) -> std::expected<FEditorAssetThumbnail, FPresentationError>
	{
		return std::make_shared<const std::uint64_t>(1);
	});

	REQUIRE(Assets);
	Assets->SetThumbnailAssets(std::array{WoodId});
	Assets->Tick();
	Fixture.Tasks->RunUntilIdle();
	std::weak_ptr<const std::uint64_t> Released = Assets->GetThumbnail(WoodId);
	CHECK_FALSE(Released.expired());
	Assets.reset();
	CHECK(Released.expired());
}

TEST_CASE("Preview assets scan Engine and Game content and publish cooked meshes between frames")
{
	FPreviewAssetsFixture Fixture;
	auto Assets = Fixture.CreateAssets();
	REQUIRE(Assets);
	CHECK(Assets->IsScanning());

	// Requests made before the first scan wait for it, like the default preview meshes at startup.
	Assets->RequestMesh(0, WoodId);
	CHECK(Assets->GetSlot(0).bLoading);
	CHECK_FALSE(Assets->GetSlot(0).Mesh);
	Fixture.Tasks->RunUntilIdle();
	CHECK_FALSE(Assets->IsScanning());

	const std::span<const FPreviewAssetOption> Options = Assets->GetOptions();
	REQUIRE(Options.size() == 3);
	CHECK(Options[0].Label == "Engine/Textures/Stone.png");
	CHECK(Options[1].Label == "Game/Models/Robot.blend");
	CHECK(Options[2].Label == "Game/Textures/Wood.png");
	CHECK(Options[2].Importer == "Texture");

	const FPreviewMeshSlot& Loaded = Assets->GetSlot(0);
	CHECK_FALSE(Loaded.bLoading);
	CHECK(Loaded.Error.empty());
	CHECK(Loaded.Label == "Game/Textures/Wood.png");
	REQUIRE(Loaded.Mesh);
	CHECK(Loaded.Mesh->GetBoundsMinimum() == FVector3{-0.5f, -0.5f, -0.5f});
	CHECK(Loaded.Mesh->GetBoundsMaximum() == FVector3{0.5f, 0.5f, 0.5f});

	// A second object showing the same asset shares the loaded GPU mesh without cooking again.
	Assets->RequestMesh(1, WoodId);
	CHECK_FALSE(Assets->GetSlot(1).bLoading);
	CHECK(Assets->GetSlot(1).Mesh == Loaded.Mesh);

	Assets->RequestMesh(1, RobotId);
	Fixture.Tasks->RunUntilIdle();
	CHECK_FALSE(Assets->GetSlot(1).Mesh);
	// The fake .blend fails whether or not Blender is installed, and a failed load clears the mesh.
	CHECK(Assets->GetSlot(1).Error.find("Blender") != std::string::npos);

	Assets->RequestMesh(1, FAssetId(0x1, 0x2));
	CHECK_FALSE(Assets->GetSlot(1).bLoading);
	CHECK(Assets->GetSlot(1).Error.find("not registered") != std::string::npos);

	// The newest request wins even when an older one finishes later.
	Assets->RequestMesh(1, WoodId);
	Assets->RequestMesh(1, StoneId);
	Fixture.Tasks->RunUntilIdle();
	const FPreviewMeshSlot& Superseded = Assets->GetSlot(1);
	CHECK(Superseded.Asset == StoneId);
	CHECK(Superseded.Label == "Engine/Textures/Stone.png");
	CHECK(Superseded.Error.empty());
	REQUIRE(Superseded.Mesh);
	CHECK(Superseded.Mesh != Loaded.Mesh);

	// Destroying with work in flight cancels it instead of publishing into a dead editor.
	Assets->RequestMesh(1, WoodId);
	Assets.reset();
}

TEST_CASE("Preview assets reimport shown assets after content edits and keep the previous mesh when a reimport fails")
{
	FPreviewAssetsFixture Fixture;
	auto Assets = Fixture.CreateAssets();
	REQUIRE(Assets);
	Assets->RequestMesh(0, WoodId);
	Assets->RequestMesh(1, WoodId);
	Fixture.Tasks->RunUntilIdle();
	const std::shared_ptr<const FRenderMesh> Original = Assets->GetSlot(0).Mesh;
	REQUIRE(Original);
	CHECK(Assets->GetSlot(1).Mesh == Original);

	// Creation records the baseline, so polling unchanged content does nothing.
	Assets->CheckForChanges();
	Fixture.Tasks->RunUntilIdle();
	CHECK(Assets->GetSlot(0).Mesh == Original);

	// An edit that does not change the asset's build key keeps the GPU copy.
	Tests::WriteText(Fixture.Scratch.GetPath() / "Game/Notes.txt", "unrelated");
	Assets->CheckForChanges();
	Fixture.Tasks->RunUntilIdle();
	CHECK(Assets->GetSlot(0).Mesh == Original);
	CHECK(Assets->GetSlot(0).Error.empty());

	// Editing the source swaps in one new mesh shared by every object showing it.
	const std::array<std::uint8_t, 16> Edited{255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255};
	Tests::WritePng(Fixture.Scratch.GetPath() / "Game/Textures/Wood.png", 2, 2, Edited);
	Assets->CheckForChanges();
	Fixture.Tasks->RunUntilIdle();
	const std::shared_ptr<const FRenderMesh> Reimported = Assets->GetSlot(0).Mesh;
	REQUIRE(Reimported);
	CHECK(Reimported != Original);
	CHECK(Assets->GetSlot(1).Mesh == Reimported);
	CHECK(Assets->GetSlot(0).Error.empty());

	// A broken save keeps the last good mesh and reports why.
	Tests::WriteText(Fixture.Scratch.GetPath() / "Game/Textures/Wood.png", "not a png");
	Assets->CheckForChanges();
	Fixture.Tasks->RunUntilIdle();
	CHECK(Assets->GetSlot(0).Mesh == Reimported);
	CHECK(Assets->GetSlot(1).Mesh == Reimported);
	CHECK(Assets->GetSlot(0).Error.find("keeping the previous version") != std::string::npos);

	// Fixing the file recovers.
	Tests::WritePng(Fixture.Scratch.GetPath() / "Game/Textures/Wood.png", 1, 1, std::array<std::uint8_t, 4>{150, 111, 51, 255});
	Assets->CheckForChanges();
	Fixture.Tasks->RunUntilIdle();
	CHECK(Assets->GetSlot(0).Error.empty());
	REQUIRE(Assets->GetSlot(0).Mesh);
	CHECK(Assets->GetSlot(0).Mesh != Reimported);
}

TEST_CASE("Preview assets restart pending loads after content changes")
{
	FPreviewAssetsFixture Fixture;
	auto Assets = Fixture.CreateAssets();
	REQUIRE(Assets);
	Fixture.Tasks->RunUntilIdle();
	Assets->RequestMesh(0, WoodId);
	Fixture.RunToCheckpoint();
	REQUIRE(Assets->GetSlot(0).bLoading);
	REQUIRE_FALSE(Assets->GetSlot(0).Mesh);

	const std::array<std::uint8_t, 16> Edited{255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255};
	Tests::WritePng(Fixture.Scratch.GetPath() / "Game/Textures/Wood.png", 2, 2, Edited);
	FPreviewAssetsTestAccess::ContentChanged(*Assets);
	Fixture.RunToCheckpoint();
	CHECK(Assets->GetSlot(0).bLoading);
	CHECK_FALSE(Assets->GetSlot(0).Mesh);
	Fixture.Tasks->RunUntilIdle();

	const FPreviewMeshSlot& Slot = Assets->GetSlot(0);
	CHECK_FALSE(Slot.bLoading);
	CHECK(Slot.Error.empty());
	REQUIRE(Slot.Mesh);
	const auto Cooked = LoadCookedAsset(Fixture.Scratch.GetPath() / "DerivedDataCache", Slot.Key);
	REQUIRE(Cooked);
	const FCookedTexture* const Texture = std::get_if<FCookedTexture>(&*Cooked);
	REQUIRE(Texture);
	CHECK(Texture->Mips[0].Width == 2);
}

TEST_CASE("Preview assets discard pending reimport results superseded by another edit")
{
	FPreviewAssetsFixture Fixture;
	auto Assets = Fixture.CreateAssets();
	REQUIRE(Assets);
	Assets->RequestMesh(0, WoodId);
	Assets->RequestMesh(1, WoodId);
	Fixture.Tasks->RunUntilIdle();
	const std::shared_ptr<const FRenderMesh> Original = Assets->GetSlot(0).Mesh;
	REQUIRE(Original);

	const std::array<std::uint8_t, 16> FirstEdit{255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255};
	Tests::WritePng(Fixture.Scratch.GetPath() / "Game/Textures/Wood.png", 2, 2, FirstEdit);
	FPreviewAssetsTestAccess::ContentChanged(*Assets);
	Fixture.RunToCheckpoint();
	Fixture.RunToCheckpoint();
	Fixture.RunToCheckpoint();
	REQUIRE(Assets->GetSlot(0).Mesh == Original);

	const std::array<std::uint8_t, 4> SecondEdit{0, 255, 0, 255};
	Tests::WritePng(Fixture.Scratch.GetPath() / "Game/Textures/Wood.png", 1, 1, SecondEdit);
	FPreviewAssetsTestAccess::ContentChanged(*Assets);
	Fixture.RunToCheckpoint();
	CHECK(Assets->GetSlot(0).Mesh == Original);
	CHECK(Assets->GetSlot(1).Mesh == Original);
	Fixture.Tasks->RunUntilIdle();

	CHECK(Assets->GetSlot(0).Mesh != Original);
	CHECK(Assets->GetSlot(1).Mesh == Assets->GetSlot(0).Mesh);
	CHECK(Assets->GetSlot(0).Error.empty());
	const auto Cooked = LoadCookedAsset(Fixture.Scratch.GetPath() / "DerivedDataCache", Assets->GetSlot(0).Key);
	REQUIRE(Cooked);
	const FCookedTexture* const Texture = std::get_if<FCookedTexture>(&*Cooked);
	REQUIRE(Texture);
	CHECK(Texture->Mips[0].Width == 1);
	CHECK(Texture->Mips[0].Pixels[0] == std::byte{0});
	CHECK(Texture->Mips[0].Pixels[1] == std::byte{255});
}

TEST_CASE("Preview assets do not share stale meshes while their reimport is pending")
{
	FPreviewAssetsFixture Fixture;
	auto Assets = Fixture.CreateAssets();
	REQUIRE(Assets);
	Assets->RequestMesh(0, WoodId);
	Assets->RequestMesh(1, StoneId);
	Fixture.Tasks->RunUntilIdle();

	const std::array<std::uint8_t, 16> Edited{255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255};
	Tests::WritePng(Fixture.Scratch.GetPath() / "Game/Textures/Wood.png", 2, 2, Edited);
	FPreviewAssetsTestAccess::ContentChanged(*Assets);
	Assets->RequestMesh(1, WoodId);
	CHECK(Assets->GetSlot(1).bLoading);
	Fixture.Tasks->RunUntilIdle();
	CHECK_FALSE(Assets->GetSlot(1).bLoading);
	REQUIRE(Assets->GetSlot(0).Mesh);
	CHECK(Assets->GetSlot(1).Mesh == Assets->GetSlot(0).Mesh);
}

TEST_CASE("Meshless preview bindings stay empty without scheduling cooks or GPU uploads")
{
	for (const bool bScanned : {false, true})
	{
		CAPTURE(bScanned);
		FPreviewAssetsFixture Fixture;
		auto Assets = Fixture.CreateAssets();
		REQUIRE(Assets);
		if (bScanned)
		{
			Fixture.Tasks->RunUntilIdle();
		}

		const std::size_t Outstanding = FPreviewAssetsTestAccess::GetOutstandingTaskCount(*Assets);
		Assets->RebindObjects(std::array{FAssetId{}, FAssetId{}, FAssetId{}});
		Assets->RequestMesh(1, {});
		CHECK(FPreviewAssetsTestAccess::GetOutstandingTaskCount(*Assets) == Outstanding);
		CHECK(FPreviewAssetsTestAccess::GetCachedMeshCount(*Assets) == 0);
		Fixture.Tasks->RunUntilIdle();
		FPreviewAssetsTestAccess::ContentChanged(*Assets);
		Fixture.Tasks->RunUntilIdle();
		CHECK(FPreviewAssetsTestAccess::GetRequestGeneration(*Assets) == 0);
		CHECK(FPreviewAssetsTestAccess::GetCachedMeshCount(*Assets) == 0);
		CHECK(Fixture.Device.Submissions == 0);
		CHECK(Fixture.Device.Events.empty());
		for (std::size_t Index = 0; Index < 3; ++Index)
		{
			CheckEmptyMeshSlot(Assets->GetSlot(Index));
		}
	}
}

TEST_CASE("Mixed meshless preview bindings preserve shared assets across clearing and readding")
{
	FPreviewAssetsFixture Fixture;
	auto Assets = Fixture.CreateAssets();
	REQUIRE(Assets);
	Assets->RebindObjects(std::array{WoodId, StoneId});
	Fixture.Tasks->RunUntilIdle();
	const auto Wood = Assets->GetSlot(0).Mesh;
	const auto Stone = Assets->GetSlot(1).Mesh;
	REQUIRE(Wood);
	REQUIRE(Stone);
	const auto Generation = FPreviewAssetsTestAccess::GetRequestGeneration(*Assets);
	const auto Submissions = Fixture.Device.Submissions;
	Assets->RebindObjects(std::array{FAssetId{}, WoodId, StoneId, FAssetId{}});
	CheckEmptyMeshSlot(Assets->GetSlot(0));
	CheckEmptyMeshSlot(Assets->GetSlot(3));
	CHECK(Assets->GetSlot(1).Mesh == Wood);
	CHECK(Assets->GetSlot(2).Mesh == Stone);
	CHECK(FPreviewAssetsTestAccess::GetRequestGeneration(*Assets) == Generation);
	CHECK(Fixture.Device.Submissions == Submissions);
	Assets->RequestMesh(0, WoodId);
	CHECK(Assets->GetSlot(0).Mesh == Wood);
	Assets->RequestMesh(1, {});
	CheckEmptyMeshSlot(Assets->GetSlot(1));
	CHECK(FPreviewAssetsTestAccess::GetCachedMeshCount(*Assets) == 2);
	Assets->RequestMesh(0, {});
	CheckEmptyMeshSlot(Assets->GetSlot(0));
	CHECK(FPreviewAssetsTestAccess::GetCachedMeshCount(*Assets) == 1);
	Assets->RequestMesh(3, StoneId);
	CHECK(Assets->GetSlot(3).Mesh == Stone);
	CHECK(FPreviewAssetsTestAccess::GetRequestGeneration(*Assets) == Generation);
	Assets->RebindObjects(std::array{FAssetId{}, StoneId, FAssetId{}});
	CHECK(Assets->GetSlot(1).Mesh == Stone);
	CheckEmptyMeshSlot(Assets->GetSlot(0));
	CheckEmptyMeshSlot(Assets->GetSlot(2));
	Assets->RebindObjects(std::array{FAssetId{}, FAssetId{}});
	CHECK(FPreviewAssetsTestAccess::GetCachedMeshCount(*Assets) == 0);
}

TEST_CASE("Clearing mesh requests before the first scan prevents worker cooking")
{
	FPreviewAssetsFixture Fixture;
	auto Assets = Fixture.CreateAssets();
	REQUIRE(Assets);
	Assets->RequestMesh(0, WoodId);
	REQUIRE(Assets->GetSlot(0).bLoading);
	Assets->RequestMesh(0, {});
	CheckEmptyMeshSlot(Assets->GetSlot(0));
	CHECK(FPreviewAssetsTestAccess::GetCachedMeshCount(*Assets) == 0);
	Fixture.Tasks->RunUntilIdle();
	CHECK(FPreviewAssetsTestAccess::GetRequestGeneration(*Assets) == 0);
	CHECK(Fixture.Device.Submissions == 0);
	CheckEmptyMeshSlot(Assets->GetSlot(0));
}

TEST_CASE("Pending mesh results cannot repopulate cleared meshless slots")
{
	FPreviewAssetsFixture Fixture;
	auto Assets = Fixture.CreateAssets();
	REQUIRE(Assets);
	Fixture.Tasks->RunUntilIdle();
	Assets->RequestMesh(0, WoodId);
	REQUIRE(Assets->GetSlot(0).bLoading);
	Fixture.RunToCheckpoint();
	Assets->RequestMesh(0, {});
	CheckEmptyMeshSlot(Assets->GetSlot(0));
	CHECK(FPreviewAssetsTestAccess::GetCachedMeshCount(*Assets) == 1);
	Fixture.Tasks->RunUntilIdle();
	CHECK(FPreviewAssetsTestAccess::GetCachedMeshCount(*Assets) == 0);
	CHECK(Fixture.Device.Submissions == 0);
	CheckEmptyMeshSlot(Assets->GetSlot(0));
	Assets->RequestMesh(1, WoodId);
	Fixture.Tasks->RunUntilIdle();
	REQUIRE(Assets->GetSlot(1).Mesh);
	CHECK(Fixture.Device.Submissions == 1);
	CheckEmptyMeshSlot(Assets->GetSlot(0));
}

TEST_CASE("Meshless slots can rejoin pending assets without duplicate requests")
{
	for (const bool bScanned : {false, true})
	{
		CAPTURE(bScanned);
		FPreviewAssetsFixture Fixture;
		auto Assets = Fixture.CreateAssets();
		REQUIRE(Assets);
		if (bScanned)
		{
			Fixture.Tasks->RunUntilIdle();
		}

		Assets->RebindObjects(std::array{WoodId, FAssetId{}});
		const auto Generation = FPreviewAssetsTestAccess::GetRequestGeneration(*Assets);
		Assets->RequestMesh(0, {});
		CheckEmptyMeshSlot(Assets->GetSlot(0));
		Assets->RebindObjects(std::array{FAssetId{}, WoodId, WoodId, FAssetId{}});
		CHECK(FPreviewAssetsTestAccess::GetRequestGeneration(*Assets) == Generation);
		CHECK(Assets->GetSlot(1).bLoading);
		CHECK(Assets->GetSlot(2).bLoading);
		Fixture.Tasks->RunUntilIdle();
		REQUIRE(Assets->GetSlot(1).Mesh);
		CHECK(Assets->GetSlot(2).Mesh == Assets->GetSlot(1).Mesh);
		CHECK(FPreviewAssetsTestAccess::GetRequestGeneration(*Assets) == 1);
		CHECK(FPreviewAssetsTestAccess::GetCachedMeshCount(*Assets) == 1);
		CHECK(Fixture.Device.Submissions == 1);
		CheckEmptyMeshSlot(Assets->GetSlot(0));
		CheckEmptyMeshSlot(Assets->GetSlot(3));
	}
}

TEST_CASE("Level rebinding retains loaded meshes across copies reorder deletion and load")
{
	FPreviewAssetsFixture Fixture;
	auto Assets = Fixture.CreateAssets();
	REQUIRE(Assets);
	Assets->RebindObjects(std::array{WoodId, StoneId});
	Fixture.Tasks->RunUntilIdle();
	const auto Wood = Assets->GetSlot(0).Mesh;
	const auto Stone = Assets->GetSlot(1).Mesh;
	REQUIRE(Wood);
	REQUIRE(Stone);
	const auto Generation = FPreviewAssetsTestAccess::GetRequestGeneration(*Assets);
	const auto Submissions = Fixture.Device.Submissions;

	Assets->RebindObjects(std::array{WoodId, WoodId, StoneId});
	CHECK(Assets->GetSlot(0).Mesh == Wood);
	CHECK(Assets->GetSlot(1).Mesh == Wood);
	CHECK(Assets->GetSlot(2).Mesh == Stone);
	CHECK_FALSE(Assets->GetSlot(1).bLoading);
	Assets->RebindObjects(std::array{StoneId, WoodId, StoneId});
	CHECK(Assets->GetSlot(0).Mesh == Stone);
	CHECK(Assets->GetSlot(1).Mesh == Wood);
	CHECK(Assets->GetSlot(2).Mesh == Stone);
	CHECK(FPreviewAssetsTestAccess::GetRequestGeneration(*Assets) == Generation);
	CHECK(Fixture.Device.Submissions == Submissions);

	Assets->RebindObjects(std::array{WoodId});
	CHECK(Assets->GetSlot(0).Mesh == Wood);
	CHECK(FPreviewAssetsTestAccess::GetCachedMeshCount(*Assets) == 1);
	Assets->RebindObjects({});
	CHECK(FPreviewAssetsTestAccess::GetCachedMeshCount(*Assets) == 0);
}

TEST_CASE("Level rebinding coalesces pending copies before and after the first content scan")
{
	for (const bool bScanned : {false, true})
	{
		CAPTURE(bScanned);
		FPreviewAssetsFixture Fixture;
		auto Assets = Fixture.CreateAssets();
		REQUIRE(Assets);
		if (bScanned)
		{
			Fixture.Tasks->RunUntilIdle();
		}

		Assets->RebindObjects(std::array{WoodId, WoodId, StoneId});
		const auto Generation = FPreviewAssetsTestAccess::GetRequestGeneration(*Assets);
		CHECK(Generation == (bScanned ? 2 : 0));
		Assets->RebindObjects(std::array{StoneId, WoodId, WoodId, WoodId});
		CHECK(FPreviewAssetsTestAccess::GetRequestGeneration(*Assets) == Generation);
		CHECK(Assets->GetSlot(1).bLoading);
		CHECK_FALSE(Assets->GetSlot(1).Mesh);
		Fixture.Tasks->RunUntilIdle();
		CHECK(FPreviewAssetsTestAccess::GetRequestGeneration(*Assets) == 2);
		REQUIRE(Assets->GetSlot(0).Mesh);
		REQUIRE(Assets->GetSlot(1).Mesh);
		CHECK(Assets->GetSlot(2).Mesh == Assets->GetSlot(1).Mesh);
		CHECK(Assets->GetSlot(3).Mesh == Assets->GetSlot(1).Mesh);
		CHECK(Fixture.Device.Submissions == 2);
	}
}

TEST_CASE("Rebound slots cannot receive removed loads and can restore pending assets without restarting")
{
	FPreviewAssetsFixture Fixture;
	auto Assets = Fixture.CreateAssets();
	REQUIRE(Assets);
	Fixture.Tasks->RunUntilIdle();
	Assets->RebindObjects(std::array{WoodId, StoneId});
	const auto Generation = FPreviewAssetsTestAccess::GetRequestGeneration(*Assets);
	Assets->RebindObjects({});
	CHECK(FPreviewAssetsTestAccess::GetCachedMeshCount(*Assets) == 2);
	Assets->RebindObjects(std::array{WoodId, WoodId});
	CHECK(FPreviewAssetsTestAccess::GetRequestGeneration(*Assets) == Generation);
	Fixture.Tasks->RunUntilIdle();
	REQUIRE(Assets->GetSlot(0).Mesh);
	CHECK(Assets->GetSlot(0).Asset == WoodId);
	CHECK(Assets->GetSlot(1).Mesh == Assets->GetSlot(0).Mesh);
	CHECK(FPreviewAssetsTestAccess::GetCachedMeshCount(*Assets) == 1);
	CHECK(Fixture.Device.Submissions == 1);
}

TEST_CASE("Copies retain visible meshes and share reimport results while level bindings change")
{
	FPreviewAssetsFixture Fixture;
	auto Assets = Fixture.CreateAssets();
	REQUIRE(Assets);
	Assets->RebindObjects(std::array{WoodId, StoneId});
	Fixture.Tasks->RunUntilIdle();
	const auto Wood = Assets->GetSlot(0).Mesh;
	const auto Stone = Assets->GetSlot(1).Mesh;
	REQUIRE(Wood);
	REQUIRE(Stone);
	const std::array<std::uint8_t, 16> Edited{255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255};
	Tests::WritePng(Fixture.Scratch.GetPath() / "Game/Textures/Wood.png", 2, 2, Edited);
	FPreviewAssetsTestAccess::ContentChanged(*Assets);
	Assets->RebindObjects(std::array{StoneId, WoodId, WoodId});
	CHECK(Assets->GetSlot(0).Mesh == Stone);
	CHECK(Assets->GetSlot(1).Mesh == Wood);
	CHECK(Assets->GetSlot(2).Mesh == Wood);
	Fixture.RunToCheckpoint();
	Fixture.RunToCheckpoint();
	Fixture.RunToCheckpoint();
	const auto Generation = FPreviewAssetsTestAccess::GetRequestGeneration(*Assets);
	Assets->RebindObjects(std::array{WoodId, WoodId, StoneId, WoodId});
	CHECK(FPreviewAssetsTestAccess::GetRequestGeneration(*Assets) == Generation);
	CHECK(Assets->GetSlot(0).Mesh == Wood);
	CHECK(Assets->GetSlot(1).Mesh == Wood);
	CHECK(Assets->GetSlot(2).Mesh == Stone);
	CHECK(Assets->GetSlot(3).Mesh == Wood);
	Fixture.Tasks->RunUntilIdle();
	REQUIRE(Assets->GetSlot(0).Mesh);
	CHECK(Assets->GetSlot(0).Mesh != Wood);
	CHECK(Assets->GetSlot(1).Mesh == Assets->GetSlot(0).Mesh);
	CHECK(Assets->GetSlot(3).Mesh == Assets->GetSlot(0).Mesh);
	CHECK(Assets->GetSlot(2).Mesh == Stone);
	CHECK_FALSE(Assets->GetSlot(0).bLoading);
	CHECK(Assets->GetSlot(0).Error.empty());
}

TEST_CASE("Rebinding pending mesh replacements preserves visible fallbacks until success or failure")
{
	for (const bool bFailure : {false, true})
	{
		CAPTURE(bFailure);
		FPreviewAssetsFixture Fixture;
		auto Assets = Fixture.CreateAssets();
		REQUIRE(Assets);
		Assets->RebindObjects(std::array{WoodId, WoodId});
		Fixture.Tasks->RunUntilIdle();
		const auto Wood = Assets->GetSlot(0).Mesh;
		REQUIRE(Wood);
		const FAssetId Replacement = bFailure ? RobotId : StoneId;
		Assets->RequestMesh(0, Replacement);
		REQUIRE(Assets->GetSlot(0).bLoading);
		CHECK(Assets->GetSlot(0).Mesh == Wood);
		const auto Generation = FPreviewAssetsTestAccess::GetRequestGeneration(*Assets);
		Assets->RebindObjects(std::array{WoodId, Replacement, Replacement});
		CHECK(Assets->GetSlot(0).Mesh == Wood);
		CHECK(Assets->GetSlot(1).Mesh == Wood);
		CHECK(Assets->GetSlot(2).Mesh == Wood);
		Assets->RebindObjects(std::array{Replacement, WoodId, Replacement});
		CHECK(Assets->GetSlot(0).Mesh == Wood);
		CHECK(Assets->GetSlot(1).Mesh == Wood);
		CHECK(Assets->GetSlot(2).Mesh == Wood);
		CHECK(FPreviewAssetsTestAccess::GetRequestGeneration(*Assets) == Generation);
		Fixture.Tasks->RunUntilIdle();
		CHECK_FALSE(Assets->GetSlot(0).bLoading);
		CHECK(Assets->GetSlot(1).Mesh == Wood);
		CHECK(Assets->GetSlot(2).Mesh == Assets->GetSlot(0).Mesh);
		if (bFailure)
		{
			CHECK_FALSE(Assets->GetSlot(0).Mesh);
			CHECK_FALSE(Assets->GetSlot(0).Error.empty());
		}
		else
		{
			REQUIRE(Assets->GetSlot(0).Mesh);
			CHECK(Assets->GetSlot(0).Mesh != Wood);
			CHECK(Assets->GetSlot(0).Error.empty());
		}
	}
}

TEST_CASE("Dropped files import into Game content through asset.import and appear after the next poll")
{
	FPreviewAssetsFixture Fixture;
	auto Assets = Fixture.CreateAssets();
	REQUIRE(Assets);
	Fixture.Tasks->RunUntilIdle();
	const std::array<std::uint8_t, 4> Pixel{10, 200, 30, 255};
	Tests::WritePng(Fixture.Scratch.GetPath() / "Desktop/Grass Tile.png", 1, 1, Pixel);
	Tests::WriteText(Fixture.Scratch.GetPath() / "Desktop/Notes.txt", "not an asset");

	Assets->ImportFiles({Fixture.Scratch.GetPath() / "Desktop/Grass Tile.png", Fixture.Scratch.GetPath() / "Desktop/Notes.txt"});
	CHECK(Assets->IsImporting());
	Fixture.Tasks->RunUntilIdle();
	CHECK_FALSE(Assets->IsImporting());
	CHECK(std::filesystem::exists(Fixture.Scratch.GetPath() / "Game/Textures/Grass Tile.png.hmeta"));
	CHECK_FALSE(std::filesystem::exists(Fixture.Scratch.GetPath() / "Game/Models/Notes.txt"));
	const std::span<const FPreviewAssetOption> Options = Assets->GetOptions();
	CHECK(std::ranges::any_of(Options, [](const FPreviewAssetOption& Option)
	{
		return Option.Label == "Game/Textures/Grass Tile.png";
	}));
}

TEST_CASE("Dropped files import into the chosen Game subfolder and refresh registry options without GPU work")
{
	FPreviewAssetsFixture Fixture;
	auto Assets = Fixture.CreateAssets();
	REQUIRE(Assets);
	Fixture.Tasks->RunUntilIdle();
	const auto BeforeGeneration = Assets->GetOptionsGeneration();
	const auto BeforeEngine = TakeContentSnapshot(Fixture.Scratch.GetPath() / "Engine");
	const auto Source = Fixture.Scratch.GetPath() / "Desktop/Grass Tile.png";
	const std::array<std::uint8_t, 4> Pixel{10, 200, 30, 255};
	Tests::WritePng(Source, 1, 1, Pixel);
	Assets->ImportFiles({Source}, "Art Assets/Imported Props");
	CHECK(Assets->IsImporting());
	CHECK(Assets->GetOptionsGeneration() == BeforeGeneration);
	Fixture.Tasks->RunUntilIdle();
	CHECK_FALSE(Assets->IsImporting());
	CHECK_FALSE(Assets->IsScanning());
	CHECK(Assets->GetOptionsGeneration() > BeforeGeneration);
	const auto Destination = Fixture.Scratch.GetPath() / "Game/Art Assets/Imported Props/Grass Tile.png";
	CHECK(std::filesystem::is_regular_file(Destination));
	CHECK(std::filesystem::is_regular_file(Destination.string() + ".hmeta"));
	CHECK_FALSE(std::filesystem::exists(Fixture.Scratch.GetPath() / "Game/Textures/Grass Tile.png"));
	CHECK(TakeContentSnapshot(Fixture.Scratch.GetPath() / "Engine") == BeforeEngine);
	const auto Scan = ScanContentRoot(Fixture.Scratch.GetPath() / "Game");
	REQUIRE(Scan);
	CHECK(Scan->Errors.empty());
	CHECK(Scan->UnregisteredSources.empty());
	const auto* Imported = Scan->Registry.FindBySourcePath("Art Assets/Imported Props/Grass Tile.png");
	REQUIRE(Imported);
	CHECK(Imported->Id.IsValid());
	CHECK(Imported->Importer == "Texture");
	const auto Options = Assets->GetOptions();
	const auto Option = std::ranges::find(Options, Imported->Id, &FPreviewAssetOption::Id);
	REQUIRE(Option != Options.end());
	CHECK(Option->Label == "Game/Art Assets/Imported Props/Grass Tile.png");
	CHECK(Option->Importer == "Texture");
	CHECK(Fixture.Device.Submissions == 0);
	CHECK(Fixture.Device.Events.empty());
}

TEST_CASE("Dropped file imports reject escaping destinations before scheduling or writing files")
{
	FPreviewAssetsFixture Fixture;
	auto Assets = Fixture.CreateAssets();
	REQUIRE(Assets);
	Fixture.Tasks->RunUntilIdle();
	const auto Source = Fixture.Scratch.GetPath() / "Desktop/Grass Tile.png";
	const std::array<std::uint8_t, 4> Pixel{10, 200, 30, 255};
	Tests::WritePng(Source, 1, 1, Pixel);
	const auto Before = TakeContentSnapshot(Fixture.Scratch.GetPath());
	const auto BeforeGeneration = Assets->GetOptionsGeneration();
	const auto BeforeTasks = FPreviewAssetsTestAccess::GetOutstandingTaskCount(*Assets);
	const auto BeforeOptions = Assets->GetOptions().size();

	for (const std::string_view Destination : {"../Escaped", "Imported/../../Escaped", "../Engine/Textures"})
	{
		CAPTURE(Destination);
		Assets->ImportFiles({Source}, std::string(Destination));
		CHECK_FALSE(Assets->IsImporting());
		CHECK(FPreviewAssetsTestAccess::GetOutstandingTaskCount(*Assets) == BeforeTasks);
		Fixture.Tasks->RunUntilIdle();
		CHECK(TakeContentSnapshot(Fixture.Scratch.GetPath()) == Before);
		CHECK(Assets->GetOptionsGeneration() == BeforeGeneration);
		CHECK(Assets->GetOptions().size() == BeforeOptions);
		CHECK_FALSE(std::filesystem::exists(Fixture.Scratch.GetPath() / "Escaped"));
		CHECK_FALSE(std::filesystem::exists(Fixture.Scratch.GetPath() / "Engine/Textures/Grass Tile.png"));
	}

	CHECK(Fixture.Device.Submissions == 0);
	CHECK(Fixture.Device.Events.empty());
}

TEST_CASE("Preview folders include mount roots and empty filesystem directories")
{
	FPreviewAssetsFixture Fixture;
	const auto& Root = Fixture.Scratch.GetPath();
	REQUIRE(std::filesystem::create_directories(Root / "Engine/Empty/Nested"));
	REQUIRE(std::filesystem::create_directories(Root / "Game/Empty/Nested"));
	REQUIRE(std::filesystem::create_directories(Root / "Game/.Hidden/Nested"));
	auto Assets = Fixture.CreateAssets();
	REQUIRE(Assets);
	Fixture.Tasks->RunUntilIdle();
	const auto Folders = Assets->GetFolders();
	const std::vector<std::string> Expected{
	    "Engine",
	    "Engine/Empty",
	    "Engine/Empty/Nested",
	    "Engine/Textures",
	    "Game",
	    "Game/Empty",
	    "Game/Empty/Nested",
	    "Game/Models",
	    "Game/Textures",
	};

	CHECK(std::ranges::equal(Folders, Expected));
	CHECK(Assets->GetOptions().size() == 3);
	CHECK(Fixture.Device.Submissions == 0);
}

TEST_CASE("Preview folder creation returns a selectable mounted path and preserves collisions")
{
	FPreviewAssetsFixture Fixture;
	auto Assets = Fixture.CreateAssets();
	REQUIRE(Assets);
	Fixture.Tasks->RunUntilIdle();
	const auto InitialGeneration = Assets->GetOptionsGeneration();
	const auto InitialOptions = Assets->GetOptions().size();
	const auto Parent = Assets->CreateFolder("Game", "Art Assets");
	REQUIRE(Parent);
	CHECK(*Parent == "Game/Art Assets");
	CHECK(std::filesystem::is_empty(Fixture.Scratch.GetPath() / "Game/Art Assets"));
	CHECK(std::ranges::find(Assets->GetFolders(), *Parent) != Assets->GetFolders().end());
	const auto Nested = Assets->CreateFolder(*Parent, "Props");
	REQUIRE(Nested);
	CHECK(*Nested == "Game/Art Assets/Props");
	CHECK(std::filesystem::is_empty(Fixture.Scratch.GetPath() / "Game/Art Assets/Props"));
	CHECK(std::ranges::find(Assets->GetFolders(), *Nested) != Assets->GetFolders().end());
	CHECK(Assets->GetOptionsGeneration() > InitialGeneration);
	Fixture.Tasks->RunUntilIdle();
	CHECK(std::ranges::find(Assets->GetFolders(), *Nested) != Assets->GetFolders().end());
	CHECK(Assets->GetOptions().size() == InitialOptions);
	const auto Generation = Assets->GetOptionsGeneration();
	const auto FolderCount = Assets->GetFolders().size();
	Tests::WriteText(Fixture.Scratch.GetPath() / "Game/Art Assets/Keep.txt", "original");
	const auto Before = TakeContentSnapshot(Fixture.Scratch.GetPath());
	CHECK_FALSE(Assets->CreateFolder(*Parent, "Props"));
	CHECK_FALSE(Assets->CreateFolder(*Parent, "pRoPs"));
	CHECK_FALSE(Assets->CreateFolder(*Parent, "Keep.txt"));
	CHECK_FALSE(Assets->CreateFolder(*Parent, "KEEP.TXT"));
	CHECK(TakeContentSnapshot(Fixture.Scratch.GetPath()) == Before);
	CHECK(Assets->GetFolders().size() == FolderCount);
	CHECK(Assets->GetOptionsGeneration() == Generation);
	CHECK(Fixture.Device.Submissions == 0);
}

TEST_CASE("Preview folder creation rejects invalid names and unsafe mounted parents without writes")
{
	FPreviewAssetsFixture Fixture;
	auto Assets = Fixture.CreateAssets();
	REQUIRE(Assets);
	Fixture.Tasks->RunUntilIdle();
	const auto Before = TakeContentSnapshot(Fixture.Scratch.GetPath());
	const auto Generation = Assets->GetOptionsGeneration();
	const auto Outstanding = FPreviewAssetsTestAccess::GetOutstandingTaskCount(*Assets);
	const std::vector<std::string> Folders(Assets->GetFolders().begin(), Assets->GetFolders().end());
	const std::vector<std::string> InvalidNames{
	    "",
	    ".",
	    "..",
	    ".Hidden",
	    "../Escaped",
	    "Nested/Child",
	    "Nested\\Child",
	    "Trailing.",
	    "Trailing ",
	    "Bad?Name",
	    "Bad\tName",
	    "C:",
	    "CON",
	    "con.txt",
	    "NUL",
	    "COM1",
	    "lpt9",
	    std::string(256, 'A'),
	    std::string("\xc0\xaf", 2),
	    std::string("\xed\xa0\x80", 3),
	    std::string("\xf4\x90\x80\x80", 4),
	};

	for (const std::string& Name : InvalidNames)
	{
		CAPTURE(Name);
		CHECK_FALSE(Assets->CreateFolder("Game", Name));
	}

	for (const std::string_view Parent : {"", "Engine", "Engine/Textures", "game", "Other", "Game/../Engine", "Game/Missing", "Game/Textures/..", "Game//Textures", "Game\\Textures"})
	{
		CAPTURE(Parent);
		CHECK_FALSE(Assets->CreateFolder(Parent, "MustNotExist"));
	}

	CHECK(TakeContentSnapshot(Fixture.Scratch.GetPath()) == Before);
	CHECK(std::ranges::equal(Assets->GetFolders(), Folders));
	CHECK(Assets->GetOptionsGeneration() == Generation);
	CHECK(FPreviewAssetsTestAccess::GetOutstandingTaskCount(*Assets) == Outstanding);
	CHECK_FALSE(std::filesystem::exists(Fixture.Scratch.GetPath() / "Escaped"));
	CHECK_FALSE(std::filesystem::exists(Fixture.Scratch.GetPath() / "Engine/MustNotExist"));
	CHECK_FALSE(std::filesystem::exists(Fixture.Scratch.GetPath() / "Game/Missing"));
}

TEST_CASE("Preview automatic folder names increment across case-folded file and directory collisions")
{
	FPreviewAssetsFixture Fixture;
	auto Assets = Fixture.CreateAssets();
	REQUIRE(Assets);
	Fixture.Tasks->RunUntilIdle();
	const auto First = Assets->CreateFolder("Game", "New Folder", true);
	REQUIRE(First);
	CHECK(*First == "Game/New Folder");
	const auto Second = Assets->CreateFolder("Game", "New Folder", true);
	REQUIRE(Second);
	CHECK(*Second == "Game/New Folder 2");
	REQUIRE(std::filesystem::create_directory(Fixture.Scratch.GetPath() / "Game/nEW fOLDER 3"));
	Tests::WriteText(Fixture.Scratch.GetPath() / "Game/nEW fOLDER 4", "keep");
	const auto Before = TakeContentSnapshot(Fixture.Scratch.GetPath());
	const auto Fifth = Assets->CreateFolder("Game", "New Folder", true);
	REQUIRE(Fifth);
	CHECK(*Fifth == "Game/New Folder 5");
	CHECK(TakeContentSnapshot(Fixture.Scratch.GetPath()) == Before);
	Fixture.Tasks->RunUntilIdle();

	for (const std::string* const Created : {&*First, &*Second, &*Fifth})
	{
		CHECK(std::ranges::find(Assets->GetFolders(), *Created) != Assets->GetFolders().end());
		CHECK(std::filesystem::is_empty(Fixture.Scratch.GetPath() / *Created));
	}

	CHECK_FALSE(Assets->CreateFolder("Game", "New Folder"));
	CHECK(Assets->GetOptions().size() == 3);
}

TEST_CASE("Preview inline folder rename updates the filesystem and published mounted selection")
{
	FPreviewAssetsFixture Fixture;
	auto Assets = Fixture.CreateAssets();
	REQUIRE(Assets);
	Fixture.Tasks->RunUntilIdle();
	const auto Created = Assets->CreateFolder("Game/Textures", "New Folder", true);
	REQUIRE(Created);
	const auto BeforeGeneration = Assets->GetOptionsGeneration();
	const auto Renamed = Assets->RenameFolder(*Created, "Artwork");
	REQUIRE(Renamed);
	CHECK(*Renamed == "Game/Textures/Artwork");
	CHECK_FALSE(std::filesystem::exists(Fixture.Scratch.GetPath() / *Created));
	CHECK(std::filesystem::is_empty(Fixture.Scratch.GetPath() / *Renamed));
	CHECK(std::ranges::find(Assets->GetFolders(), *Created) == Assets->GetFolders().end());
	CHECK(std::ranges::find(Assets->GetFolders(), *Renamed) != Assets->GetFolders().end());
	CHECK(Assets->GetOptionsGeneration() > BeforeGeneration);
	Fixture.Tasks->RunUntilIdle();
	CHECK(std::ranges::find(Assets->GetFolders(), *Created) == Assets->GetFolders().end());
	CHECK(std::ranges::find(Assets->GetFolders(), *Renamed) != Assets->GetFolders().end());
	const auto Generation = Assets->GetOptionsGeneration();
	const auto Unchanged = Assets->RenameFolder(*Renamed, "Artwork");
	REQUIRE(Unchanged);
	CHECK(*Unchanged == *Renamed);
	CHECK(Assets->GetOptionsGeneration() == Generation);
	CHECK(Assets->GetOptions().size() == 3);
	CHECK(Fixture.Device.Submissions == 0);
}

TEST_CASE("Preview inline folder rename rejects collisions and invalid destinations without overwriting")
{
	FPreviewAssetsFixture Fixture;
	auto Assets = Fixture.CreateAssets();
	REQUIRE(Assets);
	Fixture.Tasks->RunUntilIdle();
	const auto Original = Assets->CreateFolder("Game", "Original");
	const auto Existing = Assets->CreateFolder("Game", "Existing");
	REQUIRE(Original);
	REQUIRE(Existing);
	Tests::WriteText(Fixture.Scratch.GetPath() / "Game/Keep.txt", "preserve");
	Fixture.Tasks->RunUntilIdle();
	const auto Before = TakeContentSnapshot(Fixture.Scratch.GetPath());
	const auto Generation = Assets->GetOptionsGeneration();
	const std::vector<std::string> Folders(Assets->GetFolders().begin(), Assets->GetFolders().end());

	for (const std::string_view Name : {"Existing", "eXiStInG", "Keep.txt", "KEEP.TXT", "original", "", ".", "..", ".Hidden", "../Escaped", "Child/Nested", "Child\\Nested", "Trailing.", "Trailing ", "CON", "LPT1", "Bad?Name"})
	{
		CAPTURE(Name);
		CHECK_FALSE(Assets->RenameFolder(*Original, Name));
	}

	for (const std::string_view Folder : {"Game", "Engine", "Engine/Textures", "Game/../Engine", "Game/Missing", "Game/Keep.txt"})
	{
		CAPTURE(Folder);
		CHECK_FALSE(Assets->RenameFolder(Folder, "MustNotExist"));
	}

	CHECK(TakeContentSnapshot(Fixture.Scratch.GetPath()) == Before);
	CHECK(std::filesystem::is_empty(Fixture.Scratch.GetPath() / *Original));
	CHECK(std::filesystem::is_empty(Fixture.Scratch.GetPath() / *Existing));
	CHECK(std::ranges::equal(Assets->GetFolders(), Folders));
	CHECK(Assets->GetOptionsGeneration() == Generation);
	CHECK_FALSE(std::filesystem::exists(Fixture.Scratch.GetPath() / "Escaped"));
}

TEST_CASE("Preview inline folder rename rejects nonempty folders without changing assets")
{
	FPreviewAssetsFixture Fixture;
	REQUIRE(std::filesystem::create_directories(Fixture.Scratch.GetPath() / "Game/Container/EmptyChild"));
	auto Assets = Fixture.CreateAssets();
	REQUIRE(Assets);
	Fixture.Tasks->RunUntilIdle();
	const auto Before = TakeContentSnapshot(Fixture.Scratch.GetPath());
	const auto Generation = Assets->GetOptionsGeneration();
	const std::vector<std::string> Folders(Assets->GetFolders().begin(), Assets->GetFolders().end());
	CHECK_FALSE(Assets->RenameFolder("Game/Textures", "MovedTextures"));
	CHECK_FALSE(Assets->RenameFolder("Game/Container", "MovedContainer"));
	CHECK(TakeContentSnapshot(Fixture.Scratch.GetPath()) == Before);
	CHECK(std::filesystem::is_directory(Fixture.Scratch.GetPath() / "Game/Container/EmptyChild"));
	CHECK_FALSE(std::filesystem::exists(Fixture.Scratch.GetPath() / "Game/MovedTextures"));
	CHECK_FALSE(std::filesystem::exists(Fixture.Scratch.GetPath() / "Game/MovedContainer"));
	CHECK(std::ranges::equal(Assets->GetFolders(), Folders));
	CHECK(Assets->GetOptionsGeneration() == Generation);
	const auto Scan = ScanContentRoot(Fixture.Scratch.GetPath() / "Game");
	REQUIRE(Scan);
	REQUIRE(Scan->Registry.Find(WoodId));
	CHECK(Scan->Registry.Find(WoodId)->SourcePath == "Textures/Wood.png");
	CHECK(Scan->Errors.empty());
}

TEST_CASE("Preview folder creation cannot escape Game content through linked directories")
{
	FPreviewAssetsFixture Fixture;
	const auto& Root = Fixture.Scratch.GetPath();
	const auto Outside = Root / "Outside";
	const auto Linked = Root / "Game/Linked";
	REQUIRE(std::filesystem::create_directory(Outside));
	std::error_code Error;
	std::filesystem::create_directory_symlink(Outside, Linked, Error);

#ifdef _WIN32
	if (Error)
	{
		const auto Junction = RunProcess(MakeShellRequest("mklink /J \"" + Linked.string() + "\" \"" + Outside.string() + "\""));
		REQUIRE(Junction);
		REQUIRE(Junction->ExitCode == 0);
	}
#else
	REQUIRE_FALSE(Error);
#endif

	auto Assets = Fixture.CreateAssets();
	REQUIRE(Assets);
	Fixture.Tasks->RunUntilIdle();
	CHECK(std::ranges::find(Assets->GetFolders(), "Game/Linked") == Assets->GetFolders().end());
	CHECK_FALSE(Assets->CreateFolder("Game/Linked", "Escaped"));
	CHECK_FALSE(Assets->CreateFolder("Game/Linked/Nested", "Escaped"));
	CHECK_FALSE(Assets->RenameFolder("Game/Linked", "Moved"));
	CHECK_FALSE(Assets->RenameFolder("Game/Linked/Nested", "Moved"));
	CHECK_FALSE(Assets->GetFolderPath("Game/Linked"));
	CHECK(std::filesystem::is_empty(Outside));
	CHECK_FALSE(std::filesystem::exists(Outside / "Escaped"));
	CHECK_FALSE(std::filesystem::exists(Outside / "Nested"));
}

TEST_CASE("Preview folder paths resolve known mounts and reject invalid or missing directories")
{
	FPreviewAssetsFixture Fixture;
	auto Assets = Fixture.CreateAssets();
	REQUIRE(Assets);
	Fixture.Tasks->RunUntilIdle();
	const auto& Root = Fixture.Scratch.GetPath();
	CHECK(Assets->GetFolderPath("Game") == std::filesystem::canonical(Root / "Game"));
	CHECK(Assets->GetFolderPath("Game/Models") == std::filesystem::canonical(Root / "Game/Models"));
	CHECK(Assets->GetFolderPath("Engine") == std::filesystem::canonical(Root / "Engine"));
	CHECK_FALSE(Assets->GetFolderPath(""));
	CHECK_FALSE(Assets->GetFolderPath("Unknown"));
	CHECK_FALSE(Assets->GetFolderPath("Gameplay"));
	CHECK_FALSE(Assets->GetFolderPath("Game/Missing"));
	CHECK_FALSE(Assets->GetFolderPath("Game/../Engine"));
	CHECK_FALSE(Assets->GetFolderPath("Game/Models/Robot.blend"));
}

TEST_CASE("Preview folder polling notices empty directory additions and removals without mesh requests")
{
	FPreviewAssetsFixture Fixture;
	auto Assets = Fixture.CreateAssets();
	REQUIRE(Assets);
	Fixture.Tasks->RunUntilIdle();
	const auto BeforeOptions = Assets->GetOptions().size();
	const auto RequestGeneration = FPreviewAssetsTestAccess::GetRequestGeneration(*Assets);
	const auto InitialGeneration = Assets->GetOptionsGeneration();
	const auto Empty = Fixture.Scratch.GetPath() / "Game/ExternalEmpty";
	REQUIRE(std::filesystem::create_directory(Empty));
	Assets->CheckForChanges();
	Fixture.Tasks->RunUntilIdle();
	CHECK(std::ranges::find(Assets->GetFolders(), "Game/ExternalEmpty") != Assets->GetFolders().end());
	CHECK(Assets->GetOptionsGeneration() > InitialGeneration);
	const auto AddedGeneration = Assets->GetOptionsGeneration();
	REQUIRE(std::filesystem::remove(Empty));
	Assets->CheckForChanges();
	Fixture.Tasks->RunUntilIdle();
	CHECK(std::ranges::find(Assets->GetFolders(), "Game/ExternalEmpty") == Assets->GetFolders().end());
	CHECK(Assets->GetOptionsGeneration() > AddedGeneration);
	CHECK(Assets->GetOptions().size() == BeforeOptions);
	CHECK(FPreviewAssetsTestAccess::GetRequestGeneration(*Assets) == RequestGeneration);
	CHECK(Fixture.Device.Submissions == 0);
	CHECK(Fixture.Device.Events.empty());
}

TEST_CASE("Preview folders created during an older scan cannot be replaced by its stale folder snapshot")
{
	FPreviewAssetsFixture Fixture;
	auto Assets = Fixture.CreateAssets();
	REQUIRE(Assets);
	Fixture.Tasks->RunUntilIdle();
	Assets->RequestScan();
	Fixture.RunToCheckpoint();
	CHECK(Assets->IsScanning());
	const auto Created = Assets->CreateFolder("Game", "CreatedDuringScan");
	REQUIRE(Created);
	Fixture.Tasks->RunUntilIdle();
	CHECK(std::ranges::find(Assets->GetFolders(), *Created) != Assets->GetFolders().end());
	CHECK(std::filesystem::is_directory(Fixture.Scratch.GetPath() / "Game/CreatedDuringScan"));
	Assets->CheckForChanges();
	Fixture.Tasks->RunUntilIdle();
	CHECK(std::ranges::find(Assets->GetFolders(), *Created) != Assets->GetFolders().end());
}

TEST_CASE("Content snapshots notice edits, additions, and removals but skip dot-prefixed entries")
{
	const Tests::FScratchDirectory Scratch("HertaContentSnapshot");
	Tests::WriteText(Scratch.GetPath() / "Models/A.gltf", "a");
	Tests::WriteText(Scratch.GetPath() / ".cache/Ignored.txt", "x");
	const FContentSnapshot First = TakeContentSnapshot(Scratch.GetPath());
	REQUIRE(First.size() == 1);
	CHECK(First.contains("Models/A.gltf"));
	CHECK(TakeContentSnapshot(Scratch.GetPath()) == First);

	Tests::WriteText(Scratch.GetPath() / "Models/A.gltf", "changed");
	const FContentSnapshot Edited = TakeContentSnapshot(Scratch.GetPath());
	CHECK(Edited != First);
	Tests::WriteText(Scratch.GetPath() / "Models/B.gltf", "b");
	CHECK(TakeContentSnapshot(Scratch.GetPath()).size() == 2);
	std::filesystem::remove(Scratch.GetPath() / "Models/A.gltf");
	CHECK_FALSE(TakeContentSnapshot(Scratch.GetPath()).contains("Models/A.gltf"));
}

TEST_CASE("Engine content provides the 1 m preview cube with outward faces and unmirrored UVs")
{
	const auto Scan = ScanContentRoot("Engine/Content");
	REQUIRE(Scan);
	CHECK(Scan->Errors.empty());
	CHECK(Scan->UnregisteredSources.empty());
	const FAssetRecord* const Cube = Scan->Registry.Find(EngineCubeAsset);
	REQUIRE(Cube);
	CHECK(Cube->SourcePath == "Shapes/Cube.gltf");

	const Tests::FScratchDirectory Scratch("HertaEngineContent");
	const auto Cooked = CookAsset({.ContentRoot = "Engine/Content", .DerivedDataRoot = Scratch.GetPath(), .SourcePath = Cube->SourcePath, .TargetPlatform = "TestPlatform", .bForce = false});
	REQUIRE(Cooked);
	CHECK(Cooked->Warnings.empty());
	auto Asset = LoadCookedAsset(Scratch.GetPath(), Cooked->Key);
	REQUIRE(Asset);
	REQUIRE(std::holds_alternative<FCookedModel>(*Asset));
	const FCookedModel& Model = std::get<FCookedModel>(*Asset);
	// Corners where two faces share position and UV are merged, because vertices carry no normals yet.
	CHECK(Model.Vertices.size() <= 24);
	REQUIRE(Model.Indices.size() == 36);
	REQUIRE(Model.Textures.size() == 1);
	CHECK(Model.Textures[0].Mips.size() == 10);

	for (const FCookedVertex& Vertex : Model.Vertices)
	{
		for (const float Coordinate : Vertex.Position)
		{
			CHECK(std::abs(Coordinate) == 0.5f);
		}
	}

	for (std::size_t Index = 0; Index < Model.Indices.size(); Index += 3)
	{
		const FCookedVertex& A = Model.Vertices[Model.Indices[Index]];
		const FCookedVertex& B = Model.Vertices[Model.Indices[Index + 1]];
		const FCookedVertex& C = Model.Vertices[Model.Indices[Index + 2]];
		const FVector3 PositionA{A.Position[0], A.Position[1], A.Position[2]};
		const FVector3 PositionB{B.Position[0], B.Position[1], B.Position[2]};
		const FVector3 PositionC{C.Position[0], C.Position[1], C.Position[2]};
		CHECK((PositionB - PositionA).Cross(PositionC - PositionA).Dot(PositionA + PositionB + PositionC) > 0.f);
		const float UVArea = (B.UV[0] - A.UV[0]) * (C.UV[1] - A.UV[1]) - (B.UV[1] - A.UV[1]) * (C.UV[0] - A.UV[0]);
		CHECK(UVArea < 0.f);
	}
}
}
