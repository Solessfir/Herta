#include "Herta/Assets/AssetId.h"

#include <array>
#include <format>
#include <random>

namespace Herta
{
namespace
{
inline constexpr std::array<std::size_t, 4> HyphenOffsets{8, 13, 18, 23};
inline constexpr std::size_t CanonicalLength = 36;

[[nodiscard]] constexpr int ParseLowerHexDigit(const char Character) noexcept
{
	if (Character >= '0' && Character <= '9')
	{
		return Character - '0';
	}
	if (Character >= 'a' && Character <= 'f')
	{
		return Character - 'a' + 10;
	}
	return -1;
}
}

FAssetId FAssetId::Generate()
{
	std::random_device Device;
	const auto Next = [&Device]
	{
		return (static_cast<std::uint64_t>(Device()) << 32) | Device();
	};

	std::uint64_t GeneratedHigh = Next();
	std::uint64_t GeneratedLow = Next();
	GeneratedHigh = (GeneratedHigh & ~0xf000ull) | 0x4000ull;
	GeneratedLow = (GeneratedLow & ~(0xc0ull << 56)) | (0x80ull << 56);
	return {GeneratedHigh, GeneratedLow};
}

std::optional<FAssetId> FAssetId::Parse(const std::string_view Text) noexcept
{
	if (Text.size() != CanonicalLength)
	{
		return std::nullopt;
	}

	std::array<std::uint64_t, 2> Words{};
	std::size_t Digit = 0;
	std::size_t HyphenIndex = 0;
	for (std::size_t Index = 0; Index < Text.size(); ++Index)
	{
		if (HyphenIndex < HyphenOffsets.size() && Index == HyphenOffsets[HyphenIndex])
		{
			if (Text[Index] != '-')
			{
				return std::nullopt;
			}
			++HyphenIndex;
			continue;
		}

		const int Value = ParseLowerHexDigit(Text[Index]);
		if (Value < 0)
		{
			return std::nullopt;
		}
		std::uint64_t& Word = Words[Digit / 16];
		Word = (Word << 4) | static_cast<std::uint64_t>(Value);
		++Digit;
	}

	const FAssetId Id(Words[0], Words[1]);
	return Id.IsValid() ? std::optional<FAssetId>(Id) : std::nullopt;
}

std::string FAssetId::ToString() const
{
	return std::format("{:08x}-{:04x}-{:04x}-{:04x}-{:012x}", High >> 32, (High >> 16) & 0xffff, High & 0xffff, Low >> 48, Low & 0xffffffffffffull);
}
}
