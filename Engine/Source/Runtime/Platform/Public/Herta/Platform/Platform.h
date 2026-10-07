#pragma once

#include <cstdint>
#include <filesystem>
#include <string_view>
#include <vector>

namespace Herta
{
enum class EPlatform : std::uint8_t
{
	Windows,
	Linux
};

[[nodiscard]] constexpr EPlatform GetCurrentPlatform() noexcept
{
#if (defined(HERTA_PLATFORM_WINDOWS) + defined(HERTA_PLATFORM_LINUX)) != 1
	#error Exactly one Herta platform must be defined
#endif

#if defined(HERTA_PLATFORM_WINDOWS)
	return EPlatform::Windows;
#else
	return EPlatform::Linux;
#endif
}

[[nodiscard]] std::string_view GetPlatformName(EPlatform Platform) noexcept;

// Process arguments after the executable, as paths. Windows rereads the wide command line so non-ANSI paths, such as files dropped on the executable, survive.
[[nodiscard]] std::vector<std::filesystem::path> GetProcessArgumentPaths(int ArgumentCount, char** Arguments);
}
