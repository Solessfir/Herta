#pragma once

#include "RayTracingScene.h"

#include <array>
#include <cstdint>
#include <expected>
#include <memory>

namespace Herta
{
struct FGlobalIlluminationInputs
{
	FExtent2D Extent;
	const FVisualUniforms* Uniforms = nullptr;
	const FRayTracingScene* Scene = nullptr;
	// Reversed-Z depth from the depth prepass; probes and rays start from the surfaces it holds.
	FTextureHandle Depth;
	FTextureHandle Environment;
	FTextureHandle SkyView;
	FTextureHandle SpecularAlbedo;
	FBufferHandle Lights;
};

struct FRadianceCascadeShaders
{
	FShaderAsset Probes;
	FShaderAsset Trace;
	FShaderAsset Irradiance;
	FShaderAsset Shade;
	FShaderAsset Arguments;
	FShaderAsset Upsample;
};

// Split Radiance Cascades (Freeman and Sannikov 2026) as the High tier's dynamic diffuse global illumination: sparse world-space probes near
// visible surfaces, filled each frame by one ray per 2x2 pixels without temporal accumulation, and gathered into per-pixel irradiance.
class FRadianceCascades final
{
public:
	[[nodiscard]] static std::expected<std::unique_ptr<FRadianceCascades>, FPresentationError> Create(IGraphicsDevice& Device, FRadianceCascadeShaders Shaders);
	// Records every pass inside the caller's recording, after the depth prepass and the scene's update.
	[[nodiscard]] std::expected<void, FPresentationError> Render(const FGlobalIlluminationInputs& Inputs);
	// Diffuse irradiance per pixel at 1/16 scale, with alpha 1 where probes covered the pixel.
	const FTextureHandle& GetIrradiance() const noexcept;
	std::size_t GetMemoryBytes() const noexcept;
	FGlobalIlluminationStatistics GetStatistics() const noexcept;
	void Release() noexcept;

private:
	FRadianceCascades(IGraphicsDevice& InDevice, std::array<FComputePipelineHandle, 6> InPipelines);
	[[nodiscard]] std::expected<void, FPresentationError> Allocate(FExtent2D Extent);

	IGraphicsDevice& Device;
	// Probes, trace, irradiance, gather, indirect arguments, and upsample.
	std::array<FComputePipelineHandle, 6> Pipelines;

	// The hash slots and irradiance tiles alternate between frames, so rays can read the light that last frame's probes received.
	std::array<FBufferHandle, 2> Slots;
	FBufferHandle Probes;
	FBufferHandle Counters;
	FBufferHandle Deposits;
	FBufferHandle RecordCounts;
	FBufferHandle Merged;
	// Group counts sized to the probes that exist, so stages skip the unused capacity.
	FBufferHandle Arguments;
	std::array<FTextureHandle, 2> Atlas;
	// Gathered once per 2x2 block, then upsampled to every pixel.
	FTextureHandle Gathered;
	FTextureHandle Irradiance;
	// Counters copy into these in turn and are read once their frame completes, so nothing waits.
	std::array<FBufferHandle, 3> CounterReadback;
	FGlobalIlluminationStatistics Statistics;
	std::array<float, 3> PreviousEye{};
	bool bPrevious = false;
	// New record storage holds garbage until one reset dispatch; after that every merge clears what it read.
	bool bRecordsReset = false;
	std::uint32_t Frame = 0;
};
}
