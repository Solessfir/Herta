#pragma once

#include "Herta/AssetPipeline/ContentRoot.h"
#include "Herta/Assets/AssetRegistry.h"
#include "Herta/Core/Hash.h"
#include "Herta/EditorCore/CommandRegistry.h"
#include "Herta/EditorFramework/EditorFramework.h"
#include "Herta/Renderer/MeshRenderer.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
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
	// Build key of Mesh, so a reimport that cooks the same key keeps the GPU copy.
	FHash128 Key;
	std::uint64_t ContentGeneration = 0;
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
// Content edits are polled; a change rescans and reimports every shown asset, keeping the previous mesh when a reimport fails.
class FPreviewAssets final
{
public:
	[[nodiscard]] static std::unique_ptr<FPreviewAssets> Create(FTaskSystem& Tasks, IGraphicsDevice& Device, FLogService& Log, FEditorAssetPaths Paths, std::size_t ObjectCount);
	~FPreviewAssets();
	FPreviewAssets(const FPreviewAssets&) = delete;
	FPreviewAssets& operator=(const FPreviewAssets&) = delete;

	void RequestScan();
	// Polls content for edits about once per second. Call once per frame.
	void Tick();
	// Starts a content poll now unless one is running. The first poll records the baseline.
	void CheckForChanges();
	// Imports files into Game content in the background through asset.import, the same command HertaEditorCmd runs.
	// Models land in Models and images in Textures; the next poll picks the new assets up.
	void ImportFiles(std::vector<std::filesystem::path> Files);

	[[nodiscard]] bool IsImporting() const noexcept
	{
		return ImportsInFlight > 0;
	}

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
	friend struct FPreviewAssetsTestAccess;

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
	void ContentChanged();
	void StartLoad(std::size_t Object);
	[[nodiscard]] bool SubmitCook(const FLocation& Location, const std::string& Label, std::function<void(FMeshLoad&)> Publish);
	void PublishMesh(std::size_t Object, std::uint64_t Generation, FMeshLoad& Load);
	void ReimportShownAssets();
	void PublishReimport(const FAssetId& Asset, const std::vector<std::pair<std::size_t, std::uint64_t>>& Targets, FMeshLoad& Load);
	[[nodiscard]] const FPreviewMeshSlot* FindLoadedSlot(const FAssetId& Asset, std::size_t ExcludedObject) const;

	FTaskSystem& Tasks;
	IGraphicsDevice& Device;
	FLogService& Log;
	FEditorAssetPaths Paths;
	std::vector<FMount> Mounts;
	std::vector<FPreviewAssetOption> Options;
	std::map<FAssetId, FLocation> Locations;
	std::vector<FPreviewMeshSlot> Slots;
	std::uint64_t ContentGeneration = 0;
	bool bScanning = false;
	bool bScanned = false;
	std::vector<FContentSnapshot> Snapshots;
	// One per mount. Only the scan task touches these, and RequestScan never overlaps two scans.
	std::vector<FContentScanCache> ScanCaches;
	std::chrono::steady_clock::time_point NextPoll;
	bool bPolling = false;
	bool bReimportAfterScan = false;
	// Asset commands run on blocking-IO tasks, so they stay off the console, which executes on the main thread.
	FEditorCommandRegistry AssetCommands;
	std::size_t ImportsInFlight = 0;
	std::unique_ptr<FTaskScope> Scope;
};
}
