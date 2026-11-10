#pragma once

#include "Herta/Assets/AssetRegistry.h"

#include <compare>
#include <cstdint>
#include <expected>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Herta
{
inline constexpr std::uint32_t DataModFormatVersion = 1;
// The manifest every mod folder holds.
inline constexpr std::string_view DataModManifestName = "Mod.hmod";

struct FModVersion
{
	auto operator<=>(const FModVersion&) const = default;

	std::uint32_t Major = 0;
	std::uint32_t Minor = 0;
	std::uint32_t Patch = 0;
};

struct FModDependency
{
	bool operator==(const FModDependency&) const = default;

	std::string Namespace{};
	FModVersion MinimumVersion{};
};

// A data mod's manifest: who it is, what it needs, where it loads, and which of the game's files it means to replace. Mods only add and
// replace files; they never run code.
struct FModManifest
{
	bool operator==(const FModManifest&) const = default;

	// A unique identifier, such as NeonCoral: letters, digits, and underscores, starting with a letter.
	std::string Namespace{};
	FModVersion Version{};
	// The cooked asset format its files were cooked in, which must be this engine's.
	std::uint32_t CookedFormat = 0;
	std::vector<FModDependency> Dependencies{};
	std::vector<std::string> LoadAfter{};
	std::vector<std::string> LoadBefore{};
	// Package paths of files it replaces, in the game or in mods loaded before it. Replacing anything not listed refuses the mod.
	std::vector<std::string> Overrides{};
};

[[nodiscard]] std::expected<FModManifest, FAssetError> ParseModManifest(std::string_view Text);

// A mod found on disk: its manifest, the package paths of the files it provides, and where it came from, for diagnostics.
struct FModCandidate
{
	FModManifest Manifest{};
	std::vector<std::string> Files{};
	std::string Source{};
};

struct FModResolution
{
	// Indices of the mods to load, first loaded first; later mods win where they replace the same file.
	std::vector<std::size_t> LoadOrder{};
	// Why mods were refused, and warnings such as overrides of files that nothing provides.
	std::vector<std::string> Diagnostics{};
};

// Orders mods after their dependencies and load constraints, ties broken by namespace so the order never depends on discovery. Refuses mods
// with duplicate namespaces, another cooked format, missing or too-old dependencies, load cycles, or files that replace content without
// declaring it, and every mod that depends on a refused one. BaseHas says whether the game's own content holds a path.
[[nodiscard]] FModResolution ResolveMods(std::span<const FModCandidate> Mods, const std::function<bool(std::string_view)>& BaseHas);
}
