#include "Herta/Platform/Platform.h"

#ifdef HERTA_PLATFORM_WINDOWS
	#include <windows.h>

	#include <bcrypt.h>
	#include <dbghelp.h>
	#include <shellapi.h>
	#include <shlobj.h>
#else
	#include <cerrno>
	#include <csignal>
	#include <ctime>
	#include <elf.h>
	#include <execinfo.h>
	#include <fcntl.h>
	#include <fstream>
	#include <unistd.h>

	#include <sys/random.h>
#endif

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <format>
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

namespace
{
// Prepared when the reporter is installed, since a crashing process must not allocate or format.
struct FCrashReporter
{
	std::array<char, 512> Header{};
	std::size_t HeaderSize = 0;
#ifdef HERTA_PLATFORM_WINDOWS
	std::wstring Directory;
#else
	std::array<char, 1024> Directory{};
#endif
};

FCrashReporter CrashReporter;

// Writes Value in decimal or hex into Buffer without allocating, returning the characters written.
std::size_t AppendNumber(char* const Buffer, std::uint64_t Value, const unsigned Base)
{
	char Digits[32];
	std::size_t Count = 0;
	do
	{
		const unsigned Digit = static_cast<unsigned>(Value % Base);
		Digits[Count++] = static_cast<char>(Digit < 10 ? '0' + Digit : 'a' + Digit - 10);
		Value /= Base;
	} while (Value != 0 && Count < sizeof(Digits));

	for (std::size_t Index = 0; Index < Count; ++Index)
	{
		Buffer[Index] = Digits[Count - 1 - Index];
	}

	return Count;
}

#ifdef HERTA_PLATFORM_WINDOWS
LONG WINAPI WriteCrashReport(EXCEPTION_POINTERS* const Exception)
{
	SYSTEMTIME Time{};
	GetSystemTime(&Time);
	wchar_t Stem[64];
	swprintf(Stem, 64, L"Crash-%04u%02u%02u-%02u%02u%02u-%lu", Time.wYear, Time.wMonth, Time.wDay, Time.wHour, Time.wMinute, Time.wSecond, GetCurrentProcessId());
	std::array<wchar_t, 1024> Path{};
	swprintf(Path.data(), Path.size(), L"%s\\%s.txt", CrashReporter.Directory.c_str(), Stem);
	if (const HANDLE Report = CreateFileW(Path.data(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr); Report != INVALID_HANDLE_VALUE)
	{
		char Line[128] = "exception 0x";
		std::size_t Size = 12;
		Size += AppendNumber(Line + Size, Exception->ExceptionRecord->ExceptionCode, 16);
		const char At[] = " at 0x";
		std::memcpy(Line + Size, At, sizeof(At) - 1);
		Size += sizeof(At) - 1;
		Size += AppendNumber(Line + Size, reinterpret_cast<std::uintptr_t>(Exception->ExceptionRecord->ExceptionAddress), 16);
		Line[Size++] = '\n';
		DWORD Written = 0;
		WriteFile(Report, CrashReporter.Header.data(), static_cast<DWORD>(CrashReporter.HeaderSize), &Written, nullptr);
		WriteFile(Report, Line, static_cast<DWORD>(Size), &Written, nullptr);
		CloseHandle(Report);
	}

	swprintf(Path.data(), Path.size(), L"%s\\%s.dmp", CrashReporter.Directory.c_str(), Stem);
	if (const HANDLE Dump = CreateFileW(Path.data(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr); Dump != INVALID_HANDLE_VALUE)
	{
		MINIDUMP_EXCEPTION_INFORMATION Information{.ThreadId = GetCurrentThreadId(), .ExceptionPointers = Exception, .ClientPointers = FALSE};
		MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), Dump, MiniDumpNormal, &Information, nullptr, nullptr);
		CloseHandle(Dump);
	}

	return EXCEPTION_CONTINUE_SEARCH;
}
#else
extern "C" void WriteCrashReport(const int Signal)
{
	char Path[1200];
	std::size_t Size = 0;
	for (const char* Character = CrashReporter.Directory.data(); *Character != '\0' && Size < 1024; ++Character)
	{
		Path[Size++] = *Character;
	}

	const char Prefix[] = "/Crash-";
	std::memcpy(Path + Size, Prefix, sizeof(Prefix) - 1);
	Size += sizeof(Prefix) - 1;
	Size += AppendNumber(Path + Size, static_cast<std::uint64_t>(std::time(nullptr)), 10);
	Path[Size++] = '-';
	Size += AppendNumber(Path + Size, static_cast<std::uint64_t>(getpid()), 10);
	const char Suffix[] = ".txt";
	std::memcpy(Path + Size, Suffix, sizeof(Suffix));
	if (const int Report = open(Path, O_WRONLY | O_CREAT | O_TRUNC, 0644); Report >= 0)
	{
		char Line[64] = "signal ";
		std::size_t LineSize = 7;
		LineSize += AppendNumber(Line + LineSize, static_cast<std::uint64_t>(Signal), 10);
		Line[LineSize++] = '\n';
		(void)write(Report, CrashReporter.Header.data(), CrashReporter.HeaderSize);
		(void)write(Report, Line, LineSize);
		// glibc's backtrace is not strictly async-signal-safe, but it is what a crash leaves to go on; the report is written first.
		void* Frames[64];
		backtrace_symbols_fd(Frames, backtrace(Frames, 64), Report);
		close(Report);
	}

	// The handler was installed to reset itself, so raising again dies as the signal would have.
	raise(Signal);
}
#endif
}

void InstallCrashReporter(const std::string_view BuildId, const std::filesystem::path& Directory)
{
	std::error_code Error;
	std::filesystem::create_directories(Directory, Error);
	const std::string Header = std::format("build {}\n", BuildId);
	CrashReporter.HeaderSize = std::min(Header.size(), CrashReporter.Header.size());
	std::memcpy(CrashReporter.Header.data(), Header.data(), CrashReporter.HeaderSize);
#ifdef HERTA_PLATFORM_WINDOWS
	CrashReporter.Directory = std::filesystem::absolute(Directory, Error).wstring();
	SetUnhandledExceptionFilter(&WriteCrashReport);
#else
	const std::string Path = std::filesystem::absolute(Directory, Error).string();
	if (Path.size() >= 1024)
	{
		return;
	}

	std::memcpy(CrashReporter.Directory.data(), Path.c_str(), Path.size() + 1);
	struct sigaction Action{};
	Action.sa_handler = &WriteCrashReport;
	Action.sa_flags = SA_RESETHAND;
	sigemptyset(&Action.sa_mask);
	for (const int Signal : {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT})
	{
		sigaction(Signal, &Action, nullptr);
	}
#endif
}
}
