#pragma once

#include <cstdint>
#include <string_view>

namespace Herta
{
enum class EBuildConfiguration : std::uint8_t
{
	Debug,
	Development,
	Shipping
};

#if (defined(HERTA_DEBUG) + defined(HERTA_DEVELOPMENT) + defined(HERTA_SHIPPING)) != 1
	#error Exactly one Herta build configuration must be defined
#endif

[[nodiscard]] constexpr EBuildConfiguration GetBuildConfiguration() noexcept
{
#if defined(HERTA_DEBUG)
	return EBuildConfiguration::Debug;
#elif defined(HERTA_DEVELOPMENT)
	return EBuildConfiguration::Development;
#else
	return EBuildConfiguration::Shipping;
#endif
}

[[nodiscard]] std::string_view GetBuildConfigurationName(EBuildConfiguration Configuration) noexcept;
}
