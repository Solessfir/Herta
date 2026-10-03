#include "Herta/Core/Hash.h"

#include <format>

#define XXH_INLINE_ALL
#include <xxhash.h>

namespace Herta
{
FHash128 HashBytes(const std::span<const std::byte> Bytes) noexcept
{
	const XXH128_hash_t Hash = XXH3_128bits(Bytes.data(), Bytes.size());
	return FHash128{.High = Hash.high64, .Low = Hash.low64};
}

std::string ToString(const FHash128& Hash)
{
	return std::format("{:016x}{:016x}", Hash.High, Hash.Low);
}

std::optional<FHash128> ParseHash128(const std::string_view Text) noexcept
{
	if (Text.size() != 32)
	{
		return std::nullopt;
	}

	FHash128 Hash;
	for (std::size_t Index = 0; Index < Text.size(); ++Index)
	{
		const char Character = Text[Index];
		const auto Code = static_cast<std::uint64_t>(static_cast<unsigned char>(Character));
		std::uint64_t Digit = 0;
		if (Character >= '0' && Character <= '9')
		{
			Digit = Code - '0';
		}
		else if (Character >= 'a' && Character <= 'f')
		{
			Digit = Code - 'a' + 10;
		}
		else
		{
			return std::nullopt;
		}

		std::uint64_t& Word = Index < 16 ? Hash.High : Hash.Low;
		Word = (Word << 4) | Digit;
	}

	return Hash;
}
}
