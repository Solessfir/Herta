#pragma once

#include "Herta/Assets/CookedAsset.h"
#include "Herta/Math/Matrix.h"
#include "Herta/Math/Vector.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Herta
{
struct FAnimationError
{
	std::string Message;
};

// ozz trusts its archives and reads past the end of a truncated one, so the asset worker seals each archive with its size and hash and the runtime rejects anything that does not match before ozz sees it.
[[nodiscard]] std::vector<std::byte> SealAnimationArchive(std::span<const std::byte> Archive);

// A cooked skeleton and its clips, immutable after creation so every character playing them can share one copy.
class FAnimationLibrary
{
public:
	[[nodiscard]] static std::expected<std::shared_ptr<const FAnimationLibrary>, FAnimationError> Create(const FCookedSkeleton& Skeleton, std::span<const FCookedAnimationClip> Clips);

	FAnimationLibrary(const FAnimationLibrary&) = delete;
	FAnimationLibrary(FAnimationLibrary&&) = delete;
	FAnimationLibrary& operator=(const FAnimationLibrary&) = delete;
	FAnimationLibrary& operator=(FAnimationLibrary&&) = delete;
	~FAnimationLibrary();

	[[nodiscard]] std::size_t GetJointCount() const noexcept;
	[[nodiscard]] std::span<const std::string> GetJointNames() const noexcept;
	// Parents precede children; roots have -1.
	[[nodiscard]] std::span<const std::int16_t> GetJointParents() const noexcept;
	[[nodiscard]] std::span<const FMatrix4> GetInverseBindMatrices() const noexcept;

	[[nodiscard]] std::size_t GetClipCount() const noexcept;
	[[nodiscard]] std::optional<std::size_t> FindClip(std::string_view Name) const noexcept;
	[[nodiscard]] std::string_view GetClipName(std::size_t Clip) const noexcept;
	[[nodiscard]] float GetClipDuration(std::size_t Clip) const noexcept;
	// The root's horizontal travel in model space from one clip time to another. A To before From wraps through the end of the clip once, as a looping clip does.
	[[nodiscard]] FVector3 GetRootMotion(std::size_t Clip, float FromSeconds, float ToSeconds) const noexcept;

private:
	struct FImplementation;

	explicit FAnimationLibrary(std::unique_ptr<FImplementation> InImplementation);

	friend class FAnimationPose;

	std::unique_ptr<FImplementation> Implementation;
};

struct FAnimationLayer
{
	std::size_t Clip = 0;
	// Clamped to the clip; callers wrap looping time themselves.
	float TimeSeconds = 0.f;
	float Weight = 1.f;
	// Multiply Weight on the upper body, the spine and everything above it, and on the rest, so a layer can move only the arms or only
	// the legs.
	float UpperBodyWeight = 1.f;
	float LowerBodyWeight = 1.f;
};

// One character's working memory for sampling and blending. Reuse it every frame; evaluation allocates only when more layers are blended than ever before.
class FAnimationPose
{
public:
	explicit FAnimationPose(std::shared_ptr<const FAnimationLibrary> Library);
	FAnimationPose(const FAnimationPose&) = delete;
	FAnimationPose(FAnimationPose&&) noexcept;
	FAnimationPose& operator=(const FAnimationPose&) = delete;
	FAnimationPose& operator=(FAnimationPose&&) noexcept;
	~FAnimationPose();

	inline static constexpr std::size_t MaximumLayers = 8;

	// Samples each layer and blends them by weight. With no layers, or no positive weight, the result is the bind pose.
	[[nodiscard]] std::expected<void, FAnimationError> Evaluate(std::span<const FAnimationLayer> Layers);

	[[nodiscard]] const std::shared_ptr<const FAnimationLibrary>& GetLibrary() const noexcept;
	// Joint transforms in model space.
	[[nodiscard]] std::span<const FMatrix4> GetModelTransforms() const noexcept;
	// Model transform times inverse bind matrix, per joint: what a skinned vertex multiplies by.
	[[nodiscard]] std::span<const FMatrix4> GetSkinningMatrices() const noexcept;
	// Shrinks a joint and everything below it to the joint's position until the next Evaluate, such as the head a first-person camera sits inside.
	void CollapseJoint(std::size_t Joint) noexcept;
	// Moves Joints to model-space Transforms, such as the ragdoll bodies that drive them, blended over the evaluated pose by Weight from 0 to 1,
	// until the next Evaluate. Joints below them that are not given keep their evaluated transforms relative to their parent.
	void OverrideJoints(std::span<const std::size_t> Joints, std::span<const FMatrix4> Transforms, float Weight);

private:
	struct FImplementation;

	std::unique_ptr<FImplementation> Implementation;
};

// Blends each bind-pose vertex by its four skinning matrices. Normals and tangents use the same matrices, which is exact for rigid and uniformly scaled joints.
void SkinVertices(std::span<const FMatrix4> SkinningMatrices, std::span<const FCookedVertex> Bind, std::span<const FCookedSkinInfluence> Influences, std::span<FCookedVertex> Output) noexcept;
}
