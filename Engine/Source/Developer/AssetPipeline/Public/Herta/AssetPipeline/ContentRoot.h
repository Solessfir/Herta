#pragma once

#include "Herta/AssetPipeline/AssetMetadata.h"
#include "Herta/Assets/Material.h"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Herta
{
struct FContentDiagnostic
{
	std::string Path;
	std::string Message;
};

struct FContentScanResult
{
	FAssetRegistry Registry;
	// Assets with errors are excluded from the registry. IDs shared by several files exclude all of them.
	std::vector<FContentDiagnostic> Errors;
	// Content-relative importable source files that have no metadata yet.
	std::vector<std::string> UnregisteredSources;
};

struct FImportedSource
{
	std::string SourcePath;
	FAssetMetadata Metadata;
};

struct FContentFileStamp
{
	std::uintmax_t Size = 0;
	std::filesystem::file_time_type LastWriteTime;

	[[nodiscard]] bool operator==(const FContentFileStamp&) const = default;
};

// Content-relative path to size and write time for every regular file, using the same filtering as ScanContentRoot.
using FContentSnapshot = std::map<std::string, FContentFileStamp>;

// ponytail: editors poll this instead of native watchers; switch to ReadDirectoryChangesW and inotify if content trees grow large.
[[nodiscard]] FContentSnapshot TakeContentSnapshot(const std::filesystem::path& ContentRoot);

// Sidecars parsed by earlier scans of one content root, reused while their size and write time are unchanged.
struct FContentScanCache
{
	struct FEntry
	{
		FContentFileStamp Stamp;
		std::expected<FAssetMetadata, FAssetError> Metadata;
	};

	std::map<std::string, FEntry> Metadata;
};

// Dot-prefixed files and directories are ignored. Symbolic links are reported and never followed.
// With a cache, only new or changed sidecars are parsed, and the cache is updated to this scan.
[[nodiscard]] std::expected<FContentScanResult, FAssetError> ScanContentRoot(const std::filesystem::path& ContentRoot, FContentScanCache* Cache = nullptr);

[[nodiscard]] std::optional<std::string_view> FindImporterForSource(const std::filesystem::path& Source);
// Lowercase extensions with the leading dot, such as ".blend", in importer-table order.
[[nodiscard]] std::vector<std::string_view> GetImportableExtensions();

// Registers a source with a new ID. Sources outside the content root are first copied into DestinationDirectory.
[[nodiscard]] std::expected<FImportedSource, FAssetError> ImportSource(const std::filesystem::path& ContentRoot, const std::filesystem::path& Source, std::string_view DestinationDirectory = {});

// Publishes canonical material JSON without changing its asset identity sidecar.
[[nodiscard]] std::expected<void, FAssetError> WriteMaterialAsset(const std::filesystem::path& Path, const FMaterialAsset& Material, bool bReplaceExisting = true);
[[nodiscard]] std::expected<FMaterialAsset, FAssetError> LoadMaterialAsset(const std::filesystem::path& Path);
}
