#include "Herta/Assets/CookedContent.h"

#include <format>
#include <fstream>
#include <system_error>
#include <utility>

namespace Herta
{
namespace
{
// Larger than any valid cooked asset: textures are capped at 4096 px with mips, and buffers at the upload budget.
constexpr std::uintmax_t MaximumStagedAssetBytes = std::uintmax_t{1} << 30;
}

std::filesystem::path GetStagedAssetPath(const std::filesystem::path& MountRoot, const FAssetId& Asset, const std::optional<ETextureColorSpace> ColorSpace)
{
	const std::string_view Suffix = !ColorSpace ? ".hasset" : *ColorSpace == ETextureColorSpace::Srgb ? ".srgb.hasset"
	                                                                                                  : ".linear.hasset";
	return MountRoot / (Asset.ToString() + std::string(Suffix));
}

FCookedContent::FCookedContent(std::vector<std::filesystem::path> InMountRoots)
    : MountRoots(std::move(InMountRoots))
{
}

std::expected<FCookedAsset, FAssetError> FCookedContent::Load(const FAssetId& Asset, const std::optional<ETextureColorSpace> ColorSpace) const
{
	for (const std::filesystem::path& Root : MountRoots)
	{
		const std::filesystem::path Path = GetStagedAssetPath(Root, Asset, ColorSpace);
		std::error_code Error;
		const std::uintmax_t Size = std::filesystem::file_size(Path, Error);
		if (Error)
		{
			continue;
		}

		if (Size > MaximumStagedAssetBytes)
		{
			return std::unexpected(FAssetError{std::format("Staged asset {} exceeds the cooked asset size limit", Asset.ToString())});
		}

		std::vector<std::byte> Bytes(static_cast<std::size_t>(Size));
		std::ifstream File(Path, std::ios::binary);
		if (!File.read(reinterpret_cast<char*>(Bytes.data()), static_cast<std::streamsize>(Bytes.size())))
		{
			return std::unexpected(FAssetError{std::format("Cannot read staged asset {}", Asset.ToString())});
		}

		auto Cooked = DeserializeCookedAsset(Bytes);
		if (!Cooked)
		{
			return std::unexpected(FAssetError{std::format("Staged asset {} is invalid: {}", Asset.ToString(), Cooked.error().Message)});
		}

		const auto* const Texture = std::get_if<FCookedTexture>(&*Cooked);
		if (ColorSpace.has_value() != (Texture != nullptr) || (Texture && Texture->ColorSpace != *ColorSpace))
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
