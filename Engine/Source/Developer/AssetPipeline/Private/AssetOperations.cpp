#include "Herta/AssetPipeline/AssetOperations.h"

#include "FileUtilities.h"

#include <algorithm>
#include <chrono>
#include <format>
#include <utility>

namespace Herta
{
namespace
{
[[nodiscard]] std::unexpected<FAssetError> OperationError(std::string Message)
{
	return std::unexpected(FAssetError{std::move(Message)});
}

// Renames each pair in order; on a failure the ones already renamed go back, newest first.
[[nodiscard]] std::expected<void, FAssetError> RenameAll(const std::vector<std::pair<std::filesystem::path, std::filesystem::path>>& Moves)
{
	for (std::size_t Index = 0; Index < Moves.size(); ++Index)
	{
		std::error_code Error;
		std::filesystem::create_directories(Moves[Index].second.parent_path(), Error);
		std::filesystem::rename(Moves[Index].first, Moves[Index].second, Error);
		if (Error)
		{
			for (std::size_t Undo = Index; Undo-- > 0;)
			{
				std::error_code Ignored;
				std::filesystem::rename(Moves[Undo].second, Moves[Undo].first, Ignored);
			}

			return OperationError(std::format("Cannot move {} to {}: {}", PathToUtf8(Moves[Index].first), PathToUtf8(Moves[Index].second), Error.message()));
		}
	}

	return {};
}
}

std::expected<std::vector<std::filesystem::path>, FAssetError> GetAssetFiles(const std::filesystem::path& ContentRoot, const std::string_view Path)
{
	if (!IsValidAssetPath(Path))
	{
		return OperationError(std::format("'{}' is not a content path", Path));
	}

	const std::filesystem::path Source = ContentRoot / Utf8ToPath(Path);
	std::error_code Error;
	if (!std::filesystem::is_regular_file(Source, Error) || !std::filesystem::is_regular_file(Source.string() + ".hmeta", Error))
	{
		return OperationError(std::format("{} is not an asset with metadata", Path));
	}

	std::vector<std::filesystem::path> Files{Source};
	const std::u8string Prefix = Source.filename().u8string() + u8".";
	for (const auto& Entry : std::filesystem::directory_iterator(Source.parent_path(), Error))
	{
		if (Entry.is_regular_file(Error) && Entry.path().filename().u8string().starts_with(Prefix))
		{
			Files.push_back(Entry.path());
		}
	}

	return Files;
}

std::expected<void, FAssetError> MoveAsset(const std::filesystem::path& ContentRoot, const std::string_view From, const std::string_view To)
{
	auto Files = GetAssetFiles(ContentRoot, From);
	if (!Files)
	{
		return std::unexpected(Files.error());
	}

	if (!IsValidAssetPath(To) || Utf8ToPath(From).extension() != Utf8ToPath(To).extension())
	{
		return OperationError(std::format("'{}' is not a content path with {}'s extension, which names its importer", To, From));
	}

	const std::filesystem::path Source = ContentRoot / Utf8ToPath(From);
	const std::filesystem::path Destination = ContentRoot / Utf8ToPath(To);
	std::vector<std::pair<std::filesystem::path, std::filesystem::path>> Moves;
	for (const std::filesystem::path& File : *Files)
	{
		// Companions keep their suffix after the source's name, such as .hmeta or .slang.
		const std::u8string Suffix = File.filename().u8string().substr(Source.filename().u8string().size());
		const std::filesystem::path Target = Destination.parent_path() / (Destination.filename().u8string() + Suffix);
		std::error_code Error;
		if (std::filesystem::exists(Target, Error))
		{
			return OperationError(std::format("{} already exists", PathToUtf8(Target)));
		}

		Moves.emplace_back(File, Target);
	}

	return RenameAll(Moves);
}

std::expected<std::filesystem::path, FAssetError> DeleteAsset(const std::filesystem::path& ContentRoot, const std::string_view Path, const std::filesystem::path& TrashRoot)
{
	auto Files = GetAssetFiles(ContentRoot, Path);
	if (!Files)
	{
		return std::unexpected(Files.error());
	}

	// One folder per deletion, so deleting the same path twice keeps both.
	const auto Stamp = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
	const std::filesystem::path Folder = TrashRoot / std::format("{}", Stamp);
	std::vector<std::pair<std::filesystem::path, std::filesystem::path>> Moves;
	for (const std::filesystem::path& File : *Files)
	{
		Moves.emplace_back(File, Folder / File.filename());
	}

	if (auto Moved = RenameAll(Moves); !Moved)
	{
		return std::unexpected(Moved.error());
	}

	return Folder;
}

std::expected<void, FAssetError> RestoreAsset(const std::filesystem::path& Trashed, const std::filesystem::path& ContentRoot, const std::string_view Path)
{
	if (!IsValidAssetPath(Path))
	{
		return OperationError(std::format("'{}' is not a content path", Path));
	}

	const std::filesystem::path Destination = ContentRoot / Utf8ToPath(Path);
	std::vector<std::pair<std::filesystem::path, std::filesystem::path>> Moves;
	std::error_code Error;
	for (const auto& Entry : std::filesystem::directory_iterator(Trashed, Error))
	{
		const std::filesystem::path Target = Destination.parent_path() / Entry.path().filename();
		if (std::filesystem::exists(Target, Error))
		{
			return OperationError(std::format("{} already exists", PathToUtf8(Target)));
		}

		Moves.emplace_back(Entry.path(), Target);
	}

	if (Error || Moves.empty())
	{
		return OperationError(std::format("{} holds no deleted asset", PathToUtf8(Trashed)));
	}

	if (auto Moved = RenameAll(Moves); !Moved)
	{
		return std::unexpected(Moved.error());
	}

	std::filesystem::remove(Trashed, Error);
	return {};
}
}
