#include "Herta/AssetPipeline/AssetCooker.h"
#include "Herta/Core/Log.h"
#include "Herta/Tasks/TaskSystem.h"
#include "PreviewAssets.h"
#include "PreviewScene.h"
#include "TestFiles.h"
#include "TestGraphicsDevice.h"

#include <array>
#include <cmath>
#include <doctest/doctest.h>

namespace Herta
{
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

	static void WriteTexture(const std::filesystem::path& Root, const std::string& Path, const FAssetId& Id, const std::array<std::uint8_t, 4>& Pixel)
	{
		Tests::WritePng(Root / Path, 1, 1, Pixel);
		Tests::WriteText(Root / (Path + ".hmeta"), "Format = HertaAssetMetadata\nVersion = 1\nId = " + Id.ToString() + "\nImporter = Texture\n");
	}

	[[nodiscard]] std::unique_ptr<FPreviewAssets> CreateAssets()
	{
		const std::filesystem::path& Root = Scratch.GetPath();
		return FPreviewAssets::Create(*Tasks, Device, *Log, {Root / "Engine", Root / "Game", Root / "DerivedDataCache", Tests::GetSiblingExecutable("HertaAssetWorker"), "TestPlatform"}, 2);
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
	const auto Cooked = CookAsset({"Engine/Content", Scratch.GetPath(), Cube->SourcePath, "TestPlatform", false});
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
		CHECK((PositionB - PositionA).Cross(PositionC - PositionA).Dot(PositionA + PositionB + PositionC) > 0.0f);
		const float UVArea = (B.UV[0] - A.UV[0]) * (C.UV[1] - A.UV[1]) - (B.UV[1] - A.UV[1]) * (C.UV[0] - A.UV[0]);
		CHECK(UVArea < 0.0f);
	}
}
}
