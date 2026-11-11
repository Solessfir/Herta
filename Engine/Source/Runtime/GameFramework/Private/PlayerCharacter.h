#pragma once

#include "Herta/Animation/Animation.h"
#include "Herta/Animation/AnimationStateMachine.h"
#include "Herta/Animation/MeshSlice.h"
#include "Herta/Assets/CookedAsset.h"
#include "Herta/GameFramework/GameSession.h"
#include "Herta/Physics/PhysicsWorld.h"
#include "Herta/Renderer/MeshRenderer.h"

#include <array>
#include <functional>
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

// What is under a foot: the ground's height in world space and the way it faces.
struct FFootGround
{
	float Height = 0.f;
	FVector3 Normal = FVector3::Up();
};

// Finds the ground under a world-space point, searching from Top down to Bottom.
using FFootGroundCast = std::function<std::optional<FFootGround>(const FVector3& Point, float Top, float Bottom)>;

// The animated body the player plays as: its pose, state machine, and own copies of the mesh, skinned on the CPU every frame.
// In first person it keeps two: the intact body for shadows and ray-traced lighting, and a copy without head and neck for the camera, which sits where the head would be.
class FPlayerCharacter
{
public:
	// Device may be null, as in headless tests; the character then animates without a mesh to draw.
	[[nodiscard]] static std::expected<std::unique_ptr<FPlayerCharacter>, FGameError> Create(std::shared_ptr<const FCookedModel> Model, IGraphicsDevice* Device);

	void Restart();
	void Update(float DeltaSeconds, const FPlayerMotion& Motion);
	// Evaluates the state machine's pose without skinning, so gameplay can read joints, such as the sword hand, in the same update.
	[[nodiscard]] std::expected<void, FGameError> EvaluatePose();
	// The right hand's grip in model space: the origin in the palm, +Y along the closed fist from the little finger to the index, which is
	// where a held blade points, +X along the fingers, where its edge leads, and +Z out of the hand. Built from the finger joints, so it does
	// not depend on how the rig orients its hand bone. Empty without them.
	[[nodiscard]] std::optional<FMatrix4> GetGripFrame() const;
	// Skins the current pose into the intact body, and in first person also into the headless camera body.
	[[nodiscard]] std::expected<void, FGameError> Skin(bool bFirstPerson);
	// Finds the ground under each foot and eases the legs onto it, for the next Skin: uneven ground bends the legs and the pelvis drops so the
	// lower foot can reach. Only what is drawn changes, never what gameplay reads from EvaluatePose. Disabled, it fades out without casting.
	void UpdateFootPlacement(const FMatrix4& BodyModel, float DeltaSeconds, bool bEnabled, const FFootGroundCast& Cast);

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
	// the next, hinged on its parent part. A cut body gets parts only for the joints it still carries. Empty when the skeleton lacks one of the joints.
	[[nodiscard]] std::vector<FPhysicsRagdollPart> BuildRagdoll(const FMatrix4& BodyToPhysics);
	// From the next Skin on, the joints BuildRagdoll drives follow Parts, in its order, blended over the animation by Weight, where ModelInverse
	// maps physics space back to the body's model space. Empty Parts or zero Weight returns the body to animation.
	void SetRagdollPose(std::span<const FPhysicsBodyTransform> Parts, const FMatrix4& ModelInverse, float Weight);
	// Cuts the body along a plane in its model space, through the pose of the last Skin. The part holding the pelvis stays on the body and keeps
	// animating; the other comes back baked in that pose, in model space, closed with a wound cap. Empty when the plane misses the body or would
	// take the pelvis side's whole mesh.
	[[nodiscard]] std::expected<std::optional<FSkinnedMeshPiece>, FGameError> Slice(const FMeshSlicePlane& Plane);
	// Cuts off the limb below Joint where its skin weights pass to the rest of the body, and binds the stump wholly to the joint's parent so it
	// does not stretch as the missing limb's joints keep animating.
	[[nodiscard]] std::expected<std::optional<FSkinnedMeshPiece>, FGameError> Sever(std::size_t Joint);
	// After a cut, takes the parts of the body no longer joined to the pelvis, such as a hand left behind when a slice took its forearm, one
	// piece per part; see SplitJoinedParts. Their skin would otherwise float where the animation holds them.
	[[nodiscard]] std::expected<std::vector<FSkinnedMeshPiece>, FGameError> DetachStranded();
	// Takes everything left of the body, such as when a dead character collapses as a ragdoll, leaving nothing to draw until Restart.
	[[nodiscard]] std::optional<FSkinnedMeshPiece> DetachAll();
	[[nodiscard]] bool IsEmpty() const;
	[[nodiscard]] const std::shared_ptr<const FCookedModel>& GetCookedModel() const noexcept;
	[[nodiscard]] const std::shared_ptr<const FAnimationLibrary>& GetLibrary() const noexcept;
	// The joints' model-space pose and skinning matrices of the last Skin or EvaluatePose.
	[[nodiscard]] std::span<const FMatrix4> GetModelTransforms() const noexcept;
	[[nodiscard]] std::span<const FMatrix4> GetSkinningMatrices() const noexcept;
	// The severable joint, such as an elbow or knee, whose cut lies nearest a model-space point within Reach meters, among those still on the body.
	[[nodiscard]] std::optional<std::size_t> FindSeverableJoint(const FVector3& Point, float Reach) const;
	// Whether any of the body's geometry still follows Joint, such as the head after a cut through the neck.
	[[nodiscard]] bool Carries(std::size_t Joint) const;
	[[nodiscard]] std::optional<std::size_t> FindJoint(std::string_view Name) const;
	[[nodiscard]] std::string_view GetJointName(std::size_t Joint) const;
	// Drives a parameter the session does not otherwise set, such as a hit reaction; unknown names are ignored.
	void FireTrigger(std::string_view Parameter);
	void SetBool(std::string_view Parameter, bool bValue);
	// How long the named state's clip plays, or empty when the state machine has no such state.
	[[nodiscard]] std::optional<float> GetStateSeconds(std::string_view Name) const noexcept;

private:
	FPlayerCharacter(std::shared_ptr<const FCookedModel> InModel, std::shared_ptr<const FAnimationLibrary> InLibrary, FAnimationStateMachine InMachine);

	[[nodiscard]] const FSkinnedMeshPiece& GetPiece() const;
	// Keeps one half of a cut on the body and returns the other, in bind space. Cuts that would leave a sliver on either side do not happen.
	[[nodiscard]] std::expected<std::optional<FSkinnedMeshPiece>, FGameError> Keep(FMeshSliceResult Sliced, bool bKeepFront);
	// Makes Kept what is left of the body after Off leaves it, checking both draw.
	[[nodiscard]] std::expected<void, FGameError> Replace(FSkinnedMeshPiece Kept, const FSkinnedMeshPiece& Off);
	// Skins the current piece with the pose last evaluated, so a cut sees exactly the pose it is made in.
	void SkinForCut();

	std::shared_ptr<const FCookedModel> Model;
	std::shared_ptr<const FAnimationLibrary> Library;
	IGraphicsDevice* Device = nullptr;
	// The whole mesh as a piece, and what is left of it once cut; empty while the body is whole.
	FSkinnedMeshPiece Whole;
	std::optional<FSkinnedMeshPiece> Remaining;
	std::shared_ptr<const FRenderMesh> WholeMesh;
	std::optional<std::size_t> PelvisJoint;
	// The right hand's wrist, middle, index, and little finger roots, which the grip frame is built from.
	std::array<std::optional<std::size_t>, 4> GripJoints{};
	FAnimationStateMachine Machine;
	FAnimationStateMachine StartMachine;
	std::vector<std::string> Events;
	FAnimationPose Pose;
	std::shared_ptr<const FRenderMesh> Mesh;
	std::vector<FCookedVertex> Vertices;
	std::shared_ptr<const FRenderMesh> CameraMesh;
	std::vector<FCookedVertex> CameraVertices;
	struct FFootPlacement
	{
		std::optional<std::size_t> Hip{};
		std::optional<std::size_t> Knee{};
		std::optional<std::size_t> Ankle{};
		// How far the ankle's rest height sits above the sole, which tells a planted foot from a lifted one.
		float RestHeight = 0.1f;
		// In model space, eased.
		FVector3 Offset{};
		FVector3 Normal = FVector3::Up();
	};

	void ApplyFootPlacement();

	std::array<FFootPlacement, 2> Feet{};
	FVector3 PelvisOffset{};
	float FootWeight = 0.f;
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

// A ragdoll fitted to a pose: its parts, the joint each drives, and that joint's transform relative to its part's capsule.
struct FRagdollFit
{
	std::vector<FPhysicsRagdollPart> Parts{};
	std::vector<std::size_t> Joints{};
	std::vector<FMatrix4> Offsets{};
};

// Fits the ragdoll profile's capsules to a pose, where BodyToPhysics maps model space to physics space. Keep, when given, chooses which parts by
// the joint they drive; a part whose parent part is left out hangs from the first part kept, which is the root. Empty when the skeleton lacks
// a profile joint.
[[nodiscard]] FRagdollFit FitRagdoll(const FAnimationLibrary& Library, std::span<const FMatrix4> ModelTransforms, const FMatrix4& BodyToPhysics, const std::function<bool(std::size_t Joint)>& Keep = nullptr);
// The model a cut piece draws: its geometry with the source model's materials and textures, and the wound material after them.
[[nodiscard]] FCookedModel MakeCutModel(const FCookedModel& Source, const FSkinnedMeshPiece& Piece);
// Too small to simulate or see: fewer than 40 triangles, or under 4 cm across in its bind pose. Cuts that would make one keep the sliver on its
// neighbour instead.
[[nodiscard]] bool IsSliver(const FSkinnedMeshPiece& Piece);
// Whether a plane through posed vertices would leave either side less than 8 cm deep, such as a skim along the surface or a cut down the
// length of a limb, which look like peeled skin rather than a cut.
[[nodiscard]] bool IsThinCut(std::span<const FCookedVertex> Posed, const FMeshSlicePlane& Plane);
// Splits a piece into the parts a cut left joined by bone, one per group of joints that reach each other without crossing a bone Skinned in
// the whole model but carrying none of the piece. The first part holds Anchor, or is the largest without one; parts too small to fall alone
// stay with it. Empty when the piece is one part.
[[nodiscard]] std::vector<FSkinnedMeshPiece> SplitJoinedParts(const FSkinnedMeshPiece& Piece, const FAnimationLibrary& Library, const std::vector<bool>& Skinned, std::optional<std::size_t> Anchor);
// The joints a piece mostly follows: those that weigh more than half on at least eight of its vertices.
[[nodiscard]] std::vector<bool> FindCarriedJoints(const FSkinnedMeshPiece& Piece, std::size_t JointCount);

// The locomotion the Sandbox Mannequin plays until levels author their own graph: an idle to sprint blend, crouching, slides, jumps, wall
// runs, wall jumps, and mantles.
[[nodiscard]] FAnimationStateMachineDesc MakeDefaultLocomotion();
}
