#include "Herta/Assets/VirtualFileSystem.h"

#include "Herta/Assets/ContentPackage.h"

#include <algorithm>
#include <format>
#include <fstream>
#include <system_error>
#include <utility>

namespace Herta
{
std::expected<std::vector<std::byte>, FAssetError> ReadDiskFile(const std::filesystem::path& Path)
{
	std::error_code Error;
	const std::uintmax_t Size = std::filesystem::file_size(Path, Error);
	if (Error)
	{
		return std::unexpected(FAssetError{std::format("Cannot find {}", Path.generic_string())});
	}

	if (Size > MaximumContentPackageEntrySize)
	{
		return std::unexpected(FAssetError{std::format("{} is larger than a staged file may be", Path.generic_string())});
	}

	std::vector<std::byte> Bytes(static_cast<std::size_t>(Size));
	std::ifstream File(Path, std::ios::binary);
	if (!File || !File.read(reinterpret_cast<char*>(Bytes.data()), static_cast<std::streamsize>(Bytes.size())))
	{
		return std::unexpected(FAssetError{std::format("Cannot read {}", Path.generic_string())});
	}

	return Bytes;
}

FVirtualFileSystem::FVirtualFileSystem(const std::filesystem::path& InRoot)
    : Root(std::filesystem::absolute(InRoot).lexically_normal())
{
}

void FVirtualFileSystem::MountDirectory(const std::filesystem::path& Directory, const bool bOverride)
{
	Mounts.insert(bOverride ? Mounts.begin() : Mounts.end(), FMount{.Directory = std::filesystem::absolute(Directory).lexically_normal()});
}

void FVirtualFileSystem::MountPackage(std::shared_ptr<const FContentPackage> Package, const bool bOverride)
{
	Mounts.insert(bOverride ? Mounts.begin() : Mounts.end(), FMount{.Package = std::move(Package)});
}

std::string FVirtualFileSystem::ToPackagePath(const std::filesystem::path& Path) const
{
	if (Root.empty())
	{
		return {};
	}

	const std::filesystem::path Relative = std::filesystem::absolute(Path).lexically_normal().lexically_relative(Root);
	const std::u8string Text = Relative.generic_u8string();
	const std::string Result(Text.begin(), Text.end());
	return IsValidPackagePath(Result) ? Result : std::string{};
}

bool FVirtualFileSystem::Exists(const std::filesystem::path& Path) const
{
	const std::string Relative = ToPackagePath(Path);
	std::error_code Error;
	if (Relative.empty())
	{
		return std::filesystem::is_regular_file(Path, Error);
	}

	const std::filesystem::path RelativePath(std::u8string(Relative.begin(), Relative.end()));
	return std::ranges::any_of(Mounts, [&](const FMount& Mount)
	{
		return Mount.Package ? Mount.Package->Find(Relative) != nullptr : std::filesystem::is_regular_file(Mount.Directory / RelativePath, Error);
	});
}

std::expected<std::vector<std::byte>, FAssetError> FVirtualFileSystem::Read(const std::filesystem::path& Path) const
{
	const std::string Relative = ToPackagePath(Path);
	if (Relative.empty())
	{
		return ReadDiskFile(Path);
	}

	std::error_code Error;
	for (const FMount& Mount : Mounts)
	{
		if (Mount.Package)
		{
			if (Mount.Package->Find(Relative))
			{
				return Mount.Package->Read(Relative);
			}

			continue;
		}

		const std::filesystem::path File = Mount.Directory / std::filesystem::path(std::u8string(Relative.begin(), Relative.end()));
		if (std::filesystem::is_regular_file(File, Error))
		{
			return ReadDiskFile(File);
		}
	}

	return std::unexpected(FAssetError{std::format("{} is in no mounted content", Relative)});
}

std::vector<std::filesystem::path> FVirtualFileSystem::FindFiles(const std::string_view Extension, const std::size_t Depth) const
{
	std::vector<std::string> Found;
	const auto Consider = [&](const std::string_view Relative)
	{
		if (Relative.ends_with(Extension) && static_cast<std::size_t>(std::ranges::count(Relative, '/')) <= Depth)
		{
			Found.emplace_back(Relative);
		}
	};

	for (const FMount& Mount : Mounts)
	{
		if (Mount.Package)
		{
			for (const FPackageEntry& Entry : Mount.Package->GetEntries())
			{
				Consider(Entry.Path);
			}

			continue;
		}

		std::error_code Error;
		for (auto Iterator = std::filesystem::recursive_directory_iterator(Mount.Directory, Error); !Error && Iterator != std::filesystem::recursive_directory_iterator(); Iterator.increment(Error))
		{
			if (static_cast<std::size_t>(Iterator.depth()) >= Depth)
			{
				Iterator.disable_recursion_pending();
			}

			if (Iterator->is_regular_file(Error))
			{
				const std::u8string Relative = Iterator->path().lexically_relative(Mount.Directory).generic_u8string();
				Consider(std::string(Relative.begin(), Relative.end()));
			}
		}
	}

	std::ranges::sort(Found);
	const auto [First, Last] = std::ranges::unique(Found);
	Found.erase(First, Last);
	std::vector<std::filesystem::path> Paths;
	Paths.reserve(Found.size());
	for (const std::string& Relative : Found)
	{
		Paths.push_back(Root / std::filesystem::path(std::u8string(Relative.begin(), Relative.end())));
	}

	return Paths;
}

const std::filesystem::path& FVirtualFileSystem::GetRoot() const noexcept
{
	return Root;
}

std::size_t FVirtualFileSystem::GetMountCount() const noexcept
{
	return Mounts.size();
}

std::vector<std::string> FVirtualFileSystem::DescribeMounts() const
{
	std::vector<std::string> Descriptions;
	Descriptions.reserve(Mounts.size());
	for (const FMount& Mount : Mounts)
	{
		Descriptions.push_back(Mount.Package ? std::format("package {}{}", Mount.Package->GetName(), Mount.Package->IsTrusted() ? ", signed" : "") : std::format("directory {}", Mount.Directory.generic_string()));
	}

	return Descriptions;
}
}
