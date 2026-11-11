#include "Herta/Animation/Animation.h"
#include "Herta/Animation/AnimationStateMachine.h"
#include "Herta/Animation/MeshSlice.h"
#include "Herta/AssetPipeline/AssetCooker.h"
#include "Herta/Assets/CookedContent.h"
#include "Herta/GameFramework/GameAssets.h"
#include "Herta/GameFramework/GameSession.h"
#include "Herta/Math/AffineTransform.h"
#include "Herta/Navigation/NavigationWorld.h"
#include "Herta/NavigationBuilder/NavigationBuilder.h"
#include "Herta/StateTreeCompiler/StateTreeCompiler.h"
#include "TestFiles.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <format>
#include <tuple>
#include <numbers>
#include <map>
#include <fstream>
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

TEST_CASE("A model whose metadata names an animation graph cooks its state machine, which the Mannequin's clips can play")
{
	const auto Source = FindMannequin();
	REQUIRE_FALSE(Source.empty());
	const Tests::FScratchDirectory Scratch("HertaAnimationGraphCook");
	const auto Content = Scratch.GetPath() / "Content";
	std::filesystem::create_directories(Content / "Characters");
	std::filesystem::copy_file(Source, Content / "Characters/Mannequin.glb");
	std::filesystem::copy_file(Source.parent_path() / "Mannequin.hanimgraph", Content / "Characters/Mannequin.hanimgraph");
	const auto Metadata = [&](const std::string_view Graph, const std::string_view Severable = "neck_01, calf_l")
	{
		Tests::WriteText(Content / "Characters/Mannequin.glb.hmeta", std::format("Format = HertaAssetMetadata\nVersion = 1\nId = 00000000-0000-4000-8000-0000000000a2\nImporter = Gltf\nSetting.AnimationGraph = {}\nSetting.SeverableJoints = {}\n", Graph, Severable));
	};

	const FAssetCookRequest Request{.ContentRoot = Content, .DerivedDataRoot = Scratch.GetPath() / "DerivedDataCache", .SourcePath = "Characters/Mannequin.glb", .TargetPlatform = "TestPlatform", .bForce = false};
	Metadata("Mannequin.hanimgraph");
	const auto Cooked = CookAsset(Request);
	REQUIRE_MESSAGE(Cooked, (Cooked ? std::string() : Cooked.error().Message));
	auto Asset = LoadCookedAsset(Request.DerivedDataRoot, Cooked->Key);
	REQUIRE(Asset);
	const FCookedModel& Model = std::get<FCookedModel>(*Asset);
	REQUIRE(Model.StateMachine);
	const auto Library = FAnimationLibrary::Create(*Model.Skeleton, Model.Clips);
	REQUIRE(Library);
	const auto Machine = FAnimationStateMachine::Create(*Model.StateMachine, **Library);
	REQUIRE_MESSAGE(Machine, (Machine ? std::string() : Machine.error().Message));
	CHECK(Machine->FindParameter("Speed"));
	// The authored graph keeps up with the built-in machine, including the air attack's swing.
	const auto AirAttack = std::ranges::find(Model.StateMachine->States, "Air attack", &FAnimationStateDesc::Name);
	REQUIRE(AirAttack != Model.StateMachine->States.end());
	CHECK(AirAttack->Events.size() == 2);
	CHECK(Model.SeverableJoints == std::vector<std::string>{"neck_01", "calf_l"});

	// Editing the graph changes the model's build key, so the editor's live reimport recooks the character.
	std::ofstream(Content / "Characters/Mannequin.hanimgraph", std::ios::app) << '\n';
	const auto Edited = CookAsset(Request);
	REQUIRE(Edited);
	CHECK_FALSE(Edited->Key == Cooked->Key);

	Metadata("Mannequin.hanimgraph", "neck_01, tail_01");
	CHECK_FALSE(CookAsset(Request));
	Metadata("Mannequin.hanimgraph", "root");
	CHECK_FALSE(CookAsset(Request));
	Metadata("../Elsewhere.hanimgraph");
	CHECK_FALSE(CookAsset(Request));
	Metadata("Missing.hanimgraph");
	CHECK_FALSE(CookAsset(Request));
	Tests::WriteText(Content / "Characters/Broken.hanimgraph", "{}");
	Metadata("Broken.hanimgraph");
	CHECK_FALSE(CookAsset(Request));
}

TEST_CASE("The Mannequin slices at any angle through a pose into halves that close every cut")
{
	const FCookedModel& Model = Mannequin();
	const auto Library = FAnimationLibrary::Create(*Model.Skeleton, Model.Clips);
	REQUIRE(Library);
	FAnimationPose Pose(*Library);
	REQUIRE(Pose.Evaluate(std::array{FAnimationLayer{.Clip = *(*Library)->FindClip("Jog_Fwd_Loop"), .TimeSeconds = 0.3f}}));
	const FSkinnedMeshPiece Mesh{.Vertices = Model.Vertices, .Influences = Model.Influences, .Indices = Model.Indices, .Sections = Model.Sections};
	std::vector<FCookedVertex> Posed(Mesh.Vertices.size());
	SkinVertices(Pose.GetSkinningMatrices(), Mesh.Vertices, Mesh.Influences, Posed);

	// Open edges the cooked mesh already has, welded by position; a slice must not add any.
	const auto OpenEdges = [](const FSkinnedMeshPiece& Piece, const std::span<const FCookedVertex> Vertices)
	{
		std::map<std::array<std::int64_t, 3>, std::uint32_t> Welded;
		std::vector<std::uint32_t> Id(Vertices.size());
		for (std::size_t Vertex = 0; Vertex < Vertices.size(); ++Vertex)
		{
			const auto& P = Vertices[Vertex].Position;
			Id[Vertex] = Welded.try_emplace({std::llround(P[0] * 1.e5), std::llround(P[1] * 1.e5), std::llround(P[2] * 1.e5)}, static_cast<std::uint32_t>(Welded.size())).first->second;
		}

		std::map<std::pair<std::uint32_t, std::uint32_t>, int> Edges;
		for (std::size_t Corner = 0; Corner + 2 < Piece.Indices.size(); Corner += 3)
		{
			const std::array Triangle{Id[Piece.Indices[Corner]], Id[Piece.Indices[Corner + 1]], Id[Piece.Indices[Corner + 2]]};
			if (Triangle[0] != Triangle[1] && Triangle[1] != Triangle[2] && Triangle[0] != Triangle[2])
			{
				for (std::size_t Side = 0; Side < 3; ++Side)
				{
					++Edges[{Triangle[Side], Triangle[(Side + 1) % 3]}];
				}
			}
		}

		return static_cast<std::size_t>(std::ranges::count_if(Edges, [&Edges](const auto& Edge)
		{
			const auto Reverse = Edges.find({Edge.first.second, Edge.first.first});
			return Edge.second != 1 || Reverse == Edges.end() || Reverse->second != 1;
		}));
	};

	const std::size_t Original = OpenEdges(Mesh, Posed);
	const auto JointPosition = [&](const std::string_view Name)
	{
		return Pose.GetModelTransforms()[static_cast<std::size_t>(std::ranges::find(Model.Skeleton->JointNames, Name) - Model.Skeleton->JointNames.begin())].TransformPosition({});
	};

	const FVector3 Chest = JointPosition("spine_03");
	const FVector3 Elbow = JointPosition("lowerarm_l");
	for (const auto& [Normal, Point] : {std::pair{FVector3{0.f, 1.f, 0.f}, Chest}, std::pair{FVector3{1.f, 1.f, 0.3f}.Normalized(), Chest}, std::pair{FVector3{0.2f, 0.4f, 1.f}.Normalized(), Chest - FVector3{0.f, 0.3f, 0.f}}, std::pair{FVector3{1.f, 0.f, 0.f}, Elbow}})
	{
		const auto Start = std::chrono::steady_clock::now();
		const auto Sliced = SliceSkinnedMesh(Mesh, Posed, Pose.GetSkinningMatrices(), {.Normal = Normal, .Distance = Normal.Dot(Point)}, 0);
		const double Milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - Start).count();
		REQUIRE(Sliced);
		CHECK(Sliced->CapLoops > 0);
		std::size_t HalvesOpen = 0;
		for (const FSkinnedMeshPiece* Half : {&Sliced->Back, &Sliced->Front})
		{
			std::vector<FCookedVertex> HalfPosed(Half->Vertices.size());
			SkinVertices(Pose.GetSkinningMatrices(), Half->Vertices, Half->Influences, HalfPosed);
			HalvesOpen += OpenEdges(*Half, HalfPosed);
		}

		MESSAGE(std::format("{} vertices sliced in {:.2f} ms into {} loops; open edges {} before, {} after", Mesh.Vertices.size(), Milliseconds, Sliced->CapLoops, Original, HalvesOpen));
		CHECK(HalvesOpen <= Original);
	}

	// Severing splits where skin weights pass from a limb to its parent; those caps must close every cut too.
	const auto Parents = (*Library)->GetJointParents();
	for (const std::string_view Name : {"neck_01", "upperarm_l", "upperarm_r", "lowerarm_l", "lowerarm_r", "thigh_l", "thigh_r", "calf_l", "calf_r"})
	{
		const auto Joint = static_cast<std::size_t>(std::ranges::find(Model.Skeleton->JointNames, Name) - Model.Skeleton->JointNames.begin());
		std::vector<bool> Limb(Parents.size(), false);
		Limb[Joint] = true;
		for (std::size_t Index = Joint + 1; Index < Parents.size(); ++Index)
		{
			Limb[Index] = Parents[Index] >= 0 && Limb[static_cast<std::size_t>(Parents[Index])];
		}

		std::vector<float> Sides(Mesh.Vertices.size());
		for (std::size_t Vertex = 0; Vertex < Sides.size(); ++Vertex)
		{
			float Weight = 0.f;
			for (std::size_t Slot = 0; Slot < 4; ++Slot)
			{
				Weight += Limb[Mesh.Influences[Vertex].Joints[Slot]] ? static_cast<float>(Mesh.Influences[Vertex].Weights[Slot]) : 0.f;
			}

			Sides[Vertex] = Weight / static_cast<float>(CookedSkinWeightTotal) - 0.5f;
		}

		const FVector3 At = JointPosition(Name);
		const FVector3 Bone = (At - Pose.GetModelTransforms()[static_cast<std::size_t>(Parents[Joint])].TransformPosition({})).Normalized();
		const auto Start = std::chrono::steady_clock::now();
		const auto Split = SplitSkinnedMesh(Mesh, Posed, Pose.GetSkinningMatrices(), Sides, {.Normal = Bone, .Distance = Bone.Dot(At)}, 0);
		const double Milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - Start).count();
		REQUIRE(Split);
		std::size_t HalvesOpen = 0;
		for (const FSkinnedMeshPiece* Half : {&Split->Back, &Split->Front})
		{
			std::vector<FCookedVertex> HalfPosed(Half->Vertices.size());
			SkinVertices(Pose.GetSkinningMatrices(), Half->Vertices, Half->Influences, HalfPosed);
			HalvesOpen += OpenEdges(*Half, HalfPosed);
		}

		MESSAGE(std::format("{} severed in {:.2f} ms into {} loops; open edges {} after", Name, Milliseconds, Split->CapLoops, HalvesOpen));
		CHECK(Split->CapLoops > 0);
		CHECK(HalvesOpen <= Original);
	}
}

TEST_CASE("A skinned glTF cooks its skeleton, weights, and clips")
{
	const FCookedModel& Model = Mannequin();
	REQUIRE(Model.Skeleton);
	CHECK(Model.Skeleton->JointNames.size() == 65);
	CHECK(Model.Skeleton->JointNames.front() == "root");
	CHECK(Model.Skeleton->JointParents.front() == -1);
	CHECK(Model.Influences.size() == Model.Vertices.size());
	CHECK(Model.Clips.size() == 33);

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
	CHECK((*Library)->GetJointCount() == 65);
	CHECK((*Library)->GetClipCount() == 33);

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

TEST_CASE("Overridden joints carry their children and the joints above them, but not the root")
{
	const FCookedModel& Model = Mannequin();
	const auto Library = FAnimationLibrary::Create(*Model.Skeleton, Model.Clips);
	REQUIRE(Library);
	const auto& Names = (*Library)->GetJointNames();
	const auto Find = [&Names](const std::string_view Name)
	{
		const auto Found = std::ranges::find(Names, Name);
		REQUIRE(Found != Names.end());
		return static_cast<std::size_t>(Found - Names.begin());
	};

	FAnimationPose Pose(*Library);
	REQUIRE(Pose.Evaluate({}));
	const std::vector<FMatrix4> Before(Pose.GetModelTransforms().begin(), Pose.GetModelTransforms().end());
	// Moving only the chest, as a cut piece's ragdoll does, takes the head above it and the spine and pelvis below it the same way, so skin
	// weighted between them never stretches; the root keeps its root motion.
	const FMatrix4 Shift = FMatrix4::Translation({0.f, -1.f, 2.f});
	const std::size_t Chest = Find("spine_02");
	const std::array Joints{Chest};
	const std::array Transforms{Shift * Before[Chest]};
	Pose.OverrideJoints(Joints, Transforms, 1.f);
	for (const std::string_view Name : {"spine_02", "neck_01", "spine_01", "pelvis"})
	{
		const std::size_t Joint = Find(Name);
		CHECK_MESSAGE(MaximumDifference(Pose.GetModelTransforms()[Joint], Shift * Before[Joint]) < 1.e-4f, Name);
	}

	CHECK(MaximumDifference(Pose.GetModelTransforms().front(), Before.front()) < 1.e-6f);

	// With the chest and an arm both moved, the upper spine and the clavicle follow the chest they hang from, never the arm below them.
	REQUIRE(Pose.Evaluate({}));
	const std::size_t Arm = Find("upperarm_l");
	const std::array Both{Chest, Arm};
	const std::array BothTransforms{Shift * Before[Chest], FMatrix4::Translation({3.f, 0.f, 0.f}) * Before[Arm]};
	Pose.OverrideJoints(Both, BothTransforms, 1.f);
	for (const std::string_view Name : {"spine_03", "clavicle_l"})
	{
		const std::size_t Joint = Find(Name);
		CHECK_MESSAGE(MaximumDifference(Pose.GetModelTransforms()[Joint], Shift * Before[Joint]) < 1.e-4f, Name);
	}
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
		const auto Hand = std::ranges::find(Names, "hand_l");
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

TEST_CASE("Skinning reproduces the bind pose, moves with a clip, and collapses hidden joints")
{
	const FCookedModel& Model = Mannequin();
	const auto Library = FAnimationLibrary::Create(*Model.Skeleton, Model.Clips);
	REQUIRE(Library);
	FAnimationPose Pose(*Library);
	REQUIRE(Pose.Evaluate({}));
	std::vector<FCookedVertex> Skinned(Model.Vertices.size());
	SkinVertices(Pose.GetSkinningMatrices(), Model.Vertices, Model.Influences, Skinned);
	float Worst = 0.f;

	for (std::size_t Index = 0; Index < Skinned.size(); ++Index)
	{
		for (std::size_t Axis = 0; Axis < 3; ++Axis)
		{
			Worst = std::max(Worst, std::abs(Skinned[Index].Position[Axis] - Model.Vertices[Index].Position[Axis]));
		}
	}

	CHECK(Worst < 1.e-3f);

	const std::array Walking{FAnimationLayer{.Clip = *(*Library)->FindClip("Walk_Loop"), .TimeSeconds = 0.4f}};
	const auto Start = std::chrono::steady_clock::now();
	REQUIRE(Pose.Evaluate(Walking));
	const auto Evaluated = std::chrono::steady_clock::now();
	SkinVertices(Pose.GetSkinningMatrices(), Model.Vertices, Model.Influences, Skinned);
	const auto SkinnedAt = std::chrono::steady_clock::now();
	MESSAGE(std::format("{} vertices: evaluate {:.3f} ms, skin {:.3f} ms", Skinned.size(), std::chrono::duration<double, std::milli>(Evaluated - Start).count(), std::chrono::duration<double, std::milli>(SkinnedAt - Evaluated).count()));
	float Moved = 0.f;

	for (std::size_t Index = 0; Index < Skinned.size(); ++Index)
	{
		Moved = std::max(Moved, std::abs(Skinned[Index].Position[2] - Model.Vertices[Index].Position[2]));
	}

	CHECK(Moved > 0.1f);

	// Collapsing the head pulls every vertex it fully owns onto the head joint.
	const auto Names = (*Library)->GetJointNames();
	const auto Head = static_cast<std::size_t>(std::ranges::find(Names, "Head") - Names.begin());
	REQUIRE(Head < Names.size());
	Pose.CollapseJoint(Head);
	SkinVertices(Pose.GetSkinningMatrices(), Model.Vertices, Model.Influences, Skinned);
	const FVector3 HeadPoint = Pose.GetModelTransforms()[Head].TransformPosition({});
	std::size_t Collapsed = 0;

	for (std::size_t Index = 0; Index < Skinned.size(); ++Index)
	{
		const FCookedSkinInfluence& Influence = Model.Influences[Index];
		if (Influence.Joints[0] == Head && Influence.Weights[0] == CookedSkinWeightTotal)
		{
			CHECK((FVector3{Skinned[Index].Position[0], Skinned[Index].Position[1], Skinned[Index].Position[2]} - HeadPoint).Length() < 1.e-4f);
			++Collapsed;
		}
	}

	CHECK(Collapsed > 10);
}

TEST_CASE("Animation state machines blend along a parameter and take transitions")
{
	const FCookedModel& Model = Mannequin();
	const auto Library = FAnimationLibrary::Create(*Model.Skeleton, Model.Clips);
	REQUIRE(Library);

	FAnimationStateMachineDesc Desc{
	    .Parameters = {{.Name = "Speed", .Type = EAnimationParameterType::Float}, {.Name = "Jump", .Type = EAnimationParameterType::Trigger}, {.Name = "Grounded", .Type = EAnimationParameterType::Bool, .Default = 1.f}},
	    .States = {
	        {.Name = "Locomotion", .Samples = {{.Clip = "Walk_Loop", .Position = 2.f}, {.Clip = "Idle_Loop", .Position = 0.f}}, .BlendParameter = 0},
	        {.Name = "Jump", .Samples = {{.Clip = "Jump_Start"}}, .bLoop = false},
	        {.Name = "Air", .Samples = {{.Clip = "Jump_Loop"}}},
	    },
	    .Transitions = {
	        {.From = 1, .To = 2, .DurationSeconds = 0.1f, .bAtEnd = true},
	        {.From = 2, .To = 0, .DurationSeconds = 0.2f, .Conditions = {{.Parameter = 2, .Test = EAnimationCondition::IsTrue}}},
	        {.From = AnyAnimationState, .To = 1, .DurationSeconds = 0.25f, .Conditions = {{.Parameter = 1, .Test = EAnimationCondition::Triggered}}},
	    },
	    .Entry = 0,
	};

	auto Machine = FAnimationStateMachine::Create(Desc, **Library);
	REQUIRE_MESSAGE(Machine, (Machine ? std::string() : Machine.error().Message));
	const std::size_t Speed = *Machine->FindParameter("Speed");
	const std::size_t Jump = *Machine->FindParameter("Jump");
	const std::size_t Grounded = *Machine->FindParameter("Grounded");
	const std::size_t Idle = *(*Library)->FindClip("Idle_Loop");
	const std::size_t Walk = *(*Library)->FindClip("Walk_Loop");

	// Samples sort by position, so speed 0.5 is a quarter of the way from idle to walk.
	Machine->SetFloat(Speed, 0.5f);
	Machine->Update(0.1f);
	REQUIRE(Machine->GetLayers().size() == 2);
	CHECK(Machine->GetLayers()[0].Clip == Idle);
	CHECK(Machine->GetLayers()[0].Weight == doctest::Approx(0.75f));
	CHECK(Machine->GetLayers()[1].Clip == Walk);
	CHECK(Machine->GetLayers()[1].Weight == doctest::Approx(0.25f));

	// A trigger fades into the jump, and the jump hands over to the air loop when it ends.
	Machine->SetTrigger(Jump);
	Machine->SetBool(Grounded, false);
	Machine->Update(0.05f);
	CHECK(Machine->GetActiveState() == 1);
	CHECK(Machine->GetActiveTransition() == 2u);
	// The fade starts at zero, so only the outgoing blend contributes on the first frame.
	CHECK(Machine->GetLayers().size() == 2);
	Machine->Update(0.05f);
	CHECK(Machine->GetActiveState() == 1);
	CHECK(Machine->GetLayers().front().Weight == doctest::Approx(0.2f));

	for (int Frame = 0; Frame < 120 && Machine->GetActiveState() == 1; ++Frame)
	{
		Machine->Update(1.f / 60.f);
	}

	CHECK(Machine->GetActiveState() == 2);
	// Unconsumed triggers clear, so landing does not jump again.
	Machine->SetBool(Grounded, true);
	Machine->Update(1.f / 60.f);
	CHECK(Machine->GetActiveState() == 0);

	FAnimationPose Pose(*Library);
	CHECK(Pose.Evaluate(Machine->GetLayers()));

	// Order is priority: a trigger from any state listed first beats the active state's own transition that holds in the same update.
	FAnimationStateMachineDesc Ordered = Desc;
	std::ranges::rotate(Ordered.Transitions, Ordered.Transitions.begin() + 2);
	auto Prioritized = FAnimationStateMachine::Create(Ordered, **Library);
	REQUIRE(Prioritized);
	Prioritized->SetBool(Grounded, false);
	Prioritized->SetTrigger(Jump);
	Prioritized->Update(1.f / 60.f);
	REQUIRE(Prioritized->GetActiveState() == 1);
	for (int Frame = 0; Frame < 120 && Prioritized->GetActiveState() == 1; ++Frame)
	{
		Prioritized->Update(1.f / 60.f);
	}

	REQUIRE(Prioritized->GetActiveState() == 2);
	Prioritized->SetBool(Grounded, true);
	Prioritized->SetTrigger(Jump);
	Prioritized->Update(1.f / 60.f);
	CHECK(Prioritized->GetActiveState() == 1);

	FAnimationStateMachineDesc Broken = Desc;
	Broken.States[2].Samples[0].Clip = "Missing";
	CHECK_FALSE(FAnimationStateMachine::Create(Broken, **Library));
	Broken = Desc;
	Broken.Transitions[1].Conditions[0].Test = EAnimationCondition::Greater;
	CHECK_FALSE(FAnimationStateMachine::Create(Broken, **Library));
	Broken = Desc;
	Broken.States[0].BlendParameter.reset();
	CHECK_FALSE(FAnimationStateMachine::Create(Broken, **Library));
	Broken = Desc;
	Broken.States[0].Events = {{.Name = "Footstep", .Phase = 1.5f}};
	CHECK_FALSE(FAnimationStateMachine::Create(Broken, **Library));
}

TEST_CASE("Animation events fire once each time a state's playback reaches them")
{
	const FCookedModel& Model = Mannequin();
	const auto Library = FAnimationLibrary::Create(*Model.Skeleton, Model.Clips);
	REQUIRE(Library);
	const float Walk = (*Library)->GetClipDuration(*(*Library)->FindClip("Walk_Loop"));
	const float Jump = (*Library)->GetClipDuration(*(*Library)->FindClip("Jump_Start"));

	FAnimationStateMachineDesc Desc{
	    .Parameters = {{.Name = "Jump", .Type = EAnimationParameterType::Trigger}},
	    .States = {
	        {.Name = "Walk", .Samples = {{.Clip = "Walk_Loop"}}, .Events = {{.Name = "Left", .Phase = 0.f}, {.Name = "Right", .Phase = 0.5f}}},
	        {.Name = "Jump", .Samples = {{.Clip = "Jump_Start"}}, .bLoop = false, .Events = {{.Name = "TakeOff", .Phase = 0.f}, {.Name = "Apex", .Phase = 1.f}}},
	    },
	    .Transitions = {{.From = AnyAnimationState, .To = 1, .DurationSeconds = 0.f, .Conditions = {{.Parameter = 0, .Test = EAnimationCondition::Triggered}}}},
	};

	auto Machine = FAnimationStateMachine::Create(Desc, **Library);
	REQUIRE(Machine);
	const auto Fired = [&](const float Seconds)
	{
		Machine->Update(Seconds);
		return std::vector<std::string>(Machine->GetEvents().begin(), Machine->GetEvents().end());
	};

	// The start fires on the first update, the halfway point when the playback crosses it, and the start again once the loop wraps.
	CHECK(Fired(Walk * 0.1f) == std::vector<std::string>{"Left"});
	CHECK(Fired(Walk * 0.3f).empty());
	CHECK(Fired(Walk * 0.2f) == std::vector<std::string>{"Right"});
	CHECK(Fired(Walk * 0.45f) == std::vector<std::string>{"Left"});
	// A step longer than a whole cycle fires each event once.
	CHECK(Fired(Walk * 2.5f) == std::vector<std::string>{"Left", "Right"});

	// A one-shot fires its start after the transition and its end once, then holds without firing again.
	Machine->SetTrigger(0);
	CHECK(Fired(0.01f).empty());
	CHECK(Fired(Jump * 0.5f) == std::vector<std::string>{"TakeOff"});
	CHECK(Fired(Jump) == std::vector<std::string>{"Apex"});
	CHECK(Fired(Jump).empty());
}

namespace
{
class FNoAnimationGameAssets final : public IGameAssets
{
public:
	[[nodiscard]] const FRenderMesh* FindMesh(const FAssetId&) const override
	{
		return nullptr;
	}

	[[nodiscard]] const FRenderMaterial* FindMaterial(const FAssetId&) const override
	{
		return nullptr;
	}

	[[nodiscard]] std::shared_ptr<const FCookedTexture> FindEnvironment(const FAssetId&) const override
	{
		return nullptr;
	}
};
}

namespace
{
// Serves the Mannequin as the skinned model behind one asset, so Character entities animate headless.
class FDummyGameAssets final : public IGameAssets
{
public:
	[[nodiscard]] const FRenderMesh* FindMesh(const FAssetId&) const override
	{
		return nullptr;
	}

	[[nodiscard]] const FRenderMaterial* FindMaterial(const FAssetId&) const override
	{
		return nullptr;
	}

	[[nodiscard]] std::shared_ptr<const FCookedTexture> FindEnvironment(const FAssetId&) const override
	{
		return nullptr;
	}

	[[nodiscard]] std::shared_ptr<const FCookedModel> FindSkinnedModel(const FAssetId& Asset) const override
	{
		return Asset == FAssetId{7, 7} ? Model : nullptr;
	}

	std::shared_ptr<const FCookedModel> Model = std::make_shared<const FCookedModel>(Mannequin());
};
}

TEST_CASE("Character entities stand animated, and slices, severs, and thrown balls cut them until a restart restores them")
{
	const FDummyGameAssets Assets;
	const std::vector<FLevelEntity> Level{
	    FLevelEntity{.Id = FObjectId::Generate(), .Name = "Floor", .Transform = {.Translation = {0., -0.5, 0.}, .Scale = {60.f, 1.f, 60.f}}, .Mesh = FStaticMeshComponent{.Asset = FAssetId{1, 1}}, .BodyType = ELevelBodyType::Static},
	    FLevelEntity{.Id = FObjectId::Generate(), .Name = "Dummy", .Transform = {.Translation = {0., 0., 3.}}, .Mesh = FStaticMeshComponent{.Asset = FAssetId{7, 7}}, .Character = FCharacterComponent{}},
	};
	auto Session = FGameSession::Create(Level, Assets, nullptr, {}, nullptr, nullptr);
	REQUIRE_MESSAGE(Session, (Session ? std::string() : Session.error().Message));
	REQUIRE((*Session)->GetCharacterCount() == 1);
	const auto Step = [&](const FGameInput& Input, const int Frames)
	{
		for (int Frame = 0; Frame < Frames; ++Frame)
		{
			REQUIRE((*Session)->Update(1.f / 60.f, Frame == 0 ? Input : FGameInput{}));
			(void)(*Session)->GetRenderView({320, 180});
		}
	};

	Step({}, 30);
	// Severing the left arm at the shoulder and the left leg at the knee: each limb falls on its own, and a limb already gone cannot be severed
	// again. The arm carries the upper and lower arm, so it falls as a two-part ragdoll; the shin falls as a hull. Without the leg the dummy
	// falls as its own ragdoll but lives on the ground, where its other leg can still be cut off.
	auto Severed = (*Session)->SeverCharacter(0, "upperarm_l");
	REQUIRE_MESSAGE(Severed, (Severed ? std::string() : Severed.error().Message));
	CHECK(*Severed);
	Step({}, 5);
	CHECK_FALSE((*Session)->IsCharacterDowned(0));
	Severed = (*Session)->SeverCharacter(0, "calf_l");
	REQUIRE(Severed);
	CHECK(*Severed);
	CHECK((*Session)->GetCutPieceCount() == 2);
	CHECK((*Session)->IsCharacterDowned(0));
	Step({}, 5);
	Severed = (*Session)->SeverCharacter(0, "calf_l");
	REQUIRE(Severed);
	CHECK_FALSE(*Severed);
	Step({}, 60);
	Severed = (*Session)->SeverCharacter(0, "calf_r");
	REQUIRE_MESSAGE(Severed, (Severed ? std::string() : Severed.error().Message));
	CHECK(*Severed);
	CHECK((*Session)->GetCutPieceCount() == 3);
	Step({}, 5);

	// Every severable joint cuts off its limb on a whole body.
	for (const std::string_view Joint : {"neck_01", "upperarm_l", "upperarm_r", "lowerarm_l", "lowerarm_r", "thigh_l", "thigh_r", "calf_l", "calf_r"})
	{
		REQUIRE((*Session)->Restart());
		Step({}, 5);
		const auto Each = (*Session)->SeverCharacter(0, Joint);
		REQUIRE(Each);
		CHECK_MESSAGE(*Each, Joint);
	}

	REQUIRE((*Session)->Restart());
	Step({}, 5);
	// A slant through the upper arm leaves the forearm and hand on the body's side of the plane but joined to it by nothing, so they fall as
	// a piece of their own instead of floating where the animation holds them.
	const auto Strand = (*Session)->SliceCharacter(0, {-0.2372f, 1.1749f, 2.9353f}, FVector3{0.5162f, -0.6098f, 0.6014f}.Normalized());
	REQUIRE_MESSAGE(Strand, (Strand ? std::string() : Strand.error().Message));
	CHECK(*Strand);
	CHECK((*Session)->GetCutPieceCount() == 2);
	CHECK_FALSE((*Session)->IsCharacterDowned(0));

	// A cut through the bent arm that takes the forearm leaves the cut piece holding the upper arm and the hand with nothing between them, so
	// the piece falls apart into each.
	REQUIRE((*Session)->Restart());
	Step({}, 5);
	const auto Apart = (*Session)->SliceCharacter(0, {-0.2554f, 1.3501f, 3.0006f}, FVector3{0.2292f, 0.1551f, 0.9609f}.Normalized());
	REQUIRE_MESSAGE(Apart, (Apart ? std::string() : Apart.error().Message));
	CHECK(*Apart);
	CHECK((*Session)->GetCutPieceCount() >= 3);
	CHECK_FALSE((*Session)->IsCharacterDowned(0));

	REQUIRE((*Session)->Restart());
	Step({}, 5);
	// A cut through both shins leaves the calves some skin but takes the feet, so the dummy goes down rather than standing on its stumps.
	const auto Shins = (*Session)->SliceCharacter(0, {0.f, 0.25f, 3.f}, FVector3{0.f, -1.f, 0.f});
	REQUIRE_MESSAGE(Shins, (Shins ? std::string() : Shins.error().Message));
	CHECK(*Shins);
	CHECK((*Session)->IsCharacterDowned(0));

	REQUIRE((*Session)->Restart());
	Step({}, 5);
	// A cut 3 cm above the soles would only peel them off, so it does not cut.
	const auto Skim = (*Session)->SliceCharacter(0, {0.f, 0.03f, 3.f}, FVector3{0.f, 1.f, 0.f});
	REQUIRE(Skim);
	CHECK_FALSE(*Skim);
	REQUIRE((*Session)->SeverCharacter(0, "upperarm_l"));
	REQUIRE((*Session)->SeverCharacter(0, "lowerarm_r"));
	// A cut through the waist takes the chest, which kills the dummy: the cut-off half falls, and what is left collapses as a ragdoll, so nothing
	// stands to cut again.
	auto Cut = (*Session)->SliceCharacter(0, {0.f, 0.95f, 3.f}, FVector3{0.f, 1.f, 0.f});
	REQUIRE_MESSAGE(Cut, (Cut ? std::string() : Cut.error().Message));
	CHECK(*Cut);
	CHECK((*Session)->GetCutPieceCount() == 4);
	Cut = (*Session)->SliceCharacter(0, {0.f, 0.5f, 3.f}, FVector3{0.f, 1.f, 0.f});
	REQUIRE(Cut);
	CHECK_FALSE(*Cut);
	// Pieces cut again: an upright cut down the middle splits both halves of the body, ragdolls as they are, into two pieces each.
	const auto Split = (*Session)->SliceWorld({0.f, 0.f, 3.f}, {1.f, 0.f, 0.f});
	REQUIRE_MESSAGE(Split, (Split ? std::string() : Split.error().Message));
	CHECK(*Split == 2);
	CHECK((*Session)->GetCutPieceCount() == 6);
	Step({}, 120);
	CHECK((*Session)->GetCutPieceCount() == 6);

	// Each restart restores the whole body, which then cuts again at another angle: flat through the thighs, steeply through the chest, and
	// upright through a shoulder. Only the shoulder cut leaves the dummy alive.
	for (const auto& [Point, Normal, Pieces] : {std::tuple{FVector3{0.f, 0.7f, 3.f}, FVector3{0.f, 1.f, 0.f}, std::size_t{1}}, std::tuple{FVector3{0.f, 1.3f, 3.f}, FVector3{1.f, 0.6f, 0.2f}, std::size_t{2}}, std::tuple{FVector3{0.15f, 1.3f, 3.f}, FVector3{1.f, 0.f, 0.f}, std::size_t{1}}})
	{
		REQUIRE((*Session)->Restart());
		Step({}, 10);
		const auto Angled = (*Session)->SliceCharacter(0, Point, Normal.Normalized());
		REQUIRE_MESSAGE(Angled, (Angled ? std::string() : Angled.error().Message));
		CHECK(*Angled);
		MESSAGE(std::format("{} pieces", (*Session)->GetCutPieceCount()));
		CHECK((*Session)->GetCutPieceCount() >= Pieces);
	}

	// Restarting restores the whole body, and a ball thrown at it from the start severs whatever limb it strikes.
	REQUIRE((*Session)->Restart());
	CHECK((*Session)->GetCutPieceCount() == 0);
	const std::size_t Before = (*Session)->GetCutCount();
	Step({}, 30);
	Step({.bThrow = true}, 60);
	CHECK((*Session)->GetCutCount() == Before + 1);
	CHECK((*Session)->GetCutPieceCount() == 1);
}

TEST_CASE("The player's sword swings slice a training dummy along the blade's plane")
{
	const FDummyGameAssets Assets;
	const std::vector<FLevelEntity> Level{
	    FLevelEntity{.Id = FObjectId::Generate(), .Name = "Floor", .Transform = {.Translation = {0., -0.5, 0.}, .Scale = {60.f, 1.f, 60.f}}, .Mesh = FStaticMeshComponent{.Asset = FAssetId{1, 1}}, .BodyType = ELevelBodyType::Static},
	    FLevelEntity{.Id = FObjectId::Generate(), .Name = "Dummy", .Transform = {.Translation = {0., 0., 1.6}, .Rotation = FQuaternion::FromAxisAngle({0.f, 1.f, 0.f}, std::numbers::pi_v<float>)}, .Mesh = FStaticMeshComponent{.Asset = FAssetId{7, 7}}, .Character = FCharacterComponent{}},
	};
	auto Session = FGameSession::Create(Level, Assets, nullptr, {}, nullptr, Assets.Model);
	REQUIRE_MESSAGE(Session, (Session ? std::string() : Session.error().Message));
	std::vector<std::string> States;
	const auto Step = [&](const FGameInput& Input, const int Frames)
	{
		for (int Frame = 0; Frame < Frames; ++Frame)
		{
			REQUIRE((*Session)->Update(1.f / 60.f, Frame == 0 ? Input : FGameInput{}));
			(void)(*Session)->GetRenderView({320, 180});
			if (States.empty() || States.back() != (*Session)->GetCharacterStateName())
			{
				States.emplace_back((*Session)->GetCharacterStateName());
			}
		}
	};

	Step({}, 90);
	// The first swing starts the combo; pressing again during its recovery chains the second, from the other side.
	Step({.bAttack = true}, 30);
	CHECK(std::ranges::contains(States, std::string("Attack A")));
	const std::size_t AfterFirst = (*Session)->GetCutCount();
	Step({.bAttack = true}, 60);
	CHECK(std::ranges::contains(States, std::string("Attack B")));
	std::string Visited;
	for (const std::string& State : States)
	{
		Visited += State + "; ";
	}

	MESSAGE(std::format("states {}cuts after the first swing {}, after the second {}", Visited, AfterFirst, (*Session)->GetCutCount()));
	CHECK((*Session)->GetCutCount() >= 1);
	CHECK((*Session)->GetCutPieceCount() >= 1);

	// Attacking at a sprint lunges into the dash slash instead.
	REQUIRE((*Session)->Restart());
	States.clear();
	Step({}, 60);
	// Away from the dummy, which stands in front of the start.
	Step({.YawDelta = std::numbers::pi_v<float>}, 1);
	for (int Frame = 0; Frame < 60; ++Frame)
	{
		REQUIRE((*Session)->Update(1.f / 60.f, {.Move = {0.f, 1.f}, .bSprint = true, .bAttack = Frame == 59}));
	}

	for (int Frame = 0; Frame < 10; ++Frame)
	{
		REQUIRE((*Session)->Update(1.f / 60.f, {.Move = {0.f, 1.f}, .bSprint = true}));
		States.emplace_back((*Session)->GetCharacterStateName());
	}

	CHECK(std::ranges::contains(States, std::string("Dash attack")));
}

TEST_CASE("A jump pressed again against a low obstacle vaults it instead of kicking off it")
{
	// The course's low hurdle: by the second press the feet are already within the 0.5 m a mantle needs, and the player touches its face,
	// which used to make the press a wall kick back the way they came.
	const FNoAnimationGameAssets Assets;
	const std::vector<FLevelEntity> Course{
	    FLevelEntity{.Id = FObjectId::Generate(), .Name = "Floor", .Transform = {.Translation = {0., -0.5, 0.}, .Scale = {60.f, 1.f, 60.f}}, .Mesh = FStaticMeshComponent{.Asset = FAssetId{1, 1}}, .BodyType = ELevelBodyType::Static},
	    FLevelEntity{.Id = FObjectId::Generate(), .Name = "Hurdle", .Transform = {.Translation = {0., 0.3, 5.}, .Scale = {4.f, 0.6f, 0.45f}}, .Mesh = FStaticMeshComponent{.Asset = FAssetId{1, 1}}, .BodyType = ELevelBodyType::Static},
	};
	auto Session = FGameSession::Create(Course, Assets, nullptr, {}, nullptr, std::make_shared<const FCookedModel>(Mannequin()));
	REQUIRE_MESSAGE(Session, (Session ? std::string() : Session.error().Message));
	for (int Frame = 0; Frame < 60; ++Frame)
	{
		REQUIRE((*Session)->Update(1.f / 60.f, {}));
	}

	std::optional<int> Jumped;
	for (int Frame = 0; Frame < 150; ++Frame)
	{
		const bool bAgainst = 4.775f - (*Session)->GetPlayerFootPosition().Z < 0.4f;
		const bool bPress = (!Jumped && bAgainst) || (Jumped && Frame == *Jumped + 4);
		Jumped = !Jumped && bPress ? std::optional(Frame) : Jumped;
		REQUIRE((*Session)->Update(1.f / 60.f, {.Move = {0.f, 1.f}, .bJump = bPress, .bSprint = true}));
	}

	REQUIRE(Jumped);
	CHECK((*Session)->GetPlayerFootPosition().Z > 6.f);
}

TEST_CASE("A game session drives its character's locomotion, jumps, and camera")
{
	const FNoAnimationGameAssets Assets;
	const std::vector<FLevelEntity> Floor{FLevelEntity{.Id = FObjectId::Generate(), .Name = "Floor", .Transform = {.Translation = {0., -0.5, 0.}, .Scale = {60.f, 1.f, 60.f}}, .Mesh = FStaticMeshComponent{.Asset = FAssetId{1, 1}}, .BodyType = ELevelBodyType::Static}};
	auto Session = FGameSession::Create(Floor, Assets, nullptr, {}, nullptr, std::make_shared<const FCookedModel>(Mannequin()));
	REQUIRE_MESSAGE(Session, (Session ? std::string() : Session.error().Message));
	REQUIRE((*Session)->GetCharacterState());
	const auto Step = [&](const FGameInput& Input, const int Frames)
	{
		for (int Frame = 0; Frame < Frames; ++Frame)
		{
			REQUIRE((*Session)->Update(1.f / 60.f, Input));
		}
	};

	// Settling and walking stay in the locomotion blend; a jump leaves it and landing returns to it.
	// The player spawns just above the floor, so it lands first.
	const auto State = [&Session]
	{
		return std::string((*Session)->GetCharacterStateName());
	};

	Step({}, 90);
	CHECK(State() == "Locomotion");
	Step({.Move = {0.f, 1.f}}, 30);
	CHECK(State() == "Locomotion");
	Step({.Move = {0.f, 1.f}, .bJump = true}, 1);
	Step({.Move = {0.f, 1.f}}, 5);
	CHECK(State() == "Jump");
	Step({.Move = {0.f, 1.f}, .bSprint = true}, 120);
	CHECK(State() == "Locomotion");

	// Crouching at walking speed slides: the start plays, then the loop, then the exit once it slows down.
	Step({.Move = {0.f, 1.f}, .bSprint = true, .bCrouch = true}, 2);
	CHECK(State() == "Slide start");
	CHECK((*Session)->IsPlayerSliding());
	Step({.bCrouch = true}, 40);
	CHECK(State() == "Slide");
	for (int Frame = 0; Frame < 300 && (*Session)->IsPlayerSliding(); ++Frame)
	{
		Step({.bCrouch = true}, 1);
	}

	Step({.bCrouch = true}, 2);
	CHECK(State() == "Slide exit");
	Step({.bCrouch = true}, 60);
	CHECK(State() == "Crouch");
	Step({}, 30);
	CHECK(State() == "Locomotion");

	// Attacking in the air swings overhead, plays out past the landing, and settles back into locomotion.
	Step({.bJump = true}, 1);
	Step({}, 8);
	REQUIRE_FALSE((*Session)->IsPlayerGrounded());
	Step({.bAttack = true}, 1);
	Step({}, 4);
	CHECK(State() == "Air attack");
	Step({}, 90);
	CHECK((*Session)->IsPlayerGrounded());
	CHECK(State() == "Locomotion");

	// Play starts in third person, whose camera sits behind and above the first-person eye.
	Step({}, 60);
	CHECK((*Session)->IsThirdPerson());
	const FMatrix4 ThirdPerson = (*Session)->GetRenderView({1280, 720}).View;
	Step({.bToggleCamera = true}, 1);
	CHECK_FALSE((*Session)->IsThirdPerson());
	const FMatrix4 FirstPerson = (*Session)->GetRenderView({1280, 720}).View;
	const auto EyeOf = [](const FMatrix4& View)
	{
		// The view is a rotation followed by a translation, so the eye is minus the transposed rotation applied to its translation.
		const FVector3 Translation{View(0, 3), View(1, 3), View(2, 3)};
		return -FVector3{View(0, 0) * Translation.X + View(1, 0) * Translation.Y + View(2, 0) * Translation.Z, View(0, 1) * Translation.X + View(1, 1) * Translation.Y + View(2, 1) * Translation.Z, View(0, 2) * Translation.X + View(1, 2) * Translation.Y + View(2, 2) * Translation.Z};
	};

	CHECK((EyeOf(ThirdPerson) - EyeOf(FirstPerson)).Length() > 2.f);
	REQUIRE((*Session)->Restart());
	CHECK(State() == "Locomotion");

	// A mantle plays the climb while its root rise is drawn away, so the body never floats above the capsule; a wall jump plays its own clip.
	std::vector<FLevelEntity> Course = Floor;
	Course.push_back(FLevelEntity{.Id = FObjectId::Generate(), .Name = "Ledge", .Transform = {.Translation = {0., 0.85, 4.}, .Scale = {4.f, 1.7f, 3.f}}, .Mesh = FStaticMeshComponent{.Asset = FAssetId{1, 1}}, .BodyType = ELevelBodyType::Static});
	Course.push_back(FLevelEntity{.Id = FObjectId::Generate(), .Name = "Wall", .Transform = {.Translation = {-1.2, 3., 0.}, .Scale = {0.5f, 6.f, 6.f}}, .Mesh = FStaticMeshComponent{.Asset = FAssetId{1, 1}}, .BodyType = ELevelBodyType::Static});
	Session = FGameSession::Create(Course, Assets, nullptr, {}, nullptr, std::make_shared<const FCookedModel>(Mannequin()));
	REQUIRE(Session);
	// The eye check below reads the first-person camera.
	Step({.bToggleCamera = true}, 1);
	Step({}, 89);
	Step({.Move = {0.f, 1.f}}, 20);
	Step({.Move = {0.f, 1.f}, .bJump = true}, 1);
	bool bClimbed = false;
	for (int Frame = 0; Frame < 60 && !bClimbed; ++Frame)
	{
		Step({.Move = {0.f, 1.f}}, 1);
		bClimbed = (*Session)->IsPlayerMantling() && State() == "Mantle";
	}

	REQUIRE(bClimbed);
	// Midway the clip's root has risen about 0.6 m; without cancelling it the first-person eye would ride that far above the head.
	Step({}, 25);
	const float EyeAboveFoot = EyeOf((*Session)->GetRenderView({1280, 720}).View).Y - (*Session)->GetPlayerFootPosition().Y;
	CHECK(EyeAboveFoot < 1.9f);
	CHECK(EyeAboveFoot > 0.8f);
	// The climb ends standing on the top: it never falls for a step, which would play a fall and a landing.
	bool bFell = false;
	for (int Frame = 0; Frame < 60; ++Frame)
	{
		Step({.Move = {0.f, 0.2f}}, 1);
		bFell = bFell || (*Session)->GetMover().GetActiveMode() == "Falling";
	}

	CHECK_FALSE(bFell);
	CHECK_FALSE((*Session)->IsPlayerMantling());
	CHECK((*Session)->GetPlayerFootPosition().Y == doctest::Approx(1.7f).epsilon(0.03));
	CHECK(State() == "Locomotion");

	REQUIRE((*Session)->Restart());
	Step({}, 90);
	Step({.Move = {1.f, 0.f}, .bJump = true}, 1);
	bool bWallJumped = false;
	for (int Frame = 0; Frame < 90 && !bWallJumped; ++Frame)
	{
		Step({.Move = {1.f, 0.f}, .bJump = !(*Session)->IsPlayerGrounded()}, 1);
		bWallJumped = State() == "Wall jump";
	}

	CHECK(bWallJumped);

	// Running at a long wall and jumping toward it runs on it with the sprint cycle.
	std::vector<FLevelEntity> Lane = Floor;
	Lane.push_back(FLevelEntity{.Id = FObjectId::Generate(), .Name = "Long wall", .Transform = {.Translation = {-2.7, 2.5, 12.}, .Scale = {0.4f, 5.f, 20.f}}, .Mesh = FStaticMeshComponent{.Asset = FAssetId{1, 1}}, .BodyType = ELevelBodyType::Static});
	Session = FGameSession::Create(Lane, Assets, nullptr, {}, nullptr, std::make_shared<const FCookedModel>(Mannequin()));
	REQUIRE(Session);
	Step({}, 90);
	Step({.YawDelta = -0.5236f}, 1);
	Step({.Move = {0.f, 1.f}}, 50);
	Step({.Move = {0.f, 1.f}, .bJump = true}, 1);
	bool bWallRan = false;
	for (int Frame = 0; Frame < 60 && !bWallRan; ++Frame)
	{
		Step({.Move = {0.f, 1.f}}, 1);
		bWallRan = (*Session)->IsPlayerWallRunning() && State() == "Wall run";
	}

	REQUIRE(bWallRan);
	// The wall is on the right, at -X, so the view's up axis tilts toward +X, away from it, as the body's lean does.
	Step({.Move = {0.f, 1.f}}, 12);
	REQUIRE((*Session)->IsPlayerWallRunning());
	CHECK((*Session)->GetRenderView({1280, 720}).View(1, 0) > 0.05f);

	// A model that is not skinned leaves the capsule playable with a warning.
	FCookedModel Static = Mannequin();
	Static.Skeleton.reset();
	Static.Influences.clear();
	Static.Clips.clear();
	auto Capsule = FGameSession::Create(Floor, Assets, nullptr, {}, nullptr, std::make_shared<const FCookedModel>(Static));
	REQUIRE(Capsule);
	CHECK_FALSE((*Capsule)->GetCharacterState());
	CHECK_FALSE((*Capsule)->GetWarnings().empty());
}

TEST_CASE("Z collapses the player into a ragdoll that lies on the floor, and pressing it again gets back up where it lies")
{
	const FNoAnimationGameAssets Assets;
	const std::vector<FLevelEntity> Floor{FLevelEntity{.Id = FObjectId::Generate(), .Name = "Floor", .Transform = {.Translation = {0., -0.5, 0.}, .Scale = {60.f, 1.f, 60.f}}, .Mesh = FStaticMeshComponent{.Asset = FAssetId{1, 1}}, .BodyType = ELevelBodyType::Static}};
	auto Session = FGameSession::Create(Floor, Assets, nullptr, {}, nullptr, std::make_shared<const FCookedModel>(Mannequin()));
	REQUIRE(Session);
	std::size_t Footsteps = 0;
	const auto Step = [&](const FGameInput& Input, const int Frames)
	{
		for (int Frame = 0; Frame < Frames; ++Frame)
		{
			REQUIRE((*Session)->Update(1.f / 60.f, Frame == 0 ? Input : FGameInput{.Move = Input.Move, .bSprint = Input.bSprint}));
			Footsteps += static_cast<std::size_t>(std::ranges::count((*Session)->GetCharacterEvents(), std::string("Footstep")));
			(void)(*Session)->GetRenderView({320, 180});
		}
	};

	Step({}, 30);
	Step({.Move = {0.f, 1.f}, .bSprint = true}, 60);
	const FVector3 Collapsed = (*Session)->GetPlayerFootPosition();
	Step({.Move = {0.f, 1.f}, .bToggleRagdoll = true}, 1);
	REQUIRE((*Session)->IsPlayerRagdolled());

	// The body tumbles on with the run's speed, and input no longer moves the player or steps its feet.
	Footsteps = 0;
	Step({.Move = {0.f, 1.f}}, 180);
	CHECK((*Session)->IsPlayerRagdolled());
	CHECK(Footsteps == 0);
	CHECK((*Session)->GetPlayerFootPosition() == Collapsed);

	Step({.bToggleRagdoll = true}, 1);
	CHECK_FALSE((*Session)->IsPlayerRagdolled());
	const FVector3 Standing = (*Session)->GetPlayerFootPosition();
	CHECK(Standing.Z > Collapsed.Z + 0.5f);
	CHECK(std::abs(Standing.Y) < 0.2f);
	Step({.Move = {0.f, 1.f}}, 60);
	CHECK((*Session)->GetPlayerFootPosition().Z > Standing.Z + 1.f);
	CHECK((*Session)->IsPlayerGrounded());
}

TEST_CASE("A paused animation update fires no events and keeps the state's start for the next update")
{
	const FCookedModel& Model = Mannequin();
	const auto Library = FAnimationLibrary::Create(*Model.Skeleton, Model.Clips);
	REQUIRE(Library);
	auto Machine = FAnimationStateMachine::Create({.States = {{.Name = "Land", .Samples = {{.Clip = "Jump_Land"}}, .bLoop = false, .Events = {{.Name = "Land", .Phase = 0.f}}}}}, **Library);
	REQUIRE(Machine);
	Machine->Update(0.f);
	CHECK(Machine->GetEvents().empty());
	Machine->Update(1.f / 60.f);
	REQUIRE(Machine->GetEvents().size() == 1);
	CHECK(Machine->GetEvents().front() == "Land");
}

namespace
{
[[nodiscard]] std::shared_ptr<const FCookedStateTree> CompileSandboxGuard()
{
	for (std::filesystem::path Path = std::filesystem::current_path(); !Path.empty(); Path = Path.parent_path())
	{
		const auto Candidate = Path / "Games/Sandbox/Content/AI/Guard.hstatetree";
		if (std::filesystem::exists(Candidate))
		{
			std::ifstream File(Candidate, std::ios::binary);
			const std::string Text{std::istreambuf_iterator<char>(File), std::istreambuf_iterator<char>()};
			const auto Graph = DeserializeGraph(Text, GetStateTreeSchema());
			REQUIRE(Graph);
			const auto Compiled = CompileStateTree(*Graph);
			REQUIRE(Compiled);
			return std::make_shared<const FCookedStateTree>(Compiled->Tree);
		}

		if (Path == Path.parent_path())
		{
			break;
		}
	}

	FAIL("The Sandbox guard tree is missing");
	return nullptr;
}
}

TEST_CASE("Characters of opposing attitudes fight each other, a neutral one only when struck, and guards surround the player taking turns")
{
	const FDummyGameAssets Assets;
	constexpr FAssetId GuardTree{9, 9};
	FLevelDocument Level{.Id = FObjectId{1, 1}, .Name = "Arena"};
	const auto Agent = [&](const std::uint64_t Id, const std::string& Name, const FVector3& Foot, const float Yaw, const EAiAttitude Attitude)
	{
		return FLevelEntity{
		    .Id = FObjectId{1, Id},
		    .Name = Name,
		    .Transform = {.Translation = FWorldPosition{{Foot.X, Foot.Y, Foot.Z}}, .Rotation = FQuaternion::FromAxisAngle(FVector3::Up(), Yaw)},
		    .Mesh = FStaticMeshComponent{.Asset = FAssetId{7, 7}},
		    .Character = FCharacterComponent{},
		    .AiAgent = FAiAgentComponent{.StateTree = GuardTree, .Attitude = Attitude},
		};
	};

	Level.Entities = {
	    FLevelEntity{.Id = FObjectId{1, 2}, .Name = "Floor", .Transform = {.Translation = FWorldPosition{{0., -0.5, 0.}}, .Scale = {60.f, 1.f, 60.f}}, .Mesh = FStaticMeshComponent{.Asset = FAssetId{1, 1}}, .BodyType = ELevelBodyType::Static},
	    FLevelEntity{.Id = FObjectId{1, 3}, .Name = "Bounds", .NavigationBounds = FNavigationBoundsComponent{.Size = {60.f, 6.f, 60.f}}},
	    // A guard and an ally face each other 5 m apart; a villager watches from the side.
	    Agent(4, "Guard", {0.f, 0.f, 8.f}, std::numbers::pi_v<float>, EAiAttitude::Hostile),
	    Agent(5, "Ally", {0.f, 0.f, 3.f}, 0.f, EAiAttitude::Friendly),
	    Agent(6, "Villager", {-5.f, 0.f, 5.f}, std::numbers::pi_v<float> * 0.5f, EAiAttitude::Neutral),
	    // A squad waits far off for the player.
	    Agent(7, "Squad 1", {20.f, 0.f, -18.f}, 0.f, EAiAttitude::Hostile),
	    Agent(8, "Squad 2", {22.f, 0.f, -18.f}, 0.f, EAiAttitude::Hostile),
	    Agent(9, "Squad 3", {24.f, 0.f, -18.f}, 0.f, EAiAttitude::Hostile),
	    Agent(10, "Squad 4", {26.f, 0.f, -18.f}, 0.f, EAiAttitude::Hostile),
	};

	const auto Navigation = BuildLevelNavigation(Level, {}, [](const FAssetId&)
	{
		return std::optional<FNavigationBox>{};
	});

	REQUIRE(Navigation);
	REQUIRE(*Navigation);
	auto Session = FGameSession::Create(Level.Entities, Assets, nullptr, {}, nullptr, Assets.Model);
	REQUIRE_MESSAGE(Session, (Session ? std::string() : Session.error().Message));
	REQUIRE((*Session)->SetNavigation(std::make_shared<const FNavigationFile>(std::move(**Navigation))));
	REQUIRE((*Session)->AddStateTree(GuardTree, CompileSandboxGuard()));
	// The player stands out of everyone's way.
	REQUIRE((*Session)->TeleportPlayer({-25.f, 0.f, 25.f}, 0.f));
	std::size_t MostTurns = 0;
	const auto Step = [&](const int Frames)
	{
		for (int Frame = 0; Frame < Frames; ++Frame)
		{
			REQUIRE((*Session)->Update(1.f / 60.f, {}));
			(void)(*Session)->GetRenderView({320, 180});
			const std::vector<FGameAgentState> States = (*Session)->GetAgentStates();
			MostTurns = std::max(MostTurns, static_cast<std::size_t>(std::ranges::count_if(States.begin() + 3, States.end(), &FGameAgentState::bHasAttackTurn)));
		}
	};

	const auto States = [&]
	{
		return (*Session)->GetAgentStates();
	};

	// The guard and the ally go for each other, not for the villager, who stays out of it.
	Step(60);
	REQUIRE(States().size() == 7);
	CHECK(States()[0].EnemyCharacter == std::optional<std::size_t>{1});
	CHECK(States()[1].EnemyCharacter == std::optional<std::size_t>{0});
	CHECK_FALSE(States()[2].EnemyCharacter);
	bool bWounded = false;
	for (int Second = 0; Second < 20 && !bWounded; ++Second)
	{
		Step(60);
		bWounded = States()[0].Health < 1.f || States()[1].Health < 1.f;
	}

	CHECK(bWounded);
	CHECK(States()[2].Health == 1.f);
	CHECK_FALSE(States()[2].EnemyCharacter);

	// Struck by the player, the villager turns on it.
	REQUIRE((*Session)->SeverCharacter(2, "lowerarm_l"));
	Step(10);
	CHECK(States()[2].bRemembersPlayer);

	// The player walks up to the squad: they surround it, at most two attacking at once.
	REQUIRE((*Session)->TeleportPlayer({23.f, 0.f, -12.f}, std::numbers::pi_v<float>));
	bool bSurrounded = false;
	for (int Second = 0; Second < 10 && !bSurrounded; ++Second)
	{
		Step(60);
		const std::vector<FGameAgentState> Now = States();
		bSurrounded = std::count_if(Now.begin() + 3, Now.end(), [](const FGameAgentState& Squad)
		{
			return Squad.State == "Surround";
		}) >= 2;
	}

	CHECK(bSurrounded);
	CHECK(MostTurns <= 2);
}

TEST_CASE("AI agents walk the navigation, chase and knock down the player, flee when wounded, and start over on restart")
{
	const FDummyGameAssets Assets;
	constexpr FAssetId GuardTree{9, 9};
	FLevelDocument Level{.Id = FObjectId{1, 1}, .Name = "Arena"};
	Level.Entities = {
	    FLevelEntity{.Id = FObjectId{1, 2}, .Name = "Floor", .Transform = {.Translation = FWorldPosition{{0., -0.5, 0.}}, .Scale = {40.f, 1.f, 40.f}}, .Mesh = FStaticMeshComponent{.Asset = FAssetId{1, 1}}, .BodyType = ELevelBodyType::Static},
	    FLevelEntity{.Id = FObjectId{1, 3}, .Name = "Bounds", .NavigationBounds = FNavigationBoundsComponent{.Size = {40.f, 6.f, 40.f}}},
	    FLevelEntity{.Id = FObjectId{1, 4}, .Name = "Guard", .Transform = {.Translation = FWorldPosition{{0., 0., 8.}}}, .Mesh = FStaticMeshComponent{.Asset = FAssetId{7, 7}}, .Character = FCharacterComponent{}, .AiAgent = FAiAgentComponent{.StateTree = GuardTree, .SightRange = 20.f}},
	};

	const auto Navigation = BuildLevelNavigation(Level, {}, [](const FAssetId&)
	{
		return std::optional<FNavigationBox>{};
	});

	REQUIRE(Navigation);
	REQUIRE(*Navigation);
	// The player plays as the Mannequin too, so a strike can knock its body down.
	auto Session = FGameSession::Create(Level.Entities, Assets, nullptr, {}, nullptr, Assets.Model);
	REQUIRE_MESSAGE(Session, (Session ? std::string() : Session.error().Message));
	CHECK((*Session)->GetStateTrees() == std::vector{GuardTree});
	REQUIRE((*Session)->SetNavigation(std::make_shared<const FNavigationFile>(std::move(**Navigation))));
	REQUIRE((*Session)->AddStateTree(GuardTree, CompileSandboxGuard()));
	REQUIRE((*Session)->GetNavigation());
	CHECK((*Session)->GetNavigation()->GetStats().Agents == 2);

	const auto Step = [&](const int Frames, const FGameInput& Input = {})
	{
		for (int Frame = 0; Frame < Frames; ++Frame)
		{
			REQUIRE((*Session)->Update(1.f / 60.f, Input));
			(void)(*Session)->GetRenderView({320, 180});
		}
	};

	const auto Guard = [&]
	{
		const std::vector<FGameAgentState> States = (*Session)->GetAgentStates();
		REQUIRE(States.size() == 1);
		return States.front();
	};

	// With the player out of sight range, the guard starts patrolling around where it stands.
	REQUIRE((*Session)->TeleportPlayer({0.f, 0.f, -16.f}, 0.f));
	Step(30);
	CHECK(Guard().bOnNavigation);
	CHECK(!Guard().bSeesPlayer);
	CHECK(Guard().State == "Stroll");
	Step(60);
	CHECK(!Guard().Foot.IsNearlyEqual({0.f, 0.f, 8.f}, 0.2f));
	REQUIRE((*Session)->GetAgentDebug(0));
	CHECK((*Session)->GetAgentDebug(0)->Program->States[(*Session)->GetAgentDebug(0)->Active.front()].Name == "Root");

	// A player facing it with the sword raised takes its strikes on the blade: the guard swings, and no blow lands.
	const FVector3 Home = Guard().Foot;
	REQUIRE((*Session)->TeleportPlayer({Home.X, 0.f, Home.Z + 7.f}, std::numbers::pi_v<float>));
	bool bSwung = false;
	for (int Second = 0; Second < 8; ++Second)
	{
		for (int Frame = 0; Frame < 60; ++Frame)
		{
			Step(1, {.bBlock = true});
			bSwung = bSwung || Guard().bAttacking;
		}
	}

	CHECK(bSwung);
	CHECK_FALSE((*Session)->IsPlayerRagdolled());

	// Measured on a blocked strike: how long after a swing starts its blow lands.
	const auto Contains = [](const std::vector<std::string>& Events, const std::string_view Text)
	{
		return std::ranges::any_of(Events, [Text](const std::string& Event)
		{
			return Event.starts_with(Text);
		});
	};

	(void)(*Session)->TakeEvents();
	std::optional<int> Lead;
	for (int Frame = 0, SwingFrame = -1; Frame < 600 && !Lead; ++Frame)
	{
		const bool bWasAttacking = Guard().bAttacking;
		Step(1, {.bBlock = true});
		SwingFrame = !bWasAttacking && Guard().bAttacking ? Frame : SwingFrame;
		if (SwingFrame >= 0 && Contains((*Session)->TakeEvents(), "The player blocked"))
		{
			Lead = Frame - SwingFrame;
		}
	}

	REQUIRE(Lead);
	CHECK(*Lead >= 12);
	CHECK(*Lead <= 40);

	// Pressing the guard a few frames before the blow lands deflects it; the guard reels, and the player's posture barely moves.
	Step(30);
	bool bDeflected = false;
	for (int Frame = 0, SwingFrame = -1; Frame < 900 && !bDeflected; ++Frame)
	{
		const bool bWasAttacking = Guard().bAttacking;
		const bool bGuard = SwingFrame >= 0 && Frame - SwingFrame >= *Lead - 5;
		Step(1, {.bBlock = bGuard});
		// A swing that misses ends without an event, so each new swing restarts the count.
		SwingFrame = !bWasAttacking && Guard().bAttacking ? Frame : Guard().bAttacking ? SwingFrame : -1;
		bDeflected = Contains((*Session)->TakeEvents(), "The player deflected");
	}

	CHECK(bDeflected);
	CHECK((*Session)->GetCombatView().Posture < 0.1f);
	REQUIRE((*Session)->GetCombatView().Target);
	CHECK((*Session)->GetCombatView().Target->Posture > 0.3f);
	CHECK(Guard().State != "");
	Step(120);

	// Without the guard up, the player is spotted, chased down, and struck until their vitality runs out, then gets back up a while later.
	REQUIRE((*Session)->TeleportPlayer({Home.X, 0.f, Home.Z + 7.f}, std::numbers::pi_v<float>));
	Step(30);
	(void)(*Session)->GetAgentStates();
	bool bKnocked = false;
	for (int Second = 0; Second < 25 && !bKnocked; ++Second)
	{
		Step(60);
		bKnocked = (*Session)->IsPlayerRagdolled();
	}

	CHECK(bKnocked);
	CHECK(Guard().bRemembersPlayer);
	bool bStoodUp = false;
	for (int Frame = 0; Frame < 200 && !bStoodUp; ++Frame)
	{
		Step(1);
		bStoodUp = !(*Session)->IsPlayerRagdolled();
	}

	CHECK(bStoodUp);

	// An arm and a forearm leave it at half health, and the next blow sends it running.
	REQUIRE((*Session)->SeverCharacter(0, "upperarm_l"));
	REQUIRE((*Session)->SeverCharacter(0, "lowerarm_r"));
	CHECK(Guard().Health == doctest::Approx(0.5f));
	REQUIRE((*Session)->SeverCharacter(0, "upperarm_r"));
	Step(30);
	CHECK((Guard().State == "Run" || Guard().State == "Cower"));

	// A restart puts it back whole where it was placed, with nothing remembered.
	REQUIRE((*Session)->Restart());
	CHECK(Guard().Health == 1.f);
	CHECK(Guard().Foot.IsNearlyEqual({0.f, 0.f, 8.f}, 1.e-4f));
	Step(10);
	CHECK(!Guard().bRemembersPlayer);

	// Without a leg it falls but lives: it stays where it fell, still remembering who cut it, and swings at no one.
	REQUIRE((*Session)->SeverCharacter(0, "calf_l"));
	CHECK((*Session)->IsCharacterDowned(0));
	CHECK(Guard().Health == doctest::Approx(0.75f));
	const FVector3 Fell = Guard().Foot;
	Step(60);
	CHECK(Guard().Foot.IsNearlyEqual(Fell, 1.e-4f));
	CHECK_FALSE(Guard().bAttacking);
	CHECK(Guard().bRemembersPlayer);
}

TEST_CASE("Leg IK puts ankles on their targets, keeps bone lengths and the animated foot, and moves the pelvis subtree")
{
	const FCookedModel& Model = Mannequin();
	const auto Library = FAnimationLibrary::Create(*Model.Skeleton, Model.Clips);
	REQUIRE(Library);
	const auto JointIndex = [&Model](const std::string_view Name)
	{
		const auto Found = std::ranges::find(Model.Skeleton->JointNames, Name);
		REQUIRE(Found != Model.Skeleton->JointNames.end());
		return static_cast<std::size_t>(Found - Model.Skeleton->JointNames.begin());
	};

	const std::size_t Pelvis = JointIndex("pelvis");
	const std::size_t Hip = JointIndex("thigh_l");
	const std::size_t Knee = JointIndex("calf_l");
	const std::size_t Ankle = JointIndex("foot_l");
	const std::size_t OtherAnkle = JointIndex("foot_r");
	const std::size_t Head = JointIndex("Head");
	const std::array Layers{FAnimationLayer{.Clip = *(*Library)->FindClip("Idle_Loop")}};
	FAnimationPose Pose(*Library);
	const auto At = [&Pose](const std::size_t Joint)
	{
		return Pose.GetModelTransforms()[Joint].TransformPosition({});
	};

	REQUIRE(Pose.Evaluate(Layers));
	const FVector3 HipStart = At(Hip);
	const FVector3 KneeStart = At(Knee);
	const FVector3 AnkleStart = At(Ankle);
	const FVector3 OtherStart = At(OtherAnkle);
	const FVector3 HeadStart = At(Head);
	const float Upper = (KneeStart - HipStart).Length();
	const float Lower = (AnkleStart - KneeStart).Length();
	const FVector3 FootAxis = Pose.GetModelTransforms()[Ankle].TransformVector({1.f, 0.f, 0.f}).Normalized();

	// A raised ankle lands on its target, the leg keeps its length, and the other leg and the body stay put.
	const FVector3 Target = AnkleStart + FVector3{0.f, 0.2f, 0.f};
	const std::array Raise{FLimbIkTarget{.Root = Hip, .Middle = Knee, .Tip = Ankle, .TipTarget = Target}};
	Pose.SolveLimbs(std::nullopt, {}, Raise);
	CHECK((At(Ankle) - Target).Length() < 1.e-3f);
	CHECK((At(Knee) - At(Hip)).Length() == doctest::Approx(Upper).epsilon(1.e-3));
	CHECK((At(Ankle) - At(Knee)).Length() == doctest::Approx(Lower).epsilon(1.e-3));
	CHECK((At(Hip) - HipStart).Length() < 1.e-4f);
	CHECK((At(OtherAnkle) - OtherStart).Length() < 1.e-4f);
	CHECK((At(Head) - HeadStart).Length() < 1.e-4f);
	// The knee comes forward as it bends, and the foot keeps its animated orientation.
	CHECK(At(Knee).Z > KneeStart.Z + 0.02f);
	CHECK((Pose.GetModelTransforms()[Ankle].TransformVector({1.f, 0.f, 0.f}).Normalized() - FootAxis).Length() < 1.e-3f);
	// The skin follows.
	const FMatrix4 Skin = Pose.GetSkinningMatrices()[Ankle] * *TryInverseAffine((*Library)->GetInverseBindMatrices()[Ankle]);
	CHECK((Skin.TransformPosition({}) - Target).Length() < 1.e-3f);

	// A target past the leg's reach stretches it straight toward the target, no farther.
	REQUIRE(Pose.Evaluate(Layers));
	const std::array Far{FLimbIkTarget{.Root = Hip, .Middle = Knee, .Tip = Ankle, .TipTarget = HipStart + FVector3{0.f, -3.f, 0.f}}};
	Pose.SolveLimbs(std::nullopt, {}, Far);
	CHECK((At(Ankle) - HipStart).Length() == doctest::Approx(Upper + Lower).epsilon(0.01));

	// The sole turns to the ground it is given.
	REQUIRE(Pose.Evaluate(Layers));
	const FVector3 Slope = FVector3{0.f, 1.f, 1.f}.Normalized();
	const std::array Tilt{FLimbIkTarget{.Root = Hip, .Middle = Knee, .Tip = Ankle, .TipTarget = AnkleStart, .TipTurnTo = Slope}};
	Pose.SolveLimbs(std::nullopt, {}, Tilt);
	CHECK((Pose.GetModelTransforms()[Ankle].TransformVector({1.f, 0.f, 0.f}).Normalized() - FootAxis).Length() > 0.05f);

	// Half weight goes halfway; a pelvis offset carries the body and leaves planted feet to the IK.
	REQUIRE(Pose.Evaluate(Layers));
	const std::array Half{FLimbIkTarget{.Root = Hip, .Middle = Knee, .Tip = Ankle, .TipTarget = Target, .Weight = 0.5f}};
	Pose.SolveLimbs(std::nullopt, {}, Half);
	CHECK(At(Ankle).Y == doctest::Approx(AnkleStart.Y + 0.1f).epsilon(1.e-2));
	REQUIRE(Pose.Evaluate(Layers));
	const std::array Planted{FLimbIkTarget{.Root = Hip, .Middle = Knee, .Tip = Ankle, .TipTarget = AnkleStart}};
	Pose.SolveLimbs(Pelvis, {0.f, -0.1f, 0.f}, Planted);
	CHECK(At(Head).Y == doctest::Approx(HeadStart.Y - 0.1f).epsilon(1.e-3));
	CHECK((At(Ankle) - AnkleStart).Length() < 1.e-3f);
	CHECK(At(OtherAnkle).Y == doctest::Approx(OtherStart.Y - 0.1f).epsilon(1.e-3));
}

TEST_CASE("Characters plant their feet on the level's ground, bending over a step and dropping the pelvis onto lower ground")
{
	const FDummyGameAssets Assets;
	const auto Dummy = [](const FObjectId Id, const FWorldPosition Position)
	{
		return FLevelEntity{.Id = Id, .Name = "Dummy", .Transform = {.Translation = Position}, .Mesh = FStaticMeshComponent{.Asset = FAssetId{7, 7}}, .Character = FCharacterComponent{}};
	};

	const std::vector<FLevelEntity> Level{
	    FLevelEntity{.Id = FObjectId{2, 1}, .Name = "Floor", .Transform = {.Translation = FWorldPosition{{0., -0.5, 0.}}, .Scale = {60.f, 1.f, 60.f}}, .Mesh = FStaticMeshComponent{.Asset = FAssetId{1, 1}}, .BodyType = ELevelBodyType::Static},
	    // Under the first dummy's left foot only; +X is left.
	    FLevelEntity{.Id = FObjectId{2, 2}, .Name = "Step", .Transform = {.Translation = FWorldPosition{{0.55, 0.075, 0.}}, .Scale = {1.f, 0.15f, 2.f}}, .Mesh = FStaticMeshComponent{.Asset = FAssetId{1, 1}}, .BodyType = ELevelBodyType::Static},
	    Dummy(FObjectId{2, 3}, FWorldPosition{{0., 0., 0.}}),
	    // Hovering a little over the floor, as navigation places agents.
	    Dummy(FObjectId{2, 4}, FWorldPosition{{6., 0.1, 0.}}),
	    Dummy(FObjectId{2, 5}, FWorldPosition{{-6., 0., 0.}}),
	};

	auto Session = FGameSession::Create(Level, Assets, nullptr, {}, nullptr, Assets.Model);
	REQUIRE_MESSAGE(Session, (Session ? std::string() : Session.error().Message));
	for (int Frame = 0; Frame < 90; ++Frame)
	{
		REQUIRE((*Session)->Update(1.f / 60.f, {}));
		(void)(*Session)->GetRenderView({320, 180});
	}

	const auto Joint = [&](const std::size_t Character, const std::string_view Name)
	{
		const auto Position = (*Session)->GetCharacterJointPosition(Character, Name);
		REQUIRE(Position);
		return *Position;
	};

	// The reference stands on flat floor at the same height as the first dummy.
	const float Flat = Joint(2, "foot_l").Y;
	CHECK(Joint(0, "foot_l").Y == doctest::Approx(Flat + 0.15f).epsilon(0.05));
	CHECK(Joint(0, "foot_r").Y == doctest::Approx(Flat).epsilon(0.05));
	CHECK(Joint(0, "pelvis").Y == doctest::Approx(Joint(2, "pelvis").Y).epsilon(0.01));
	// The hovering dummy's feet reach the floor and its pelvis comes down with them.
	CHECK(Joint(1, "foot_l").Y == doctest::Approx(Flat).epsilon(0.05));
	CHECK(Joint(1, "pelvis").Y == doctest::Approx(Joint(2, "pelvis").Y).epsilon(0.02));
}

TEST_CASE("Characters rest a hand on a wall beside them, pull the sword hand clear of one, and lean away, while open air changes nothing")
{
	const FDummyGameAssets Assets;
	const auto Dummy = [](const FObjectId Id, const double X)
	{
		return FLevelEntity{.Id = Id, .Name = "Dummy", .Transform = {.Translation = FWorldPosition{{X, 0., 0.}}}, .Mesh = FStaticMeshComponent{.Asset = FAssetId{7, 7}}, .Character = FCharacterComponent{}};
	};

	const auto Wall = [](const FObjectId Id, const double X)
	{
		return FLevelEntity{.Id = Id, .Name = "Wall", .Transform = {.Translation = FWorldPosition{{X, 1.5, 0.}}, .Scale = {0.5f, 3.f, 4.f}}, .Mesh = FStaticMeshComponent{.Asset = FAssetId{1, 1}}, .BodyType = ELevelBodyType::Static};
	};

	// +X is left. The first dummy's wall surface stands 0.55 m to its left, the second's 0.3 m to its right, and the third has open air.
	const std::vector<FLevelEntity> Level{
	    FLevelEntity{.Id = FObjectId{3, 1}, .Name = "Floor", .Transform = {.Translation = FWorldPosition{{0., -0.5, 0.}}, .Scale = {60.f, 1.f, 60.f}}, .Mesh = FStaticMeshComponent{.Asset = FAssetId{1, 1}}, .BodyType = ELevelBodyType::Static},
	    Dummy(FObjectId{3, 2}, 0.),
	    Wall(FObjectId{3, 3}, 0.8),
	    Dummy(FObjectId{3, 4}, 10.),
	    Wall(FObjectId{3, 5}, 9.45),
	    Dummy(FObjectId{3, 6}, -10.),
	    Dummy(FObjectId{3, 7}, 20.),
	    // A post narrower than a hand, close on the left: nothing to rest on.
	    FLevelEntity{.Id = FObjectId{3, 8}, .Name = "Post", .Transform = {.Translation = FWorldPosition{{20.6, 1.5, 0.15}}, .Scale = {0.1f, 3.f, 0.1f}}, .Mesh = FStaticMeshComponent{.Asset = FAssetId{1, 1}}, .BodyType = ELevelBodyType::Static},
	};

	auto Session = FGameSession::Create(Level, Assets, nullptr, {}, nullptr, Assets.Model);
	REQUIRE_MESSAGE(Session, (Session ? std::string() : Session.error().Message));
	for (int Frame = 0; Frame < 90; ++Frame)
	{
		REQUIRE((*Session)->Update(1.f / 60.f, {}));
		(void)(*Session)->GetRenderView({320, 180});
	}

	const auto Local = [&](const std::size_t Character, const std::string_view Name, const float Origin)
	{
		const auto Position = (*Session)->GetCharacterJointPosition(Character, Name);
		REQUIRE(Position);
		return FVector3{Position->X - Origin, Position->Y, Position->Z};
	};

	// The left hand stands near the wall's face, at about chest height, and well above where it hangs in open air.
	const FVector3 Hand = Local(0, "hand_l", 0.f);
	const FVector3 Hanging = Local(2, "hand_l", -10.f);
	CHECK(Hand.X == doctest::Approx(0.475f).epsilon(0.06));
	CHECK(Hand.Y > Hanging.Y + 0.2f);
	// Its fingers lie along the wall, reaching up and forward rather than into it, and the elbow sits below the wrist.
	const FVector3 Finger = Local(0, "middle_01_l", 0.f);
	CHECK(Finger.X < 0.55f);
	CHECK(Finger.Y > Hand.Y);
	CHECK(Finger.Z > Hand.Z);
	CHECK(Local(0, "lowerarm_l", 0.f).Y < Hand.Y);

	// A post too narrow to take a hand leaves the arm hanging.
	CHECK(Local(3, "hand_l", 20.f).Y == doctest::Approx(Hanging.Y).epsilon(0.02));

	// The sword hand stays on the near side of the wall to its right, and the torso leans away from each wall.
	CHECK(Local(1, "hand_r", 10.f).X > -0.3f + 0.08f);
	const float Upright = Local(2, "neck_01", -10.f).X;
	CHECK(Local(0, "neck_01", 0.f).X < Upright - 0.02f);
	CHECK(Local(1, "neck_01", 10.f).X > Upright + 0.02f);
}

TEST_CASE("Upper-body states keep the legs on the state before them while moving, and reversed states play backwards")
{
	const FCookedModel& Model = Mannequin();
	const auto Library = FAnimationLibrary::Create(*Model.Skeleton, Model.Clips);
	REQUIRE(Library);

	const FAnimationStateMachineDesc Desc{
	    .Parameters = {{.Name = "Speed", .Type = EAnimationParameterType::Float}, {.Name = "Attack", .Type = EAnimationParameterType::Trigger}, {.Name = "Back", .Type = EAnimationParameterType::Bool}},
	    .States = {
	        {.Name = "Locomotion", .Samples = {{.Clip = "Idle_Loop", .Position = 0.f}, {.Clip = "Walk_Loop", .Position = 2.f}}, .BlendParameter = 0},
	        {.Name = "Swing", .Samples = {{.Clip = "Sword_Regular_A"}}, .bLoop = false, .bUpperBody = true},
	        {.Name = "Backpedal", .Samples = {{.Clip = "Walk_Loop"}}, .bReverse = true},
	    },
	    .Transitions = {
	        {.From = AnyAnimationState, .To = 1, .DurationSeconds = 0.f, .Conditions = {{.Parameter = 1, .Test = EAnimationCondition::Triggered}}},
	        {.From = 0, .To = 2, .DurationSeconds = 0.f, .Conditions = {{.Parameter = 2, .Test = EAnimationCondition::IsTrue}}},
	        {.From = 1, .To = 0, .DurationSeconds = 0.f, .bAtEnd = true},
	    },
	};

	auto Machine = FAnimationStateMachine::Create(Desc, **Library);
	REQUIRE(Machine);
	const std::size_t Walk = *(*Library)->FindClip("Walk_Loop");
	const std::size_t Swing = *(*Library)->FindClip("Sword_Regular_A");

	// Standing still, the swing plays on the whole body.
	Machine->SetTrigger(1);
	Machine->Update(0.1f);
	REQUIRE(Machine->GetActiveState() == 1);
	REQUIRE(Machine->GetLayers().size() == 1);
	CHECK(Machine->GetLayers()[0].LowerBodyWeight == 1.f);

	// Walking, the swing keeps the arms and the walk keeps the legs.
	Machine->SetFloat(0, 2.f);
	Machine->Update(0.1f);
	REQUIRE(Machine->GetLayers().size() == 2);
	CHECK(Machine->GetLayers()[0].Clip == Swing);
	CHECK(Machine->GetLayers()[0].LowerBodyWeight == 0.f);
	CHECK(Machine->GetLayers()[1].Clip == Walk);
	CHECK(Machine->GetLayers()[1].UpperBodyWeight == 0.f);
	CHECK(Machine->GetLayers()[1].LowerBodyWeight == 1.f);
	FAnimationPose Pose(*Library);
	CHECK(Pose.Evaluate(Machine->GetLayers()));

	// A reversed state starts at the clip's end and runs toward its start.
	FAnimationStateMachineDesc Backward = Desc;
	Backward.Transitions.erase(Backward.Transitions.begin());
	auto Pedal = FAnimationStateMachine::Create(Backward, **Library);
	REQUIRE(Pedal);
	Pedal->SetBool(2, true);
	Pedal->Update(0.f);
	Pedal->Update(0.1f);
	REQUIRE(Pedal->GetActiveState() == 2);
	REQUIRE(Pedal->GetLayers().size() == 1);
	const float First = Pedal->GetLayers()[0].TimeSeconds;
	Pedal->Update(0.1f);
	CHECK(Pedal->GetLayers()[0].TimeSeconds < First);
}

TEST_CASE("Near a guard the player faces it and backpedals, steps into swings, bounces off walls, and is deflected for mashing into a guard")
{
	const FDummyGameAssets Assets;
	constexpr FAssetId GuardTree{9, 9};
	FLevelDocument Level{.Id = FObjectId{1, 1}, .Name = "Duel"};
	Level.Entities = {
	    FLevelEntity{.Id = FObjectId{1, 2}, .Name = "Floor", .Transform = {.Translation = FWorldPosition{{0., -0.5, 0.}}, .Scale = {40.f, 1.f, 40.f}}, .Mesh = FStaticMeshComponent{.Asset = FAssetId{1, 1}}, .BodyType = ELevelBodyType::Static},
	    FLevelEntity{.Id = FObjectId{1, 3}, .Name = "Bounds", .NavigationBounds = FNavigationBoundsComponent{.Size = {40.f, 6.f, 40.f}}},
	    FLevelEntity{.Id = FObjectId{1, 4}, .Name = "Guard", .Transform = {.Translation = FWorldPosition{{0., 0., 3.}}, .Rotation = FQuaternion::FromAxisAngle({0.f, 1.f, 0.f}, std::numbers::pi_v<float>)}, .Mesh = FStaticMeshComponent{.Asset = FAssetId{7, 7}}, .Character = FCharacterComponent{}, .AiAgent = FAiAgentComponent{.StateTree = GuardTree, .SightRange = 20.f}},
	    FLevelEntity{.Id = FObjectId{1, 5}, .Name = "Wall", .Transform = {.Translation = FWorldPosition{{-14., 1., 0.}}, .Scale = {1.f, 2.f, 6.f}}, .Mesh = FStaticMeshComponent{.Asset = FAssetId{1, 1}}, .BodyType = ELevelBodyType::Static},
	};

	const auto Navigation = BuildLevelNavigation(Level, {}, [](const FAssetId&)
	{
		return std::optional<FNavigationBox>{};
	});

	REQUIRE(Navigation);
	REQUIRE(*Navigation);
	auto Session = FGameSession::Create(Level.Entities, Assets, nullptr, {}, nullptr, Assets.Model);
	REQUIRE_MESSAGE(Session, (Session ? std::string() : Session.error().Message));
	// Without its StateTree loaded the guard stands its ground facing the player, so every swing meets its guard.
	REQUIRE((*Session)->SetNavigation(std::make_shared<const FNavigationFile>(std::move(**Navigation))));
	std::vector<std::string> Events;
	const auto Step = [&](const int Frames, const FGameInput& Input = {}, const bool bOnce = false)
	{
		for (int Frame = 0; Frame < Frames; ++Frame)
		{
			REQUIRE((*Session)->Update(1.f / 60.f, bOnce && Frame > 0 ? FGameInput{} : Input));
			(void)(*Session)->GetRenderView({320, 180});
			std::ranges::move((*Session)->TakeEvents(), std::back_inserter(Events));
		}
	};

	const auto Contains = [&Events](const std::string_view Text)
	{
		return std::ranges::any_of(Events, [Text](const std::string& Event)
		{
			return Event.find(Text) != std::string::npos;
		});
	};

	const auto Foot = [&]
	{
		return (*Session)->GetPlayerFootPosition();
	};

	// Away from the guard the blade bounces off the wall it swings into.
	REQUIRE((*Session)->TeleportPlayer({-12.6f, 0.f, 0.f}, -std::numbers::pi_v<float> * 0.5f));
	Step(30);
	Step(40, {.bAttack = true}, true);
	CHECK(Contains("struck a wall"));

	// Walking away from a guard it faces backs off rather than turning around.
	REQUIRE((*Session)->TeleportPlayer({0.f, 0.f, 0.5f}, 0.f));
	Step(20);
	bool bBackpedalled = false;
	for (int Frame = 0; Frame < 40; ++Frame)
	{
		Step(1, {.Move = {0.f, -1.f}});
		bBackpedalled = bBackpedalled || (*Session)->GetCharacterStateName() == "Backpedal";
	}

	CHECK(bBackpedalled);

	// A swing a stride short of the guard steps in toward it.
	REQUIRE((*Session)->TeleportPlayer({0.f, 0.f, 0.6f}, 0.f));
	Step(5);
	const float Before = Foot().Z;
	Step(20, {.bAttack = true}, true);
	CHECK(Foot().Z > Before + 0.3f);

	// Swinging again and again into its guard, the third swing in a row is deflected.
	Events.clear();
	for (int Swing = 0; Swing < 30 && !Contains("deflected the player"); ++Swing)
	{
		Step(24, {.bAttack = true}, true);
	}

	CHECK(Contains("blocked the player"));
	CHECK(Contains("deflected the player"));
	CHECK((*Session)->GetCombatView().Posture > 0.f);
}
}
