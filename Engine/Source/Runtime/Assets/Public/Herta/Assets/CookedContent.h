#pragma once

#include "Herta/Assets/AssetId.h"
#include "Herta/Assets/AssetRegistry.h"
#include "Herta/Assets/CookedAsset.h"

#include <array>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace Herta
{
// A staged build keeps one cooked asset per file, named by asset ID. Textures are staged once per color space they are sampled in, because their mips are filtered in that space.
[[nodiscard]] std::filesystem::path GetStagedAssetPath(const std::filesystem::path& MountRoot, const FAssetId& Asset, std::optional<ETextureColorSpace> ColorSpace = std::nullopt);

// The stages every material shader provides: shading, the depth prepass variants, and the bake for ray-traced hits, matching FMaterialShaderStages.
inline constexpr std::array<std::string_view, 11> MaterialShaderStageNames{"vert", "instanced.vert", "frag", "depth.frag", "velocity.frag", "instanced.velocity.vert", "surface.frag", "velocity.surface.frag", "bake.vert", "bake.surface.frag", "bake.light.frag"};
inline constexpr std::size_t MaterialShaderStageCount = MaterialShaderStageNames.size();
// A custom material shader is staged precompiled under its mount, by its mount-relative path, so the game never needs the compiler or the graph.
[[nodiscard]] std::filesystem::path GetStagedMaterialShaderPath(const std::filesystem::path& MountRoot, std::string_view RelativePath, std::string_view Stage);
// The Material Graph parameter table staged beside those stages; see SerializeMaterialShaderParameters.
[[nodiscard]] std::filesystem::path GetStagedMaterialShaderParametersPath(const std::filesystem::path& MountRoot, std::string_view RelativePath);

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
