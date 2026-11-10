#pragma once

#include "Herta/Level/World.h"
#include "Herta/Math/Vector.h"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Herta
{
// Version 1 is the first format; a later version adds a migration from the one before.
inline constexpr std::uint32_t GameSaveVersion = 1;
inline constexpr std::string_view GameSaveExtension = ".hsave";

struct FGameSaveError
{
	std::string Message{};
};

// A destructible's bonds, so a loaded save breaks it the way it was.
struct FGameSaveDestructible
{
	bool operator==(const FGameSaveDestructible&) const = default;

	FObjectId Object{};
	std::vector<bool> IntactBonds{};
};

// A playthrough's progress and the world state it changed, kept apart from the authored level. Everything refers to the level by stable
// object identities, so a save survives edits that keep them.
struct FGameSave
{
	bool operator==(const FGameSave&) const = default;

	FObjectId Level{};
	// EGameObjectiveState's value.
	std::uint32_t ObjectiveState = 0;
	double ElapsedSeconds = 0.;
	std::optional<double> BestSeconds{};
	std::uint32_t Respawns = 0;
	FVector3 PlayerFoot{};
	float PlayerYaw = 0.f;
	std::vector<FGameSaveDestructible> Destructibles{};
	// Characters that died, which collapse again on load.
	std::vector<FObjectId> DeadCharacters{};
};

// Text with a closing checksum, so a truncated or damaged file is detected rather than half loaded.
[[nodiscard]] std::string SerializeGameSave(const FGameSave& Save);
[[nodiscard]] std::expected<FGameSave, FGameSaveError> ParseGameSave(std::string_view Text);

struct FGameSaveLoad
{
	FGameSave Save{};
	// The main file was missing or damaged and this came from the backup written before it.
	bool bRecovered = false;
	std::string Problem{};
};

// Writes beside the file and renames over it, after keeping the previous save as <file>.previous, so a crash mid-write never loses both.
[[nodiscard]] std::expected<void, FGameSaveError> WriteGameSave(const std::filesystem::path& File, const FGameSave& Save);
// Loads the save, or its backup when the save is missing or damaged. Fails when neither is usable.
[[nodiscard]] std::expected<FGameSaveLoad, FGameSaveError> LoadGameSave(const std::filesystem::path& File);
[[nodiscard]] std::filesystem::path GetGameSaveBackupPath(const std::filesystem::path& File);
}
