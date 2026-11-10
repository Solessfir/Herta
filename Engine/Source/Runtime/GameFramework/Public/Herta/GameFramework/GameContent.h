#pragma once

#include "Herta/Assets/ContentPackage.h"
#include "Herta/Assets/VirtualFileSystem.h"
#include "Herta/GameFramework/GameSession.h"

#include <expected>
#include <filesystem>
#include <memory>
#include <span>

namespace Herta
{
// The packaged file name extension; packages sit beside the game's executables.
inline constexpr std::string_view ContentPackageExtension = ".hpak";

struct FGameContentOptions
{
	std::filesystem::path ExecutableDirectory{};
	// A staged project to play instead of the content beside the executable, read from its folder on the disk.
	std::filesystem::path ProjectPath{};
	// Keys trusted besides the publisher keys built into the engine.
	std::span<const FPackagePublicKey> TrustedKeys{};
	// Refuse content not in packages signed by a trusted key. Shipping builds with publisher keys always do.
	bool bRequireSignature = false;
};

struct FGameContent
{
	std::shared_ptr<FVirtualFileSystem> FileSystem{};
	// The staged project's descriptor, as a path the file system reads.
	std::filesystem::path ProjectPath{};
};

// The publisher keys this build trusts, from Config/PackagePublicKeys.txt when it was generated; empty for builds without one.
[[nodiscard]] std::span<const FPackagePublicKey> GetPublisherPackageKeys();

// Mounts the game's files: a package embedded in the executable, then the .hpak packages beside it in name order, or else the loose staged
// folder there. Packages are opened with the trusted keys, and a game that requires signatures never falls back to loose files that could
// replace signed content.
[[nodiscard]] std::expected<FGameContent, FGameError> MountGameContent(const FGameContentOptions& Options);
}
