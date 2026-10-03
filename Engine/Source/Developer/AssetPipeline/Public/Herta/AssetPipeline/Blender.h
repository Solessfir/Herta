#pragma once

#include "Herta/Assets/AssetRegistry.h"

#include <expected>
#include <filesystem>
#include <string>

namespace Herta
{
// Blender is an optional system install. Herta never downloads it; without it, only .blend imports fail.
struct FBlenderInstallation
{
	std::filesystem::path Executable;
	// Such as "5.2.2"; part of every Blender build key.
	std::string Version;
};

// Uses HERTA_BLENDER when set, otherwise the newest Blender Foundation install on Windows or `blender` on PATH on Linux.
[[nodiscard]] std::expected<FBlenderInstallation, FAssetError> FindBlender();
}
