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
	// Reversed-Z depth from the depth prepass; the field is gathered at the surfaces it holds.
	FTextureHandle Depth;
	// The prepass's shading normals and roughness; see EncodeSurface in VisualShared.slangh.
	FTextureHandle Surface;
	FTextureHandle Environment;
	FTextureHandle SkyView;
	FTextureHandle SpecularAlbedo;
	FBufferHandle Lights;
};

struct FIrradianceFieldShaders
{
	FShaderAsset Update;
	FShaderAsset Trace;
	FShaderAsset Blend;
	FShaderAsset Gather;
	FShaderAsset Reflect;
};

// The High tier's dynamic diffuse global illumination: a ray-traced irradiance field after DDGI (Majercik et al. 2019 and 2021). Probes on four clipmap
// levels follow the camera; those its pixels read trace their own rays every frame and keep octahedral irradiance and distance moments, so lighting
// is averaged in world space and moving the camera shows no noise. See IrradianceField.slangh.
class FIrradianceField final
{
public:
	[[nodiscard]] static std::expected<std::unique_ptr<FIrradianceField>, FPresentationError> Create(IGraphicsDevice& Device, FIrradianceFieldShaders Shaders);
	// Records every pass inside the caller's recording, after the depth prepass and the scene's update.
	[[nodiscard]] std::expected<void, FPresentationError> Render(const FGlobalIlluminationInputs& Inputs);
	// Two half-resolution images side by side, each sampled at every 2x2 block's first pixel with that pixel's reversed depth in alpha, or 0 where
	// it has nothing: diffuse irradiance on the left and glossy surfaces' scene reflections on the right, both at 1/16 scale. The mesh pass blends
	// the four nearest samples on each pixel's surface plane.
	const FTextureHandle& GetIrradiance() const noexcept;
	std::size_t GetMemoryBytes() const noexcept;
	FGlobalIlluminationStatistics GetStatistics() const noexcept;
	void Release() noexcept;

private:
	FIrradianceField(IGraphicsDevice& InDevice, std::array<FComputePipelineHandle, 5> InPipelines);
	[[nodiscard]] std::expected<void, FPresentationError> Allocate(FExtent2D Extent);

	IGraphicsDevice& Device;
	// Update, trace, blend, gather, and reflections.
	std::array<FComputePipelineHandle, 5> Pipelines;

	FTextureHandle IrradianceAtlas;
	FTextureHandle DistanceAtlas;
	FBufferHandle Cells;
	FBufferHandle Probes;
	FBufferHandle Used;
	FBufferHandle TraceList;
	FBufferHandle Counters;
	FBufferHandle Arguments;
	FBufferHandle RayData;
	FTextureHandle Gathered;
	// Counters copy into these in turn and are read once their frame completes, so nothing waits.
	std::array<FBufferHandle, 3> CounterReadback;
	FGlobalIlluminationStatistics Statistics;

	// New storage holds garbage until one reset dispatch.
	bool bReset = false;
	std::uint32_t Frame = 0;
};
}
