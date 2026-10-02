#include "Herta/AssetPipeline/ContentRoot.h"

#include "FileUtilities.h"

#include <algorithm>
#include <array>
#include <format>
#include <iterator>
#include <map>
#include <set>
#include <utility>

namespace Herta
{
namespace
{
inline constexpr std::uint64_t MaximumMetadataFileSize = 64ull * 1024;
inline constexpr std::uint64_t MaximumSourceFileSize = 4ull * 1024 * 1024 * 1024;

struct FImporterExtension
{
	std::string_view Extension;
	std::string_view Importer;
};

inline constexpr std::array ImporterExtensions{
    FImporterExtension{".blend", "Blender"},
    FImporterExtension{".glb", "Gltf"},
    FImporterExtension{".gltf", "Gltf"},
    FImporterExtension{".jpeg", "Texture"},
    FImporterExtension{".jpg", "Texture"},
    FImporterExtension{".png", "Texture"}};

[[nodiscard]] std::string ToLowerAscii(std::string Text)
{
	for (char& Character : Text)
	{
		if (Character >= 'A' && Character <= 'Z')
		{
			Character = static_cast<char>(Character - 'A' + 'a');
		}
	}
	return Text;
}

[[nodiscard]] bool IsInside(const std::filesystem::path& Relative)
{
	return !Relative.empty() && Relative.is_relative() && *Relative.begin() != "..";
}
}

std::optional<std::string_view> FindImporterForSource(const std::filesystem::path& Source)
{
	const std::string Extension = ToLowerAscii(PathToUtf8(Source.extension()));
	const auto Iterator = std::ranges::find(ImporterExtensions, Extension, &FImporterExtension::Extension);
	return Iterator != ImporterExtensions.end() ? std::optional(Iterator->Importer) : std::nullopt;
}

std::expected<FContentScanResult, FAssetError> ScanContentRoot(const std::filesystem::path& ContentRoot)
{
	std::error_code Error;
	if (!std::filesystem::is_directory(ContentRoot, Error))
	{
		return std::unexpected(FAssetError{std::format("Content root '{}' is not a directory", PathToUtf8(ContentRoot))});
	}

	FContentScanResult Result;
	std::set<std::string> Sources;
	std::vector<std::string> MetadataFiles;
	std::filesystem::recursive_directory_iterator Iterator(ContentRoot, Error);
	for (; !Error && Iterator != std::filesystem::recursive_directory_iterator(); Iterator.increment(Error))
	{
		const std::filesystem::directory_entry& Entry = *Iterator;
		const std::string RelativePath = GenericPathToUtf8(Entry.path().lexically_relative(ContentRoot));
		if (PathToUtf8(Entry.path().filename()).starts_with('.'))
		{
			if (Entry.is_directory(Error))
			{
				Iterator.disable_recursion_pending();
			}
			continue;
		}
		if (Entry.is_symlink(Error))
		{
			Result.Errors.push_back({RelativePath, "Symbolic links are not supported in content"});
			continue;
		}
		if (!Entry.is_regular_file(Error))
		{
			continue;
		}

		if (RelativePath.ends_with(AssetMetadataExtension))
		{
			MetadataFiles.push_back(RelativePath);
		}
		else
		{
			Sources.insert(RelativePath);
		}
	}
	if (Error)
	{
		return std::unexpected(FAssetError{std::format("Cannot enumerate content root '{}': {}", PathToUtf8(ContentRoot), Error.message())});
	}

	std::ranges::sort(MetadataFiles);
	std::set<std::string> RegisteredSources;
	std::map<FAssetId, std::vector<FAssetRecord>> RecordsById;
	for (const std::string& MetadataFile : MetadataFiles)
	{
		std::string SourcePath = MetadataFile.substr(0, MetadataFile.size() - AssetMetadataExtension.size());
		RegisteredSources.insert(SourcePath);
		if (!Sources.contains(SourcePath))
		{
			Result.Errors.push_back({MetadataFile, "Metadata has no source file"});
			continue;
		}
		if (!IsValidAssetPath(SourcePath))
		{
			Result.Errors.push_back({SourcePath, "Path is not portable across Windows and Linux"});
			continue;
		}

		std::expected<std::optional<std::vector<std::byte>>, FAssetError> Bytes = ReadWholeFile(ContentRoot / Utf8ToPath(MetadataFile), MaximumMetadataFileSize);
		if (!Bytes || !*Bytes)
		{
			Result.Errors.push_back({MetadataFile, Bytes ? "Metadata disappeared during the scan" : Bytes.error().Message});
			continue;
		}

		const std::string_view Text(reinterpret_cast<const char*>((*Bytes)->data()), (*Bytes)->size());
		std::expected<FAssetMetadata, FAssetError> Metadata = ParseAssetMetadata(Text);
		if (!Metadata)
		{
			Result.Errors.push_back({MetadataFile, Metadata.error().Message});
			continue;
		}
		RecordsById[Metadata->Id].push_back(FAssetRecord{Metadata->Id, std::move(SourcePath), std::move(Metadata->Importer)});
	}

	std::vector<FAssetRecord> Records;
	for (auto& [Id, Matches] : RecordsById)
	{
		if (Matches.size() == 1)
		{
			Records.push_back(std::move(Matches.front()));
			continue;
		}
		for (const FAssetRecord& Match : Matches)
		{
			Result.Errors.push_back({Match.SourcePath, std::format("ID {} is shared by {} assets", Id.ToString(), Matches.size())});
		}
	}

	std::expected<FAssetRegistry, FAssetError> Registry = FAssetRegistry::Create(std::move(Records));
	if (Registry)
	{
		Result.Registry = std::move(*Registry);
	}
	else
	{
		Result.Errors.push_back({{}, Registry.error().Message});
	}

	std::ranges::set_difference(Sources, RegisteredSources, std::back_inserter(Result.UnregisteredSources));
	std::ranges::stable_sort(Result.Errors, {}, &FContentDiagnostic::Path);
	return Result;
}

std::expected<FImportedSource, FAssetError> ImportSource(const std::filesystem::path& ContentRoot, const std::filesystem::path& Source, const std::string_view DestinationDirectory)
{
	std::error_code Error;
	if (!std::filesystem::is_directory(ContentRoot, Error))
	{
		return std::unexpected(FAssetError{std::format("Content root '{}' is not a directory", PathToUtf8(ContentRoot))});
	}
	if (std::filesystem::symlink_status(Source, Error).type() != std::filesystem::file_type::regular)
	{
		return std::unexpected(FAssetError{std::format("'{}' is not a regular file", PathToUtf8(Source))});
	}
	if (!DestinationDirectory.empty() && !IsValidAssetPath(DestinationDirectory))
	{
		return std::unexpected(FAssetError{std::format("Invalid destination directory '{}'", DestinationDirectory)});
	}

	const std::optional<std::string_view> Importer = FindImporterForSource(Source);
	if (!Importer)
	{
		return std::unexpected(FAssetError{std::format("No importer handles '{}' files", PathToUtf8(Source.extension()))});
	}

	const std::filesystem::path CanonicalRoot = std::filesystem::weakly_canonical(ContentRoot, Error);
	const std::filesystem::path CanonicalSource = Error ? std::filesystem::path() : std::filesystem::weakly_canonical(Source, Error);
	if (Error)
	{
		return std::unexpected(FAssetError{std::format("Cannot resolve '{}': {}", PathToUtf8(Source), Error.message())});
	}

	const std::filesystem::path RelativeSource = CanonicalSource.lexically_relative(CanonicalRoot);
	const bool bInsideContent = IsInside(RelativeSource);
	const std::filesystem::path Destination = bInsideContent ? CanonicalSource : CanonicalRoot / Utf8ToPath(DestinationDirectory) / Source.filename();
	FImportedSource Imported;
	Imported.SourcePath = GenericPathToUtf8(Destination.lexically_relative(CanonicalRoot));
	if (!IsValidAssetPath(Imported.SourcePath))
	{
		return std::unexpected(FAssetError{std::format("'{}' is not a portable content path", Imported.SourcePath)});
	}

	const std::filesystem::path MetadataPath = GetAssetMetadataPath(Destination);
	if (std::filesystem::exists(MetadataPath, Error))
	{
		return std::unexpected(FAssetError{std::format("'{}' is already imported", Imported.SourcePath)});
	}

	Imported.Metadata = FAssetMetadata{FAssetId::Generate(), std::string(*Importer), {}};
	std::expected<std::string, FAssetError> Text = SerializeAssetMetadata(Imported.Metadata);
	if (!Text)
	{
		return std::unexpected(std::move(Text.error()));
	}

	if (!bInsideContent)
	{
		if (std::filesystem::exists(Destination, Error))
		{
			return std::unexpected(FAssetError{std::format("'{}' already exists in content", Imported.SourcePath)});
		}

		std::expected<std::optional<std::vector<std::byte>>, FAssetError> Bytes = ReadWholeFile(Source, MaximumSourceFileSize);
		if (!Bytes || !*Bytes)
		{
			return std::unexpected(Bytes ? FAssetError{std::format("'{}' disappeared during import", PathToUtf8(Source))} : std::move(Bytes.error()));
		}
		if (std::expected<void, FAssetError> Copied = WriteFileAtomically(Destination, **Bytes); !Copied)
		{
			return std::unexpected(std::move(Copied.error()));
		}
	}

	if (std::expected<void, FAssetError> Written = WriteFileAtomically(MetadataPath, std::as_bytes(std::span(*Text))); !Written)
	{
		if (!bInsideContent)
		{
			std::filesystem::remove(Destination, Error);
		}
		return std::unexpected(std::move(Written.error()));
	}
	return Imported;
}
}
