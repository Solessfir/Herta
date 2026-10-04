#include "TestFiles.h"

#include "Herta/EditorCore/CommandRegistry.h"
#include "Herta/EditorCore/SceneCommands.h"
#include "Herta/Platform/Process.h"
#include "Herta/Scene/ScalingScene.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <span>

namespace Herta
{
namespace
{
constexpr FAssetId EngineCubeAsset{0x59f13694df4844e5, 0x863555194493f242};

std::string ReadText(const std::filesystem::path& Path)
{
	std::ifstream Stream(Path, std::ios::binary);
	return {std::istreambuf_iterator<char>(Stream), std::istreambuf_iterator<char>()};
}

void CheckWorkload(const FSceneDocument& Scene, const std::size_t CubeCount, const EScalingSceneWorkload Workload)
{
	REQUIRE(Scene.Entities.size() == CubeCount + 1);
	const FSceneEntity& Floor = Scene.Entities.front();
	REQUIRE(Floor.Mesh);
	CHECK(Floor.Mesh->Asset == EngineCubeAsset);
	CHECK(Floor.BodyType == (Workload == EScalingSceneWorkload::Rendering ? ESceneBodyType::None : ESceneBodyType::Static));

	const std::span<const FSceneEntity> Cubes = std::span<const FSceneEntity>(Scene.Entities).subspan(1);
	CHECK(std::ranges::all_of(Cubes, [](const FSceneEntity& Cube)
	{
		return Cube.Mesh && Cube.Mesh->Asset == EngineCubeAsset;
	}));
	const ESceneBodyType CubeBodyType = Workload == EScalingSceneWorkload::Rendering ? ESceneBodyType::None : ESceneBodyType::Dynamic;
	CHECK(std::ranges::all_of(Cubes, [CubeBodyType](const FSceneEntity& Cube)
	{
		return Cube.BodyType == CubeBodyType;
	}));
}
}

TEST_CASE("Scaling scenes provide deterministic rendering and dynamic workloads")
{
	for (const EScalingSceneWorkload Workload : {EScalingSceneWorkload::Rendering, EScalingSceneWorkload::DynamicBodies})
	{
		for (const std::size_t Count : {1000u, 5000u, 10000u})
		{
			const auto Scene = GenerateScalingScene({.CubeCount = Count, .Workload = Workload});
			REQUIRE(Scene);
			CheckWorkload(*Scene, Count, Workload);
			REQUIRE(ValidateSceneEntities(Scene->Entities));

			const auto Text = SerializeScene(*Scene);
			REQUIRE(Text);
			const auto RoundTrip = ParseScene(*Text);
			REQUIRE(RoundTrip);
			CHECK(RoundTrip->Id == Scene->Id);
			CHECK(RoundTrip->Name == Scene->Name);
			CHECK(RoundTrip->Entities == Scene->Entities);
		}
	}

	const auto First = GenerateScalingScene({.CubeCount = 1000, .Workload = EScalingSceneWorkload::DynamicBodies});
	const auto Second = GenerateScalingScene({.CubeCount = 1000, .Workload = EScalingSceneWorkload::DynamicBodies});
	REQUIRE(First);
	REQUIRE(Second);
	const auto FirstText = SerializeScene(*First);
	const auto SecondText = SerializeScene(*Second);
	REQUIRE(FirstText);
	REQUIRE(SecondText);
	CHECK(*FirstText == *SecondText);
	CHECK(First->Entities[1].Transform.Translation.Meters.Y == doctest::Approx(0.5));
	CHECK(First->Entities[10].Transform.Translation.Meters.Y > First->Entities[1].Transform.Translation.Meters.Y);
	CHECK_FALSE(GenerateScalingScene({.CubeCount = 999, .Workload = EScalingSceneWorkload::Rendering}));
}

TEST_CASE("Scaling scene command preserves an existing destination when generation fails")
{
	Tests::FScratchDirectory Scratch("HertaScalingSceneCommand");
	const std::filesystem::path Path = Scratch.GetPath() / "Scaling.hscene";
	FEditorCommandRegistry Commands;
	REQUIRE(RegisterSceneFileCommands(Commands));

	const std::string CommandPath = Path.generic_string();
	REQUIRE(Commands.Execute("scene.generate-scaling rendering 1000 \"" + CommandPath + "\""));
	const std::string Before = ReadText(Path);
	REQUIRE_FALSE(Before.empty());
	CHECK_FALSE(Commands.Execute("scene.generate-scaling rendering 999 \"" + CommandPath + "\""));
	CHECK(ReadText(Path) == Before);

	const auto Scene = LoadScene(Path);
	REQUIRE(Scene);
	CheckWorkload(*Scene, 1000, EScalingSceneWorkload::Rendering);
}

TEST_CASE("Headless editor generates scaling scenes through the shared command registry")
{
	Tests::FScratchDirectory Scratch("HertaHeadlessScalingScene");
	const std::filesystem::path Path = Scratch.GetPath() / "Scaling.hscene";
	const auto Result = RunProcess({
	    .Executable = Tests::GetSiblingExecutable("HertaEditorCmd"),
	    .Arguments = {"scene.generate-scaling", "dynamic", "1000", Path.generic_string()},
	});
	REQUIRE(Result);
	CHECK(Result->ExitCode == 0);
	CHECK(Result->StandardError.empty());

	const auto Scene = LoadScene(Path);
	REQUIRE(Scene);
	CheckWorkload(*Scene, 1000, EScalingSceneWorkload::DynamicBodies);
}
}
