#pragma once

#include "Herta/Animation/Animation.h"
#include "Herta/Animation/AnimationStateMachine.h"
#include "Herta/Assets/CookedAsset.h"
#include "Herta/GameFramework/GameSession.h"
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
	bool bJumped = false;
};

// The animated body the player plays as: its pose, state machine, and own copy of the mesh that is skinned on the CPU every frame.
class FPlayerCharacter
{
public:
	// Device may be null, as in headless tests; the character then animates without a mesh to draw.
	[[nodiscard]] static std::expected<std::unique_ptr<FPlayerCharacter>, FGameError> Create(std::shared_ptr<const FCookedModel> Model, IGraphicsDevice* Device);

	void Restart();
	void Update(float DeltaSeconds, const FPlayerMotion& Motion);
	// Skins the current pose into the vertex buffer. A first-person view hides the head the camera sits inside.
	[[nodiscard]] std::expected<void, FGameError> Skin(bool bHideHead);

	[[nodiscard]] const FRenderMesh* GetMesh() const noexcept;
	[[nodiscard]] std::span<const FCookedVertex> GetVertices() const noexcept;
	[[nodiscard]] const FAnimationStateMachine& GetStateMachine() const noexcept;
	// Model-space position of the head joint, which a first-person camera sits at.
	[[nodiscard]] std::optional<FVector3> GetHeadPosition() const noexcept;

private:
	FPlayerCharacter(std::shared_ptr<const FCookedModel> InModel, std::shared_ptr<const FAnimationLibrary> InLibrary, FAnimationStateMachine InMachine);

	std::shared_ptr<const FCookedModel> Model;
	std::shared_ptr<const FAnimationLibrary> Library;
	FAnimationStateMachine Machine;
	FAnimationStateMachine StartMachine;
	FAnimationPose Pose;
	std::shared_ptr<const FRenderMesh> Mesh;
	std::vector<FCookedVertex> Vertices;
	std::optional<std::size_t> HeadJoint;

	std::optional<std::size_t> SpeedParameter;
	std::optional<std::size_t> GroundedParameter;
	std::optional<std::size_t> CrouchedParameter;
	std::optional<std::size_t> SlidingParameter;
	std::optional<std::size_t> JumpParameter;
};

// The locomotion the Sandbox Mannequin plays until levels author their own graph: an idle to sprint blend, crouching, sliding, and jumps.
[[nodiscard]] FAnimationStateMachineDesc MakeDefaultLocomotion();
}
