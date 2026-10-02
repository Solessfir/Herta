#include "Herta/Core/Log.h"
#include "Herta/Tasks/TaskSystem.h"
#include "PreviewAssets.h"
#include "TestFiles.h"
#include "TestGraphicsDevice.h"

#include <array>
#include <doctest/doctest.h>

namespace Herta
{
namespace
{
struct FPreviewAssetsFixture
{
	Tests::FScratchDirectory Scratch{"HertaPreviewAssets"};
	std::unique_ptr<FLogService> Log;
	std::unique_ptr<FTaskSystem> Tasks;
	Tests::FTestGraphicsDevice Device;

	FPreviewAssetsFixture()
	{
		const std::filesystem::path Root = Scratch.GetPath() / "Content";
		const std::array<std::uint8_t, 4> Wood{150, 111, 51, 255};
		Tests::WritePng(Root / "Textures/Wood.png", 1, 1, Wood);
		Tests::WriteText(Root / "Textures/Wood.png.hmeta", "Format = HertaAssetMetadata\nVersion = 1\nId = 00000000-0000-4000-8000-000000000001\nImporter = Texture\n");
		Tests::WriteText(Root / "Models/Robot.blend", "blend");
		Tests::WriteText(Root / "Models/Robot.blend.hmeta", "Format = HertaAssetMetadata\nVersion = 1\nId = 00000000-0000-4000-8000-000000000002\nImporter = Blender\n");

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

	[[nodiscard]] std::unique_ptr<FPreviewAssets> CreateAssets()
	{
		return FPreviewAssets::Create(*Tasks, Device, *Log, {Scratch.GetPath() / "Content", Scratch.GetPath() / "DerivedDataCache", Tests::GetSiblingExecutable("HertaAssetWorker"), "TestPlatform"}, 2);
	}
};
}

TEST_CASE("Preview assets scan content and publish cooked meshes between frames")
{
	FPreviewAssetsFixture Fixture;
	auto Assets = Fixture.CreateAssets();
	REQUIRE(Assets);
	CHECK(Assets->IsScanning());
	Fixture.Tasks->RunUntilIdle();
	CHECK_FALSE(Assets->IsScanning());
	REQUIRE(Assets->GetRegistry().GetRecords().size() == 2);

	const FAssetRecord Wood = *Assets->GetRegistry().FindBySourcePath("Textures/Wood.png");
	Assets->RequestMesh(0, Wood);
	CHECK(Assets->GetSlot(0).bLoading);
	CHECK_FALSE(Assets->GetSlot(0).Mesh);
	Fixture.Tasks->RunUntilIdle();
	const FPreviewMeshSlot& Loaded = Assets->GetSlot(0);
	CHECK_FALSE(Loaded.bLoading);
	CHECK(Loaded.Error.empty());
	REQUIRE(Loaded.Mesh);
	CHECK(Loaded.Asset == Wood.Id);
	CHECK(Loaded.Mesh->GetBoundsMinimum() == FVector3{-1, -1, -1});
	CHECK(Loaded.Mesh->GetBoundsMaximum() == FVector3{1, 1, 1});

	const FAssetRecord Robot = *Assets->GetRegistry().FindBySourcePath("Models/Robot.blend");
	Assets->RequestMesh(1, Robot);
	Fixture.Tasks->RunUntilIdle();
	CHECK_FALSE(Assets->GetSlot(1).Mesh);
	CHECK(Assets->GetSlot(1).Error.find("not available yet") != std::string::npos);

	// A newer request wins even when the older one finishes later.
	Assets->RequestMesh(1, Wood);
	Assets->ResetMesh(1);
	Fixture.Tasks->RunUntilIdle();
	CHECK_FALSE(Assets->GetSlot(1).Mesh);
	CHECK_FALSE(Assets->GetSlot(1).bLoading);
	CHECK(Assets->GetSlot(1).Error.empty());

	// Destroying with work in flight cancels it instead of publishing into a dead editor.
	Assets->RequestMesh(1, Wood);
	Assets.reset();
}
}
