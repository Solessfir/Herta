#pragma once

#include "Herta/Animation/Animation.h"
#include "Herta/Animation/MeshSlice.h"
#include "Herta/GameFramework/GameSession.h"
#include "Herta/Physics/PhysicsWorld.h"
#include "Herta/Renderer/MeshRenderer.h"

#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace Herta
{
class IGraphicsDevice;

// A piece cut off a character: its share of the skinned mesh, posed as it was when cut and from then on moved by physics. A piece that carries
// two or more of the ragdoll profile's joints falls as a ragdoll of them, so a severed arm still bends at the elbow; a smaller piece falls as
// one rigid convex hull. Pieces can be cut again, through the same skinning data, so a cut through a falling half makes two pieces.
class FCharacterPiece
{
public:
	// Mesh is in the character's bind space and PoseTransforms the skeleton's model-space pose when it was cut; ModelToWorld places that model
	// space in the physics world, which here is also the world.
	[[nodiscard]] static std::expected<std::unique_ptr<FCharacterPiece>, FGameError> Create(FPhysicsWorld& World, IGraphicsDevice* Device, std::shared_ptr<const FCookedModel> Model, std::shared_ptr<const FAnimationLibrary> Library, std::span<const FMatrix4> PoseTransforms, FSkinnedMeshPiece Mesh, const FMatrix4& ModelToWorld, const FVector3& Velocity, std::size_t Owner);

	FCharacterPiece(const FCharacterPiece&) = delete;
	FCharacterPiece& operator=(const FCharacterPiece&) = delete;
	FCharacterPiece(FCharacterPiece&&) = delete;
	FCharacterPiece& operator=(FCharacterPiece&&) = delete;
	~FCharacterPiece() = default;

	// Removes its bodies from the world; the piece is then only data.
	[[nodiscard]] std::expected<void, FPhysicsError> Destroy(FPhysicsWorld& World);
	// Poses it from its bodies; a ragdoll piece is skinned again.
	[[nodiscard]] std::expected<void, FPhysicsError> Update(const FPhysicsWorld& World);

	[[nodiscard]] const FRenderMesh* GetMesh() const noexcept;
	// The vertices to draw and cut, posed, in the space GetModel places in the world.
	[[nodiscard]] std::span<const FCookedVertex> GetVertices() const noexcept;
	// Whether the vertices changed since creation, so the renderer must upload them.
	[[nodiscard]] bool IsDeforming() const noexcept;
	[[nodiscard]] const FMatrix4& GetModel() const noexcept;
	[[nodiscard]] std::span<const FMatrix4> GetSkinningMatrices() const noexcept;
	[[nodiscard]] std::span<const FMatrix4> GetModelTransforms() const noexcept;
	[[nodiscard]] const FSkinnedMeshPiece& GetBind() const noexcept;
	[[nodiscard]] const std::shared_ptr<const FCookedModel>& GetCookedModel() const noexcept;
	[[nodiscard]] const std::shared_ptr<const FAnimationLibrary>& GetLibrary() const noexcept;
	[[nodiscard]] FVector3 GetVelocity(const FPhysicsWorld& World) const;
	// The character it came from, which owns its share of the live piece budget.
	[[nodiscard]] std::size_t GetOwner() const noexcept;
	[[nodiscard]] bool IsRagdoll() const noexcept;

private:
	FCharacterPiece() = default;

	std::shared_ptr<const FCookedModel> Model;
	std::shared_ptr<const FAnimationLibrary> Library;
	FSkinnedMeshPiece Bind;
	std::size_t Owner = 0;
	// The pose when cut, which every update starts from before the ragdoll moves its joints.
	std::vector<FMatrix4> BasePose;
	std::unique_ptr<FAnimationPose> Pose;
	std::vector<FCookedVertex> Vertices;
	std::shared_ptr<const FRenderMesh> Mesh;

	// A ragdoll piece keeps its model space where it was cut and moves its joints.
	std::optional<FPhysicsRagdoll> Ragdoll;
	std::vector<std::size_t> Joints;
	std::vector<FMatrix4> Offsets;
	FMatrix4 WorldToModel{};
	std::vector<FPhysicsBodyTransform> Parts;
	std::vector<FMatrix4> JointPose;

	// A rigid piece moves its whole model space with one body.
	FPhysicsBodyId Body{};
	FMatrix4 BodyToModel{};

	FMatrix4 ModelToWorld{};
};
}
