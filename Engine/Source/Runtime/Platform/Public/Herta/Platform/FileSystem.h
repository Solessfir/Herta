#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <system_error>

namespace Herta
{
enum class EMoveMode : std::uint8_t
{
	ReplaceExisting,
	FailIfExists
};

// Atomically renames a file or directory on one volume. Windows retries for about a second while another process, such as an antivirus scanner, briefly holds either path open.
[[nodiscard]] std::expected<void, std::error_code> MoveAtomically(const std::filesystem::path& Source, const std::filesystem::path& Destination, EMoveMode Mode);
}
