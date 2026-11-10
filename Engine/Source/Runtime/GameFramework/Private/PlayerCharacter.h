#pragma once

#include "Herta/Animation/Animation.h"
#include "Herta/Animation/AnimationStateMachine.h"
#include "Herta/Assets/CookedAsset.h"
#include "Herta/GameFramework/GameSession.h"
#include "Herta/Physics/PhysicsWorld.h"
#include "Herta/Renderer/MeshRenderer.h"

#include <memory>
#include <optional>
#include <vector>

namespace Herta
{
class IGraphicsDevice;

// What the player controller did this step, which drives the character's state machine.
struct FPlayerMotion
{
	float GroundSpeed = 0.f;
	bool bGrounded = true;
	bool bCrouched = false;
	bool bSliding = false;
	bool bWallRunning = false;
	bool bJumped = false;
	bool bWallJumped = false;
	bool bMantled = false;
};

// The animated body the player plays as: its pose, state machine, and own copies of the mesh, skinned on the CPU every frame.
// In first person it keeps two: the intact body for shadows and ray-traced lighting, and a copy without head and neck for the camera, which sits where the head would be.
class FPlayerCharacter
{
public:
	// Device may be null, as in headless tests; the character then animates without a mesh to draw.
	[[nodiscard]] static std::expected<std::unique_ptr<FPlayerCharacter>, FGameError> Create(std::shared_ptr<const FCookedModel> Model, IGraphicsDevice* Device);

	void Restart();
	void Update(float DeltaSeconds, const FPlayerMotion& Motion);
	// Skins the current pose into the intact body, and in first person also into the headless camera body.
	[[nodiscard]] std::expected<void, FGameError> Skin(bool bFirstPerson);

	[[nodiscard]] const FRenderMesh* GetMesh() const noexcept;
	[[nodiscard]] std::span<const FCookedVertex> GetVertices() const noexcept;
	[[nodiscard]] const FRenderMesh* GetCameraMesh() const noexcept;
	[[nodiscard]] std::span<const FCookedVertex> GetCameraVertices() const noexcept;
	[[nodiscard]] const FAnimationStateMachine& GetStateMachine() const noexcept;
	// Animation events from the last update, such as Footstep and Land.
	[[nodiscard]] std::span<const std::string> GetEvents() const noexcept;
	// Model-space position of the head joint, which a first-person camera sits at.
	[[nodiscard]] std::optional<FVector3> GetHeadPosition() const noexcept;
	// How high the current pose lifts its root, which only climbing clips do; the session draws the body that much lower because the capsule
	// already carries it up.
	[[nodiscard]] float GetRootHeight() const noexcept;
	// Ragdoll parts posed like the current pose, where BodyToPhysics maps the body's model space to physics space: a capsule from each driven joint to
	// the next, hinged on its parent part. Empty when the skeleton lacks one of the joints.
	[[nodiscard]] std::vector<FPhysicsRagdollPart> BuildRagdoll(const FMatrix4& BodyToPhysics);
	// From the next Skin on, the joints BuildRagdoll drives follow Parts, in its order, blended over the animation by Weight, where ModelInverse
	// maps physics space back to the body's model space. Empty Parts or zero Weight returns the body to animation.
	void SetRagdollPose(std::span<const FPhysicsBodyTransform> Parts, const FMatrix4& ModelInverse, float Weight);
	// How long the named state's clip plays, or empty when the state machine has no such state.
	[[nodiscard]] std::optional<float> GetStateSeconds(std::string_view Name) const noexcept;

private:
	FPlayerCharacter(std::shared_ptr<const FCookedModel> InModel, std::shared_ptr<const FAnimationLibrary> InLibrary, FAnimationStateMachine InMachine);

	std::shared_ptr<const FCookedModel> Model;
	std::shared_ptr<const FAnimationLibrary> Library;
	FAnimationStateMachine Machine;
	FAnimationStateMachine StartMachine;
	std::vector<std::string> Events;
	FAnimationPose Pose;
	std::shared_ptr<const FRenderMesh> Mesh;
	std::vector<FCookedVertex> Vertices;
	std::shared_ptr<const FRenderMesh> CameraMesh;
	std::vector<FCookedVertex> CameraVertices;
	std::optional<std::size_t> HeadJoint;
	// The highest joint the camera body hides, usually the neck, so looking down never shows the inside of the collar.
	std::optional<std::size_t> HiddenJoint;

	// The joint each ragdoll part drives, the joint's transform relative to its part's capsule, and the joints' model-space pose to apply.
	std::vector<std::size_t> RagdollJoints;
	std::vector<FMatrix4> RagdollOffsets;
	std::vector<FMatrix4> RagdollPose;
	float RagdollWeight = 0.f;

	std::optional<std::size_t> SpeedParameter;
	std::optional<std::size_t> GroundedParameter;
	std::optional<std::size_t> CrouchedParameter;
	std::optional<std::size_t> SlidingParameter;
	std::optional<std::size_t> JumpParameter;
	std::optional<std::size_t> WallJumpParameter;
	std::optional<std::size_t> MantleParameter;
	std::optional<std::size_t> WallRunningParameter;
};

// The locomotion the Sandbox Mannequin plays until levels author their own graph: an idle to sprint blend, crouching, slides, jumps, wall
// runs, wall jumps, and mantles.
[[nodiscard]] FAnimationStateMachineDesc MakeDefaultLocomotion();
}
