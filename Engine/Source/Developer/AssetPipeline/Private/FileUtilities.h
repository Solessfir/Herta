#pragma once

#include "Herta/Assets/AssetRegistry.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Herta
{
// path::string() uses the ANSI code page on Windows and can throw for non-ASCII names.
[[nodiscard]] std::string PathToUtf8(const std::filesystem::path& Path);
[[nodiscard]] std::string GenericPathToUtf8(const std::filesystem::path& Path);
[[nodiscard]] std::filesystem::path Utf8ToPath(std::string_view Text);

// Publishes a completed sibling file. Exclusive publication requires filesystem hard-link support.
[[nodiscard]] std::expected<void, FAssetError> WriteFileAtomically(const std::filesystem::path& Path, std::span<const std::byte> Bytes, bool bReplaceExisting = true);

// Returns nullopt when the file does not exist.
[[nodiscard]] std::expected<std::optional<std::vector<std::byte>>, FAssetError> ReadWholeFile(const std::filesystem::path& Path, std::uint64_t MaximumSize);
}
