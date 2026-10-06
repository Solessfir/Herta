#include "Herta/EditorCore/CommandRegistry.h"
#include "Herta/EditorCore/LevelCommands.h"
#include "Herta/Level/ScalingLevel.h"
#include "Herta/Platform/Process.h"
#include "TestFiles.h"

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

void CheckWorkload(const FLevelDocument& Level, const std::size_t CubeCount, const EScalingLevelWorkload Workload)
{
	REQUIRE(Level.Entities.size() == CubeCount + 1);
	const FLevelEntity& Floor = Level.Entities.front();
	REQUIRE(Floor.Mesh);
	CHECK(Floor.Mesh->Asset == EngineCubeAsset);
	CHECK(Floor.BodyType == (Workload == EScalingLevelWorkload::Rendering ? ELevelBodyType::None : ELevelBodyType::Static));

	const std::span<const FLevelEntity> Cubes = std::span<const FLevelEntity>(Level.Entities).subspan(1);
	CHECK(std::ranges::all_of(Cubes, [](const FLevelEntity& Cube)
	{
		return Cube.Mesh && Cube.Mesh->Asset == EngineCubeAsset;
	}));
	const ELevelBodyType CubeBodyType = Workload == EScalingLevelWorkload::Rendering ? ELevelBodyType::None : ELevelBodyType::Dynamic;
	CHECK(std::ranges::all_of(Cubes, [CubeBodyType](const FLevelEntity& Cube)
	{
		return Cube.BodyType == CubeBodyType;
	}));
}
}

TEST_CASE("Scaling levels provide deterministic rendering and dynamic workloads")
{
	for (const EScalingLevelWorkload Workload : {EScalingLevelWorkload::Rendering, EScalingLevelWorkload::DynamicBodies})
	{
		for (const std::size_t Count : {1000u, 5000u, 10000u})
		{
			const auto Level = GenerateScalingLevel({.CubeCount = Count, .Workload = Workload});
			REQUIRE(Level);
			CheckWorkload(*Level, Count, Workload);
			REQUIRE(ValidateLevelEntities(Level->Entities));

			const auto Text = SerializeLevel(*Level);
			REQUIRE(Text);
			const auto RoundTrip = ParseLevel(*Text);
			REQUIRE(RoundTrip);
			CHECK(RoundTrip->Id == Level->Id);
			CHECK(RoundTrip->Name == Level->Name);
			CHECK(RoundTrip->Entities == Level->Entities);
		}
	}

	const auto First = GenerateScalingLevel({.CubeCount = 1000, .Workload = EScalingLevelWorkload::DynamicBodies});
	const auto Second = GenerateScalingLevel({.CubeCount = 1000, .Workload = EScalingLevelWorkload::DynamicBodies});
	REQUIRE(First);
	REQUIRE(Second);
	const auto FirstText = SerializeLevel(*First);
	const auto SecondText = SerializeLevel(*Second);
	REQUIRE(FirstText);
	REQUIRE(SecondText);
	CHECK(*FirstText == *SecondText);
	CHECK(First->Entities[1].Transform.Translation.Meters.Y == doctest::Approx(0.5));
	CHECK(First->Entities[10].Transform.Translation.Meters.Y > First->Entities[1].Transform.Translation.Meters.Y);
	CHECK_FALSE(GenerateScalingLevel({.CubeCount = 999, .Workload = EScalingLevelWorkload::Rendering}));
}

TEST_CASE("Scaling level command preserves an existing destination when generation fails")
{
	Tests::FScratchDirectory Scratch("HertaScalingLevelCommand");
	const std::filesystem::path Path = Scratch.GetPath() / "Scaling.hlevel";
	FEditorCommandRegistry Commands;
	REQUIRE(RegisterLevelFileCommands(Commands));

	const std::string CommandPath = Path.generic_string();
	REQUIRE(Commands.Execute("level.generate-scaling rendering 1000 \"" + CommandPath + "\""));
	const std::string Before = ReadText(Path);
	REQUIRE_FALSE(Before.empty());
	CHECK_FALSE(Commands.Execute("level.generate-scaling rendering 999 \"" + CommandPath + "\""));
	CHECK(ReadText(Path) == Before);

	const auto Level = LoadLevel(Path);
	REQUIRE(Level);
	CheckWorkload(*Level, 1000, EScalingLevelWorkload::Rendering);
}

TEST_CASE("Headless editor generates scaling levels through the shared command registry")
{
	Tests::FScratchDirectory Scratch("HertaHeadlessScalingLevel");
	const std::filesystem::path Path = Scratch.GetPath() / "Scaling.hlevel";
	const auto Result = RunProcess({
	    .Executable = Tests::GetSiblingExecutable("HertaEditorCmd"),
	    .Arguments = {"level.generate-scaling", "dynamic", "1000", Path.generic_string()},
	});
	REQUIRE(Result);
	CHECK(Result->ExitCode == 0);
	CHECK(Result->StandardError.empty());

	const auto Level = LoadLevel(Path);
	REQUIRE(Level);
	CheckWorkload(*Level, 1000, EScalingLevelWorkload::DynamicBodies);
}
}
