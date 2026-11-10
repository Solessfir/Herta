#pragma once

#include "Herta/Assets/ContentPackage.h"
#include "Herta/Assets/VirtualFileSystem.h"
#include "Herta/GameFramework/GameSession.h"

#include <expected>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

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
	// Where data mods are installed, one folder each; empty means the Mods folder beside the content.
	std::filesystem::path ModsDirectory{};
	bool bLoadMods = true;
};

struct FGameContent
{
	std::shared_ptr<FVirtualFileSystem> FileSystem{};
	// The staged project's descriptor, as a path the file system reads.
	std::filesystem::path ProjectPath{};
	// The mods loaded, in load order, as "Namespace version, from where, signed or unsigned".
	std::vector<std::string> Mods{};
	// Mods refused and why, and warnings about the ones loaded.
	std::vector<std::string> ModDiagnostics{};
};

// The publisher keys this build trusts, from Config/PackagePublicKeys.txt when it was generated; empty for builds without one.
[[nodiscard]] std::span<const FPackagePublicKey> GetPublisherPackageKeys();

// Mounts the game's files: a package embedded in the executable, then the .hpak packages beside it in name order, or else the loose staged
// folder there. Packages are opened with the trusted keys, and a game that requires signatures never falls back to loose files that could
// replace signed content. Data mods mount in front of it all; see Docs/Distribution.md. A mod is a folder holding Mod.hmod and either
// <Namespace>.hpak or a Files folder laid out like the staged content.
[[nodiscard]] std::expected<FGameContent, FGameError> MountGameContent(const FGameContentOptions& Options);
}
