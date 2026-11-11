#pragma once

#include "Herta/Assets/AssetRegistry.h"

#include <expected>
#include <filesystem>
#include <string_view>
#include <vector>

namespace Herta
{
// Content operations that keep an asset's identity: its .hmeta, which holds the ID levels and materials refer to, moves with the source.
// Each is all or nothing: a failure part way puts back what already moved.

// The files that make up the asset at Path, a content-relative source path: the source, its .hmeta, and anything generated beside it, such
// as a Material Graph's .slang, which all start with the source's file name and a dot.
[[nodiscard]] std::expected<std::vector<std::filesystem::path>, FAssetError> GetAssetFiles(const std::filesystem::path& ContentRoot, std::string_view Path);
// Renames or moves an asset within ContentRoot, creating the destination folder. Refuses a destination that exists or a source without
// metadata, and keeps the extension, which names its importer.
[[nodiscard]] std::expected<void, FAssetError> MoveAsset(const std::filesystem::path& ContentRoot, std::string_view From, std::string_view To);
// Moves an asset into a new folder under TrashRoot, from where RestoreAsset puts it back, and returns that folder.
[[nodiscard]] std::expected<std::filesystem::path, FAssetError> DeleteAsset(const std::filesystem::path& ContentRoot, std::string_view Path, const std::filesystem::path& TrashRoot);
// Puts a deleted asset back at Path, refusing to replace anything there.
[[nodiscard]] std::expected<void, FAssetError> RestoreAsset(const std::filesystem::path& Trashed, const std::filesystem::path& ContentRoot, std::string_view Path);
}
