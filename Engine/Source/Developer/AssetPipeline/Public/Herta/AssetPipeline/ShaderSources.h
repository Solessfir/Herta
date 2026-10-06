#pragma once

#include "Herta/Assets/AssetRegistry.h"

#include <cstddef>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Herta
{
struct FShaderSourceFile
{
	std::size_t Root = 0;
	std::string Path;
	std::vector<std::byte> Bytes;
};

// Reads one immutable source/include/import graph without following links or leaving the supplied roots.
[[nodiscard]] std::expected<std::vector<FShaderSourceFile>, FAssetError> CollectShaderSources(std::span<const std::filesystem::path> Roots, std::size_t SourceRoot, std::string_view SourcePath);
}
