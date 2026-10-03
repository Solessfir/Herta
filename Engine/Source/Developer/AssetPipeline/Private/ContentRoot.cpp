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
inline constexpr std::uint64_t MaximumSourceFileSize = 4ull * 1024 * 1024 * 1024;

struct FImporterExtension
{
	std::string_view Extension;
	std::string_view Importer;
};

inline constexpr std::array ImporterExtensions{
    FImporterExtension{.Extension = ".blend", .Importer = "Blender"},
    FImporterExtension{.Extension = ".glb", .Importer = "Gltf"},
    FImporterExtension{.Extension = ".gltf", .Importer = "Gltf"},
    FImporterExtension{.Extension = ".jpeg", .Importer = "Texture"},
    FImporterExtension{.Extension = ".jpg", .Importer = "Texture"},
    FImporterExtension{.Extension = ".png", .Importer = "Texture"},
};

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

std::vector<std::string_view> GetImportableExtensions()
{
	std::vector<std::string_view> Extensions;
	for (const FImporterExtension& Entry : ImporterExtensions)
	{
		Extensions.push_back(Entry.Extension);
	}

	return Extensions;
}

std::optional<std::string_view> FindImporterForSource(const std::filesystem::path& Source)
{
	const std::string Extension = ToLowerAscii(PathToUtf8(Source.extension()));
	const auto Iterator = std::ranges::find(ImporterExtensions, Extension, &FImporterExtension::Extension);
	return Iterator != ImporterExtensions.end() ? std::optional(Iterator->Importer) : std::nullopt;
}

FContentSnapshot TakeContentSnapshot(const std::filesystem::path& ContentRoot)
{
	// Errors leave entries out, so a vanished or unreadable file reads as a change on the next poll.
	FContentSnapshot Snapshot;
	std::error_code Error;
	std::filesystem::recursive_directory_iterator Iterator(ContentRoot, Error);
	for (; !Error && Iterator != std::filesystem::recursive_directory_iterator(); Iterator.increment(Error))
	{
		const std::filesystem::directory_entry& Entry = *Iterator;
		if (PathToUtf8(Entry.path().filename()).starts_with('.'))
		{
			if (Entry.is_directory(Error))
			{
				Iterator.disable_recursion_pending();
			}

			continue;
		}

		if (Entry.is_symlink(Error) || !Entry.is_regular_file(Error))
		{
			continue;
		}

		FContentFileStamp Stamp{.Size = Entry.file_size(Error), .LastWriteTime = Entry.last_write_time(Error)};
		if (!Error)
		{
			Snapshot.emplace(GenericPathToUtf8(Entry.path().lexically_relative(ContentRoot)), Stamp);
		}

		Error.clear();
	}

	return Snapshot;
}

std::expected<FContentScanResult, FAssetError> ScanContentRoot(const std::filesystem::path& ContentRoot, FContentScanCache* const Cache)
{
	std::error_code Error;
	if (!std::filesystem::is_directory(ContentRoot, Error))
	{
		return std::unexpected(FAssetError{std::format("Content root '{}' is not a directory", PathToUtf8(ContentRoot))});
	}

	FContentScanResult Result;
	std::set<std::string> Sources;
	std::vector<std::pair<std::string, FContentFileStamp>> MetadataFiles;
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
			Result.Errors.push_back({.Path = RelativePath, .Message = "Symbolic links are not supported in content"});
			continue;
		}

		if (!Entry.is_regular_file(Error))
		{
			continue;
		}

		if (RelativePath.ends_with(AssetMetadataExtension))
		{
			std::error_code StampError;
			FContentFileStamp Stamp{.Size = Entry.file_size(StampError), .LastWriteTime = Entry.last_write_time(StampError)};
			MetadataFiles.emplace_back(RelativePath, StampError ? FContentFileStamp{} : Stamp);
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

	std::ranges::sort(MetadataFiles, {}, &std::pair<std::string, FContentFileStamp>::first);
	std::set<std::string> RegisteredSources;
	std::map<FAssetId, std::vector<FAssetRecord>> RecordsById;
	std::map<std::string, FContentScanCache::FEntry> UpdatedCache;
	for (const auto& [MetadataFile, Stamp] : MetadataFiles)
	{
		std::string SourcePath = MetadataFile.substr(0, MetadataFile.size() - AssetMetadataExtension.size());
		RegisteredSources.insert(SourcePath);
		if (!Sources.contains(SourcePath))
		{
			Result.Errors.push_back({.Path = MetadataFile, .Message = "Metadata has no source file"});
			continue;
		}

		if (!IsValidAssetPath(SourcePath))
		{
			Result.Errors.push_back({.Path = SourcePath, .Message = "Path is not portable across Windows and Linux"});
			continue;
		}

		// A zero stamp means the write time could not be read, so the sidecar is always parsed again.
		std::expected<FAssetMetadata, FAssetError> Metadata = std::unexpected(FAssetError{});
		const auto Cached = Cache ? Cache->Metadata.find(MetadataFile) : std::map<std::string, FContentScanCache::FEntry>::iterator{};
		if (Cache && Cached != Cache->Metadata.end() && Cached->second.Stamp == Stamp && Stamp != FContentFileStamp{})
		{
			Metadata = Cached->second.Metadata;
		}
		else
		{
			Metadata = LoadAssetMetadata(ContentRoot / Utf8ToPath(SourcePath));
		}

		if (Cache)
		{
			UpdatedCache.insert_or_assign(MetadataFile, FContentScanCache::FEntry{.Stamp = Stamp, .Metadata = Metadata});
		}

		if (!Metadata)
		{
			Result.Errors.push_back({.Path = MetadataFile, .Message = Metadata.error().Message});
			continue;
		}

		RecordsById[Metadata->Id].push_back(FAssetRecord{.Id = Metadata->Id, .SourcePath = std::move(SourcePath), .Importer = std::move(Metadata->Importer)});
	}

	if (Cache)
	{
		Cache->Metadata = std::move(UpdatedCache);
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
			Result.Errors.push_back({.Path = Match.SourcePath, .Message = std::format("ID {} is shared by {} assets", Id.ToString(), Matches.size())});
		}
	}

	std::expected<FAssetRegistry, FAssetError> Registry = FAssetRegistry::Create(std::move(Records));
	if (Registry)
	{
		Result.Registry = std::move(*Registry);
	}
	else
	{
		Result.Errors.push_back({.Path = {}, .Message = Registry.error().Message});
	}

	// Only files an importer understands can become assets; fonts, licenses, and other raw files are not reported.
	std::ranges::copy_if(Sources, std::back_inserter(Result.UnregisteredSources), [&RegisteredSources](const std::string& Source)
	{
		return !RegisteredSources.contains(Source) && FindImporterForSource(Utf8ToPath(Source));
	});

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
	// A .gltf usually references separate buffers and images. Copying it alone would register an asset that cannot cook.
	if (!bInsideContent && ToLowerAscii(PathToUtf8(Source.extension())) == ".gltf")
	{
		return std::unexpected(FAssetError{"A .gltf is imported in place. Copy it with its buffers and images into content first, or import a self-contained .glb"});
	}

	const std::filesystem::path Destination = bInsideContent ? CanonicalSource : CanonicalRoot / Utf8ToPath(DestinationDirectory) / Source.filename();
	const std::filesystem::path DestinationParent = std::filesystem::weakly_canonical(Destination.parent_path(), Error);
	if (Error || !IsInside(DestinationParent.lexically_relative(CanonicalRoot)) || DestinationParent != Destination.parent_path().lexically_normal())
	{
		return std::unexpected(FAssetError{"Import destination must remain inside content and cannot traverse linked directories"});
	}

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

	Imported.Metadata = FAssetMetadata{.Id = FAssetId::Generate(), .Importer = std::string(*Importer), .Settings = {}};
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

		if (std::expected<void, FAssetError> Copied = WriteFileAtomically(Destination, **Bytes, false); !Copied)
		{
			return std::unexpected(std::move(Copied.error()));
		}
	}

	if (std::expected<void, FAssetError> Written = WriteFileAtomically(MetadataPath, std::as_bytes(std::span(*Text)), false); !Written)
	{
		// Preserve the source: another in-place import may have registered it while this import was copying.
		return std::unexpected(std::move(Written.error()));
	}

	return Imported;
}
}
