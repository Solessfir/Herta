#pragma once

#include "Herta/GameFramework/LevelSimulation.h"
#include "Herta/GameFramework/SoftBodyGeometry.h"
#include "Herta/Level/World.h"
#include "Herta/Math/Matrix.h"

#include <span>
#include <string>
#include <vector>

namespace Herta
{
class FRenderMesh;

struct FLevelSimulationSetup
{
	std::vector<FLevelSimulationBody> Bodies;
	std::vector<FLevelSimulationSoftBody> SoftBodies;
	std::vector<FLevelSimulationConstraint> Constraints;
	// Entities left out and why; the rest of the level still simulates.
	std::vector<std::string> Warnings;
	// Soft body settings point into these, so they live with the setup until Start copies them.
	std::vector<FSoftBodyTopology> Topologies;
	std::vector<std::vector<FVector3>> WorldVertices;
};

// Collision fitted to the mesh bounds. A missing mesh falls back to the cube a loading mesh shows, or a small marker for meshless entities.
[[nodiscard]] FLevelBodyShape GetLevelBodyShape(const FLevelEntity& Entity, const FRenderMesh* Mesh);

// Turns authored components into simulation inputs, indexed like Entities. Meshes and WorldMatrices are parallel to Entities.
[[nodiscard]] FLevelSimulationSetup BuildLevelSimulation(std::span<const FLevelEntity> Entities, std::span<const FMatrix4> WorldMatrices, std::span<const FRenderMesh* const> Meshes);
}
