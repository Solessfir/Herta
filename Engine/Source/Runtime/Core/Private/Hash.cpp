#include "Herta/Core/Hash.h"

#include <format>

#define XXH_INLINE_ALL
#include <xxhash.h>

namespace Herta
{
FHash128 HashBytes(const std::span<const std::byte> Bytes) noexcept
{
	const XXH128_hash_t Hash = XXH3_128bits(Bytes.data(), Bytes.size());
	return FHash128{Hash.high64, Hash.low64};
}

std::string ToString(const FHash128& Hash)
{
	return std::format("{:016x}{:016x}", Hash.High, Hash.Low);
}
}
