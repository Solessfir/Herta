#pragma once

#include "IrradianceField.h"

namespace Herta
{
struct FRadianceCascadesShaders
{
	FShaderAsset Update;
	FShaderAsset Trace;
	FShaderAsset Near;
	FShaderAsset Blend;
	// The irradiance field's gather and reflections, which read the cascades' probes through the same tiles.
	FShaderAsset Gather;
	FShaderAsset Reflect;
};

// Dynamic diffuse global illumination from world-space Radiance Cascades (Sannikov 2023) over the irradiance field's probes: each clipmap level
// keeps the light of one shell of distances, at four times the directions of the level below, and probes take the light past their own shell
// from the level above. Every ray keeps its direction, so nothing is averaged over time to hide noise. Pixels and reflections read the probes
// as they read the irradiance field's. See RadianceCascades.slangh.
class FRadianceCascades final : public IGlobalIllumination
{
public:
	[[nodiscard]] static std::expected<std::unique_ptr<FRadianceCascades>, FPresentationError> Create(IGraphicsDevice& Device, FRadianceCascadesShaders Shaders);
	[[nodiscard]] std::expected<void, FPresentationError> Render(const FGlobalIlluminationInputs& Inputs) override;
	const FTextureHandle& GetIrradiance() const noexcept override;
	std::size_t GetMemoryBytes() const noexcept override;
	FGlobalIlluminationStatistics GetStatistics() const noexcept override;
	void Release() noexcept override;

private:
	FRadianceCascades(IGraphicsDevice& InDevice, std::array<FComputePipelineHandle, 6> InPipelines);
	[[nodiscard]] std::expected<void, FPresentationError> Allocate(FExtent2D Extent);

	IGraphicsDevice& Device;
	// Update, shells, near rays, blend, gather, and reflections.
	std::array<FComputePipelineHandle, 6> Pipelines;

	FTextureHandle IrradianceAtlas;
	FTextureHandle DistanceAtlas;
	// Levels 1 to 3's radiance tiles; level 0 keeps none.
	std::array<FTextureHandle, 3> RadianceAtlases;
	// Each level's cells, holding the index of their probe in the pool.
	FBufferHandle Cells;
	FBufferHandle ProbeCells;
	FBufferHandle Probes;
	FBufferHandle Used;
	FBufferHandle Requests;
	FBufferHandle FreeList;
	// The near list and each level's shell list.
	FBufferHandle Lists;
	// Each list's dispatch arguments, then the update's counters.
	FBufferHandle Counters;
	FBufferHandle RayData;
	FTextureHandle Gathered;
	// Counters copy into these in turn and are read three frames later, when the device has always finished them because it keeps three frames
	// in flight, so nothing waits and no frame's counters are missed.
	std::array<FBufferHandle, 4> CounterReadback;
	FGlobalIlluminationStatistics Statistics;

	// New storage holds garbage until one reset dispatch.
	bool bReset = false;
	std::uint32_t Frame = 0;
};
}
