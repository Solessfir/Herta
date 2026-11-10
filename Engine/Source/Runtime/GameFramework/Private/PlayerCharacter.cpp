#include "PlayerCharacter.h"

#include "Herta/Math/AffineTransform.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <numbers>

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
	Hit,
	Dead,
	Attack,
	Blocking,
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
	HitReaction,
	Death,
	AttackA,
	AttackARecover,
	AttackB,
	AttackBRecover,
	Block,
	DashAttack,
};

[[nodiscard]] FAnimationConditionDesc Triggered(const ELocomotionParameter Parameter)
{
	return {.Parameter = Parameter, .Test = EAnimationCondition::Triggered};
}

[[nodiscard]] FAnimationConditionDesc Is(const ELocomotionParameter Parameter, const bool bValue)
{
	return {.Parameter = Parameter, .Test = bValue ? EAnimationCondition::IsTrue : EAnimationCondition::IsFalse};
}

// A ragdoll part runs from Joint towards End, lengthened by Extend where End stops short of the limb's tip, such as the neck to the head joint.
struct FRagdollProfilePart
{
	std::string_view Joint;
	std::string_view End;
	int Parent = -1;
	float Radius = 0.1f;
	float Extend = 1.f;
	float MassKg = 1.f;
	float SwingRadians = 0.f;
	float TwistRadians = 0.f;
};

// Eleven capsules for the Unreal-style skeleton the Sandbox Mannequin uses, weighing 80 kg together. Joints between parts, such as the clavicles,
// and below them, such as the hands' fingers, follow their parent part with their animated pose.
constexpr std::array<FRagdollProfilePart, 11> RagdollProfile{{
    {.Joint = "pelvis", .End = "spine_02", .Radius = 0.13f, .MassKg = 14.f},
    {.Joint = "spine_02", .End = "neck_01", .Parent = 0, .Radius = 0.14f, .MassKg = 18.f, .SwingRadians = 0.35f, .TwistRadians = 0.3f},
    {.Joint = "neck_01", .End = "Head", .Parent = 1, .Radius = 0.1f, .Extend = 2.4f, .MassKg = 5.f, .SwingRadians = 0.6f, .TwistRadians = 0.6f},
    {.Joint = "upperarm_l", .End = "lowerarm_l", .Parent = 1, .Radius = 0.055f, .MassKg = 2.5f, .SwingRadians = 1.3f, .TwistRadians = 0.7f},
    {.Joint = "lowerarm_l", .End = "hand_l", .Parent = 3, .Radius = 0.045f, .Extend = 1.35f, .MassKg = 1.8f, .SwingRadians = 1.2f, .TwistRadians = 0.3f},
    {.Joint = "upperarm_r", .End = "lowerarm_r", .Parent = 1, .Radius = 0.055f, .MassKg = 2.5f, .SwingRadians = 1.3f, .TwistRadians = 0.7f},
    {.Joint = "lowerarm_r", .End = "hand_r", .Parent = 5, .Radius = 0.045f, .Extend = 1.35f, .MassKg = 1.8f, .SwingRadians = 1.2f, .TwistRadians = 0.3f},
    {.Joint = "thigh_l", .End = "calf_l", .Parent = 0, .Radius = 0.075f, .MassKg = 9.f, .SwingRadians = 0.9f, .TwistRadians = 0.3f},
    {.Joint = "calf_l", .End = "foot_l", .Parent = 7, .Radius = 0.055f, .MassKg = 4.f, .SwingRadians = 1.1f, .TwistRadians = 0.15f},
    {.Joint = "thigh_r", .End = "calf_r", .Parent = 0, .Radius = 0.075f, .MassKg = 9.f, .SwingRadians = 0.9f, .TwistRadians = 0.3f},
    {.Joint = "calf_r", .End = "foot_r", .Parent = 9, .Radius = 0.055f, .MassKg = 4.f, .SwingRadians = 1.1f, .TwistRadians = 0.15f},
}};

// The rotation that turns +Y onto Direction, the axis ragdoll capsules lie along.
[[nodiscard]] FQuaternion RotationFromUp(const FVector3& Direction)
{
	const FVector3 Axis = FVector3::Up().Cross(Direction);
	const float Cosine = std::clamp(FVector3::Up().Dot(Direction), -1.f, 1.f);
	if (Axis.LengthSquared() < 1.e-10f)
	{
		return Cosine > 0.f ? FQuaternion::Identity() : FQuaternion::FromAxisAngle(FVector3::Forward(), std::numbers::pi_v<float>);
	}

	return FQuaternion::FromAxisAngle(Axis.Normalized(), std::acos(Cosine));
}
}

FAnimationStateMachineDesc MakeDefaultPlayerStateMachine()
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
	        {.Name = "Hit", .Type = EAnimationParameterType::Trigger},
	        {.Name = "Dead", .Type = EAnimationParameterType::Bool},
	        {.Name = "Attack", .Type = EAnimationParameterType::Trigger},
	        {.Name = "Blocking", .Type = EAnimationParameterType::Bool},
	    },
	    .States = {
	        // The walk, jog, and sprint cycles each take two steps, planting a foot at their start and halfway.
	        {.Name = "Locomotion", .Samples = {{.Clip = "Idle_Loop", .Position = 0.f}, {.Clip = "Walk_Loop", .Position = 1.8f}, {.Clip = "Jog_Fwd_Loop", .Position = 4.5f}, {.Clip = "Sprint_Loop", .Position = 7.f}}, .BlendParameter = Speed, .Events = {{.Name = "Footstep", .Phase = 0.f}, {.Name = "Footstep", .Phase = 0.5f}}},
	        {.Name = "Crouch", .Samples = {{.Clip = "Crouch_Idle_Loop", .Position = 0.f}, {.Clip = "Crouch_Fwd_Loop", .Position = 2.2f}}, .BlendParameter = Speed},
	        {.Name = "Slide start", .Samples = {{.Clip = "Slide_Start"}}, .bLoop = false, .Speed = 1.5f},
	        {.Name = "Slide", .Samples = {{.Clip = "Slide_Loop"}}},
	        {.Name = "Slide exit", .Samples = {{.Clip = "Slide_Exit"}}, .bLoop = false},
	        {.Name = "Jump", .Samples = {{.Clip = "Jump_Start"}}, .bLoop = false},
	        {.Name = "Wall jump", .Samples = {{.Clip = "NinjaJump_Start"}}, .bLoop = false, .Speed = 1.4f},
	        {.Name = "Air", .Samples = {{.Clip = "Jump_Loop"}}},
	        {.Name = "Land", .Samples = {{.Clip = "Jump_Land"}}, .bLoop = false, .Speed = 1.5f, .Events = {{.Name = "Land", .Phase = 0.f}}},
	        // Its root rises the 1 m it climbs, which the session cancels so the capsule carries the body up the actual ledge height.
	        {.Name = "Mantle", .Samples = {{.Clip = "ClimbUp_1m_RM"}}, .bLoop = false},
	        // The clips have no wall run, so the sprint plays while the session leans the body away from the wall.
	        {.Name = "Wall run", .Samples = {{.Clip = "Sprint_Loop"}}},
	        {.Name = "Hit", .Samples = {{.Clip = "Hit_Chest"}}, .bLoop = false},
	        {.Name = "Dead", .Samples = {{.Clip = "Death01"}}, .bLoop = false},
	        // The blade cuts between SwingStart and SwingEnd; the recoveries return the sword to guard, and an attack during one chains the other swing.
	        {.Name = "Attack A", .Samples = {{.Clip = "Sword_Regular_A"}}, .bLoop = false, .Events = {{.Name = "SwingStart", .Phase = 0.05f}, {.Name = "SwingEnd", .Phase = 0.95f}}},
	        {.Name = "Attack A recover", .Samples = {{.Clip = "Sword_Regular_A_Rec"}}, .bLoop = false},
	        {.Name = "Attack B", .Samples = {{.Clip = "Sword_Regular_B"}}, .bLoop = false, .Events = {{.Name = "SwingStart", .Phase = 0.05f}, {.Name = "SwingEnd", .Phase = 0.95f}}},
	        {.Name = "Attack B recover", .Samples = {{.Clip = "Sword_Regular_B_Rec"}}, .bLoop = false},
	        {.Name = "Block", .Samples = {{.Clip = "Sword_Block"}}, .bLoop = false},
	        // Attacking at a sprint lunges into a dash slash; the mover keeps the sprint's speed, so the clip plays in place.
	        {.Name = "Dash attack", .Samples = {{.Clip = "Sword_Dash_RM"}}, .bLoop = false, .Speed = 1.3f, .Events = {{.Name = "SwingStart", .Phase = 0.25f}, {.Name = "SwingEnd", .Phase = 0.7f}}},
	    },
	    // A jump leaves the ground in the same step it starts, so it is tested before falling, from every state the player can jump from.
	    .Transitions = {
	        // Death wins over everything and has no way out; a hit flinches anything but the dead.
	        {.From = AnyAnimationState, .To = Death, .DurationSeconds = 0.2f, .Conditions = {Is(Dead, true)}},
	        {.From = AnyAnimationState, .To = HitReaction, .DurationSeconds = 0.06f, .Conditions = {Triggered(Hit), Is(Dead, false)}},
	        {.From = HitReaction, .To = Locomotion, .DurationSeconds = 0.15f, .bAtEnd = true},
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
	        {.From = Locomotion, .To = DashAttack, .DurationSeconds = 0.08f, .Conditions = {Triggered(Attack), {.Parameter = Speed, .Test = EAnimationCondition::Greater, .Value = 5.5f}}},
	        {.From = DashAttack, .To = Locomotion, .DurationSeconds = 0.2f, .bAtEnd = true},
	        {.From = DashAttack, .To = Air, .DurationSeconds = 0.25f, .Conditions = {Is(Grounded, false)}},
	        {.From = Locomotion, .To = AttackA, .DurationSeconds = 0.08f, .Conditions = {Triggered(Attack)}},
	        {.From = Land, .To = AttackA, .DurationSeconds = 0.08f, .Conditions = {Triggered(Attack)}},
	        {.From = Locomotion, .To = Block, .DurationSeconds = 0.1f, .Conditions = {Is(Blocking, true)}},
	        {.From = Block, .To = Locomotion, .DurationSeconds = 0.15f, .Conditions = {Is(Blocking, false)}},
	        {.From = AttackA, .To = AttackARecover, .DurationSeconds = 0.05f, .bAtEnd = true},
	        {.From = AttackARecover, .To = AttackB, .DurationSeconds = 0.08f, .Conditions = {Triggered(Attack)}},
	        {.From = AttackARecover, .To = Locomotion, .DurationSeconds = 0.2f, .bAtEnd = true},
	        {.From = AttackB, .To = AttackBRecover, .DurationSeconds = 0.05f, .bAtEnd = true},
	        {.From = AttackBRecover, .To = AttackA, .DurationSeconds = 0.08f, .Conditions = {Triggered(Attack)}},
	        {.From = AttackBRecover, .To = Locomotion, .DurationSeconds = 0.2f, .bAtEnd = true},
	        {.From = AttackA, .To = Air, .DurationSeconds = 0.25f, .Conditions = {Is(Grounded, false)}},
	        {.From = AttackARecover, .To = Air, .DurationSeconds = 0.25f, .Conditions = {Is(Grounded, false)}},
	        {.From = AttackB, .To = Air, .DurationSeconds = 0.25f, .Conditions = {Is(Grounded, false)}},
	        {.From = AttackBRecover, .To = Air, .DurationSeconds = 0.25f, .Conditions = {Is(Grounded, false)}},
	        {.From = Block, .To = Air, .DurationSeconds = 0.25f, .Conditions = {Is(Grounded, false)}},
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
	Whole = {.Vertices = Model->Vertices, .Influences = Model->Influences, .Indices = Model->Indices, .Sections = Model->Sections};
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

	// A model cooked with an animation graph runs its machine; others use the built-in locomotion. The session drives parameters by name, so a
	// graph that lacks one simply ignores it.
	auto Machine = FAnimationStateMachine::Create(Model->StateMachine ? *Model->StateMachine : MakeDefaultPlayerStateMachine(), **Library);
	if (!Machine)
	{
		return std::unexpected(FGameError{std::format("The player character's state machine cannot play: {}", Machine.error().Message)});
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

	Character->Device = Device;
	Character->PelvisJoint = FindJoint({"pelvis", "hips", "DEF-hips"});
	Character->GripJoints = {FindJoint({"hand_r"}), FindJoint({"middle_01_r"}), FindJoint({"index_01_r"}), FindJoint({"pinky_01_r"})};
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
		Character->WholeMesh = Character->Mesh;
		Character->CameraMesh = std::move(*CameraMesh);
	}

	return Character;
}

void FPlayerCharacter::Restart()
{
	Machine = StartMachine;
	Remaining.reset();
	RagdollWeight = 0.f;
	RagdollPose.clear();
	Mesh = WholeMesh;
	Vertices = Model->Vertices;
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
	// The locomotion blend shares one phase, so standing still would step in place with the idle clip.
	Events.clear();
	for (const std::string& Event : Machine.GetEvents())
	{
		if (Event != "Footstep" || Motion.GroundSpeed > 0.5f)
		{
			Events.push_back(Event);
		}
	}
}

std::span<const std::string> FPlayerCharacter::GetEvents() const noexcept
{
	return Events;
}

std::expected<void, FGameError> FPlayerCharacter::EvaluatePose()
{
	if (const auto Evaluated = Pose.Evaluate(Machine.GetLayers()); !Evaluated)
	{
		return std::unexpected(FGameError{Evaluated.error().Message});
	}

	// Cuts read this pose, so a ragdolled body is cut where it lies.
	if (RagdollWeight > 0.f && RagdollPose.size() == RagdollJoints.size())
	{
		Pose.OverrideJoints(RagdollJoints, RagdollPose, RagdollWeight);
	}

	return {};
}

std::optional<FMatrix4> FPlayerCharacter::GetGripFrame() const
{
	if (!std::ranges::all_of(GripJoints, [](const std::optional<std::size_t>& Joint)
	{
		return Joint.has_value();
	}))
	{
		return std::nullopt;
	}

	const auto At = [this](const std::optional<std::size_t>& Joint)
	{
		return Pose.GetModelTransforms()[*Joint].TransformPosition({});
	};

	const FVector3 Wrist = At(GripJoints[0]);
	const FVector3 Knuckle = At(GripJoints[1]);
	const FVector3 Grip = (At(GripJoints[2]) - At(GripJoints[3])).Normalized();
	FVector3 Fingers = Knuckle - Wrist;
	Fingers = (Fingers - Grip * Fingers.Dot(Grip)).Normalized();
	const FVector3 Back = Fingers.Cross(Grip);
	const FVector3 Palm = (Wrist + Knuckle) * 0.5f;
	// Column-major: the three axes, then the palm as the origin.
	return FMatrix4(std::array{Fingers.X, Fingers.Y, Fingers.Z, 0.f, Grip.X, Grip.Y, Grip.Z, 0.f, Back.X, Back.Y, Back.Z, 0.f, Palm.X, Palm.Y, Palm.Z, 1.f});
}

std::expected<void, FGameError> FPlayerCharacter::Skin(const bool bFirstPerson)
{
	if (auto Evaluated = EvaluatePose(); !Evaluated)
	{
		return Evaluated;
	}

	// ponytail: skins the whole body twice in first person, about 0.16 ms each for the Mannequin; skin only the hidden joints' vertices again if characters multiply.
	const FSkinnedMeshPiece& Piece = GetPiece();
	Vertices.resize(Piece.Vertices.size());
	SkinVertices(Pose.GetSkinningMatrices(), Piece.Vertices, Piece.Influences, Vertices);
	if (bFirstPerson && HiddenJoint && !Remaining)
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

FRagdollFit FitRagdoll(const FAnimationLibrary& Library, const std::span<const FMatrix4> Transforms, const FMatrix4& BodyToPhysics, const std::function<bool(std::size_t Joint)>& Keep)
{
	const auto Names = Library.GetJointNames();
	const auto Find = [&Names](const std::string_view Name) -> std::optional<std::size_t>
	{
		const auto Found = std::ranges::find(Names, Name);
		return Found == Names.end() ? std::nullopt : std::optional(static_cast<std::size_t>(Found - Names.begin()));
	};

	FRagdollFit Fit;
	// Each profile part's index among the parts kept.
	std::array<std::optional<std::size_t>, RagdollProfile.size()> Kept{};
	for (std::size_t Index = 0; Index < RagdollProfile.size(); ++Index)
	{
		const FRagdollProfilePart& Profile = RagdollProfile[Index];
		const auto Joint = Find(Profile.Joint);
		const auto End = Find(Profile.End);
		if (!Joint || !End || *Joint >= Transforms.size() || *End >= Transforms.size())
		{
			return {};
		}

		if (Keep && !Keep(*Joint))
		{
			continue;
		}

		const FMatrix4 JointWorld = BodyToPhysics * Transforms[*Joint];
		const FVector3 Start = JointWorld.TransformPosition({});
		const FVector3 Tip = Start + ((BodyToPhysics * Transforms[*End]).TransformPosition({}) - Start) * Profile.Extend;
		const float Length = (Tip - Start).Length();
		const FVector3 Axis = Length > 1.e-4f ? (Tip - Start) * (1.f / Length) : FVector3::Up();
		const FPhysicsBodyTransform Capsule{.Position = (Start + Tip) * 0.5f, .Rotation = RotationFromUp(Axis)};
		const std::optional<FMatrix4> CapsuleInverse = TryInverseAffine(FMatrix4::Translation(Capsule.Position) * FMatrix4::Rotation(Capsule.Rotation));
		const std::optional<std::size_t> Parent = Profile.Parent >= 0 ? Kept[static_cast<std::size_t>(Profile.Parent)] : std::nullopt;
		Kept[Index] = Fit.Parts.size();
		Fit.Parts.push_back({
		    .Parent = Fit.Parts.empty() ? std::nullopt : std::optional(Parent.value_or(0)),
		    .Transform = Capsule,
		    .Radius = Profile.Radius,
		    .HalfHeight = std::max(Length * 0.5f - Profile.Radius, 0.01f),
		    .MassKg = Profile.MassKg,
		    .Joint = Start,
		    .TwistAxis = Axis,
		    .SwingRadians = Profile.SwingRadians,
		    .TwistRadians = Profile.TwistRadians,
		});
		Fit.Joints.push_back(*Joint);
		Fit.Offsets.push_back(CapsuleInverse ? *CapsuleInverse * JointWorld : FMatrix4{});
	}

	return Fit;
}

FCookedModel MakeCutModel(const FCookedModel& Source, const FSkinnedMeshPiece& Piece)
{
	FCookedModel Result{.Vertices = Piece.Vertices, .Indices = Piece.Indices, .Sections = Piece.Sections, .Materials = Source.Materials, .Textures = Source.Textures};
	// Cuts show the wound material, after the model's own, so pieces keep every original section.
	FCookedMaterial Wound{.Name = "Wound"};
	Wound.Parameters.BaseColor = {0.32f, 0.02f, 0.03f, 1.f};
	Wound.Parameters.Roughness = 0.35f;
	Result.Materials.push_back(std::move(Wound));
	return Result;
}

bool IsSliver(const FSkinnedMeshPiece& Piece)
{
	if (Piece.Indices.size() < 40 * 3)
	{
		return true;
	}

	FVector3 Minimum{1.e9f, 1.e9f, 1.e9f};
	FVector3 Maximum{-1.e9f, -1.e9f, -1.e9f};
	for (const FCookedVertex& Vertex : Piece.Vertices)
	{
		Minimum = {std::min(Minimum.X, Vertex.Position[0]), std::min(Minimum.Y, Vertex.Position[1]), std::min(Minimum.Z, Vertex.Position[2])};
		Maximum = {std::max(Maximum.X, Vertex.Position[0]), std::max(Maximum.Y, Vertex.Position[1]), std::max(Maximum.Z, Vertex.Position[2])};
	}

	return (Maximum - Minimum).Length() < 0.04f;
}

bool IsThinCut(const std::span<const FCookedVertex> Posed, const FMeshSlicePlane& Plane)
{
	constexpr float MinimumDepth = 0.08f;
	float Front = 0.f;
	float Back = 0.f;
	for (const FCookedVertex& Vertex : Posed)
	{
		const float Distance = Plane.Normal.Dot({Vertex.Position[0], Vertex.Position[1], Vertex.Position[2]}) - Plane.Distance;
		Front = std::max(Front, Distance);
		Back = std::max(Back, -Distance);
	}

	return std::min(Front, Back) < MinimumDepth;
}

std::vector<bool> FindCarriedJoints(const FSkinnedMeshPiece& Piece, const std::size_t JointCount)
{
	std::vector<std::uint32_t> Counts(JointCount, 0);
	for (const FCookedSkinInfluence& Influence : Piece.Influences)
	{
		for (std::size_t Slot = 0; Slot < 4; ++Slot)
		{
			if (Influence.Weights[Slot] > CookedSkinWeightTotal / 2 && Influence.Joints[Slot] < JointCount)
			{
				++Counts[Influence.Joints[Slot]];
			}
		}
	}

	std::vector<bool> Carried(JointCount);
	for (std::size_t Joint = 0; Joint < JointCount; ++Joint)
	{
		Carried[Joint] = Counts[Joint] >= 8;
	}

	return Carried;
}

std::vector<FPhysicsRagdollPart> FPlayerCharacter::BuildRagdoll(const FMatrix4& BodyToPhysics)
{
	// A cut body only gets parts for the joints it still carries.
	std::function<bool(std::size_t)> Keep;
	if (Remaining)
	{
		Keep = [Carried = FindCarriedJoints(*Remaining, Library->GetJointCount())](const std::size_t Joint)
		{
			return Joint < Carried.size() && Carried[Joint];
		};
	}

	FRagdollFit Fit = FitRagdoll(*Library, Pose.GetModelTransforms(), BodyToPhysics, Keep);
	RagdollJoints = std::move(Fit.Joints);
	RagdollOffsets = std::move(Fit.Offsets);
	return std::move(Fit.Parts);
}

void FPlayerCharacter::SetRagdollPose(const std::span<const FPhysicsBodyTransform> Parts, const FMatrix4& ModelInverse, const float Weight)
{
	RagdollWeight = Parts.size() == RagdollJoints.size() ? Weight : 0.f;
	RagdollPose.clear();
	for (std::size_t Index = 0; Index < Parts.size() && Index < RagdollOffsets.size(); ++Index)
	{
		RagdollPose.push_back(ModelInverse * FMatrix4::Translation(Parts[Index].Position) * FMatrix4::Rotation(Parts[Index].Rotation) * RagdollOffsets[Index]);
	}
}

const FSkinnedMeshPiece& FPlayerCharacter::GetPiece() const
{
	return Remaining ? *Remaining : Whole;
}

void FPlayerCharacter::SkinForCut()
{
	const FSkinnedMeshPiece& Piece = GetPiece();
	Vertices.resize(Piece.Vertices.size());
	SkinVertices(Pose.GetSkinningMatrices(), Piece.Vertices, Piece.Influences, Vertices);
}

std::expected<std::optional<FSkinnedMeshPiece>, FGameError> FPlayerCharacter::Keep(FMeshSliceResult Sliced, const bool bKeepFront)
{
	FSkinnedMeshPiece& Kept = bKeepFront ? Sliced.Front : Sliced.Back;
	FSkinnedMeshPiece& Cut = bKeepFront ? Sliced.Back : Sliced.Front;
	if (Sliced.CapLoops == 0 || IsSliver(Kept) || IsSliver(Cut))
	{
		return std::nullopt;
	}

	// Validated even without a device to upload them, so headless sessions catch a cut that could not draw.
	FCookedModel KeptModel = MakeCutModel(*Model, Kept);
	for (const FCookedModel& Piece : {KeptModel, MakeCutModel(*Model, Cut)})
	{
		if (auto Valid = ValidateCookedModel(Piece); !Valid)
		{
			return std::unexpected(FGameError{std::format("A cut produced an invalid piece: {}", Valid.error().Message)});
		}
	}

	if (Device)
	{
		auto Created = FRenderMesh::Create(*Device, KeptModel, "Cut character");
		if (!Created)
		{
			return std::unexpected(FGameError{std::format("Cannot upload a cut character: {}", Created.error().Message)});
		}

		Mesh = std::move(*Created);
	}

	Vertices.resize(Kept.Vertices.size());
	Remaining = std::move(Kept);
	return std::move(Cut);
}

std::optional<FSkinnedMeshPiece> FPlayerCharacter::DetachAll()
{
	if (IsEmpty())
	{
		return std::nullopt;
	}

	FSkinnedMeshPiece All = GetPiece();
	Remaining = FSkinnedMeshPiece{};
	Mesh.reset();
	Vertices.clear();
	return All;
}

bool FPlayerCharacter::IsEmpty() const
{
	return GetPiece().Indices.empty();
}

const std::shared_ptr<const FCookedModel>& FPlayerCharacter::GetCookedModel() const noexcept
{
	return Model;
}

const std::shared_ptr<const FAnimationLibrary>& FPlayerCharacter::GetLibrary() const noexcept
{
	return Library;
}

std::span<const FMatrix4> FPlayerCharacter::GetModelTransforms() const noexcept
{
	return Pose.GetModelTransforms();
}

std::span<const FMatrix4> FPlayerCharacter::GetSkinningMatrices() const noexcept
{
	return Pose.GetSkinningMatrices();
}

std::expected<std::optional<FSkinnedMeshPiece>, FGameError> FPlayerCharacter::Slice(const FMeshSlicePlane& Plane)
{
	if (IsEmpty())
	{
		return std::nullopt;
	}

	SkinForCut();
	if (IsThinCut(Vertices, Plane))
	{
		return std::nullopt;
	}

	const FSkinnedMeshPiece& Piece = GetPiece();
	auto Sliced = SliceSkinnedMesh(Piece, Vertices, Pose.GetSkinningMatrices(), Plane, static_cast<std::uint32_t>(Model->Materials.size()));
	if (!Sliced)
	{
		return std::unexpected(FGameError{Sliced.error().Message});
	}

	const FVector3 Pelvis = PelvisJoint ? Pose.GetModelTransforms()[*PelvisJoint].TransformPosition({}) : FVector3{};
	return Keep(std::move(*Sliced), Plane.Normal.Dot(Pelvis) >= Plane.Distance);
}

std::expected<std::optional<FSkinnedMeshPiece>, FGameError> FPlayerCharacter::Sever(const std::size_t Joint)
{
	const auto Parents = Library->GetJointParents();
	if (Joint >= Parents.size() || Parents[Joint] < 0 || IsEmpty())
	{
		return std::nullopt;
	}

	SkinForCut();
	// Parents precede children, so one forward pass marks the subtree.
	std::vector<bool> Limb(Parents.size(), false);
	Limb[Joint] = true;
	for (std::size_t Index = Joint + 1; Index < Parents.size(); ++Index)
	{
		Limb[Index] = Parents[Index] >= 0 && Limb[static_cast<std::size_t>(Parents[Index])];
	}

	const FSkinnedMeshPiece& Piece = GetPiece();
	std::vector<float> Sides(Piece.Vertices.size());
	for (std::size_t Vertex = 0; Vertex < Sides.size(); ++Vertex)
	{
		float LimbWeight = 0.f;
		for (std::size_t Slot = 0; Slot < 4; ++Slot)
		{
			LimbWeight += Limb[Piece.Influences[Vertex].Joints[Slot]] ? static_cast<float>(Piece.Influences[Vertex].Weights[Slot]) : 0.f;
		}

		Sides[Vertex] = LimbWeight / static_cast<float>(CookedSkinWeightTotal) - 0.5f;
	}

	// Caps face along the bone, from the parent towards the limb.
	const auto Parent = static_cast<std::size_t>(Parents[Joint]);
	const FVector3 At = Pose.GetModelTransforms()[Joint].TransformPosition({});
	const FVector3 Bone = At - Pose.GetModelTransforms()[Parent].TransformPosition({});
	const FVector3 Normal = Bone.LengthSquared() > 1.e-8f ? Bone.Normalized() : FVector3::Up();
	auto Sliced = SplitSkinnedMesh(Piece, Vertices, Pose.GetSkinningMatrices(), Sides, {.Normal = Normal, .Distance = Normal.Dot(At)}, static_cast<std::uint32_t>(Model->Materials.size()));
	if (!Sliced)
	{
		return std::unexpected(FGameError{Sliced.error().Message});
	}

	for (FCookedSkinInfluence& Influence : Sliced->Back.Influences)
	{
		// The stump follows the parent alone; weights that went to the missing limb move to it.
		std::uint32_t Moved = 0;
		for (std::size_t Slot = 0; Slot < 4; ++Slot)
		{
			if (Limb[Influence.Joints[Slot]] && Influence.Weights[Slot] != 0)
			{
				Moved += Influence.Weights[Slot];
				Influence.Weights[Slot] = 0;
			}
		}

		if (Moved != 0)
		{
			const auto Slot = static_cast<std::size_t>(std::ranges::find(Influence.Joints, static_cast<std::uint16_t>(Parent)) - Influence.Joints.begin());
			const std::size_t Target = Slot < 4 && Influence.Weights[Slot] != 0 ? Slot : static_cast<std::size_t>(std::ranges::min_element(Influence.Weights) - Influence.Weights.begin());
			Influence.Joints[Target] = static_cast<std::uint16_t>(Parent);
			Influence.Weights[Target] = static_cast<std::uint16_t>(Influence.Weights[Target] + Moved);
		}
	}

	return Keep(std::move(*Sliced), false);
}

std::optional<std::size_t> FPlayerCharacter::FindSeverableJoint(const FVector3& Point, const float Reach) const
{
	// The cut lies where the limb meets its parent, so a hit is measured to the joint the limb hangs from.
	std::optional<std::size_t> Best;
	float BestDistance = Reach;
	// The model names its severable joints; without them, the Unreal-style rig's neck, shoulders, elbows, hips, and knees.
	static constexpr std::array<std::string_view, 9> DefaultJoints{"neck_01", "upperarm_l", "upperarm_r", "lowerarm_l", "lowerarm_r", "thigh_l", "thigh_r", "calf_l", "calf_r"};
	std::vector<std::string_view> Names(DefaultJoints.begin(), DefaultJoints.end());
	if (!Model->SeverableJoints.empty())
	{
		Names.assign(Model->SeverableJoints.begin(), Model->SeverableJoints.end());
	}

	for (const std::string_view Name : Names)
	{
		const auto Joint = FindJoint(Name);
		if (!Joint || !Carries(*Joint))
		{
			continue;
		}

		const float Distance = (Pose.GetModelTransforms()[*Joint].TransformPosition({}) - Point).Length();
		if (Distance < BestDistance)
		{
			Best = Joint;
			BestDistance = Distance;
		}
	}

	return Best;
}

bool FPlayerCharacter::Carries(const std::size_t Joint) const
{
	return std::ranges::any_of(GetPiece().Influences, [Joint](const FCookedSkinInfluence& Influence)
	{
		for (std::size_t Slot = 0; Slot < 4; ++Slot)
		{
			if (Influence.Joints[Slot] == Joint && Influence.Weights[Slot] > CookedSkinWeightTotal / 2)
			{
				return true;
			}
		}

		return false;
	});
}

std::optional<std::size_t> FPlayerCharacter::FindJoint(const std::string_view Name) const
{
	const auto Names = Library->GetJointNames();
	const auto Found = std::ranges::find(Names, Name);
	return Found == Names.end() ? std::nullopt : std::optional(static_cast<std::size_t>(Found - Names.begin()));
}

std::string_view FPlayerCharacter::GetJointName(const std::size_t Joint) const
{
	const auto Names = Library->GetJointNames();
	return Joint < Names.size() ? std::string_view(Names[Joint]) : std::string_view{};
}

void FPlayerCharacter::FireTrigger(const std::string_view Parameter)
{
	if (const auto Index = Machine.FindParameter(Parameter))
	{
		Machine.SetTrigger(*Index);
	}
}

void FPlayerCharacter::SetBool(const std::string_view Parameter, const bool bValue)
{
	if (const auto Index = Machine.FindParameter(Parameter))
	{
		Machine.SetBool(*Index, bValue);
	}
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
