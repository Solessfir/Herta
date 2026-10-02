#include "Herta/AssetPipeline/AssetCooker.h"

#include "FileUtilities.h"
#include "GltfCooker.h"
#include "Herta/AssetPipeline/AssetMetadata.h"
#include "Herta/AssetPipeline/BuildKey.h"
#include "Herta/AssetPipeline/DerivedDataCache.h"
#include "Herta/AssetPipeline/TextureCooker.h"

#include <format>

namespace Herta
{
namespace
{
inline constexpr std::uint64_t MaximumSourceFileSize = std::uint64_t{1} << 30;

[[nodiscard]] std::expected<std::vector<std::byte>, FAssetError> ReadSource(const std::filesystem::path& Path)
{
	std::error_code Error;
	if (std::filesystem::symlink_status(Path, Error).type() != std::filesystem::file_type::regular)
	{
		return std::unexpected(FAssetError{std::format("'{}' is missing or not a regular file", PathToUtf8(Path))});
	}
	std::expected<std::optional<std::vector<std::byte>>, FAssetError> Bytes = ReadWholeFile(Path, MaximumSourceFileSize);
	if (!Bytes)
	{
		return std::unexpected(std::move(Bytes.error()));
	}
	if (!*Bytes)
	{
		return std::unexpected(FAssetError{std::format("'{}' disappeared while cooking", PathToUtf8(Path))});
	}
	return std::move(**Bytes);
}

[[nodiscard]] std::expected<ETextureColorSpace, FAssetError> GetTextureColorSpace(const FAssetImportSettings& Settings)
{
	ETextureColorSpace ColorSpace = ETextureColorSpace::Srgb;
	for (const auto& [Name, Value] : Settings)
	{
		if (Name != "ColorSpace" || (Value != "Srgb" && Value != "Linear"))
		{
			return std::unexpected(FAssetError{std::format("Unsupported texture setting '{} = {}'. Use ColorSpace = Srgb or Linear", Name, Value)});
		}
		ColorSpace = Value == "Linear" ? ETextureColorSpace::Linear : ETextureColorSpace::Srgb;
	}
	return ColorSpace;
}
}

std::expected<FAssetCookResult, FAssetError> CookAsset(const FAssetCookRequest& Request)
{
	if (!IsValidAssetPath(Request.SourcePath) || Request.TargetPlatform.empty())
	{
		return std::unexpected(FAssetError{std::format("Invalid cook request for '{}'", Request.SourcePath)});
	}

	const std::filesystem::path SourceFile = Request.ContentRoot / Utf8ToPath(Request.SourcePath);
	std::expected<FAssetMetadata, FAssetError> Metadata = LoadAssetMetadata(SourceFile);
	if (!Metadata)
	{
		return std::unexpected(std::move(Metadata.error()));
	}
	std::expected<std::vector<std::byte>, FAssetError> Source = ReadSource(SourceFile);
	if (!Source)
	{
		return std::unexpected(std::move(Source.error()));
	}

	FAssetBuildKeyInput KeyInput{HashBytes(*Source), Request.SourcePath, Metadata->Importer, 0, Metadata->Settings, {}, Request.TargetPlatform, CookedAssetFormatVersion};
	std::expected<ETextureColorSpace, FAssetError> ColorSpace = ETextureColorSpace::Srgb;
	if (Metadata->Importer == "Texture")
	{
		KeyInput.ImporterVersion = TextureImporterVersion;
		ColorSpace = GetTextureColorSpace(Metadata->Settings);
		if (!ColorSpace)
		{
			return std::unexpected(std::move(ColorSpace.error()));
		}
	}
	else if (Metadata->Importer == "Gltf")
	{
		KeyInput.ImporterVersion = GltfImporterVersion;
		if (!Metadata->Settings.empty())
		{
			return std::unexpected(FAssetError{"The Gltf importer has no settings"});
		}
		std::expected<std::vector<std::string>, FAssetError> Dependencies = FindGltfDependencies(Request.ContentRoot, Request.SourcePath, *Source);
		if (!Dependencies)
		{
			return std::unexpected(std::move(Dependencies.error()));
		}
		for (std::string& Dependency : *Dependencies)
		{
			std::expected<std::vector<std::byte>, FAssetError> Bytes = ReadSource(Request.ContentRoot / Utf8ToPath(Dependency));
			if (!Bytes)
			{
				return std::unexpected(std::move(Bytes.error()));
			}
			KeyInput.Dependencies.push_back({std::move(Dependency), HashBytes(*Bytes)});
		}
	}
	else
	{
		return std::unexpected(FAssetError{std::format("The {} importer is not available yet", Metadata->Importer)});
	}

	FAssetCookResult Result;
	Result.Key = ComputeAssetBuildKey(KeyInput);
	const FDerivedDataCache Cache(Request.DerivedDataRoot);
	if (!Request.bForce)
	{
		std::expected<std::optional<std::vector<std::byte>>, FAssetError> Cached = Cache.Get(Result.Key);
		if (Cached && *Cached)
		{
			Result.bCacheHit = true;
			return Result;
		}
		if (!Cached)
		{
			Result.Warnings.push_back(std::format("{}; cooking again", Cached.error().Message));
		}
	}

	std::expected<FCookedAsset, FAssetError> Cooked = std::unexpected(FAssetError{});
	if (Metadata->Importer == "Texture")
	{
		std::expected<FCookedTexture, FAssetError> Texture = CookEncodedTexture(*Source, *ColorSpace);
		Cooked = Texture ? std::expected<FCookedAsset, FAssetError>(std::move(*Texture)) : std::unexpected(std::move(Texture.error()));
	}
	else
	{
		std::expected<FCookedModel, FAssetError> Model = CookGltf(Request.ContentRoot, Request.SourcePath, *Source, Result.Warnings);
		Cooked = Model ? std::expected<FCookedAsset, FAssetError>(std::move(*Model)) : std::unexpected(std::move(Model.error()));
	}
	if (!Cooked)
	{
		return std::unexpected(std::move(Cooked.error()));
	}

	std::expected<std::vector<std::byte>, FAssetError> Bytes = SerializeCookedAsset(*Cooked);
	if (!Bytes)
	{
		return std::unexpected(std::move(Bytes.error()));
	}
	if (std::expected<void, FAssetError> Stored = Cache.Put(Result.Key, *Bytes); !Stored)
	{
		return std::unexpected(std::move(Stored.error()));
	}
	return Result;
}

std::expected<FCookedAsset, FAssetError> LoadCookedAsset(const std::filesystem::path& DerivedDataRoot, const FHash128& Key)
{
	std::expected<std::optional<std::vector<std::byte>>, FAssetError> Bytes = FDerivedDataCache(DerivedDataRoot).Get(Key);
	if (!Bytes)
	{
		return std::unexpected(std::move(Bytes.error()));
	}
	if (!*Bytes)
	{
		return std::unexpected(FAssetError{std::format("Derived data {} is missing", ToString(Key))});
	}
	return DeserializeCookedAsset(**Bytes);
}
}
