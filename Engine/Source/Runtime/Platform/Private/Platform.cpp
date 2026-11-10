#include "Herta/Platform/Platform.h"

#ifdef HERTA_PLATFORM_WINDOWS
	#include <windows.h>

	#include <bcrypt.h>
	#include <shellapi.h>
	#include <shlobj.h>
#else
	#include <cerrno>
	#include <elf.h>
	#include <fstream>

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

std::optional<FEmbeddedData> FindEmbeddedData(const std::string_view Name)
{
#ifdef HERTA_PLATFORM_WINDOWS
	std::wstring WideName;
	for (const char Character : Name)
	{
		WideName.push_back(static_cast<wchar_t>(Character >= 'a' && Character <= 'z' ? Character - 'a' + 'A' : Character));
	}

	HRSRC Resource = FindResourceW(nullptr, WideName.c_str(), MAKEINTRESOURCEW(10));
	HGLOBAL Loaded = Resource ? LoadResource(nullptr, Resource) : nullptr;
	const void* const Data = Loaded ? LockResource(Loaded) : nullptr;
	if (!Data)
	{
		return std::nullopt;
	}

	// Resources live as long as the executable's image, so the span stays valid for the whole process.
	return FEmbeddedData{.Memory = std::span(static_cast<const std::byte*>(Data), SizeofResource(nullptr, Resource))};
#else
	// The section headers are read from the executable's own file; the loader never maps a section without the alloc flag.
	const std::filesystem::path Executable = "/proc/self/exe";
	std::ifstream File(Executable, std::ios::binary);
	Elf64_Ehdr Header{};
	if (!File.read(reinterpret_cast<char*>(&Header), sizeof(Header)) || std::string_view(reinterpret_cast<const char*>(Header.e_ident), 4) != "\x7f" "ELF" || Header.e_ident[EI_CLASS] != ELFCLASS64 || Header.e_shentsize != sizeof(Elf64_Shdr) || Header.e_shstrndx >= Header.e_shnum)
	{
		return std::nullopt;
	}

	std::vector<Elf64_Shdr> Sections(Header.e_shnum);
	if (!File.seekg(static_cast<std::streamoff>(Header.e_shoff)) || !File.read(reinterpret_cast<char*>(Sections.data()), static_cast<std::streamsize>(Sections.size() * sizeof(Elf64_Shdr))))
	{
		return std::nullopt;
	}

	const Elf64_Shdr& Names = Sections[Header.e_shstrndx];
	std::string NameTable(static_cast<std::size_t>(std::min<std::uint64_t>(Names.sh_size, std::uint64_t{1} << 20)), '\0');
	if (!File.seekg(static_cast<std::streamoff>(Names.sh_offset)) || !File.read(NameTable.data(), static_cast<std::streamsize>(NameTable.size())))
	{
		return std::nullopt;
	}

	std::string Wanted = ".";
	for (const char Character : Name)
	{
		Wanted.push_back(static_cast<char>(Character >= 'A' && Character <= 'Z' ? Character - 'A' + 'a' : Character));
	}

	for (const Elf64_Shdr& Section : Sections)
	{
		if (Section.sh_name < NameTable.size() && std::string_view(NameTable.c_str() + Section.sh_name) == Wanted && Section.sh_type != SHT_NOBITS)
		{
			return FEmbeddedData{.File = Executable, .Offset = Section.sh_offset, .Size = Section.sh_size};
		}
	}

	return std::nullopt;
#endif
}
}
