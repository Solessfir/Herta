#pragma once

#include "Herta/Renderer/MeshRenderer.h"

#include <cstdint>
#include <expected>
#include <memory>
#include <unordered_map>
#include <vector>

namespace Herta
{
struct FPathTraceInputs
{
	FExtent2D Extent;
	const FMeshRenderView* View = nullptr;
	// The frame's visual uniforms; the trace writes its own dispatch values into the material block of a copy.
	const FVisualUniforms* Uniforms = nullptr;
	FTextureHandle Environment;
	FTextureHandle SkyView;
	FTextureHandle SpecularAlbedo;
	FBufferHandle Lights;
	// Receives the converged mean so far; a storage-capable Rgba16Float texture of Extent.
	FTextureHandle Output;
	// Keys the accumulation along with the view and scene, so loading a new environment restarts it.
	const void* EnvironmentIdentity = nullptr;
};

// The reference path tracer: scene acceleration structures, a resource table for hit data, and progressive accumulation.
// Everything it holds is rebuilt on demand, so Release can drop it whenever path tracing is off.
class FPathTracer final
{
public:
	[[nodiscard]] static std::expected<std::unique_ptr<FPathTracer>, FPresentationError> Create(IGraphicsDevice& Device, FShaderAsset Shader);
	// Records acceleration structure builds and one sample per pixel, inside the caller's recording, after the frame's vertex updates.
	[[nodiscard]] std::expected<void, FPresentationError> Render(const FPathTraceInputs& Inputs);
	std::uint32_t GetSampleCount() const noexcept;
	void Release() noexcept;

private:
	struct FMeshEntry
	{
		FBufferHandle Vertices;
		FBufferHandle Indices;
		FAccelerationStructureHandle BottomLevel;
		std::uint32_t VertexSlot = 0;
		std::uint32_t IndexSlot = 0;
		bool bDeforming = false;
		std::uint64_t LastUsed = 0;
	};

	struct FTextureEntry
	{
		FTextureHandle Texture;
		std::uint32_t Slot = 0;
		std::uint64_t LastUsed = 0;
	};

	struct FRetiredSlot
	{
		std::uint32_t Slot = 0;
		std::uint64_t Frame = 0;
	};

	FPathTracer(IGraphicsDevice& InDevice, FComputePipelineHandle InPipeline);
	[[nodiscard]] std::expected<std::uint32_t, FPresentationError> AllocateSlot();
	[[nodiscard]] std::expected<FMeshEntry*, FPresentationError> FindMesh(const FRenderMesh& Mesh, bool bDeforming);
	[[nodiscard]] std::expected<std::uint32_t, FPresentationError> FindTexture(const FTextureHandle& Texture);
	void RetireUnused();

	IGraphicsDevice& Device;
	FComputePipelineHandle Pipeline;

	// Slots are rewritten only after every recording that could read them has completed.
	FResourceTableHandle Table;
	std::uint32_t NextSlot = 0;
	std::vector<std::uint32_t> FreeSlots;
	std::vector<FRetiredSlot> RetiredSlots;
	// Keyed by resources the entries keep alive, so a key's address cannot be reused while it is mapped.
	std::unordered_map<const IRhiBuffer*, FMeshEntry> Meshes;
	std::unordered_map<const IRhiTexture*, FTextureEntry> Textures;

	FAccelerationStructureHandle TopLevel;
	std::uint32_t TopLevelCapacity = 0;
	FBufferHandle Sections;
	FBufferHandle Materials;
	FTextureHandle Accumulation;

	// The last frame's view, scene, and settings; any difference restarts accumulation.
	std::vector<std::byte> Key;
	std::vector<std::byte> NextKey;
	std::uint32_t SampleCount = 0;
	std::uint64_t Frame = 0;
};
}
