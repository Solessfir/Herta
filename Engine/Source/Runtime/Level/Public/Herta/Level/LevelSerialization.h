#pragma once

#include "Herta/Level/World.h"

#include <array>
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
// Stops either way, matching the viewport's compensation slider.
inline constexpr float MaximumLevelExposureCompensation = 5.f;

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

// Sounds gameplay plays for its own events rather than from placed emitters, in the order of ELevelSound.
enum class ELevelSound : std::uint8_t
{
	Footstep,
	Land,
	Jump,
	Slide,
	Impact,
	Swing,
	Slice,
	Sever,
	Objective,
};

struct FLevelSoundCue
{
	std::string_view Key;
	std::string_view Label;
};

inline constexpr std::array LevelSoundCues{
    FLevelSoundCue{.Key = "footstep", .Label = "Footstep"},
    FLevelSoundCue{.Key = "land", .Label = "Land"},
    FLevelSoundCue{.Key = "jump", .Label = "Jump"},
    FLevelSoundCue{.Key = "slide", .Label = "Slide"},
    FLevelSoundCue{.Key = "impact", .Label = "Impact"},
    FLevelSoundCue{.Key = "swing", .Label = "Sword Swing"},
    FLevelSoundCue{.Key = "slice", .Label = "Slice"},
    FLevelSoundCue{.Key = "sever", .Label = "Sever"},
    FLevelSoundCue{.Key = "objective", .Label = "Objective"},
};

// Invalid entries stay silent.
using FLevelSounds = std::array<FAssetId, LevelSoundCues.size()>;

// Particle systems gameplay plays for its own events, in the order of ELevelEffect.
enum class ELevelEffect : std::uint8_t
{
	Impact,
	Break,
	Land,
	Slice,
	Sever,
};

inline constexpr std::array LevelEffectCues{
    FLevelSoundCue{.Key = "impact", .Label = "Impact"},
    FLevelSoundCue{.Key = "break", .Label = "Break"},
    FLevelSoundCue{.Key = "land", .Label = "Land"},
    FLevelSoundCue{.Key = "slice", .Label = "Slice"},
    FLevelSoundCue{.Key = "sever", .Label = "Sever"},
};

// Invalid entries show nothing.
using FLevelEffects = std::array<FAssetId, LevelEffectCues.size()>;

struct FLevelDocument
{
	FObjectId Id;
	std::string Name;
	std::vector<FLevelEntity> Entities{};
	std::vector<FLevelFolder> Folders{};
	std::vector<FLevelCameraBookmark> CameraBookmarks{};
	// Stops added to auto exposure wherever the level is shown, for scenes whose metering does not suit them.
	float ExposureCompensation = 0.f;
	// The skinned model the player plays as; invalid plays the bare capsule.
	FAssetId PlayerCharacter{};
	FLevelSounds Sounds{};
	FLevelEffects Effects{};
};

[[nodiscard]] std::expected<void, FLevelError> ValidateLevelCameraBookmarks(std::span<const FLevelCameraBookmark> Bookmarks);

[[nodiscard]] std::expected<void, FLevelError> ValidateLevelDocument(const FLevelDocument& Document);
[[nodiscard]] std::expected<std::string, FLevelError> SerializeLevel(const FLevelDocument& Document);
[[nodiscard]] std::expected<FLevelDocument, FLevelError> ParseLevel(std::string_view Text);
[[nodiscard]] std::expected<FLevelDocument, FLevelError> LoadLevel(const std::filesystem::path& Path);
[[nodiscard]] std::expected<void, FLevelError> SaveLevel(const std::filesystem::path& Path, const FLevelDocument& Document);
}
