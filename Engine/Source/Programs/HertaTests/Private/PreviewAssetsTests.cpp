#include "Herta/AssetPipeline/AssetCooker.h"
#include "Herta/Core/Log.h"
#include "Herta/Tasks/TaskSystem.h"
#include "PreviewAssets.h"
#include "PreviewScene.h"
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

namespace
{
constexpr FAssetId StoneId{0x0000000000004000, 0x8000000000000003};
constexpr FAssetId WoodId{0x0000000000004000, 0x8000000000000001};
constexpr FAssetId RobotId{0x0000000000004000, 0x8000000000000002};

struct FPreviewAssetsFixture
{
	Tests::FScratchDirectory Scratch{"HertaPreviewAssets"};
	std::unique_ptr<FLogService> Log;
	std::unique_ptr<FTaskSystem> Tasks;
	Tests::FTestGraphicsDevice Device;

	FPreviewAssetsFixture()
	{
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

	[[nodiscard]] std::unique_ptr<FPreviewAssets> CreateAssets()
	{
		const std::filesystem::path& Root = Scratch.GetPath();
		return FPreviewAssets::Create(*Tasks, Device, *Log, {.EngineContentRoot = Root / "Engine", .ContentRoot = Root / "Game", .DerivedDataRoot = Root / "DerivedDataCache", .WorkerPath = Tests::GetSiblingExecutable("HertaAssetWorker"), .TargetPlatform = "TestPlatform"}, 2);
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

TEST_CASE("Scene rebinding retains loaded meshes across copies reorder deletion and load")
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

TEST_CASE("Scene rebinding coalesces pending copies before and after the first content scan")
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

TEST_CASE("Copies retain visible meshes and share reimport results while scene bindings change")
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
