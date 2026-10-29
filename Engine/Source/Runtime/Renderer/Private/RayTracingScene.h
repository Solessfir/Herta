#pragma once

#include "Herta/Renderer/MeshRenderer.h"

#include <cstdint>
#include <expected>
#include <memory>
#include <unordered_map>
#include <vector>

namespace Herta
{
// The scene as ray queries see it: a top level over the render extraction, bottom levels per shared mesh, a resource table of mesh buffers and textures,
// and the section and material records that RayTracing.slangh reads from hits. The path tracer and global illumination share one per renderer.
// Everything it holds is rebuilt on demand, so Release can drop it whenever nothing traces rays.
class FRayTracingScene final
{
public:
	explicit FRayTracingScene(IGraphicsDevice& InDevice);
	// Records bottom level builds and refits, and rebuilds the top level and records when any instance's mesh, transform, or material changed.
	// Call once per frame inside the recording, after the frame's vertex updates.
	[[nodiscard]] std::expected<void, FPresentationError> Update(const FMeshRenderView& View);
	// Advances whenever Update rebuilds, so results that depend on the scene know to restart.
	std::uint64_t GetGeneration() const noexcept;
	const FAccelerationStructureHandle& GetTopLevel() const noexcept;
	const FResourceTableHandle& GetTable() const noexcept;
	const FBufferHandle& GetSections() const noexcept;
	const FBufferHandle& GetMaterials() const noexcept;
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
		std::uint64_t Serial = 0;
	};

	[[nodiscard]] std::expected<std::uint32_t, FPresentationError> AllocateSlot();
	[[nodiscard]] std::expected<FMeshEntry*, FPresentationError> FindMesh(const FRenderMesh& Mesh, bool bDeforming);
	[[nodiscard]] std::expected<std::uint32_t, FPresentationError> FindTexture(const FTextureHandle& Texture);
	void RetireUnused();

	IGraphicsDevice& Device;

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

	// The last built scene's meshes, transforms, and materials.
	std::vector<std::byte> Key;
	std::vector<std::byte> NextKey;
	std::uint64_t Generation = 0;
	std::uint64_t Frame = 0;
};
}
