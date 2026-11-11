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
	Deflect,
	Block,
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
    FLevelSoundCue{.Key = "deflect", .Label = "Deflect"},
    FLevelSoundCue{.Key = "block", .Label = "Block"},
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
	Sparks,
	Trail,
};

inline constexpr std::array LevelEffectCues{
    FLevelSoundCue{.Key = "impact", .Label = "Impact"},
    FLevelSoundCue{.Key = "break", .Label = "Break"},
    FLevelSoundCue{.Key = "land", .Label = "Land"},
    FLevelSoundCue{.Key = "slice", .Label = "Slice"},
    FLevelSoundCue{.Key = "sever", .Label = "Sever"},
    FLevelSoundCue{.Key = "sparks", .Label = "Sparks"},
    FLevelSoundCue{.Key = "trail", .Label = "Blade Trail"},
};

// Invalid entries show nothing.
using FLevelEffects = std::array<FAssetId, LevelEffectCues.size()>;

// Another level placed into this one at Offset and streamed in while the player is near: it loads within LoadRadius of the player and unloads
// beyond UnloadRadius, measured horizontally from Offset. The same level may be placed several times, so a region doubles as a reusable
// piece of scenery. Streamed regions hold static scenery: meshes, static bodies, and lights.
struct FLevelRegion
{
	bool operator==(const FLevelRegion&) const = default;

	FObjectId Id{};
	std::string Name{};
	// Relative to this level's file, with '/' separators.
	std::string Level{};
	FWorldPosition Offset{};
	float LoadRadius = 40.f;
	float UnloadRadius = 50.f;
};

inline constexpr std::size_t MaximumLevelRegions = 256;

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
	// The static model the player and the guards hold in their grip; invalid draws the engine's plain box sword. It is in grip space: the
	// origin where the hand holds it, the blade along +Y from the guard to the tip at 1 m, and the edges facing +X and -X.
	FAssetId PlayerWeapon{};
	FLevelSounds Sounds{};
	FLevelEffects Effects{};
	std::vector<FLevelRegion> Regions{};
};

[[nodiscard]] std::expected<void, FLevelError> ValidateLevelRegions(std::span<const FLevelRegion> Regions);
// Whether a region streams an entity: static meshes and bodies and local lights do; anything that moves, simulates, or sets the level's sun,
// sky, fog, or sound does not.
[[nodiscard]] bool IsLevelRegionScenery(const FLevelEntity& Entity) noexcept;
// A region's entities placed at its offset, composed in double precision before rounding, in entity order.
[[nodiscard]] std::expected<std::vector<FMatrix4>, FLevelError> ComposeLevelRegionMatrices(const FLevelRegion& Region, std::span<const FLevelEntity> Entities);
// Placing a region more than once repeats its entities, so each placement mixes its own identity into theirs.
[[nodiscard]] constexpr FObjectId GetLevelRegionInstanceId(const FLevelRegion& Region, const FObjectId& Entity) noexcept
{
	return {Entity.GetHigh() ^ Region.Id.GetHigh(), Entity.GetLow() ^ Region.Id.GetLow()};
}

[[nodiscard]] std::expected<void, FLevelError> ValidateLevelCameraBookmarks(std::span<const FLevelCameraBookmark> Bookmarks);

[[nodiscard]] std::expected<void, FLevelError> ValidateLevelDocument(const FLevelDocument& Document);
[[nodiscard]] std::expected<std::string, FLevelError> SerializeLevel(const FLevelDocument& Document);
[[nodiscard]] std::expected<FLevelDocument, FLevelError> ParseLevel(std::string_view Text);
[[nodiscard]] std::expected<FLevelDocument, FLevelError> LoadLevel(const std::filesystem::path& Path);
// Where a region's level lives, from the path of the level that places it.
[[nodiscard]] std::filesystem::path ResolveLevelRegionPath(const std::filesystem::path& LevelPath, const FLevelRegion& Region);
[[nodiscard]] std::expected<void, FLevelError> SaveLevel(const std::filesystem::path& Path, const FLevelDocument& Document);
}
