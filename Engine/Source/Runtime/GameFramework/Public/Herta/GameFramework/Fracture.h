#pragma once

#include "Herta/Assets/CookedAsset.h"
#include "Herta/Math/Vector.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace Herta
{
// One Voronoi cell of a fractured box: a convex polyhedron in the box's local space with outward, counter-clockwise faces.
struct FFractureChunk
{
	std::vector<FVector3> Points;
	std::vector<std::vector<std::uint32_t>> Faces;
	FVector3 Centroid = FVector3::Zero();
	float Volume = 0.f;
	// Rests on the box's bottom face, which anchors it to whatever the box stands on.
	bool bTouchesBottom = false;
};

// Two chunks that share a face, held together until damage breaks the bond.
struct FFractureBond
{
	std::uint32_t First = 0;
	std::uint32_t Second = 0;
	float Area = 0.f;
	FVector3 Center = FVector3::Zero();
};

// A support graph: chunks are nodes and bonds are edges.
struct FFracture
{
	FVector3 HalfExtents = FVector3::One();
	std::vector<FFractureChunk> Chunks;
	std::vector<FFractureBond> Bonds;
};

inline constexpr std::uint32_t MinimumFractureCells = 2;
inline constexpr std::uint32_t MaximumFractureCells = 64;

// Splits a box into Cells Voronoi chunks around seeded random sites. The same inputs give the same chunks on every platform, so a fracture can be
// regenerated wherever it is needed instead of shipped.
[[nodiscard]] FFracture FractureBox(const FVector3& HalfExtents, std::uint32_t Cells, std::uint32_t Seed);
// The chunk as a flat-shaded mesh in the box's local space, with box-projected UVs and one material slot.
[[nodiscard]] FCookedModel BuildFractureChunkModel(const FFracture& Fracture, std::size_t Chunk);
}
