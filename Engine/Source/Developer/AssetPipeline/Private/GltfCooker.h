#pragma once

#include "Herta/Assets/CookedAsset.h"

#include <cstddef>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace Herta
{
// Content-relative paths of external buffers and images. Rejects URIs that are absolute or escape the content root.
[[nodiscard]] std::expected<std::vector<std::string>, FAssetError> FindGltfDependencies(const std::filesystem::path& ContentRoot, const std::string& SourcePath, std::span<const std::byte> SourceBytes);

// Flattens the default scene into one model: node transforms are baked into positions and primitives are grouped by material.
[[nodiscard]] std::expected<FCookedModel, FAssetError> CookGltf(const std::filesystem::path& ContentRoot, const std::string& SourcePath, std::span<const std::byte> SourceBytes, std::vector<std::string>& Warnings);
}
