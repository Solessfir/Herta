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
#include <set>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace Herta
{
class FLogService;
class FTaskScope;
class FTaskSystem;

struct FPreviewModelMetadata
{
	std::size_t Vertices = 0;
	std::size_t Triangles = 0;
	std::size_t Materials = 0;
	FVector3 BoundsMinimum{};
	FVector3 BoundsMaximum{};
};

struct FPreviewTextureMetadata
{
	std::uint32_t Width = 0;
	std::uint32_t Height = 0;
	std::size_t Mips = 0;
	ETextureColorSpace ColorSpace = ETextureColorSpace::Srgb;
};

using FPreviewAssetMetadata = std::variant<FPreviewModelMetadata, FPreviewTextureMetadata>;

FPreviewAssetMetadata GetPreviewAssetMetadata(const FCookedAsset& Asset);

struct FPreviewMeshSlot
{
	FAssetId Asset;
	std::string Label;
	std::shared_ptr<const FRenderMesh> Mesh;
	FEditorAssetThumbnail Thumbnail;
	std::optional<FPreviewAssetMetadata> Metadata;

	// Build key of Mesh, so a reimport that cooks the same key keeps the GPU copy.
	FHash128 Key;
	std::uint64_t ContentGeneration = 0;
	bool bLoading = false;
	std::string Error;

	// Only the newest request for an asset may publish its result.
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
	[[nodiscard]] static std::unique_ptr<FPreviewAssets> Create(FTaskSystem& Tasks, IGraphicsDevice& Device, FLogService& Log, FEditorAssetPaths Paths, std::size_t ObjectCount, FEditorAssetThumbnailRenderer RenderThumbnail = {});
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
	void ImportFiles(std::vector<std::filesystem::path> Files, std::string Destination = {});
	[[nodiscard]] std::expected<std::string, FAssetError> CreateFolder(std::string_view MountedParent, std::string_view Name, bool bUniqueName = false);
	[[nodiscard]] std::expected<std::string, FAssetError> RenameFolder(std::string_view MountedFolder, std::string_view NewName);
	[[nodiscard]] std::expected<std::filesystem::path, FAssetError> GetFolderPath(std::string_view MountedFolder) const;
	std::span<const std::string> GetFolders() const noexcept;
	std::uint64_t GetOptionsGeneration() const noexcept;

	[[nodiscard]] bool IsImporting() const noexcept
	{
		return ImportsInFlight > 0;
	}

	// Requests made before the first scan finishes wait for it. An invalid asset clears the mesh component.
	void RequestMesh(std::size_t Object, const FAssetId& Asset);
	// Scene edits change bindings, not the lifetime of loaded meshes or pending cooks. Invalid assets leave empty slots.
	void RebindObjects(std::span<const FAssetId> Assets);
	// The browser retains at most 64 visible thumbnails. Tick performs GPU work, never the draw-time request.
	void SetThumbnailAssets(std::span<const FAssetId> Assets);
	FEditorAssetThumbnail GetThumbnail(const FAssetId& Asset) const noexcept;
	const FPreviewMeshSlot* GetCachedAsset(const FAssetId& Asset) const noexcept;

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

	struct FCachedMesh
	{
		FPreviewMeshSlot Slot;
		std::uint64_t RequestContentGeneration = 0;
		std::optional<FHash128> ThumbnailKey;
		bool bThumbnailOnly = false;
	};

	FPreviewAssets(FTaskSystem& Tasks, IGraphicsDevice& Device, FLogService& Log, FEditorAssetPaths Paths, std::size_t ObjectCount, std::unique_ptr<FTaskScope> Scope, FEditorAssetThumbnailRenderer RenderThumbnail);
	void PublishScan(const std::vector<std::expected<FContentScanResult, FAssetError>>& Results);
	void PublishFolders(std::vector<std::string> NewFolders);
	void ContentChanged();
	FPreviewMeshSlot& GetOrLoadMesh(const FAssetId& Asset, bool bThumbnailOnly = false);
	void StartLoad(const FAssetId& Asset);
	[[nodiscard]] bool SubmitCook(const FLocation& Location, const std::string& Label, std::function<void(FMeshLoad&)> Publish);
	void PublishMesh(const FAssetId& Asset, std::uint64_t Generation, FMeshLoad& Load);
	void ReimportShownAssets();
	void PublishSlots(const FPreviewMeshSlot& Slot);
	void PruneCache();
	void UpdateThumbnail(FCachedMesh& Cached);

	FTaskSystem& Tasks;
	IGraphicsDevice& Device;
	FLogService& Log;

	FEditorAssetPaths Paths;
	std::vector<FMount> Mounts;

	std::vector<FPreviewAssetOption> Options;
	std::vector<std::string> Folders;
	std::map<FAssetId, FLocation> Locations;
	std::uint64_t FolderGeneration = 0;

	std::vector<FPreviewMeshSlot> Slots;
	std::map<FAssetId, FCachedMesh> MeshCache;
	std::set<FAssetId> ThumbnailAssets;
	FEditorAssetThumbnailRenderer RenderThumbnail;
	bool bThumbnailRequestsChanged = false;
	std::uint64_t RequestGeneration = 0;
	std::uint64_t ContentGeneration = 0;
	std::uint64_t OptionsGeneration = 0;

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
