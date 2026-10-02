#pragma once

#include "Herta/Assets/CookedAsset.h"
#include "Herta/Core/Hash.h"

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Herta
{
// Bump an importer version whenever its output changes for the same source and settings.
inline constexpr std::uint32_t TextureImporterVersion = 1;
inline constexpr std::uint32_t GltfImporterVersion = 1;

struct FAssetCookRequest
{
	std::filesystem::path ContentRoot;
	// Normally DerivedDataCache/<platform> in the repository.
	std::filesystem::path DerivedDataRoot;
	// Content-relative source path with '/' separators.
	std::string SourcePath;
	std::string TargetPlatform;
	// Cooks and replaces the cache entry even when a valid one exists.
	bool bForce = false;
};

struct FAssetCookResult
{
	FHash128 Key;
	bool bCacheHit = false;
	std::vector<std::string> Warnings;
};

// Cooks in the calling process. Only HertaAssetWorker and tests call this, because importers parse untrusted files.
[[nodiscard]] std::expected<FAssetCookResult, FAssetError> CookAsset(const FAssetCookRequest& Request);

struct FAssetWorkerOptions
{
	std::filesystem::path WorkerPath;
	std::chrono::milliseconds Timeout{std::chrono::minutes(10)};
	// Polled while the worker runs. Returning true kills the worker.
	std::function<bool()> ShouldCancel{};
};

// Runs CookAsset in a HertaAssetWorker process so a crashing or hanging importer cannot take down the caller.
[[nodiscard]] std::expected<FAssetCookResult, FAssetError> CookAssetInWorker(const FAssetCookRequest& Request, const FAssetWorkerOptions& Options);

// The command-line entry point of HertaAssetWorker. Returns the process exit code.
[[nodiscard]] int RunAssetWorker(std::span<const std::string_view> Arguments);

// Reads and validates a cooked asset previously stored under Key.
[[nodiscard]] std::expected<FCookedAsset, FAssetError> LoadCookedAsset(const std::filesystem::path& DerivedDataRoot, const FHash128& Key);
}
