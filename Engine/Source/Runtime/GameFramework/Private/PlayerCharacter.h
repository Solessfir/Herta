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
	std::shared_ptr<const FRenderMesh> CameraMesh;
	std::vector<FCookedVertex> CameraVertices;
	std::optional<std::size_t> HeadJoint;
	// The highest joint the camera body hides, usually the neck, so looking down never shows the inside of the collar.
	std::optional<std::size_t> HiddenJoint;

	std::optional<std::size_t> SpeedParameter;
	std::optional<std::size_t> GroundedParameter;
	std::optional<std::size_t> CrouchedParameter;
	std::optional<std::size_t> SlidingParameter;
	std::optional<std::size_t> JumpParameter;
};

// The locomotion the Sandbox Mannequin plays until levels author their own graph: an idle to sprint blend, crouching, sliding, and jumps.
[[nodiscard]] FAnimationStateMachineDesc MakeDefaultLocomotion();
}
