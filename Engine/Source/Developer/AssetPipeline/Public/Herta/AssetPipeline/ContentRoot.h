#pragma once

#include "Herta/AssetPipeline/AssetMetadata.h"

#include <expected>
#include <filesystem>
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
	// Content-relative source files that have no metadata yet.
	std::vector<std::string> UnregisteredSources;
};

struct FImportedSource
{
	std::string SourcePath;
	FAssetMetadata Metadata;
};

// Dot-prefixed files and directories are ignored. Symbolic links are reported and never followed.
[[nodiscard]] std::expected<FContentScanResult, FAssetError> ScanContentRoot(const std::filesystem::path& ContentRoot);

[[nodiscard]] std::optional<std::string_view> FindImporterForSource(const std::filesystem::path& Source);

// Registers a source with a new ID. Sources outside the content root are first copied into DestinationDirectory.
[[nodiscard]] std::expected<FImportedSource, FAssetError> ImportSource(const std::filesystem::path& ContentRoot, const std::filesystem::path& Source, std::string_view DestinationDirectory = {});
}
