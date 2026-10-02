#pragma once

#include "Herta/AssetPipeline/AssetMetadata.h"
#include "Herta/Core/Hash.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Herta
{
struct FAssetBuildDependency
{
	std::string Path;
	FHash128 ContentHash;
};

struct FAssetBuildKeyInput
{
	FHash128 SourceHash;
	std::string SourcePath;
	std::string Importer;
	std::uint32_t ImporterVersion = 0;
	FAssetImportSettings Settings;
	// Order does not affect the key.
	std::vector<FAssetBuildDependency> Dependencies;
	std::string TargetPlatform;
	std::uint32_t CookedFormatVersion = 0;
};

// The asset ID is deliberately excluded so identical sources share derived data.
[[nodiscard]] FHash128 ComputeAssetBuildKey(const FAssetBuildKeyInput& Input);
}
