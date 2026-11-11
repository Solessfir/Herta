#include "PlayerCharacter.h"

#include "Herta/Math/AffineTransform.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <map>
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
	HeavyAttack,
	Deflect,
	Recoil,
	Staggered,
	Backpedal,
	Dash,
	DashBack,
	WallSwinging,
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
	AirAttack,
	HeavyAttackState,
	DeflectState,
	RecoilState,
	StaggerState,
	BackpedalState,
	AttackC,
	SlideAttack,
	WallRunAttack,
	DashState,
	DashBackState,
};

// Part of a rotation, by blending from identity along the short way.
[[nodiscard]] FQuaternion ScaledTurn(const FQuaternion& Turn, const float Weight)
{
	const FQuaternion Aligned = Turn.W < 0.f ? FQuaternion{-Turn.X, -Turn.Y, -Turn.Z, -Turn.W} : Turn;
	return FQuaternion{Aligned.X * Weight, Aligned.Y * Weight, Aligned.Z * Weight, 1.f - Weight + Aligned.W * Weight}.NormalizedOrIdentity();
}

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
	        {.Name = "HeavyAttack", .Type = EAnimationParameterType::Trigger},
	        {.Name = "Deflect", .Type = EAnimationParameterType::Trigger},
	        {.Name = "Recoil", .Type = EAnimationParameterType::Trigger},
	        {.Name = "Staggered", .Type = EAnimationParameterType::Bool},
	        {.Name = "Backpedal", .Type = EAnimationParameterType::Bool},
	        {.Name = "Dash", .Type = EAnimationParameterType::Trigger},
	        {.Name = "DashBack", .Type = EAnimationParameterType::Trigger},
	        {.Name = "WallSwinging", .Type = EAnimationParameterType::Bool},
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
	        // The blade cuts between SwingStart and SwingEnd, around where the arm moves fastest, so the wind-up cuts nothing; the recoveries return
	        // the sword to guard, and an attack during one chains the next swing. Sword states move only the upper body while walking.
	        {.Name = "Attack A", .Samples = {{.Clip = "Sword_Regular_A"}}, .bLoop = false, .Events = {{.Name = "SwingStart", .Phase = 0.42f}, {.Name = "SwingEnd", .Phase = 0.82f}}, .bUpperBody = true},
	        {.Name = "Attack A recover", .Samples = {{.Clip = "Sword_Regular_A_Rec"}}, .bLoop = false, .bUpperBody = true},
	        {.Name = "Attack B", .Samples = {{.Clip = "Sword_Regular_B"}}, .bLoop = false, .Events = {{.Name = "SwingStart", .Phase = 0.34f}, {.Name = "SwingEnd", .Phase = 0.66f}}, .bUpperBody = true},
	        {.Name = "Attack B recover", .Samples = {{.Clip = "Sword_Regular_B_Rec"}}, .bLoop = false, .bUpperBody = true},
	        {.Name = "Block", .Samples = {{.Clip = "Sword_Block"}}, .bLoop = false, .bUpperBody = true},
	        // Attacking at a sprint lunges into a dash slash; the mover keeps the sprint's speed, so the clip plays in place.
	        {.Name = "Dash attack", .Samples = {{.Clip = "Sword_Dash_RM"}}, .bLoop = false, .Speed = 1.3f, .Events = {{.Name = "SwingStart", .Phase = 0.25f}, {.Name = "SwingEnd", .Phase = 0.7f}}},
	        // Attacking in the air brings the sword down overhead; it plays out even past landing, so the swing always ends.
	        {.Name = "Air attack", .Samples = {{.Clip = "Sword_Attack"}}, .bLoop = false, .Speed = 1.6f, .Events = {{.Name = "SwingStart", .Phase = 0.15f}, {.Name = "SwingEnd", .Phase = 0.42f}}},
	        // The guards' overhead strike: the sword rises for about 0.35 s before it comes down, long enough to see it coming and block.
	        {.Name = "Heavy attack", .Samples = {{.Clip = "Sword_Attack"}}, .bLoop = false, .Speed = 0.75f, .Events = {{.Name = "SwingStart", .Phase = 0.2f}, {.Name = "SwingEnd", .Phase = 0.36f}}},
	        // A deflect snaps the guard up; a deflected attacker's head is knocked back; a broken posture reels until it recovers.
	        {.Name = "Deflect", .Samples = {{.Clip = "Sword_Block"}}, .bLoop = false, .Speed = 2.f, .bUpperBody = true},
	        {.Name = "Recoil", .Samples = {{.Clip = "Hit_Head"}}, .bLoop = false, .Speed = 1.2f, .bUpperBody = true},
	        {.Name = "Stagger", .Samples = {{.Clip = "Hit_Knockback"}}, .bLoop = false, .Speed = 0.8f},
	        // Facing a foe, stepping away walks backwards rather than turning around.
	        {.Name = "Backpedal", .Samples = {{.Clip = "Walk_Loop", .Position = 1.8f}, {.Clip = "Jog_Fwd_Loop", .Position = 4.5f}}, .BlendParameter = Speed, .Events = {{.Name = "Footstep", .Phase = 0.f}, {.Name = "Footstep", .Phase = 0.5f}}, .bReverse = true},
	        // The third swing of a chain, a heavier cut with its own recovery.
	        {.Name = "Attack C", .Samples = {{.Clip = "Sword_Regular_C"}}, .bLoop = false, .Speed = 1.15f, .Events = {{.Name = "SwingStart", .Phase = 0.26f}, {.Name = "SwingEnd", .Phase = 0.42f}}, .bUpperBody = true},
	        // A cut on the way past: the whole body stays in the slide, and FPlayerCharacter::ApplySlideSwing moves the sword arm.
	        {.Name = "Slide attack", .Samples = {{.Clip = "Slide_Loop"}}, .bLoop = false, .Speed = 3.6f, .Events = {{.Name = "SwingStart", .Phase = 0.15f}, {.Name = "SwingEnd", .Phase = 0.8f}}},
	        // A swing on the way along a wall: the arms cut while the legs keep running on it.
	        {.Name = "Wall run attack", .Samples = {{.Clip = "Sword_Regular_A"}}, .bLoop = false, .Events = {{.Name = "SwingStart", .Phase = 0.42f}, {.Name = "SwingEnd", .Phase = 0.82f}}, .bUpperBody = true},
	        // Held poses rather than strides, so the burst reads as one motion: leaned into a lunge, or crouched in a backstep.
	        {.Name = "Dash", .Samples = {{.Clip = "Dash"}}, .bLoop = false, .Speed = 1.3f},
	        {.Name = "Dash back", .Samples = {{.Clip = "Dash_Back"}}, .bLoop = false, .Speed = 1.3f},
	    },
	    // A jump leaves the ground in the same step it starts, so it is tested before falling, from every state the player can jump from.
	    .Transitions = {
	        // Death wins over everything and has no way out; a hit flinches anything but the dead.
	        {.From = AnyAnimationState, .To = Death, .DurationSeconds = 0.2f, .Conditions = {Is(Dead, true)}},
	        {.From = AnyAnimationState, .To = StaggerState, .DurationSeconds = 0.08f, .Conditions = {Is(Staggered, true), Is(Dead, false)}},
	        {.From = AnyAnimationState, .To = HitReaction, .DurationSeconds = 0.06f, .Conditions = {Triggered(Hit), Is(Dead, false), Is(Staggered, false)}},
	        {.From = AnyAnimationState, .To = DeflectState, .DurationSeconds = 0.04f, .Conditions = {Triggered(Deflect), Is(Dead, false), Is(Staggered, false)}},
	        {.From = AnyAnimationState, .To = RecoilState, .DurationSeconds = 0.05f, .Conditions = {Triggered(Recoil), Is(Dead, false), Is(Staggered, false)}},
	        {.From = HitReaction, .To = Locomotion, .DurationSeconds = 0.15f, .bAtEnd = true},
	        // Mantles and wall jumps can start from any state, and win over everything else in the step they start.
	        {.From = AnyAnimationState, .To = Climb, .DurationSeconds = 0.1f, .Conditions = {Triggered(Mantle)}},
	        {.From = AnyAnimationState, .To = WallJumpStart, .DurationSeconds = 0.1f, .Conditions = {Triggered(WallJump)}},
	        {.From = AnyAnimationState, .To = DashState, .DurationSeconds = 0.05f, .Conditions = {Triggered(Dash), Is(Dead, false)}},
	        {.From = AnyAnimationState, .To = DashBackState, .DurationSeconds = 0.05f, .Conditions = {Triggered(DashBack), Is(Dead, false)}},
	        {.From = AnyAnimationState, .To = WallRunState, .DurationSeconds = 0.15f, .Conditions = {Is(WallRunning, true), Is(WallSwinging, false)}},
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
	        {.From = AttackBRecover, .To = AttackC, .DurationSeconds = 0.08f, .Conditions = {Triggered(Attack)}},
	        {.From = AttackBRecover, .To = Locomotion, .DurationSeconds = 0.2f, .bAtEnd = true},
	        {.From = AttackA, .To = Air, .DurationSeconds = 0.25f, .Conditions = {Is(Grounded, false)}},
	        {.From = AttackARecover, .To = Air, .DurationSeconds = 0.25f, .Conditions = {Is(Grounded, false)}},
	        {.From = AttackB, .To = Air, .DurationSeconds = 0.25f, .Conditions = {Is(Grounded, false)}},
	        {.From = AttackBRecover, .To = Air, .DurationSeconds = 0.25f, .Conditions = {Is(Grounded, false)}},
	        {.From = Block, .To = Air, .DurationSeconds = 0.25f, .Conditions = {Is(Grounded, false)}},
	        {.From = Air, .To = AirAttack, .DurationSeconds = 0.08f, .Conditions = {Triggered(Attack)}},
	        {.From = JumpStart, .To = AirAttack, .DurationSeconds = 0.08f, .Conditions = {Triggered(Attack)}},
	        {.From = WallJumpStart, .To = AirAttack, .DurationSeconds = 0.08f, .Conditions = {Triggered(Attack)}},
	        {.From = AirAttack, .To = Locomotion, .DurationSeconds = 0.2f, .Conditions = {Is(Grounded, true)}, .bAtEnd = true},
	        {.From = AirAttack, .To = Air, .DurationSeconds = 0.2f, .bAtEnd = true},
	        {.From = Locomotion, .To = HeavyAttackState, .DurationSeconds = 0.1f, .Conditions = {Triggered(HeavyAttack)}},
	        {.From = HeavyAttackState, .To = Locomotion, .DurationSeconds = 0.2f, .bAtEnd = true},
	        {.From = StaggerState, .To = Locomotion, .DurationSeconds = 0.25f, .Conditions = {Is(Staggered, false)}},
	        {.From = DeflectState, .To = Block, .DurationSeconds = 0.08f, .Conditions = {Is(Blocking, true)}, .bAtEnd = true},
	        {.From = DeflectState, .To = Locomotion, .DurationSeconds = 0.15f, .bAtEnd = true},
	        {.From = RecoilState, .To = Locomotion, .DurationSeconds = 0.2f, .bAtEnd = true},
	        {.From = Locomotion, .To = BackpedalState, .DurationSeconds = 0.2f, .Conditions = {Is(Backpedal, true), Is(Grounded, true)}},
	        {.From = BackpedalState, .To = Locomotion, .DurationSeconds = 0.2f, .Conditions = {Is(Backpedal, false)}},
	        {.From = BackpedalState, .To = JumpStart, .DurationSeconds = 0.1f, .Conditions = {Triggered(Jump)}},
	        {.From = BackpedalState, .To = Air, .DurationSeconds = 0.25f, .Conditions = {Is(Grounded, false)}},
	        {.From = BackpedalState, .To = AttackA, .DurationSeconds = 0.08f, .Conditions = {Triggered(Attack)}},
	        {.From = BackpedalState, .To = Block, .DurationSeconds = 0.1f, .Conditions = {Is(Blocking, true)}},
	        {.From = AttackC, .To = Locomotion, .DurationSeconds = 0.25f, .bAtEnd = true},
	        {.From = AttackC, .To = Air, .DurationSeconds = 0.25f, .Conditions = {Is(Grounded, false)}},
	        {.From = SlideStart, .To = SlideAttack, .DurationSeconds = 0.08f, .Conditions = {Triggered(Attack)}},
	        {.From = Slide, .To = SlideAttack, .DurationSeconds = 0.08f, .Conditions = {Triggered(Attack)}},
	        {.From = SlideExit, .To = AttackA, .DurationSeconds = 0.08f, .Conditions = {Triggered(Attack)}},
	        {.From = SlideAttack, .To = Air, .DurationSeconds = 0.25f, .Conditions = {Is(Grounded, false)}},
	        // A slide that ends mid-swing finishes the cut before standing up.
	        {.From = SlideAttack, .To = Slide, .DurationSeconds = 0.15f, .Conditions = {Is(Sliding, true)}, .bAtEnd = true},
	        {.From = SlideAttack, .To = SlideExit, .DurationSeconds = 0.15f, .bAtEnd = true},
	        {.From = WallRunState, .To = WallRunAttack, .DurationSeconds = 0.08f, .Conditions = {Triggered(Attack)}},
	        {.From = WallRunAttack, .To = Air, .DurationSeconds = 0.2f, .Conditions = {Is(WallRunning, false)}, .bAtEnd = true},
	        {.From = WallRunAttack, .To = WallRunState, .DurationSeconds = 0.12f, .bAtEnd = true},
	        {.From = Land, .To = SlideStart, .DurationSeconds = 0.1f, .Conditions = {Is(Sliding, true)}},
	        // A dash cuts straight into a slide, a swing, or the air.
	        {.From = DashState, .To = SlideStart, .DurationSeconds = 0.1f, .Conditions = {Is(Sliding, true)}},
	        {.From = DashBackState, .To = SlideStart, .DurationSeconds = 0.1f, .Conditions = {Is(Sliding, true)}},
	        {.From = DashState, .To = AttackA, .DurationSeconds = 0.08f, .Conditions = {Triggered(Attack)}},
	        {.From = DashBackState, .To = AttackA, .DurationSeconds = 0.08f, .Conditions = {Triggered(Attack)}},
	        {.From = DashState, .To = JumpStart, .DurationSeconds = 0.1f, .Conditions = {Triggered(Jump)}},
	        {.From = DashBackState, .To = JumpStart, .DurationSeconds = 0.1f, .Conditions = {Triggered(Jump)}},
	        {.From = DashState, .To = Locomotion, .DurationSeconds = 0.15f, .Conditions = {Is(Grounded, true)}, .bAtEnd = true},
	        {.From = DashBackState, .To = Locomotion, .DurationSeconds = 0.15f, .Conditions = {Is(Grounded, true)}, .bAtEnd = true},
	        {.From = DashState, .To = Air, .DurationSeconds = 0.15f, .bAtEnd = true},
	        {.From = DashBackState, .To = Air, .DurationSeconds = 0.15f, .bAtEnd = true},
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
	Character->SpineJoint = FindJoint({"spine_01"});
	Character->Arms[0] = {.Shoulder = FindJoint({"upperarm_l"}), .Elbow = FindJoint({"lowerarm_l"}), .Wrist = FindJoint({"hand_l"})};
	Character->Arms[1] = {.Shoulder = FindJoint({"upperarm_r"}), .Elbow = FindJoint({"lowerarm_r"}), .Wrist = FindJoint({"hand_r"})};
	const auto& MachineStates = Character->Machine.GetDesc().States;
	const auto SlideSwing = std::ranges::find(MachineStates, std::string_view("Slide attack"), &FAnimationStateDesc::Name);
	Character->SlideSwingState = SlideSwing != MachineStates.end() ? std::optional(static_cast<std::size_t>(SlideSwing - MachineStates.begin())) : std::nullopt;
	const auto WallSwing = std::ranges::find(MachineStates, std::string_view("Wall run attack"), &FAnimationStateDesc::Name);
	Character->WallSwingState = WallSwing != MachineStates.end() ? std::optional(static_cast<std::size_t>(WallSwing - MachineStates.begin())) : std::nullopt;
	Character->LeftFingers = {FindJoint({"middle_01_l"}), FindJoint({"index_01_l"}), FindJoint({"pinky_01_l"})};
	const std::vector<FAnimationStateDesc>& States = Character->Machine.GetDesc().States;
	for (std::size_t State = 0; State < States.size(); ++State)
	{
		if (States[State].Name == "Locomotion" || States[State].Name == "Crouch" || States[State].Name == "Backpedal")
		{
			Character->RelaxedStates.push_back(State);
		}
	}

	const std::span<const FMatrix4> InverseBinds = Character->Library->GetInverseBindMatrices();
	for (const auto& [Foot, Hip, Knee, Ankle] : {std::tuple{0, "thigh_l", "calf_l", "foot_l"}, std::tuple{1, "thigh_r", "calf_r", "foot_r"}})
	{
		FFootPlacement& Placement = Character->Feet[static_cast<std::size_t>(Foot)];
		Placement.Hip = FindJoint({Hip});
		Placement.Knee = FindJoint({Knee});
		Placement.Ankle = FindJoint({Ankle});
		const auto Bind = Placement.Ankle ? TryInverseAffine(InverseBinds[*Placement.Ankle]) : std::nullopt;
		Placement.RestHeight = Bind ? Bind->TransformPosition({}).Y : Placement.RestHeight;
	}

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
	Character->DashParameter = Locomotion.FindParameter("Dash");
	Character->DashBackParameter = Locomotion.FindParameter("DashBack");
	Character->WallSwingingParameter = Locomotion.FindParameter("WallSwinging");

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
	FootWeight = 0.f;
	PelvisOffset = {};
	WallCastCountdown = 0;
	for (FArmReach& Arm : Arms)
	{
		Arm.Wanted = 0.f;
		Arm.Weight = 0.f;
	}

	for (FFootPlacement& Foot : Feet)
	{
		Foot.Offset = {};
		Foot.Normal = FVector3::Up();
	}

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
	Trigger(DashParameter, Motion.bDashed && !Motion.bDashedBack);
	Trigger(DashBackParameter, Motion.bDashed && Motion.bDashedBack);
	Set(WallSwingingParameter, WallSwingState && Machine.GetActiveState() == *WallSwingState);

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

	ApplySlideSwing();
	return {};
}

void FPlayerCharacter::ApplySlideSwing()
{
	const FArmReach& Arm = Arms[1];
	const bool bReady = SlideSwingState && Machine.GetActiveState() == *SlideSwingState && RagdollWeight == 0.f && Arm.Shoulder && Arm.Elbow && Arm.Wrist && GripJoints[1] && GripJoints[2] && GripJoints[3];
	if (!bReady || (Remaining && !Carries(*Arm.Wrist)))
	{
		return;
	}

	// Model space, +X to the character's left: from beside the head on the right, down, then low across the feet to the left.
	constexpr std::array HandPath{FVector3{-0.7f, 0.6f, 0.35f}, FVector3{-0.85f, -0.55f, 0.55f}, FVector3{0.f, -0.85f, 1.f}, FVector3{0.6f, -0.7f, 0.45f}};
	constexpr std::array BladePath{FVector3{-0.45f, 0.88f, 0.15f}, FVector3{-0.92f, -0.05f, 0.4f}, FVector3{0.f, -0.08f, 1.f}, FVector3{0.9f, -0.05f, 0.4f}};
	const auto Sample = [](const std::array<FVector3, 4>& Path, const float Along)
	{
		const float Segment = std::clamp(Along, 0.f, 1.f) * 3.f;
		const auto Index = std::min(static_cast<std::size_t>(Segment), std::size_t{2});
		const float T = Segment - static_cast<float>(Index);
		const FVector3& P0 = Path[Index == 0 ? 0 : Index - 1];
		const FVector3& P1 = Path[Index];
		const FVector3& P2 = Path[Index + 1];
		const FVector3& P3 = Path[std::min(Index + 2, std::size_t{3})];
		return (P1 * 2.f + (P2 - P0) * T + (P0 * 2.f - P1 * 5.f + P2 * 4.f - P3) * (T * T) + (P1 * 3.f - P0 - P2 * 3.f + P3) * (T * T * T)).Normalized();
	};

	const float Phase = Machine.GetActivePhase();
	const float Weight = std::clamp(std::min(Phase / 0.12f, (1.f - Phase) / 0.15f), 0.f, 1.f);
	const float Time = std::clamp((Phase - 0.08f) / 0.74f, 0.f, 1.f);
	const float Along = Time * Time * (3.f - 2.f * Time);

	// The shoulders turn with the cut, which sells it from behind.
	if (SpineJoint)
	{
		constexpr float TwistRadians = 0.5f;
		Pose.RotateJoint(*SpineJoint, FQuaternion::FromAxisAngle(FVector3::Up(), TwistRadians * (2.f * Along - 1.f) * Weight));
	}

	const std::span<const FMatrix4> Joints = Pose.GetModelTransforms();
	const auto At = [&Joints](const std::size_t Joint)
	{
		return Joints[Joint].TransformPosition({});
	};

	const FVector3 Shoulder = At(*Arm.Shoulder);
	const float Reach = ((At(*Arm.Elbow) - Shoulder).Length() + (At(*Arm.Wrist) - At(*Arm.Elbow)).Length()) * 0.85f;
	const FVector3 Blade = Sample(BladePath, Along);
	const FVector3 Lead = Sample(BladePath, Along + 0.03f) - Sample(BladePath, Along - 0.03f);

	// The blade points along the path and its edge, which runs along the fingers, leads the cut.
	const FVector3 Grip = (At(*GripJoints[2]) - At(*GripJoints[3])).Normalized();
	const FQuaternion Aim = FQuaternion::Between(Grip, Blade);
	const FVector3 Fingers = Aim.RotateVector(At(*GripJoints[1]) - At(*Arm.Wrist));
	const FVector3 EdgeFrom = Fingers - Blade * Fingers.Dot(Blade);
	const FVector3 EdgeTo = Lead - Blade * Lead.Dot(Blade);
	const FQuaternion Full = (EdgeFrom.LengthSquared() > 1.e-6f && EdgeTo.LengthSquared() > 1.e-6f ? FQuaternion::Between(EdgeFrom.Normalized(), EdgeTo.Normalized()) : FQuaternion::Identity()) * Aim;
	const std::array Limb{FLimbIkTarget{.Root = *Arm.Shoulder, .Middle = *Arm.Elbow, .Tip = *Arm.Wrist, .TipTarget = Shoulder + Sample(HandPath, Along) * Reach, .TipExtraTurn = ScaledTurn(Full, Weight), .Weight = Weight, .BendFallback = FVector3{0.f, -0.8f, -0.6f}, .bForceBend = true}};
	Pose.SolveLimbs(std::nullopt, {}, Limb);
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

void FPlayerCharacter::UpdateFootPlacement(const FMatrix4& BodyModel, const float DeltaSeconds, const bool bEnabled, const FFootGroundCast& Cast)
{
	// A body cut below the hips has no whole legs to place.
	const bool bLegs = PelvisJoint && std::ranges::all_of(Feet, [this](const FFootPlacement& Foot)
	{
		return Foot.Hip && Foot.Knee && Foot.Ankle && (!Remaining || Carries(*Foot.Ankle));
	});

	const bool bPlace = bEnabled && bLegs && RagdollWeight == 0.f;
	FootWeight = std::clamp(FootWeight + (bPlace ? 10.f : -10.f) * DeltaSeconds, 0.f, 1.f);
	const auto Inverse = TryInverseAffine(BodyModel);
	if (!bPlace || !Inverse || !EvaluatePose())
	{
		return;
	}

	constexpr float SearchHeight = 0.6f;
	constexpr float MaximumOffset = 0.5f;
	const float Ease = 1.f - std::exp(-18.f * DeltaSeconds);
	const float Origin = BodyModel(1, 3);
	const std::span<const FMatrix4> Joints = Pose.GetModelTransforms();
	float Lowest = 0.f;
	for (FFootPlacement& Foot : Feet)
	{
		const FVector3 Ankle = BodyModel.TransformPosition(Joints[*Foot.Ankle].TransformPosition({}));
		const std::optional<FFootGround> Ground = Cast(Ankle, Origin + SearchHeight, Origin - SearchHeight);
		// Feet are animated for flat ground at the body's origin, so a foot moves by how far the ground under it differs from that.
		const float Height = Ground ? std::clamp(Ground->Height - Origin, -MaximumOffset, MaximumOffset) : 0.f;
		const FVector3 Normal = Ground && Ground->Normal.Y > 0.6f ? Inverse->TransformVector(Ground->Normal).Normalized() : FVector3::Up();
		Foot.Offset = Foot.Offset + (Inverse->TransformVector({0.f, Height, 0.f}) - Foot.Offset) * Ease;
		Foot.Normal = (Foot.Normal + (Normal - Foot.Normal) * Ease).Normalized();
		Lowest = std::min(Lowest, Foot.Offset.Y);
	}

	PelvisOffset = FVector3{0.f, Lowest, 0.f};
}

std::optional<FVector3> FPlayerCharacter::FindHandContact(const FVector3& Wall, const FVector3& Normal, const FVector3& Forward, const float Distance, const FWallCast& Cast)
{
	const auto SurfaceAt = [&Normal, &Cast](const FVector3& Point) -> std::optional<FVector3>
	{
		const std::optional<FWallHit> Hit = Cast(Point + Normal * 0.2f, Point - Normal * 0.15f);
		return Hit && Hit->Normal.Dot(Normal) > 0.85f ? std::optional(Hit->Point) : std::nullopt;
	};

	const FVector3 Along = Normal.Cross(FVector3::Up()).Normalized();
	const FVector3 Ahead = (Forward - Normal * Forward.Dot(Normal)).Normalized();
	const FVector3 Low = Wall - FVector3::Up() * 0.12f;
	const float Lead = 0.1f + std::clamp(0.5f - Distance, 0.f, 0.3f) * 0.6f;
	for (const float Offset : {Lead, 0.f})
	{
		const FVector3 Center = Low + Ahead * Offset;
		const std::optional<FVector3> Hit = SurfaceAt(Center);
		if (Hit && SurfaceAt(Center + Along * 0.1f) && SurfaceAt(Center - Along * 0.1f) && SurfaceAt(Center + FVector3::Up() * 0.12f))
		{
			return Hit;
		}
	}

	return std::nullopt;
}

void FPlayerCharacter::UpdateWallReach(const FMatrix4& BodyModel, const float DeltaSeconds, const bool bEnabled, const FWallCast& Cast)
{
	const bool bRelaxed = std::ranges::contains(RelaxedStates, Machine.GetActiveState());
	const bool bReact = bEnabled && bRelaxed && RagdollWeight == 0.f;
	const auto Inverse = TryInverseAffine(BodyModel);
	if (bReact && Inverse && WallCastCountdown == 0 && EvaluatePose())
	{
		constexpr float Reach = 0.75f;
		constexpr float FullReach = 0.5f;
		const FVector3 Forward = BodyModel.TransformVector({0.f, 0.f, 1.f}).Normalized();
		const FVector3 Left = BodyModel.TransformVector({1.f, 0.f, 0.f}).Normalized();
		for (std::size_t Side = 0; Side < Arms.size(); ++Side)
		{
			FArmReach& Arm = Arms[Side];
			Arm.Wanted = 0.f;
			// A cut arm has nothing to move.
			if (!Arm.Shoulder || !Arm.Elbow || !Arm.Wrist || (Remaining && !Carries(*Arm.Wrist)))
			{
				continue;
			}

			const FVector3 Shoulder = BodyModel.TransformPosition(Pose.GetModelTransforms()[*Arm.Shoulder].TransformPosition({}));
			const FVector3 Out = Left * (Side == 0 ? 1.f : -1.f);
			std::optional<FWallHit> Nearest;
			for (const FVector3& Direction : {Out, (Out + Forward).Normalized()})
			{
				const std::optional<FWallHit> Hit = Cast(Shoulder, Shoulder + Direction * Reach);
				// Only walls: a floor or ceiling is not something to lean a hand on.
				if (Hit && std::abs(Hit->Normal.Y) < 0.4f && (!Nearest || Hit->Distance < Nearest->Distance))
				{
					Nearest = Hit;
				}
			}

			if (!Nearest)
			{
				continue;
			}

			FVector3 Contact = Nearest->Point;
			if (Side == 0)
			{
				// A hand needs a surface as big as itself: a post or a corner would leave it wrapped around the edge. Closer walls take the hand
				// farther ahead, so the elbow has room.
				const std::optional<FVector3> Fits = FindHandContact(Nearest->Point, Nearest->Normal, Forward, Nearest->Distance, Cast);
				if (!Fits)
				{
					continue;
				}

				Contact = *Fits;
			}

			const FVector3 Point = Inverse->TransformPosition(Contact);
			const FVector3 Normal = Inverse->TransformVector(Nearest->Normal).Normalized();
			Arm.Point = Arm.Weight > 0.f ? Arm.Point : Point;
			Arm.Normal = Arm.Weight > 0.f ? Arm.Normal : Normal;
			Arm.Wanted = std::clamp((Reach - Nearest->Distance) / (Reach - FullReach), 0.f, 1.f);
			// The easing below moves toward these.
			Arm.PointTarget = Point;
			Arm.NormalTarget = Normal;
		}
	}

	WallCastCountdown = (WallCastCountdown + 1) % 3;
	const float Ease = 1.f - std::exp(-14.f * DeltaSeconds);
	for (FArmReach& Arm : Arms)
	{
		const float Goal = bReact ? Arm.Wanted : 0.f;
		const float Step = 7.f * DeltaSeconds;
		Arm.Weight = std::clamp(Goal, Arm.Weight - Step, Arm.Weight + Step);
		Arm.Point = Arm.Point + (Arm.PointTarget - Arm.Point) * Ease;
		Arm.Normal = Arm.Normal + (Arm.NormalTarget - Arm.Normal) * Ease;
		Arm.Normal = Arm.Normal.LengthSquared() > 1.e-6f ? Arm.Normal.Normalized() : Arm.NormalTarget;
	}
}

void FPlayerCharacter::ApplyWallReach()
{
	if ((Arms[0].Weight <= 0.f && Arms[1].Weight <= 0.f) || RagdollWeight > 0.f)
	{
		return;
	}

	// Rotating about +Z takes the spine's top toward -X, away from a wall on the left; a wall on the right leans it the other way.
	if (SpineJoint)
	{
		constexpr float LeanRadians = 0.08f;
		Pose.RotateJoint(*SpineJoint, FQuaternion::FromAxisAngle({0.f, 0.f, 1.f}, LeanRadians * (Arms[0].Weight - Arms[1].Weight)));
	}

	const std::span<const FMatrix4> Joints = Pose.GetModelTransforms();
	const auto At = [&Joints](const std::size_t Joint)
	{
		return Joints[Joint].TransformPosition({});
	};

	std::array<FLimbIkTarget, 2> Limbs{};
	std::size_t Count = 0;
	const FArmReach& Free = Arms[0];
	if (Free.Weight > 0.f)
	{
		// The elbow drops down and back, as an arm does reaching for a wall, whatever small bend the animation has. The wrist stays a hand's
		// thickness off the surface.
		FLimbIkTarget Limb{.Root = *Free.Shoulder, .Middle = *Free.Elbow, .Tip = *Free.Wrist, .TipTarget = Free.Point + Free.Normal * 0.075f, .Weight = Free.Weight, .BendFallback = FVector3{0.f, -0.8f, -0.6f}, .bForceBend = true};
		if (LeftFingers[0] && LeftFingers[1] && LeftFingers[2])
		{
			// Fingers crossed with index-to-little gives the palm of a left hand. The palm goes flat on the wall, fingers up and forward along it.
			const FVector3 Fingers = (At(*LeftFingers[0]) - At(*Free.Wrist)).Normalized();
			const FVector3 Across = At(*LeftFingers[1]) - At(*LeftFingers[2]);
			const FQuaternion Flat = FQuaternion::Between(Fingers.Cross(Across).Normalized(), -Free.Normal);
			const FVector3 Reach = FVector3{0.f, 0.7f, 0.7f} - Free.Normal * FVector3{0.f, 0.7f, 0.7f}.Dot(Free.Normal);
			const FVector3 Turned = Flat.RotateVector(Fingers);
			const FVector3 InPlane = Turned - Free.Normal * Turned.Dot(Free.Normal);
			const FQuaternion Full = (InPlane.LengthSquared() > 1.e-6f && Reach.LengthSquared() > 1.e-6f ? FQuaternion::Between(InPlane.Normalized(), Reach.Normalized()) : FQuaternion::Identity()) * Flat;
			Limb.TipExtraTurn = ScaledTurn(Full, Free.Weight);
		}

		Limbs[Count++] = Limb;
	}

	const FArmReach& Sword = Arms[1];
	if (Sword.Weight > 0.f)
	{
		// The sword hand stays where it is unless the wall is in the way, then moves out just far enough.
		constexpr float Margin = 0.12f;
		const FVector3 Wrist = At(*Sword.Wrist);
		const float Clearance = (Wrist - Sword.Point).Dot(Sword.Normal);
		Limbs[Count++] = {.Root = *Sword.Shoulder, .Middle = *Sword.Elbow, .Tip = *Sword.Wrist, .TipTarget = Wrist + Sword.Normal * std::max(Margin - Clearance, 0.f), .Weight = Sword.Weight, .BendFallback = {0.f, 0.f, -1.f}};
	}

	Pose.SolveLimbs(std::nullopt, {}, std::span(Limbs).first(Count));
}

void FPlayerCharacter::ApplyFootPlacement()
{
	if (FootWeight <= 0.f || RagdollWeight > 0.f || !PelvisJoint || !Feet[0].Ankle || !Feet[1].Ankle)
	{
		return;
	}

	const std::span<const FMatrix4> Joints = Pose.GetModelTransforms();
	std::array<FLimbIkTarget, 2> Legs{};
	for (std::size_t Index = 0; Index < Feet.size(); ++Index)
	{
		const FFootPlacement& Foot = Feet[Index];
		// A lifted foot follows its animation; only a planted one lies flat on the ground.
		const float Lift = Joints[*Foot.Ankle].TransformPosition({}).Y - Foot.RestHeight;
		const float Planted = 1.f - std::clamp((Lift - 0.03f) / 0.17f, 0.f, 1.f);
		const FVector3 Up = FVector3::Up();
		Legs[Index] = {
		    .Root = *Foot.Hip,
		    .Middle = *Foot.Knee,
		    .Tip = *Foot.Ankle,
		    .TipTarget = Joints[*Foot.Ankle].TransformPosition({}) + Foot.Offset * FootWeight,
		    .TipTurnTo = Up + (Foot.Normal - Up) * (Planted * FootWeight),
		};
	}

	Pose.SolveLimbs(PelvisJoint, PelvisOffset * FootWeight, Legs);
}

std::expected<void, FGameError> FPlayerCharacter::Skin(const bool bFirstPerson)
{
	if (auto Evaluated = EvaluatePose(); !Evaluated)
	{
		return Evaluated;
	}

	ApplyFootPlacement();
	ApplyWallReach();

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

		// A part stays while the piece carries its joint or any joint along it, such as the chest by spine_03 once a cut took spine_02's own
		// skin; without it, the head and arms hung on the chest would move apart and stretch the skin between them.
		const auto KeepsAlong = [&]
		{
			const auto Parents = Library.GetJointParents();
			for (std::int16_t Along = *End < Parents.size() ? Parents[*End] : -1; Along >= 0; Along = Parents[static_cast<std::size_t>(Along)])
			{
				if (Keep(static_cast<std::size_t>(Along)))
				{
					return true;
				}

				if (static_cast<std::size_t>(Along) == *Joint)
				{
					break;
				}
			}

			return false;
		};

		if (Keep && !Keep(*Joint) && !KeepsAlong())
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

std::vector<FSkinnedMeshPiece> SplitJoinedParts(const FSkinnedMeshPiece& Piece, const FAnimationLibrary& Library, const std::vector<bool>& Skinned, const std::optional<std::size_t> Anchor)
{
	// Joints join their parent unless the parent is a bone that skins part of the whole body but none of this piece: the gap a cut left.
	// Bones that never skin anything, such as clavicles or helpers, join across.
	const auto Parents = Library.GetJointParents();
	const std::size_t JointCount = Parents.size();
	const std::vector<bool> Carried = FindCarriedJoints(Piece, JointCount);
	std::vector<std::size_t> Group(JointCount);
	for (std::size_t Joint = 0; Joint < JointCount; ++Joint)
	{
		const std::int16_t Parent = Parents[Joint];
		const auto Up = static_cast<std::size_t>(Parent);
		Group[Joint] = Parent >= 0 && Up < Joint && (Carried[Up] || Up >= Skinned.size() || !Skinned[Up]) ? Group[Up] : Joint;
	}

	const auto GroupOf = [&](const std::uint32_t Vertex)
	{
		const FCookedSkinInfluence& Influence = Piece.Influences[Vertex];
		const auto Heaviest = static_cast<std::size_t>(std::ranges::max_element(Influence.Weights) - Influence.Weights.begin());
		const std::size_t Joint = Influence.Joints[Heaviest];
		return Joint < JointCount ? Group[Joint] : 0;
	};

	// Each triangle goes with the group of its first corner, so a seam stays whole on one side.
	std::map<std::size_t, std::vector<std::uint32_t>> Triangles;
	for (const FCookedMeshSection& Section : Piece.Sections)
	{
		for (std::uint32_t Index = Section.FirstIndex; Index + 2 < Section.FirstIndex + Section.IndexCount; Index += 3)
		{
			Triangles[GroupOf(Piece.Indices[Index])].push_back(Index);
		}
	}

	if (Triangles.size() < 2)
	{
		return {};
	}

	const auto Extract = [&Piece](const std::vector<std::uint32_t>& Firsts)
	{
		FSkinnedMeshPiece Part;
		std::vector<std::uint32_t> Map(Piece.Vertices.size(), UINT32_MAX);
		for (const FCookedMeshSection& Section : Piece.Sections)
		{
			const auto First = static_cast<std::uint32_t>(Part.Indices.size());
			for (const std::uint32_t Triangle : Firsts)
			{
				if (Triangle < Section.FirstIndex || Triangle >= Section.FirstIndex + Section.IndexCount)
				{
					continue;
				}

				for (std::uint32_t Corner = Triangle; Corner < Triangle + 3; ++Corner)
				{
					const std::uint32_t Vertex = Piece.Indices[Corner];
					if (Map[Vertex] == UINT32_MAX)
					{
						Map[Vertex] = static_cast<std::uint32_t>(Part.Vertices.size());
						Part.Vertices.push_back(Piece.Vertices[Vertex]);
						Part.Influences.push_back(Piece.Influences[Vertex]);
					}

					Part.Indices.push_back(Map[Vertex]);
				}
			}

			if (Part.Indices.size() > First)
			{
				Part.Sections.push_back({.FirstIndex = First, .IndexCount = static_cast<std::uint32_t>(Part.Indices.size()) - First, .Material = Section.Material});
			}
		}

		return Part;
	};

	// The anchor's group comes first, or the largest without one; groups too small to fall on their own stay with it.
	const std::size_t AnchorGroup = Anchor && *Anchor < JointCount ? Group[*Anchor] : 0;
	auto Main = Anchor && Triangles.contains(AnchorGroup) ? Triangles.find(AnchorGroup) : std::ranges::max_element(Triangles, {}, [](const auto& Entry)
	{
		return Entry.second.size();
	});

	std::vector<std::uint32_t> MainTriangles = Main->second;
	std::vector<FSkinnedMeshPiece> Parts(1);
	for (const auto& [Key, Firsts] : Triangles)
	{
		if (Key == Main->first)
		{
			continue;
		}

		FSkinnedMeshPiece Part = Extract(Firsts);
		if (IsSliver(Part))
		{
			MainTriangles.insert(MainTriangles.end(), Firsts.begin(), Firsts.end());
			continue;
		}

		Parts.push_back(std::move(Part));
	}

	if (Parts.size() < 2)
	{
		return {};
	}

	std::ranges::sort(MainTriangles);
	Parts.front() = Extract(MainTriangles);
	return Parts;
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

	if (auto Replaced = Replace(std::move(Kept), Cut); !Replaced)
	{
		return std::unexpected(Replaced.error());
	}

	return std::move(Cut);
}

std::expected<void, FGameError> FPlayerCharacter::Replace(FSkinnedMeshPiece Kept, const FSkinnedMeshPiece& Off)
{
	// Validated even without a device to upload them, so headless sessions catch a cut that could not draw.
	FCookedModel KeptModel = MakeCutModel(*Model, Kept);
	for (const FCookedModel& Piece : {KeptModel, MakeCutModel(*Model, Off)})
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
	return {};
}

std::expected<std::vector<FSkinnedMeshPiece>, FGameError> FPlayerCharacter::DetachStranded()
{
	if (!Remaining || !PelvisJoint)
	{
		return std::vector<FSkinnedMeshPiece>{};
	}

	std::vector<FSkinnedMeshPiece> Parts = SplitJoinedParts(*Remaining, *Library, FindCarriedJoints(Whole, Library->GetJointCount()), *PelvisJoint);
	if (Parts.size() < 2)
	{
		return std::vector<FSkinnedMeshPiece>{};
	}

	FSkinnedMeshPiece Kept = std::move(Parts.front());
	Parts.erase(Parts.begin());
	for (std::size_t Part = 1; Part < Parts.size(); ++Part)
	{
		if (auto Valid = ValidateCookedModel(MakeCutModel(*Model, Parts[Part])); !Valid)
		{
			return std::unexpected(FGameError{std::format("A cut produced an invalid piece: {}", Valid.error().Message)});
		}
	}

	if (auto Replaced = Replace(std::move(Kept), Parts.front()); !Replaced)
	{
		return std::unexpected(Replaced.error());
	}

	return Parts;
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
