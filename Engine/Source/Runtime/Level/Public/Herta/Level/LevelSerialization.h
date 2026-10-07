#pragma once

#include "Herta/Level/World.h"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Herta
{
struct FLevelFolder
{
	bool operator==(const FLevelFolder&) const = default;

	FObjectId Id;
	std::string Name;
	FObjectId Parent{};
	std::vector<FObjectId> Entities{};
};

inline constexpr std::uint32_t MaximumLevelCameraBookmarks = 9;

// Editor viewpoint saved with the level and recalled by its number key.
struct FLevelCameraBookmark
{
	bool operator==(const FLevelCameraBookmark&) const = default;

	// 1 through MaximumLevelCameraBookmarks, unique within a level.
	std::uint32_t Slot = 1;
	std::string Name{};
	FWorldPosition Position{};
	// Radians. Zero yaw looks along +Z and positive yaw turns towards +X; negative pitch looks down.
	float Yaw = 0.f;
	float Pitch = 0.f;
};

struct FLevelDocument
{
	FObjectId Id;
	std::string Name;
	std::vector<FLevelEntity> Entities{};
	std::vector<FLevelFolder> Folders{};
	std::vector<FLevelCameraBookmark> CameraBookmarks{};
};

[[nodiscard]] std::expected<void, FLevelError> ValidateLevelCameraBookmarks(std::span<const FLevelCameraBookmark> Bookmarks);

[[nodiscard]] std::expected<void, FLevelError> ValidateLevelDocument(const FLevelDocument& Document);
[[nodiscard]] std::expected<std::string, FLevelError> SerializeLevel(const FLevelDocument& Document);
[[nodiscard]] std::expected<FLevelDocument, FLevelError> ParseLevel(std::string_view Text);
[[nodiscard]] std::expected<FLevelDocument, FLevelError> LoadLevel(const std::filesystem::path& Path);
[[nodiscard]] std::expected<void, FLevelError> SaveLevel(const std::filesystem::path& Path, const FLevelDocument& Document);
}
