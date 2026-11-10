#pragma once

#include "Herta/Assets/AssetRegistry.h"

#include <cstddef>
#include <expected>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Herta
{
class FContentPackage;

// The files a game reads, as if they were staged under Root. Directories and content packages are mounted in order, and the first that holds
// a path wins; a directory mirrors Root's layout, and a package names files relative to Root. Paths outside Root, such as a developer's
// --project elsewhere, read the disk. Without a root it reads the disk alone. Mounting happens before reads start; reads may run on any thread.
class FVirtualFileSystem final
{
public:
	FVirtualFileSystem() = default;
	explicit FVirtualFileSystem(const std::filesystem::path& InRoot);

	void MountDirectory(const std::filesystem::path& Directory);
	void MountPackage(std::shared_ptr<const FContentPackage> Package);

	[[nodiscard]] bool Exists(const std::filesystem::path& Path) const;
	[[nodiscard]] std::expected<std::vector<std::byte>, FAssetError> Read(const std::filesystem::path& Path) const;
	// The mounted files with Extension at most Depth folders below the root, in path order, without duplicates.
	[[nodiscard]] std::vector<std::filesystem::path> FindFiles(std::string_view Extension, std::size_t Depth) const;
	[[nodiscard]] const std::filesystem::path& GetRoot() const noexcept;
	[[nodiscard]] std::size_t GetMountCount() const noexcept;
	// Each mount, the package name or the directory, in search order, for logs.
	[[nodiscard]] std::vector<std::string> DescribeMounts() const;

private:
	struct FMount
	{
		std::filesystem::path Directory{};
		std::shared_ptr<const FContentPackage> Package{};
	};

	// The package path of Path under the root, or empty when it lies outside it.
	[[nodiscard]] std::string ToPackagePath(const std::filesystem::path& Path) const;

	std::filesystem::path Root{};
	std::vector<FMount> Mounts{};
};

// Reads a whole file from the disk, refusing anything over the 1 GiB a cooked asset may take.
[[nodiscard]] std::expected<std::vector<std::byte>, FAssetError> ReadDiskFile(const std::filesystem::path& Path);
}
