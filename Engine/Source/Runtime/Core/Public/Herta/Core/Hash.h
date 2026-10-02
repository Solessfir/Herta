#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace Herta
{
struct FHash128
{
	std::uint64_t High = 0;
	std::uint64_t Low = 0;

	[[nodiscard]] constexpr auto operator<=>(const FHash128&) const noexcept = default;
};

// Stable XXH3-128 content identity. Not collision-resistant against deliberate attacks.
[[nodiscard]] FHash128 HashBytes(std::span<const std::byte> Bytes) noexcept;

// 32 lowercase hex digits, High first.
[[nodiscard]] std::string ToString(const FHash128& Hash);
[[nodiscard]] std::optional<FHash128> ParseHash128(std::string_view Text) noexcept;
}
