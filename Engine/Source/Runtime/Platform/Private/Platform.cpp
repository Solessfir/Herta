#include "Herta/Platform/Platform.h"

#ifdef HERTA_PLATFORM_WINDOWS
	#include <windows.h>

	#include <shellapi.h>
#endif

#include <string>

namespace Herta
{
std::string_view GetPlatformName(const EPlatform Platform) noexcept
{
	switch (Platform)
	{
		case EPlatform::Windows:
			return "Windows";
		case EPlatform::Linux:
			return "Linux";
	}

	return "Unknown";
}

std::vector<std::filesystem::path> GetProcessArgumentPaths(const int ArgumentCount, char** const Arguments)
{
	std::vector<std::filesystem::path> Result;
#ifdef HERTA_PLATFORM_WINDOWS
	int WideCount = 0;
	LPWSTR* const WideArguments = CommandLineToArgvW(GetCommandLineW(), &WideCount);
	if (WideArguments != nullptr)
	{
		for (int Index = 1; Index < WideCount; ++Index)
		{
			Result.emplace_back(WideArguments[Index]);
		}

		LocalFree(WideArguments);
		return Result;
	}
#endif

	for (int Index = 1; Index < ArgumentCount; ++Index)
	{
		if (Arguments[Index] != nullptr)
		{
			const std::string_view Argument(Arguments[Index]);
			Result.emplace_back(std::u8string(Argument.begin(), Argument.end()));
		}
	}

	return Result;
}
}
