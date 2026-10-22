#include "Herta/Platform/Platform.h"

#ifdef HERTA_PLATFORM_WINDOWS
	#include <windows.h>

	#include <shellapi.h>
	#include <shlobj.h>
#endif

#include <cstdlib>
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

std::filesystem::path GetUserSettingsDirectory()
{
#ifdef HERTA_PLATFORM_WINDOWS
	PWSTR Path = nullptr;
	std::filesystem::path Result;
	if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &Path)))
	{
		Result = Path;
	}

	CoTaskMemFree(Path);
	return Result;
#else
	// NOLINTNEXTLINE(concurrency-mt-unsafe): read once at startup, before any thread changes the environment.
	if (const char* const Config = std::getenv("XDG_CONFIG_HOME"); Config != nullptr && Config[0] == '/')
	{
		return std::filesystem::path(Config);
	}

	// NOLINTNEXTLINE(concurrency-mt-unsafe)
	const char* const Home = std::getenv("HOME");
	return Home != nullptr && Home[0] == '/' ? std::filesystem::path(Home) / ".config" : std::filesystem::path{};
#endif
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
