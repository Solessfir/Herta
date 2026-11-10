#include "Herta/Assets/CookedContent.h"

#include "Herta/Assets/VirtualFileSystem.h"
#include "Herta/Core/Hash.h"

#include <format>
#include <fstream>
#include <span>
#include <system_error>
#include <utility>

namespace Herta
{
std::filesystem::path GetStagedAssetPath(const std::filesystem::path& MountRoot, const FAssetId& Asset, const std::optional<ETextureColorSpace> ColorSpace)
{
	const std::string_view Suffix = !ColorSpace ? ".hasset" : *ColorSpace == ETextureColorSpace::Srgb ? ".srgb.hasset"
	                                                                                                  : ".linear.hasset";
	return MountRoot / (Asset.ToString() + std::string(Suffix));
}

namespace
{
// Staged shaders sit flat under Shaders, named by a hash of their mount-relative key: mirroring source folders and switch suffixes made paths that
// passed the Windows path limit in deep output directories, and a key's '#', '=', and ',' have no business in file names.
std::filesystem::path StagedMaterialShaderFile(const std::filesystem::path& MountRoot, const std::string_view RelativePath, const std::string_view Suffix)
{
	return MountRoot / "Shaders" / std::format("{}{}", ToString(HashBytes(std::as_bytes(std::span(RelativePath)))), Suffix);
}
}

std::filesystem::path GetStagedMaterialShaderPath(const std::filesystem::path& MountRoot, const std::string_view RelativePath, const std::string_view Stage)
{
	return StagedMaterialShaderFile(MountRoot, RelativePath, std::format(".{}.hshader", Stage));
}

std::filesystem::path GetStagedMaterialShaderParametersPath(const std::filesystem::path& MountRoot, const std::string_view RelativePath)
{
	return StagedMaterialShaderFile(MountRoot, RelativePath, ".parameters");
}

FCookedContent::FCookedContent(std::vector<std::filesystem::path> InMountRoots, std::shared_ptr<const FVirtualFileSystem> InFileSystem)
    : MountRoots(std::move(InMountRoots))
    , FileSystem(InFileSystem ? std::move(InFileSystem) : std::make_shared<const FVirtualFileSystem>())
{
}

std::expected<std::vector<std::byte>, FAssetError> FCookedContent::ReadFile(const std::filesystem::path& Path) const
{
	return FileSystem ? FileSystem->Read(Path) : ReadDiskFile(Path);
}

bool FCookedContent::Exists(const std::filesystem::path& Path) const
{
	std::error_code Error;
	return FileSystem ? FileSystem->Exists(Path) : std::filesystem::is_regular_file(Path, Error);
}

std::expected<FCookedAsset, FAssetError> FCookedContent::Load(const FAssetId& Asset, const std::optional<ETextureColorSpace> ColorSpace) const
{
	for (const std::filesystem::path& Root : MountRoots)
	{
		const std::filesystem::path Path = GetStagedAssetPath(Root, Asset, ColorSpace);
		if (!Exists(Path))
		{
			continue;
		}

		const auto Bytes = ReadFile(Path);
		if (!Bytes)
		{
			return std::unexpected(FAssetError{std::format("Cannot read staged asset {}: {}", Asset.ToString(), Bytes.error().Message)});
		}

		auto Cooked = DeserializeCookedAsset(*Bytes);
		if (!Cooked)
		{
			return std::unexpected(FAssetError{std::format("Staged asset {} is invalid: {}", Asset.ToString(), Cooked.error().Message)});
		}

		const auto* const Texture = std::get_if<FCookedTexture>(&*Cooked);
		if (ColorSpace.has_value() != (Texture != nullptr) || (Texture && Texture->ColorSpace != ColorSpace))
		{
			return std::unexpected(FAssetError{std::format("Staged asset {} does not match the requested asset kind or color space", Asset.ToString())});
		}

		return Cooked;
	}

	return std::unexpected(FAssetError{std::format("Asset {} is not staged in any mounted content", Asset.ToString())});
}

std::span<const std::filesystem::path> FCookedContent::GetMountRoots() const noexcept
{
	return MountRoots;
}
}
