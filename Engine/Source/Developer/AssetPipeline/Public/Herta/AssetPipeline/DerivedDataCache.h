#pragma once

#include "Herta/Assets/AssetRegistry.h"
#include "Herta/Core/Hash.h"

#include <cstddef>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <vector>

namespace Herta
{
// Local content-addressed cache. Entries are immutable per key, so concurrent writers of one key are harmless.
class FDerivedDataCache final
{
public:
	// Root is normally DerivedDataCache/<platform> in the repository.
	explicit FDerivedDataCache(std::filesystem::path InRoot);

	// Returns nullopt on a miss. A corrupt entry is an error so callers can report it before rebuilding.
	[[nodiscard]] std::expected<std::optional<std::vector<std::byte>>, FAssetError> Get(const FHash128& Key) const;
	[[nodiscard]] std::expected<void, FAssetError> Put(const FHash128& Key, std::span<const std::byte> Payload) const;

	[[nodiscard]] std::filesystem::path GetEntryPath(const FHash128& Key) const;
	[[nodiscard]] const std::filesystem::path& GetRoot() const noexcept
	{
		return Root;
	}

private:
	std::filesystem::path Root;
};
}
