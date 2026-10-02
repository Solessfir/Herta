#pragma once

#include "Herta/Assets/AssetId.h"

#include <cstddef>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Herta
{
struct FAssetError
{
	std::string Message;
};

struct FAssetRecord
{
	FAssetId Id;
	// Content-root-relative UTF-8 path with '/' separators.
	std::string SourcePath;
	std::string Importer;
};

// Relative, '/'-separated, and portable to both Windows and Linux file systems.
[[nodiscard]] bool IsValidAssetPath(std::string_view Path) noexcept;

// Immutable snapshot. Publishers replace the whole registry instead of mutating it.
class FAssetRegistry final
{
public:
	FAssetRegistry() = default;

	// Rejects invalid IDs or paths, duplicate IDs, and paths that collide on case-insensitive file systems.
	[[nodiscard]] static std::expected<FAssetRegistry, FAssetError> Create(std::vector<FAssetRecord> Records);

	[[nodiscard]] const FAssetRecord* Find(const FAssetId& Id) const noexcept;
	[[nodiscard]] const FAssetRecord* FindBySourcePath(std::string_view SourcePath) const noexcept;
	// Sorted by source path.
	[[nodiscard]] std::span<const FAssetRecord> GetRecords() const noexcept
	{
		return Records;
	}

private:
	std::vector<FAssetRecord> Records;
	std::vector<std::size_t> IdOrder;
};
}
