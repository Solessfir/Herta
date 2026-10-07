#pragma once

#include "Herta/Assets/CookedAsset.h"
#include "Herta/Level/World.h"
#include "Herta/Physics/PhysicsWorld.h"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace Herta
{
inline constexpr float SoftBodyVertexSpacing = 0.1f;

// Simulation topology in the entity's local space. A rope hangs along -Y from the origin, a cloth hangs along -Y below its top edge centered on X, and a ball is centered on the origin.
struct FSoftBodyTopology
{
	std::vector<FVector3> Vertices;
	std::vector<std::uint32_t> Pinned;
	std::vector<std::array<std::uint32_t, 2>> StretchEdges;
	std::vector<std::array<std::uint32_t, 2>> BendEdges;
	// Outward, counter-clockwise triangles; empty for ropes.
	std::vector<std::array<std::uint32_t, 3>> Faces;
};

[[nodiscard]] FSoftBodyTopology BuildSoftBodyTopology(const FSoftBodyComponent& SoftBody);

// Physics settings over the topology; the spans reference Topology and WorldVertices, which must outlive the call that consumes them.
[[nodiscard]] FPhysicsSoftBodySettings MakeSoftBodyPhysicsSettings(const FSoftBodyComponent& SoftBody, const FSoftBodyTopology& Topology, std::span<const FVector3> WorldVertices);

// Renderable surface for topology vertex positions given in local space: a tube for ropes, a two-sided sheet for cloth, and a smooth shell for balls.
[[nodiscard]] FCookedModel BuildSoftBodyModel(const FSoftBodyComponent& SoftBody, const FSoftBodyTopology& Topology, std::span<const FVector3> Positions);
}
