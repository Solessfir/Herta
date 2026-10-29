#pragma once

#include "RayTracingScene.h"

#include <cstdint>
#include <expected>
#include <memory>
#include <vector>

namespace Herta
{
struct FPathTraceInputs
{
	FExtent2D Extent;
	const FMeshRenderView* View = nullptr;
	// The frame's visual uniforms; the trace writes its own dispatch values into the material block of a copy.
	const FVisualUniforms* Uniforms = nullptr;
	const FRayTracingScene* Scene = nullptr;
	FTextureHandle Environment;
	FTextureHandle SkyView;
	FTextureHandle SpecularAlbedo;
	FBufferHandle Lights;
	// Receives the converged mean so far; a storage-capable Rgba16Float texture of Extent.
	FTextureHandle Output;
	// Keys the accumulation along with the view and scene, so loading a new environment restarts it.
	const void* EnvironmentIdentity = nullptr;
};

// The reference path tracer: progressive accumulation over the shared ray traced scene.
class FPathTracer final
{
public:
	[[nodiscard]] static std::expected<std::unique_ptr<FPathTracer>, FPresentationError> Create(IGraphicsDevice& Device, FShaderAsset Shader);
	// Records one sample per pixel inside the caller's recording, after the scene's update.
	[[nodiscard]] std::expected<void, FPresentationError> Render(const FPathTraceInputs& Inputs);
	std::uint32_t GetSampleCount() const noexcept;
	void Release() noexcept;

private:
	FPathTracer(IGraphicsDevice& InDevice, FComputePipelineHandle InPipeline);

	IGraphicsDevice& Device;
	FComputePipelineHandle Pipeline;
	FTextureHandle Accumulation;

	// The last frame's view, scene generation, and lighting; any difference restarts accumulation.
	std::vector<std::byte> Key;
	std::vector<std::byte> NextKey;
	std::uint32_t SampleCount = 0;
};
}
