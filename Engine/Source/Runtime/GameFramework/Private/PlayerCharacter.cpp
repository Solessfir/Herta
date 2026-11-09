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
	WallJump,
	Mantle,
	WallRunning,
};

enum ELocomotionState : std::size_t
{
	Locomotion,
	Crouching,
	SlideStart,
	Slide,
	SlideExit,
	JumpStart,
	WallJumpStart,
	Air,
	Land,
	Climb,
	WallRunState,
};

[[nodiscard]] FAnimationConditionDesc Triggered(const ELocomotionParameter Parameter)
{
	return {.Parameter = Parameter, .Test = EAnimationCondition::Triggered};
}

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
	        {.Name = "WallJump", .Type = EAnimationParameterType::Trigger},
	        {.Name = "Mantle", .Type = EAnimationParameterType::Trigger},
	        {.Name = "WallRunning", .Type = EAnimationParameterType::Bool},
	    },
	    .States = {
	        {.Name = "Locomotion", .Samples = {{.Clip = "Idle_Loop", .Position = 0.f}, {.Clip = "Walk_Loop", .Position = 1.8f}, {.Clip = "Jog_Fwd_Loop", .Position = 4.5f}, {.Clip = "Sprint_Loop", .Position = 7.f}}, .BlendParameter = Speed},
	        {.Name = "Crouch", .Samples = {{.Clip = "Crouch_Idle_Loop", .Position = 0.f}, {.Clip = "Crouch_Fwd_Loop", .Position = 2.2f}}, .BlendParameter = Speed},
	        {.Name = "Slide start", .Samples = {{.Clip = "Slide_Start"}}, .bLoop = false, .Speed = 1.5f},
	        {.Name = "Slide", .Samples = {{.Clip = "Slide_Loop"}}},
	        {.Name = "Slide exit", .Samples = {{.Clip = "Slide_Exit"}}, .bLoop = false},
	        {.Name = "Jump", .Samples = {{.Clip = "Jump_Start"}}, .bLoop = false},
	        {.Name = "Wall jump", .Samples = {{.Clip = "NinjaJump_Start"}}, .bLoop = false, .Speed = 1.4f},
	        {.Name = "Air", .Samples = {{.Clip = "Jump_Loop"}}},
	        {.Name = "Land", .Samples = {{.Clip = "Jump_Land"}}, .bLoop = false, .Speed = 1.5f},
	        // Its root rises the 1 m it climbs, which the session cancels so the capsule carries the body up the actual ledge height.
	        {.Name = "Mantle", .Samples = {{.Clip = "ClimbUp_1m_RM"}}, .bLoop = false},
	        // The clips have no wall run, so the sprint plays while the session leans the body away from the wall.
	        {.Name = "Wall run", .Samples = {{.Clip = "Sprint_Loop"}}},
	    },
	    // A jump leaves the ground in the same step it starts, so it is tested before falling, from every state the player can jump from.
	    .Transitions = {
	        // Mantles and wall jumps can start from any state, and win over everything else in the step they start.
	        {.From = AnyAnimationState, .To = Climb, .DurationSeconds = 0.1f, .Conditions = {Triggered(Mantle)}},
	        {.From = AnyAnimationState, .To = WallJumpStart, .DurationSeconds = 0.1f, .Conditions = {Triggered(WallJump)}},
	        {.From = AnyAnimationState, .To = WallRunState, .DurationSeconds = 0.15f, .Conditions = {Is(WallRunning, true)}},
	        {.From = WallRunState, .To = Land, .DurationSeconds = 0.1f, .Conditions = {Is(Grounded, true)}},
	        {.From = WallRunState, .To = Air, .DurationSeconds = 0.2f, .Conditions = {Is(WallRunning, false)}},
	        {.From = Climb, .To = Locomotion, .DurationSeconds = 0.2f, .bAtEnd = true},
	        {.From = Locomotion, .To = JumpStart, .DurationSeconds = 0.1f, .Conditions = {Triggered(Jump)}},
	        {.From = Crouching, .To = JumpStart, .DurationSeconds = 0.1f, .Conditions = {Triggered(Jump)}},
	        {.From = SlideStart, .To = JumpStart, .DurationSeconds = 0.1f, .Conditions = {Triggered(Jump)}},
	        {.From = Slide, .To = JumpStart, .DurationSeconds = 0.1f, .Conditions = {Triggered(Jump)}},
	        {.From = SlideExit, .To = JumpStart, .DurationSeconds = 0.1f, .Conditions = {Triggered(Jump)}},
	        {.From = Land, .To = JumpStart, .DurationSeconds = 0.1f, .Conditions = {Triggered(Jump)}},
	        {.From = Locomotion, .To = SlideStart, .DurationSeconds = 0.12f, .Conditions = {Is(Sliding, true)}},
	        {.From = Locomotion, .To = Crouching, .DurationSeconds = 0.2f, .Conditions = {Is(Crouched, true)}},
	        {.From = Locomotion, .To = Air, .DurationSeconds = 0.25f, .Conditions = {Is(Grounded, false)}},
	        {.From = Crouching, .To = SlideStart, .DurationSeconds = 0.12f, .Conditions = {Is(Sliding, true)}},
	        {.From = Crouching, .To = Locomotion, .DurationSeconds = 0.2f, .Conditions = {Is(Crouched, false)}},
	        {.From = Crouching, .To = Air, .DurationSeconds = 0.25f, .Conditions = {Is(Grounded, false)}},
	        // A slide that ends before its start finishes still plays the exit, so the body never pops out of the slide pose.
	        {.From = SlideStart, .To = SlideExit, .DurationSeconds = 0.15f, .Conditions = {Is(Sliding, false)}},
	        {.From = SlideStart, .To = Slide, .DurationSeconds = 0.15f, .bAtEnd = true},
	        {.From = SlideStart, .To = Air, .DurationSeconds = 0.25f, .Conditions = {Is(Grounded, false)}},
	        {.From = Slide, .To = SlideExit, .DurationSeconds = 0.15f, .Conditions = {Is(Sliding, false)}},
	        {.From = Slide, .To = Air, .DurationSeconds = 0.25f, .Conditions = {Is(Grounded, false)}},
	        {.From = SlideExit, .To = SlideStart, .DurationSeconds = 0.12f, .Conditions = {Is(Sliding, true)}},
	        {.From = SlideExit, .To = Crouching, .DurationSeconds = 0.2f, .Conditions = {Is(Crouched, true)}, .bAtEnd = true},
	        {.From = SlideExit, .To = Locomotion, .DurationSeconds = 0.2f, .bAtEnd = true},
	        {.From = JumpStart, .To = Air, .DurationSeconds = 0.15f, .bAtEnd = true},
	        {.From = WallJumpStart, .To = Air, .DurationSeconds = 0.2f, .bAtEnd = true},
	        {.From = WallJumpStart, .To = Land, .DurationSeconds = 0.1f, .Conditions = {Is(Grounded, true)}},
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
    , CameraVertices(Model->Vertices)
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
	const auto FindJoint = [&Names](const std::initializer_list<std::string_view> Candidates) -> std::optional<std::size_t>
	{
		for (const std::string_view Candidate : Candidates)
		{
			if (const auto Found = std::ranges::find(Names, Candidate); Found != Names.end())
			{
				return static_cast<std::size_t>(Found - Names.begin());
			}
		}

		return std::nullopt;
	};

	Character->HeadJoint = FindJoint({"Head", "head", "DEF-head"});
	Character->HiddenJoint = FindJoint({"neck_01", "neck", "DEF-neck"});
	if (!Character->HiddenJoint)
	{
		Character->HiddenJoint = Character->HeadJoint;
	}

	const FAnimationStateMachine& Locomotion = Character->Machine;
	Character->SpeedParameter = Locomotion.FindParameter("Speed");
	Character->GroundedParameter = Locomotion.FindParameter("Grounded");
	Character->CrouchedParameter = Locomotion.FindParameter("Crouched");
	Character->SlidingParameter = Locomotion.FindParameter("Sliding");
	Character->JumpParameter = Locomotion.FindParameter("Jump");
	Character->WallJumpParameter = Locomotion.FindParameter("WallJump");
	Character->MantleParameter = Locomotion.FindParameter("Mantle");
	Character->WallRunningParameter = Locomotion.FindParameter("WallRunning");

	if (Device)
	{
		auto Mesh = FRenderMesh::Create(*Device, *Character->Model, "Player character");
		if (!Mesh)
		{
			return std::unexpected(FGameError{std::format("Cannot upload the player character: {}", Mesh.error().Message)});
		}

		auto CameraMesh = FRenderMesh::Create(*Device, *Character->Model, "Player character, first person");
		if (!CameraMesh)
		{
			return std::unexpected(FGameError{std::format("Cannot upload the player character: {}", CameraMesh.error().Message)});
		}

		Character->Mesh = std::move(*Mesh);
		Character->CameraMesh = std::move(*CameraMesh);
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
	Set(WallRunningParameter, Motion.bWallRunning);
	const auto Trigger = [this](const std::optional<std::size_t>& Parameter, const bool bFired)
	{
		if (bFired && Parameter)
		{
			Machine.SetTrigger(*Parameter);
		}
	};

	Trigger(JumpParameter, Motion.bJumped);
	Trigger(WallJumpParameter, Motion.bWallJumped);
	Trigger(MantleParameter, Motion.bMantled);

	Machine.Update(DeltaSeconds);
}

std::expected<void, FGameError> FPlayerCharacter::Skin(const bool bFirstPerson)
{
	if (const auto Evaluated = Pose.Evaluate(Machine.GetLayers()); !Evaluated)
	{
		return std::unexpected(FGameError{Evaluated.error().Message});
	}

	// ponytail: skins the whole body twice in first person, about 0.16 ms each for the Mannequin; skin only the hidden joints' vertices again if characters multiply.
	SkinVertices(Pose.GetSkinningMatrices(), Model->Vertices, Model->Influences, Vertices);
	if (bFirstPerson && HiddenJoint)
	{
		Pose.CollapseJoint(*HiddenJoint);
		SkinVertices(Pose.GetSkinningMatrices(), Model->Vertices, Model->Influences, CameraVertices);
	}

	return {};
}

const FRenderMesh* FPlayerCharacter::GetCameraMesh() const noexcept
{
	return CameraMesh.get();
}

std::span<const FCookedVertex> FPlayerCharacter::GetCameraVertices() const noexcept
{
	return CameraVertices;
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

float FPlayerCharacter::GetRootHeight() const noexcept
{
	// Parents precede children, so the first joint is the root.
	const auto Transforms = Pose.GetModelTransforms();
	return Transforms.empty() ? 0.f : Transforms.front().TransformPosition({}).Y;
}

std::optional<float> FPlayerCharacter::GetStateSeconds(const std::string_view Name) const noexcept
{
	const FAnimationStateMachineDesc& Desc = Machine.GetDesc();
	const auto State = std::ranges::find(Desc.States, Name, &FAnimationStateDesc::Name);
	if (State == Desc.States.end() || State->Samples.empty() || State->Speed <= 0.f)
	{
		return std::nullopt;
	}

	const auto Clip = Library->FindClip(State->Samples.front().Clip);
	return Clip ? std::optional(Library->GetClipDuration(*Clip) / State->Speed) : std::nullopt;
}
}
