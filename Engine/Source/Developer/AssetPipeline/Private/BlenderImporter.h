#pragma once

#include "Herta/AssetPipeline/Blender.h"

#include <cstddef>
#include <expected>
#include <filesystem>
#include <string>
#include <vector>

namespace Herta
{
struct FBlenderExport
{
	// A self-contained GLB with embedded images.
	std::vector<std::byte> Glb;
	// Content-relative external images and libraries the .blend reads, sorted.
	std::vector<std::string> Dependencies;
	std::vector<std::string> Warnings;
};

// Exports a .blend to GLB with Herta's fixed preset in a headless Blender that runs no scripts from the file.
[[nodiscard]] std::expected<FBlenderExport, FAssetError> ExportBlend(const FBlenderInstallation& Blender, const std::filesystem::path& ContentRoot, const std::string& SourcePath);
}
