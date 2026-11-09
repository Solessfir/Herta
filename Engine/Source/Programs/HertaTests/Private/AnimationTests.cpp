#include "Herta/Animation/Animation.h"
#include "Herta/AssetPipeline/AssetCooker.h"
#include "Herta/Assets/CookedContent.h"
#include "TestFiles.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <string_view>

namespace Herta
{
namespace
{
std::filesystem::path FindMannequin()
{
	std::error_code Error;

	for (auto Path = std::filesystem::current_path(Error); !Path.empty(); Path = Path.parent_path())
	{
		const auto Candidate = Path / "Games/Sandbox/Content/Characters/Mannequin/Mannequin.glb";
		if (std::filesystem::is_regular_file(Candidate, Error))
		{
			return Candidate;
		}

		if (Path == Path.parent_path())
		{
			break;
		}
	}

	return {};
}

FCookedModel CookMannequin(const Tests::FScratchDirectory& Scratch)
{
	const auto Source = FindMannequin();
	REQUIRE_FALSE(Source.empty());
	const auto Content = Scratch.GetPath() / "Content";
	std::filesystem::create_directories(Content / "Characters");
	std::filesystem::copy_file(Source, Content / "Characters/Mannequin.glb");
	Tests::WriteText(Content / "Characters/Mannequin.glb.hmeta", "Format = HertaAssetMetadata\nVersion = 1\nId = 00000000-0000-4000-8000-0000000000a1\nImporter = Gltf\n");

	const auto Result = CookAsset({.ContentRoot = Content, .DerivedDataRoot = Scratch.GetPath() / "DerivedDataCache", .SourcePath = "Characters/Mannequin.glb", .TargetPlatform = "TestPlatform", .bForce = false});
	REQUIRE_MESSAGE(Result, (Result ? std::string() : Result.error().Message));

	auto Asset = LoadCookedAsset(Scratch.GetPath() / "DerivedDataCache", Result->Key);
	REQUIRE(Asset);
	REQUIRE(std::holds_alternative<FCookedModel>(*Asset));
	return std::get<FCookedModel>(std::move(*Asset));
}

float MaximumDifference(const FMatrix4& A, const FMatrix4& B)
{
	float Result = 0.f;

	for (std::size_t Index = 0; Index < 16; ++Index)
	{
		Result = std::max(Result, std::abs(A.Data()[Index] - B.Data()[Index]));
	}

	return Result;
}

const FCookedModel& Mannequin()
{
	static const Tests::FScratchDirectory Scratch("HertaAnimationMannequin");
	static const FCookedModel Model = CookMannequin(Scratch);
	return Model;
}
}

TEST_CASE("A skinned glTF cooks its skeleton, weights, and clips")
{
	const FCookedModel& Model = Mannequin();
	REQUIRE(Model.Skeleton);
	CHECK(Model.Skeleton->JointNames.size() == 53);
	CHECK(Model.Skeleton->JointNames.front() == "root");
	CHECK(Model.Skeleton->JointParents.front() == -1);
	CHECK(Model.Influences.size() == Model.Vertices.size());
	CHECK(Model.Clips.size() == 11);

	const auto Clip = [&](const std::string_view Name)
	{
		return std::ranges::find(Model.Clips, Name, &FCookedAnimationClip::Name);
	};

	REQUIRE(Clip("Walk_Loop") != Model.Clips.end());
	REQUIRE(Clip("Roll_RM") != Model.Clips.end());
	CHECK(Clip("Walk_Loop")->DurationSeconds == doctest::Approx(1.333f).epsilon(0.01));
	// In-place loops carry no root motion; the root-motion roll does.
	CHECK(Clip("Walk_Loop")->RootMotion.empty());
	CHECK_FALSE(Clip("Roll_RM")->RootMotion.empty());

	// The cooked bytes survive a round trip unchanged.
	const auto Bytes = SerializeCookedAsset(Model);
	REQUIRE(Bytes);
	const auto Loaded = DeserializeCookedAsset(*Bytes);
	REQUIRE(Loaded);
	const auto& Copy = std::get<FCookedModel>(*Loaded);
	REQUIRE(Copy.Skeleton);
	CHECK(Copy.Skeleton->JointNames == Model.Skeleton->JointNames);
	CHECK(Copy.Skeleton->Runtime == Model.Skeleton->Runtime);
	CHECK(Copy.Influences.size() == Model.Influences.size());
	CHECK(Copy.Clips.back().Animation == Model.Clips.back().Animation);
}

TEST_CASE("The bind pose skins every vertex back onto itself")
{
	const FCookedModel& Model = Mannequin();
	REQUIRE(Model.Skeleton);
	const auto Library = FAnimationLibrary::Create(*Model.Skeleton, Model.Clips);
	REQUIRE_MESSAGE(Library, (Library ? std::string() : Library.error().Message));
	CHECK((*Library)->GetJointCount() == 53);
	CHECK((*Library)->GetClipCount() == 11);

	// The rest pose of this rig is its bind pose, so every skinning matrix is identity.
	FAnimationPose Pose(*Library);
	REQUIRE(Pose.Evaluate({}));
	float Worst = 0.f;

	for (const FMatrix4& Matrix : Pose.GetSkinningMatrices())
	{
		Worst = std::max(Worst, MaximumDifference(Matrix, FMatrix4::Identity()));
	}

	CHECK(Worst < 1.e-3f);
}

TEST_CASE("Clips sample, blend, and report root motion")
{
	const FCookedModel& Model = Mannequin();
	const auto Library = FAnimationLibrary::Create(*Model.Skeleton, Model.Clips);
	REQUIRE(Library);
	const auto Walk = (*Library)->FindClip("Walk_Loop");
	const auto Jog = (*Library)->FindClip("Jog_Fwd_Loop");
	const auto Roll = (*Library)->FindClip("Roll_RM");
	REQUIRE(Walk);
	REQUIRE(Jog);
	REQUIRE(Roll);
	CHECK_FALSE((*Library)->FindClip("Missing"));

	FAnimationPose Pose(*Library);
	const auto Hands = [&]
	{
		const auto& Names = (*Library)->GetJointNames();
		const auto Hand = std::ranges::find(Names, "DEF-hand.L");
		REQUIRE(Hand != Names.end());
		return Pose.GetModelTransforms()[static_cast<std::size_t>(Hand - Names.begin())].TransformPosition({});
	};

	const std::array Start{FAnimationLayer{.Clip = *Walk, .TimeSeconds = 0.f}};
	REQUIRE(Pose.Evaluate(Start));
	const FVector3 WalkStart = Hands();
	const std::array Middle{FAnimationLayer{.Clip = *Walk, .TimeSeconds = 0.6f}};
	REQUIRE(Pose.Evaluate(Middle));
	const FVector3 WalkMiddle = Hands();
	CHECK((WalkMiddle - WalkStart).Length() > 0.05f);

	// Half walk and half jog lands between the two poses.
	const std::array JogOnly{FAnimationLayer{.Clip = *Jog, .TimeSeconds = 0.f}};
	REQUIRE(Pose.Evaluate(JogOnly));
	const FVector3 JogStart = Hands();
	const std::array Blend{FAnimationLayer{.Clip = *Walk, .TimeSeconds = 0.f, .Weight = 0.5f}, FAnimationLayer{.Clip = *Jog, .TimeSeconds = 0.f, .Weight = 0.5f}};
	REQUIRE(Pose.Evaluate(Blend));
	const FVector3 Blended = Hands();
	CHECK((Blended - (WalkStart + JogStart) * 0.5f).Length() < (WalkStart - JogStart).Length() * 0.5f + 0.01f);

	// The roll travels about five meters forward, wraps once across its end, and is removed from the pose itself.
	const float Duration = (*Library)->GetClipDuration(*Roll);
	const FVector3 Travel = (*Library)->GetRootMotion(*Roll, 0.f, Duration);
	CHECK(Travel.Z == doctest::Approx(4.99f).epsilon(0.02));
	CHECK(std::abs(Travel.X) < 0.05f);
	CHECK(Travel.Y == 0.f);
	const FVector3 Wrapped = (*Library)->GetRootMotion(*Roll, Duration * 0.75f, Duration * 0.25f);
	const FVector3 Pieces = (*Library)->GetRootMotion(*Roll, Duration * 0.75f, Duration) + (*Library)->GetRootMotion(*Roll, 0.f, Duration * 0.25f);
	CHECK((Wrapped - Pieces).Length() < 1.e-4f);
	CHECK((*Library)->GetRootMotion(*Walk, 0.f, 1.f).Length() == 0.f);

	const std::array RollEnd{FAnimationLayer{.Clip = *Roll, .TimeSeconds = Duration}};
	REQUIRE(Pose.Evaluate(RollEnd));
	CHECK(std::abs(Pose.GetModelTransforms()[0].TransformPosition({}).Z) < 0.05f);

	// Too many layers and missing clips fail without touching the pose.
	const std::array Missing{FAnimationLayer{.Clip = 99}};
	CHECK_FALSE(Pose.Evaluate(Missing));
	std::array<FAnimationLayer, FAnimationPose::MaximumLayers + 1> TooMany{};
	CHECK_FALSE(Pose.Evaluate(TooMany));
}

TEST_CASE("Animation libraries reject archives that do not match their skeleton")
{
	const FCookedModel& Model = Mannequin();
	FCookedSkeleton Broken = *Model.Skeleton;
	Broken.Runtime.resize(Broken.Runtime.size() / 2);
	CHECK_FALSE(FAnimationLibrary::Create(Broken, Model.Clips));

	FCookedSkeleton Renamed = *Model.Skeleton;
	Renamed.JointNames[3] = "Renamed";
	CHECK_FALSE(FAnimationLibrary::Create(Renamed, Model.Clips));

	std::vector<FCookedAnimationClip> Clips = Model.Clips;
	Clips.front().Animation = Model.Skeleton->Runtime;
	CHECK_FALSE(FAnimationLibrary::Create(*Model.Skeleton, Clips));
}
}
