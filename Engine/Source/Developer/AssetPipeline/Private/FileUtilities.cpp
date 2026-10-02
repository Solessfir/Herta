#include "FileUtilities.h"

#include <format>
#include <fstream>
#include <random>

namespace Herta
{
std::string PathToUtf8(const std::filesystem::path& Path)
{
	const std::u8string Text = Path.u8string();
	return {Text.begin(), Text.end()};
}

std::string GenericPathToUtf8(const std::filesystem::path& Path)
{
	const std::u8string Text = Path.generic_u8string();
	return {Text.begin(), Text.end()};
}

std::filesystem::path Utf8ToPath(const std::string_view Text)
{
	return {std::u8string(Text.begin(), Text.end())};
}

std::expected<void, FAssetError> WriteFileAtomically(const std::filesystem::path& Path, const std::span<const std::byte> Bytes)
{
	std::error_code Error;
	std::filesystem::create_directories(Path.parent_path(), Error);
	if (Error)
	{
		return std::unexpected(FAssetError{std::format("Cannot create directory '{}': {}", PathToUtf8(Path.parent_path()), Error.message())});
	}

	std::random_device Device;
	std::filesystem::path TemporaryPath = Path;
	TemporaryPath += std::format(".{:08x}{:08x}.tmp", Device(), Device());
	{
		std::ofstream Stream(TemporaryPath, std::ios::binary | std::ios::trunc);
		Stream.write(reinterpret_cast<const char*>(Bytes.data()), static_cast<std::streamsize>(Bytes.size()));
		Stream.close();
		if (!Stream)
		{
			std::filesystem::remove(TemporaryPath, Error);
			return std::unexpected(FAssetError{std::format("Cannot write '{}'", PathToUtf8(TemporaryPath))});
		}
	}

	std::filesystem::rename(TemporaryPath, Path, Error);
	if (Error)
	{
		const std::string Message = Error.message();
		std::filesystem::remove(TemporaryPath, Error);
		return std::unexpected(FAssetError{std::format("Cannot replace '{}': {}", PathToUtf8(Path), Message)});
	}
	return {};
}

std::expected<std::optional<std::vector<std::byte>>, FAssetError> ReadWholeFile(const std::filesystem::path& Path, const std::uint64_t MaximumSize)
{
	std::error_code Error;
	const std::uint64_t Size = std::filesystem::file_size(Path, Error);
	if (Error)
	{
		if (!std::filesystem::exists(Path, Error) && !Error)
		{
			return std::optional<std::vector<std::byte>>{};
		}
		return std::unexpected(FAssetError{std::format("Cannot query '{}'", PathToUtf8(Path))});
	}
	if (Size > MaximumSize)
	{
		return std::unexpected(FAssetError{std::format("'{}' exceeds the {} byte limit", PathToUtf8(Path), MaximumSize)});
	}

	std::vector<std::byte> Bytes(static_cast<std::size_t>(Size));
	std::ifstream Stream(Path, std::ios::binary);
	if (!Stream || !Stream.read(reinterpret_cast<char*>(Bytes.data()), static_cast<std::streamsize>(Bytes.size())) || Stream.peek() != std::ifstream::traits_type::eof())
	{
		return std::unexpected(FAssetError{std::format("Cannot read '{}'", PathToUtf8(Path))});
	}
	return std::optional<std::vector<std::byte>>(std::move(Bytes));
}
}
