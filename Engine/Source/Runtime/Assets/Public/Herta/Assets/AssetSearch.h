#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace Herta
{
struct FAssetSearchMatch
{
	// Index into the searched candidates.
	std::size_t Index = 0;
	int Score = 0;
};

// Case-insensitive subsequence matching that favors consecutive characters and word starts such as path segments.
// Results are sorted by score, then by candidate order, so equal inputs always rank the same way. An empty query matches
// everything in order. Returns nullopt when ShouldCancel reports true; it is polled every few hundred candidates.
[[nodiscard]] std::optional<std::vector<FAssetSearchMatch>> SearchAssets(std::span<const std::string_view> Candidates, std::string_view Query, const std::function<bool()>& ShouldCancel = {});
}
