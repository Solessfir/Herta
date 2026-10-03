#include "Herta/AssetPipeline/AssetCooker.h"

#include "BlenderImporter.h"
#include "FileUtilities.h"
#include "GltfCooker.h"
#include "Herta/AssetPipeline/AssetMetadata.h"
#include "Herta/AssetPipeline/BuildKey.h"
#include "Herta/AssetPipeline/DerivedDataCache.h"
#include "Herta/AssetPipeline/TextureCooker.h"

#include <format>
#include <optional>
#include <ranges>

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

// Content-relative paths, one per line. A record is keyed by the .blend and Blender version, so its dependencies are known without starting Blender.
[[nodiscard]] std::vector<std::byte> SerializeDependencyRecord(const std::vector<std::string>& Dependencies)
{
	std::string Text;
	for (const std::string& Dependency : Dependencies)
	{
		Text += Dependency;
		Text += '\n';
	}
	const std::span<const std::byte> Bytes = std::as_bytes(std::span(Text));
	return {Bytes.begin(), Bytes.end()};
}

[[nodiscard]] std::optional<std::vector<std::string>> ParseDependencyRecord(const std::span<const std::byte> Bytes)
{
	std::vector<std::string> Dependencies;
	const std::string_view Text(reinterpret_cast<const char*>(Bytes.data()), Bytes.size());
	for (const auto Line : std::views::split(Text, '\n'))
	{
		const std::string_view Path(Line.begin(), Line.end());
		if (Path.empty())
		{
			continue;
		}
		if (!IsValidAssetPath(Path))
		{
			return std::nullopt;
		}
		Dependencies.emplace_back(Path);
	}
	return Dependencies;
}

// Missing dependencies hash to zero, so creating the file later changes the key.
[[nodiscard]] std::expected<FHash128, FAssetError> HashDependency(const std::filesystem::path& ContentRoot, const std::string& Path)
{
	std::expected<std::optional<std::vector<std::byte>>, FAssetError> Bytes = ReadWholeFile(ContentRoot / Utf8ToPath(Path), MaximumSourceFileSize);
	if (!Bytes)
	{
		return std::unexpected(std::move(Bytes.error()));
	}
	return *Bytes ? HashBytes(**Bytes) : FHash128{};
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
	const FDerivedDataCache Cache(Request.DerivedDataRoot);
	FAssetCookResult Result;
	std::optional<FBlenderInstallation> Blender;
	std::optional<FBlenderExport> BlenderExport;
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
	else if (Metadata->Importer == "Blender")
	{
		KeyInput.ImporterVersion = BlenderImporterVersion;
		if (!Metadata->Settings.empty())
		{
			return std::unexpected(FAssetError{"The Blender importer has no settings"});
		}
		std::expected<FBlenderInstallation, FAssetError> Found = FindBlender();
		if (!Found)
		{
			return std::unexpected(std::move(Found.error()));
		}
		Blender = std::move(*Found);
		KeyInput.Dependencies.push_back({"<Blender>", HashBytes(std::as_bytes(std::span(Blender->Version)))});

		FAssetBuildKeyInput RecordInput = KeyInput;
		RecordInput.Importer = "Blender/Dependencies";
		const FHash128 RecordKey = ComputeAssetBuildKey(RecordInput);
		std::optional<std::vector<std::string>> Dependencies;
		if (!Request.bForce)
		{
			std::expected<std::optional<std::vector<std::byte>>, FAssetError> Record = Cache.Get(RecordKey);
			if (Record && *Record)
			{
				Dependencies = ParseDependencyRecord(**Record);
			}
		}
		if (!Dependencies)
		{
			std::expected<FBlenderExport, FAssetError> Exported = ExportBlend(*Blender, Request.ContentRoot, Request.SourcePath);
			if (!Exported)
			{
				return std::unexpected(std::move(Exported.error()));
			}
			BlenderExport = std::move(*Exported);
			Dependencies = BlenderExport->Dependencies;
			std::ranges::move(BlenderExport->Warnings, std::back_inserter(Result.Warnings));
			if (std::expected<void, FAssetError> Stored = Cache.Put(RecordKey, SerializeDependencyRecord(*Dependencies)); !Stored)
			{
				return std::unexpected(std::move(Stored.error()));
			}
		}
		for (std::string& Dependency : *Dependencies)
		{
			std::expected<FHash128, FAssetError> Hash = HashDependency(Request.ContentRoot, Dependency);
			if (!Hash)
			{
				return std::unexpected(std::move(Hash.error()));
			}
			KeyInput.Dependencies.push_back({std::move(Dependency), *Hash});
		}
	}
	else
	{
		return std::unexpected(FAssetError{std::format("The {} importer is not available yet", Metadata->Importer)});
	}

	Result.Key = ComputeAssetBuildKey(KeyInput);
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
	else if (Metadata->Importer == "Gltf")
	{
		std::expected<FCookedModel, FAssetError> Model = CookGltf(Request.ContentRoot, Request.SourcePath, *Source, Result.Warnings);
		Cooked = Model ? std::expected<FCookedAsset, FAssetError>(std::move(*Model)) : std::unexpected(std::move(Model.error()));
	}
	else
	{
		if (!BlenderExport)
		{
			std::expected<FBlenderExport, FAssetError> Exported = ExportBlend(*Blender, Request.ContentRoot, Request.SourcePath);
			if (!Exported)
			{
				return std::unexpected(std::move(Exported.error()));
			}
			BlenderExport = std::move(*Exported);
			std::ranges::move(BlenderExport->Warnings, std::back_inserter(Result.Warnings));
		}
		// The GLB is parsed as if it sat beside the .blend. The preset embeds every image, so an external reference means the export is not what Herta asked for.
		std::expected<std::vector<std::string>, FAssetError> External = FindGltfDependencies(Request.ContentRoot, Request.SourcePath, BlenderExport->Glb);
		if (!External || !External->empty())
		{
			return std::unexpected(External ? FAssetError{std::format("Blender's export of '{}' references external files", Request.SourcePath)} : std::move(External.error()));
		}
		std::expected<FCookedModel, FAssetError> Model = CookGltf(Request.ContentRoot, Request.SourcePath, BlenderExport->Glb, Result.Warnings);
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
