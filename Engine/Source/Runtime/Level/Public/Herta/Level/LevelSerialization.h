#pragma once

#include "Herta/Level/World.h"

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace Herta
{
struct FLevelDocument
{
	FObjectId Id;
	std::string Name;
	std::vector<FLevelEntity> Entities{};
};

[[nodiscard]] std::expected<void, FLevelError> ValidateLevelDocument(const FLevelDocument& Document);
[[nodiscard]] std::expected<std::string, FLevelError> SerializeLevel(const FLevelDocument& Document);
[[nodiscard]] std::expected<FLevelDocument, FLevelError> ParseLevel(std::string_view Text);
[[nodiscard]] std::expected<FLevelDocument, FLevelError> LoadLevel(const std::filesystem::path& Path);
[[nodiscard]] std::expected<void, FLevelError> SaveLevel(const std::filesystem::path& Path, const FLevelDocument& Document);
}
