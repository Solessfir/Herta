#pragma once

#include "Herta/Assets/AssetId.h"
#include "Herta/Assets/AssetRegistry.h"
#include "Herta/Assets/CookedAsset.h"

#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <vector>

namespace Herta
{
// A staged build keeps one cooked asset per file, named by asset ID. Textures are staged once per color space they are sampled in, because their mips are filtered in that space.
[[nodiscard]] std::filesystem::path GetStagedAssetPath(const std::filesystem::path& MountRoot, const FAssetId& Asset, std::optional<ETextureColorSpace> ColorSpace = std::nullopt);

// Directory-backed mounts searched in order. Loading only reads staged files; it never imports or cooks.
class FCookedContent final
{
public:
	FCookedContent() = default;
	explicit FCookedContent(std::vector<std::filesystem::path> MountRoots);

	[[nodiscard]] std::expected<FCookedAsset, FAssetError> Load(const FAssetId& Asset, std::optional<ETextureColorSpace> ColorSpace = std::nullopt) const;
	[[nodiscard]] std::span<const std::filesystem::path> GetMountRoots() const noexcept;

private:
	std::vector<std::filesystem::path> MountRoots;
};
}
