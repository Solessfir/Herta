#include "PlayerCharacter.h"

#include <algorithm>
#include <format>

namespace Herta
{
namespace
{
enum ELocomotionParameter : std::size_t
{
	Speed,
	Grounded,
	Crouched,
	Sliding,
	Jump,
};

enum ELocomotionState : std::size_t
{
	Locomotion,
	Crouching,
	Slide,
	JumpStart,
	Air,
	Land,
};

[[nodiscard]] FAnimationConditionDesc Is(const ELocomotionParameter Parameter, const bool bValue)
{
	return {.Parameter = Parameter, .Test = bValue ? EAnimationCondition::IsTrue : EAnimationCondition::IsFalse};
}
}

FAnimationStateMachineDesc MakeDefaultLocomotion()
{
	// Blend positions are each clip's ground speed in meters per second, so the blend follows the controller's own walk and sprint speeds.
	return {
	    .Parameters = {
	        {.Name = "Speed", .Type = EAnimationParameterType::Float},
	        {.Name = "Grounded", .Type = EAnimationParameterType::Bool, .Default = 1.f},
	        {.Name = "Crouched", .Type = EAnimationParameterType::Bool},
	        {.Name = "Sliding", .Type = EAnimationParameterType::Bool},
	        {.Name = "Jump", .Type = EAnimationParameterType::Trigger},
	    },
	    .States = {
	        {.Name = "Locomotion", .Samples = {{.Clip = "Idle_Loop", .Position = 0.f}, {.Clip = "Walk_Loop", .Position = 1.8f}, {.Clip = "Jog_Fwd_Loop", .Position = 4.5f}, {.Clip = "Sprint_Loop", .Position = 7.f}}, .BlendParameter = Speed},
	        {.Name = "Crouch", .Samples = {{.Clip = "Crouch_Idle_Loop", .Position = 0.f}, {.Clip = "Crouch_Fwd_Loop", .Position = 2.2f}}, .BlendParameter = Speed},
	        {.Name = "Slide", .Samples = {{.Clip = "Crouch_Idle_Loop"}}},
	        {.Name = "Jump", .Samples = {{.Clip = "Jump_Start"}}, .bLoop = false},
	        {.Name = "Air", .Samples = {{.Clip = "Jump_Loop"}}},
	        {.Name = "Land", .Samples = {{.Clip = "Jump_Land"}}, .bLoop = false, .Speed = 1.5f},
	    },
	    // A jump leaves the ground in the same step it starts, so it is tested before falling, from every state the player can jump from.
	    .Transitions = {
	        {.From = Locomotion, .To = JumpStart, .DurationSeconds = 0.1f, .Conditions = {{.Parameter = Jump, .Test = EAnimationCondition::Triggered}}},
	        {.From = Crouching, .To = JumpStart, .DurationSeconds = 0.1f, .Conditions = {{.Parameter = Jump, .Test = EAnimationCondition::Triggered}}},
	        {.From = Slide, .To = JumpStart, .DurationSeconds = 0.1f, .Conditions = {{.Parameter = Jump, .Test = EAnimationCondition::Triggered}}},
	        {.From = Land, .To = JumpStart, .DurationSeconds = 0.1f, .Conditions = {{.Parameter = Jump, .Test = EAnimationCondition::Triggered}}},
	        // Wall jumps start from the air.
	        {.From = Air, .To = JumpStart, .DurationSeconds = 0.1f, .Conditions = {{.Parameter = Jump, .Test = EAnimationCondition::Triggered}}},
	        {.From = Locomotion, .To = Slide, .DurationSeconds = 0.15f, .Conditions = {Is(Sliding, true)}},
	        {.From = Locomotion, .To = Crouching, .DurationSeconds = 0.2f, .Conditions = {Is(Crouched, true)}},
	        {.From = Locomotion, .To = Air, .DurationSeconds = 0.25f, .Conditions = {Is(Grounded, false)}},
	        {.From = Crouching, .To = Slide, .DurationSeconds = 0.15f, .Conditions = {Is(Sliding, true)}},
	        {.From = Crouching, .To = Locomotion, .DurationSeconds = 0.2f, .Conditions = {Is(Crouched, false)}},
	        {.From = Crouching, .To = Air, .DurationSeconds = 0.25f, .Conditions = {Is(Grounded, false)}},
	        {.From = Slide, .To = Crouching, .DurationSeconds = 0.2f, .Conditions = {Is(Sliding, false), Is(Crouched, true)}},
	        {.From = Slide, .To = Locomotion, .DurationSeconds = 0.2f, .Conditions = {Is(Sliding, false)}},
	        {.From = JumpStart, .To = Air, .DurationSeconds = 0.15f, .bAtEnd = true},
	        {.From = JumpStart, .To = Land, .DurationSeconds = 0.1f, .Conditions = {Is(Grounded, true)}},
	        {.From = Air, .To = Land, .DurationSeconds = 0.08f, .Conditions = {Is(Grounded, true)}},
	        {.From = Land, .To = Locomotion, .DurationSeconds = 0.2f, .bAtEnd = true},
	        {.From = Land, .To = Locomotion, .DurationSeconds = 0.25f, .Conditions = {{.Parameter = Speed, .Test = EAnimationCondition::Greater, .Value = 2.f}}},
	    },
	    .Entry = Locomotion,
	};
}

FPlayerCharacter::FPlayerCharacter(std::shared_ptr<const FCookedModel> InModel, std::shared_ptr<const FAnimationLibrary> InLibrary, FAnimationStateMachine InMachine)
    : Model(std::move(InModel))
    , Library(std::move(InLibrary))
    , Machine(InMachine)
    , StartMachine(std::move(InMachine))
    , Pose(Library)
    , Vertices(Model->Vertices)
{
}

std::expected<std::unique_ptr<FPlayerCharacter>, FGameError> FPlayerCharacter::Create(std::shared_ptr<const FCookedModel> Model, IGraphicsDevice* const Device)
{
	if (!Model || !Model->Skeleton || Model->Influences.size() != Model->Vertices.size())
	{
		return std::unexpected(FGameError{"The player character must be a skinned model"});
	}

	auto Library = FAnimationLibrary::Create(*Model->Skeleton, Model->Clips);
	if (!Library)
	{
		return std::unexpected(FGameError{std::format("The player character cannot animate: {}", Library.error().Message)});
	}

	auto Machine = FAnimationStateMachine::Create(MakeDefaultLocomotion(), **Library);
	if (!Machine)
	{
		return std::unexpected(FGameError{std::format("The player character lacks a locomotion clip: {}", Machine.error().Message)});
	}

	std::unique_ptr<FPlayerCharacter> Character(new FPlayerCharacter(std::move(Model), std::move(*Library), std::move(*Machine)));
	const auto Names = Character->Library->GetJointNames();
	const auto Head = std::ranges::find_if(Names, [](const std::string& Name)
	{
		return Name == "DEF-head" || Name == "head" || Name == "Head";
	});

	if (Head != Names.end())
	{
		Character->HeadJoint = static_cast<std::size_t>(Head - Names.begin());
	}

	const FAnimationStateMachine& Locomotion = Character->Machine;
	Character->SpeedParameter = Locomotion.FindParameter("Speed");
	Character->GroundedParameter = Locomotion.FindParameter("Grounded");
	Character->CrouchedParameter = Locomotion.FindParameter("Crouched");
	Character->SlidingParameter = Locomotion.FindParameter("Sliding");
	Character->JumpParameter = Locomotion.FindParameter("Jump");

	if (Device)
	{
		auto Mesh = FRenderMesh::Create(*Device, *Character->Model, "Player character");
		if (!Mesh)
		{
			return std::unexpected(FGameError{std::format("Cannot upload the player character: {}", Mesh.error().Message)});
		}

		Character->Mesh = std::move(*Mesh);
	}

	return Character;
}

void FPlayerCharacter::Restart()
{
	Machine = StartMachine;
}

void FPlayerCharacter::Update(const float DeltaSeconds, const FPlayerMotion& Motion)
{
	const auto Set = [this](const std::optional<std::size_t>& Parameter, const bool bValue)
	{
		if (Parameter)
		{
			Machine.SetBool(*Parameter, bValue);
		}
	};

	if (SpeedParameter)
	{
		Machine.SetFloat(*SpeedParameter, Motion.GroundSpeed);
	}

	Set(GroundedParameter, Motion.bGrounded);
	Set(CrouchedParameter, Motion.bCrouched);
	Set(SlidingParameter, Motion.bSliding);
	if (Motion.bJumped && JumpParameter)
	{
		Machine.SetTrigger(*JumpParameter);
	}

	Machine.Update(DeltaSeconds);
}

std::expected<void, FGameError> FPlayerCharacter::Skin(const bool bHideHead)
{
	if (const auto Evaluated = Pose.Evaluate(Machine.GetLayers()); !Evaluated)
	{
		return std::unexpected(FGameError{Evaluated.error().Message});
	}

	if (bHideHead && HeadJoint)
	{
		Pose.CollapseJoint(*HeadJoint);
	}

	SkinVertices(Pose.GetSkinningMatrices(), Model->Vertices, Model->Influences, Vertices);
	return {};
}

const FRenderMesh* FPlayerCharacter::GetMesh() const noexcept
{
	return Mesh.get();
}

std::span<const FCookedVertex> FPlayerCharacter::GetVertices() const noexcept
{
	return Vertices;
}

const FAnimationStateMachine& FPlayerCharacter::GetStateMachine() const noexcept
{
	return Machine;
}

std::optional<FVector3> FPlayerCharacter::GetHeadPosition() const noexcept
{
	if (!HeadJoint)
	{
		return std::nullopt;
	}

	return Pose.GetModelTransforms()[*HeadJoint].TransformPosition({});
}
}
