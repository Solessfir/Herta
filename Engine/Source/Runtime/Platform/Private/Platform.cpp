#include "Herta/Platform/Platform.h"

#ifdef HERTA_PLATFORM_WINDOWS
	#include <windows.h>

	#include <bcrypt.h>
	#include <shellapi.h>
	#include <shlobj.h>
#else
	#include <cerrno>

	#include <sys/random.h>
#endif

#include <algorithm>
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

std::optional<std::string> ReadEnvironment(const char* const Name)
{
#ifdef HERTA_PLATFORM_WINDOWS
	char* Value = nullptr;
	std::size_t Length = 0;
	std::optional<std::string> Result;
	if (_dupenv_s(&Value, &Length, Name) == 0 && Value != nullptr && *Value != '\0')
	{
		Result = Value;
	}

	std::free(Value);
	return Result;
#else
	const char* const Value = std::getenv(Name);
	return Value != nullptr && *Value != '\0' ? std::optional<std::string>(Value) : std::nullopt;
#endif
}

bool FillSecureRandom(const std::span<std::byte> Bytes) noexcept
{
#ifdef HERTA_PLATFORM_WINDOWS
	for (std::size_t Offset = 0; Offset < Bytes.size();)
	{
		const auto Count = static_cast<ULONG>(std::min<std::size_t>(Bytes.size() - Offset, 1u << 20));
		if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(Bytes.data() + Offset), Count, BCRYPT_USE_SYSTEM_PREFERRED_RNG)))
		{
			return false;
		}

		Offset += Count;
	}

	return true;
#else
	for (std::size_t Offset = 0; Offset < Bytes.size();)
	{
		const ssize_t Count = getrandom(Bytes.data() + Offset, Bytes.size() - Offset, 0);
		if (Count < 0 && errno != EINTR)
		{
			return false;
		}

		Offset += Count > 0 ? static_cast<std::size_t>(Count) : 0;
	}

	return true;
#endif
}
}
