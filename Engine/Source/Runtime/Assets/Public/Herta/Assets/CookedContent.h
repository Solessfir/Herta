#pragma once

#include "Herta/Assets/AssetId.h"
#include "Herta/Assets/AssetRegistry.h"
#include "Herta/Assets/CookedAsset.h"

#include <array>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace Herta
{
// A staged build keeps one cooked asset per file, named by asset ID. Textures are staged once per color space they are sampled in, because their mips are filtered in that space.
[[nodiscard]] std::filesystem::path GetStagedAssetPath(const std::filesystem::path& MountRoot, const FAssetId& Asset, std::optional<ETextureColorSpace> ColorSpace = std::nullopt);

// The stages every material shader provides: shading, the depth prepass variants, the bake for ray-traced hits, and the masked shadow cutout,
// matching FMaterialShaderStages.
inline constexpr std::array<std::string_view, 12> MaterialShaderStageNames{"vert", "instanced.vert", "frag", "depth.frag", "velocity.frag", "instanced.velocity.vert", "surface.frag", "velocity.surface.frag", "bake.vert", "bake.surface.frag", "bake.light.frag", "shadow.frag"};
inline constexpr std::size_t MaterialShaderStageCount = MaterialShaderStageNames.size();
// A custom material shader is staged precompiled under its mount, named by a hash of its mount-relative key, so the game never needs the compiler or the graph.
[[nodiscard]] std::filesystem::path GetStagedMaterialShaderPath(const std::filesystem::path& MountRoot, std::string_view RelativePath, std::string_view Stage);
// The Material Graph parameter table staged beside those stages; see SerializeMaterialShaderParameters.
[[nodiscard]] std::filesystem::path GetStagedMaterialShaderParametersPath(const std::filesystem::path& MountRoot, std::string_view RelativePath);

class FVirtualFileSystem;

// Content roots searched in order, such as the project's then the engine's, read through a file system that may hold them on disk or in
// packages. Loading only reads staged files; it never imports or cooks.
class FCookedContent final
{
public:
	FCookedContent() = default;
	// Without a file system the roots are read from the disk.
	explicit FCookedContent(std::vector<std::filesystem::path> MountRoots, std::shared_ptr<const FVirtualFileSystem> InFileSystem = nullptr);

	[[nodiscard]] std::expected<FCookedAsset, FAssetError> Load(const FAssetId& Asset, std::optional<ETextureColorSpace> ColorSpace = std::nullopt) const;
	// Any other staged file, such as a level, its navigation, or a shader's parameter table.
	[[nodiscard]] std::expected<std::vector<std::byte>, FAssetError> ReadFile(const std::filesystem::path& Path) const;
	[[nodiscard]] bool Exists(const std::filesystem::path& Path) const;
	[[nodiscard]] std::span<const std::filesystem::path> GetMountRoots() const noexcept;

private:
	std::vector<std::filesystem::path> MountRoots;
	std::shared_ptr<const FVirtualFileSystem> FileSystem;
};
}
