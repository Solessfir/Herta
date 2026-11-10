#pragma once

#include "Herta/Animation/Animation.h"
#include "Herta/Assets/CookedAsset.h"
#include "Herta/Math/Vector.h"

#include <cstdint>
#include <expected>
#include <span>
#include <vector>

namespace Herta
{
// A skinned mesh in its bind pose, shaped like the mesh part of a cooked model, so a slice of a slice works the same way.
struct FSkinnedMeshPiece
{
	std::vector<FCookedVertex> Vertices{};
	std::vector<FCookedSkinInfluence> Influences{};
	std::vector<std::uint32_t> Indices{};
	std::vector<FCookedMeshSection> Sections{};
};

// Points where Dot(Normal, Point) >= Distance are in front. Normal must be unit length.
struct FMeshSlicePlane
{
	FVector3 Normal{0.f, 1.f, 0.f};
	float Distance = 0.f;
};

struct FMeshSliceResult
{
	FSkinnedMeshPiece Back{};
	FSkinnedMeshPiece Front{};
	// Outlines the cut closed; zero when the plane missed the mesh, which leaves one half empty.
	std::size_t CapLoops = 0;
};

// Splits a skinned mesh along a plane through its current pose, so a cut follows a blade wherever the body is. Posed holds the skinned vertices,
// one per bind vertex, and SkinningMatrices the pose they came from; the plane is in the same space. Crossing triangles split, with new vertices
// interpolating bind-space attributes and keeping the four strongest skin weights, so both halves animate from the same skeleton. Each half is
// closed with a cap in WoundMaterial facing out of it, with planar UVs in meters on the cut. Slicing the same pose with the same plane gives the
// same halves, so cuts can replicate from the hit.
[[nodiscard]] std::expected<FMeshSliceResult, FAnimationError> SliceSkinnedMesh(const FSkinnedMeshPiece& Mesh, std::span<const FCookedVertex> Posed, std::span<const FMatrix4> SkinningMatrices, const FMeshSlicePlane& Plane, std::uint32_t WoundMaterial);
// Splits where Sides, one per vertex, changes sign rather than at a plane, such as where skin weights pass from a limb to its parent, so a cut
// follows the weight boundary. Vertices with a side of zero or more are in front. Plane only orients the caps and gives them UVs, so the cut's
// outlines should lie roughly flat across it.
[[nodiscard]] std::expected<FMeshSliceResult, FAnimationError> SplitSkinnedMesh(const FSkinnedMeshPiece& Mesh, std::span<const FCookedVertex> Posed, std::span<const FMatrix4> SkinningMatrices, std::span<const float> Sides, const FMeshSlicePlane& Plane, std::uint32_t WoundMaterial);
// Makes the normal unit length and the tangent unit length and perpendicular to it, as cooked models require of vertices that blending, such as
// interpolation or skinning with several joints, has pulled slightly off.
void OrthonormalizeTangent(FCookedVertex& Vertex);
}
