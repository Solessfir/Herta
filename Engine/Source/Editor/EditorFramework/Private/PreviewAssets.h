#pragma once

#include "Herta/AssetPipeline/ContentRoot.h"
#include "Herta/Assets/AssetRegistry.h"
#include "Herta/EditorFramework/EditorFramework.h"
#include "Herta/Renderer/MeshRenderer.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <span>
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

struct FPreviewAssetOption
{
	FAssetId Id;
	// Mount-prefixed content path, such as Engine/Shapes/Cube.gltf.
	std::string Label;
	std::string Importer;
};

// Scans the Engine and Game content roots and loads preview meshes without blocking the editor.
// Cooking runs in HertaAssetWorker on a blocking-IO task. Main-thread continuations publish results between frames,
// so GPU uploads never overlap a frame recording. Continuations capture this; the destructor cancels and drains them first.
class FPreviewAssets final
{
public:
	[[nodiscard]] static std::unique_ptr<FPreviewAssets> Create(FTaskSystem& Tasks, IGraphicsDevice& Device, FLogService& Log, FEditorAssetPaths Paths, std::size_t ObjectCount);
	~FPreviewAssets();
	FPreviewAssets(const FPreviewAssets&) = delete;
	FPreviewAssets& operator=(const FPreviewAssets&) = delete;

	void RequestScan();
	// Requests made before the first scan finishes wait for it.
	void RequestMesh(std::size_t Object, const FAssetId& Asset);

	[[nodiscard]] std::span<const FPreviewAssetOption> GetOptions() const noexcept
	{
		return Options;
	}
	[[nodiscard]] bool IsScanning() const noexcept
	{
		return bScanning;
	}
	[[nodiscard]] const FPreviewMeshSlot& GetSlot(std::size_t Object) const
	{
		return Slots.at(Object);
	}

private:
	struct FMount
	{
		std::string Name;
		std::filesystem::path Root;
	};

	struct FLocation
	{
		std::size_t Mount = 0;
		std::string SourcePath;
	};

	struct FMeshLoad;

	FPreviewAssets(FTaskSystem& Tasks, IGraphicsDevice& Device, FLogService& Log, FEditorAssetPaths Paths, std::size_t ObjectCount, std::unique_ptr<FTaskScope> Scope);
	void PublishScan(const std::vector<std::expected<FContentScanResult, FAssetError>>& Results);
	void StartLoad(std::size_t Object);
	void PublishMesh(std::size_t Object, std::uint64_t Generation, FMeshLoad& Load);
	[[nodiscard]] std::shared_ptr<const FRenderMesh> FindLoadedMesh(const FAssetId& Asset, std::size_t ExcludedObject) const;

	FTaskSystem& Tasks;
	IGraphicsDevice& Device;
	FLogService& Log;
	FEditorAssetPaths Paths;
	std::vector<FMount> Mounts;
	std::vector<FPreviewAssetOption> Options;
	std::map<FAssetId, FLocation> Locations;
	std::vector<FPreviewMeshSlot> Slots;
	bool bScanning = false;
	bool bScanned = false;
	std::unique_ptr<FTaskScope> Scope;
};
}
