#include "Herta/Animation/Animation.h"
#include "Herta/Animation/AnimationStateMachine.h"
#include "Herta/AssetPipeline/AssetCooker.h"
#include "Herta/Assets/CookedContent.h"
#include "Herta/GameFramework/GameAssets.h"
#include "Herta/GameFramework/GameSession.h"
#include "TestFiles.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <format>
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
	Step({}, 60);
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
}
