#pragma once

#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace Herta
{
// Stable serialized asset identity. The canonical text form is a lowercase 8-4-4-4-12 UUID.
class FAssetId final
{
public:
	constexpr FAssetId() noexcept = default;

	constexpr FAssetId(const std::uint64_t InHigh, const std::uint64_t InLow) noexcept
	    : High(InHigh)
	    , Low(InLow)
	{
	}

	// Random version 4 UUID.
	[[nodiscard]] static FAssetId Generate();
	// Accepts only the canonical form so every ID has exactly one textual representation.
	[[nodiscard]] static std::optional<FAssetId> Parse(std::string_view Text) noexcept;

	[[nodiscard]] std::string ToString() const;

	[[nodiscard]] constexpr bool IsValid() const noexcept
	{
		return High != 0 || Low != 0;
	}

	[[nodiscard]] constexpr std::uint64_t GetHigh() const noexcept
	{
		return High;
	}

	[[nodiscard]] constexpr std::uint64_t GetLow() const noexcept
	{
		return Low;
	}

	[[nodiscard]] constexpr auto operator<=>(const FAssetId&) const noexcept = default;

private:
	std::uint64_t High = 0;
	std::uint64_t Low = 0;
};
}
