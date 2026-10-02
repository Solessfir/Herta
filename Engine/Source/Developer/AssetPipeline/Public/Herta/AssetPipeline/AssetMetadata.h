#pragma once

#include "Herta/Assets/AssetRegistry.h"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <string_view>

namespace Herta
{
inline constexpr std::uint32_t AssetMetadataVersion = 1;
inline constexpr std::string_view AssetMetadataExtension = ".hmeta";

using FAssetImportSettings = std::map<std::string, std::string, std::less<>>;

// Source-control-facing sidecar stored beside each source file as <source>.hmeta.
struct FAssetMetadata
{
	FAssetId Id;
	std::string Importer;
	FAssetImportSettings Settings;
};

[[nodiscard]] std::expected<FAssetMetadata, FAssetError> ParseAssetMetadata(std::string_view Text);
// Canonical UTF-8 text with LF line endings and settings in byte-wise name order.
[[nodiscard]] std::expected<std::string, FAssetError> SerializeAssetMetadata(const FAssetMetadata& Metadata);

[[nodiscard]] std::filesystem::path GetAssetMetadataPath(const std::filesystem::path& SourcePath);
}
