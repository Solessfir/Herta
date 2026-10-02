#pragma once

#include "Herta/Assets/AssetRegistry.h"
#include "Herta/EditorFramework/EditorFramework.h"
#include "Herta/Renderer/MeshRenderer.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Herta
{
class FLogService;
class FTaskScope;
class FTaskSystem;

struct FPreviewMeshSlot
{
	FAssetId Asset;
	std::string Label;
	std::shared_ptr<const FRenderMesh> Mesh;
	bool bLoading = false;
	std::string Error;
	// Only the newest request for a slot may publish its result.
	std::uint64_t Generation = 0;
};

// Scans content and loads preview meshes without blocking the editor. Cooking runs in HertaAssetWorker on a blocking-IO task.
// Results are published by main-thread continuations, which run between frames, so GPU uploads never overlap a frame recording.
class FPreviewAssets final
{
public:
	[[nodiscard]] static std::unique_ptr<FPreviewAssets> Create(FTaskSystem& Tasks, IGraphicsDevice& Device, FLogService& Log, FEditorAssetPaths Paths, std::size_t ObjectCount);
	~FPreviewAssets();
	FPreviewAssets(const FPreviewAssets&) = delete;
	FPreviewAssets& operator=(const FPreviewAssets&) = delete;

	void RequestScan();
	void RequestMesh(std::size_t Object, const FAssetRecord& Record);
	void ResetMesh(std::size_t Object);

	[[nodiscard]] const FAssetRegistry& GetRegistry() const noexcept;
	[[nodiscard]] bool IsScanning() const noexcept;
	[[nodiscard]] const FPreviewMeshSlot& GetSlot(std::size_t Object) const;

private:
	struct FState;

	FPreviewAssets(FTaskSystem& Tasks, IGraphicsDevice& Device, FLogService& Log, FEditorAssetPaths Paths, std::size_t ObjectCount, std::unique_ptr<FTaskScope> Scope);

	FTaskSystem& Tasks;
	IGraphicsDevice& Device;
	FLogService& Log;
	FEditorAssetPaths Paths;
	std::shared_ptr<FState> State;
	std::unique_ptr<FTaskScope> Scope;
};
}
