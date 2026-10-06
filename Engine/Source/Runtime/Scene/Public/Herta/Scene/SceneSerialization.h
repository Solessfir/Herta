#pragma once

#include "Herta/Scene/World.h"

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace Herta
{
struct FSceneDocument
{
	FObjectId Id;
	std::string Name;
	std::vector<FSceneEntity> Entities{};
};

[[nodiscard]] std::expected<void, FSceneError> ValidateSceneDocument(const FSceneDocument& Document);
[[nodiscard]] std::expected<std::string, FSceneError> SerializeScene(const FSceneDocument& Document);
[[nodiscard]] std::expected<FSceneDocument, FSceneError> ParseScene(std::string_view Text);
[[nodiscard]] std::expected<FSceneDocument, FSceneError> LoadScene(const std::filesystem::path& Path);
[[nodiscard]] std::expected<void, FSceneError> SaveScene(const std::filesystem::path& Path, const FSceneDocument& Document);
}
