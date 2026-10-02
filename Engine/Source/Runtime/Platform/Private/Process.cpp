#include "Herta/Platform/Process.h"

#include <array>
#include <format>
#include <optional>
#include <vector>

#ifdef HERTA_PLATFORM_WINDOWS
	#include <atomic>
	#include <memory>
	#include <string_view>
	#include <type_traits>
	#include <windows.h>
#else
	#include <cerrno>
	#include <csignal>
	#include <cstring>
	#include <fcntl.h>
	#include <spawn.h>
	#include <sys/wait.h>
	#include <thread>
	#include <unistd.h>
#endif

#ifdef HERTA_PLATFORM_WINDOWS
namespace Herta
{
namespace
{
struct FHandleCloser
{
	void operator()(const HANDLE Handle) const noexcept
	{
		if (Handle != nullptr && Handle != INVALID_HANDLE_VALUE)
		{
			CloseHandle(Handle);
		}
	}
};

using FUniqueHandle = std::unique_ptr<std::remove_pointer_t<HANDLE>, FHandleCloser>;

[[nodiscard]] FUniqueHandle MakeHandle(const HANDLE Handle) noexcept
{
	return FUniqueHandle(Handle == INVALID_HANDLE_VALUE ? nullptr : Handle);
}

[[nodiscard]] std::unexpected<FProcessError> Failure(const EProcessErrorCode Code, const std::string_view Message)
{
	return std::unexpected(FProcessError{Code, std::format("{} (Windows error {})", Message, GetLastError())});
}

[[nodiscard]] bool Widen(const std::string_view Text, std::wstring& Result)
{
	Result.clear();
	if (Text.empty())
	{
		return true;
	}
	const int Size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, Text.data(), static_cast<int>(Text.size()), nullptr, 0);
	if (Size <= 0)
	{
		return false;
	}
	Result.resize(static_cast<std::size_t>(Size));
	return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, Text.data(), static_cast<int>(Text.size()), Result.data(), Size) == Size;
}

// Follows the CommandLineToArgvW rules: backslashes are literal unless they precede a quote.
void AppendQuotedArgument(std::wstring& CommandLine, const std::wstring_view Argument)
{
	if (!CommandLine.empty())
	{
		CommandLine.push_back(L' ');
	}
	if (!Argument.empty() && Argument.find_first_of(L" \t\n\v\"") == std::wstring_view::npos)
	{
		CommandLine.append(Argument);
		return;
	}

	CommandLine.push_back(L'"');
	for (auto Character = Argument.begin();; ++Character)
	{
		std::size_t Backslashes = 0;
		while (Character != Argument.end() && *Character == L'\\')
		{
			++Character;
			++Backslashes;
		}
		if (Character == Argument.end())
		{
			CommandLine.append(Backslashes * 2, L'\\');
			break;
		}
		if (*Character == L'"')
		{
			CommandLine.append(Backslashes * 2 + 1, L'\\');
		}
		else
		{
			CommandLine.append(Backslashes, L'\\');
		}
		CommandLine.push_back(*Character);
	}
	CommandLine.push_back(L'"');
}

// The child writes through an inherited handle; the file disappears when the last handle closes.
[[nodiscard]] FUniqueHandle CreateCaptureFile(SECURITY_ATTRIBUTES& Inheritable)
{
	static std::atomic<std::uint64_t> Counter{0};
	std::error_code Error;
	const std::filesystem::path Path = std::filesystem::temp_directory_path(Error) / std::format("Herta-{}-{}-{}.log", GetCurrentProcessId(), GetTickCount64(), Counter.fetch_add(1));
	if (Error)
	{
		return {};
	}
	return MakeHandle(CreateFileW(Path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &Inheritable, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr));
}

[[nodiscard]] std::string ReadCapture(const HANDLE File, const std::size_t MaximumBytes)
{
	std::string Text;
	LARGE_INTEGER Start{};
	if (!SetFilePointerEx(File, Start, nullptr, FILE_BEGIN))
	{
		return Text;
	}
	std::array<char, 4096> Buffer{};
	DWORD Read = 0;
	while (Text.size() < MaximumBytes && ReadFile(File, Buffer.data(), static_cast<DWORD>(Buffer.size()), &Read, nullptr) && Read > 0)
	{
		Text.append(Buffer.data(), std::min<std::size_t>(Read, MaximumBytes - Text.size()));
	}
	return Text;
}
}

std::expected<FProcessResult, FProcessError> RunProcess(const FProcessRequest& Request)
{
	if (Request.Executable.empty() || Request.Timeout.count() < 0)
	{
		return std::unexpected(FProcessError{EProcessErrorCode::InvalidRequest, "A process requires an executable and a nonnegative timeout"});
	}

	std::wstring CommandLine;
	AppendQuotedArgument(CommandLine, Request.Executable.native());
	std::wstring WideArgument;
	for (const std::string& Argument : Request.Arguments)
	{
		if (!Widen(Argument, WideArgument))
		{
			return std::unexpected(FProcessError{EProcessErrorCode::InvalidRequest, "Process arguments must be valid UTF-8"});
		}
		AppendQuotedArgument(CommandLine, WideArgument);
	}

	SECURITY_ATTRIBUTES Inheritable{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
	const FUniqueHandle Output = CreateCaptureFile(Inheritable);
	const FUniqueHandle ErrorOutput = CreateCaptureFile(Inheritable);
	const FUniqueHandle Input = MakeHandle(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &Inheritable, OPEN_EXISTING, 0, nullptr));
	if (!Output || !ErrorOutput || !Input)
	{
		return Failure(EProcessErrorCode::LaunchFailed, "Cannot create process output files");
	}

	// Restrict inheritance to these three handles so the child cannot hold unrelated editor files open.
	std::array<HANDLE, 3> InheritedHandles{Input.get(), Output.get(), ErrorOutput.get()};
	SIZE_T AttributeSize = 0;
	InitializeProcThreadAttributeList(nullptr, 1, 0, &AttributeSize);
	std::vector<std::byte> AttributeStorage(AttributeSize);
	const auto Attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(AttributeStorage.data());
	if (!InitializeProcThreadAttributeList(Attributes, 1, 0, &AttributeSize))
	{
		return Failure(EProcessErrorCode::LaunchFailed, "Cannot initialize process attributes");
	}
	const std::unique_ptr<std::remove_pointer_t<LPPROC_THREAD_ATTRIBUTE_LIST>, decltype(&DeleteProcThreadAttributeList)> AttributeScope(Attributes, &DeleteProcThreadAttributeList);
	if (!UpdateProcThreadAttribute(Attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, InheritedHandles.data(), sizeof(InheritedHandles), nullptr, nullptr))
	{
		return Failure(EProcessErrorCode::LaunchFailed, "Cannot restrict inherited handles");
	}

	// Closing the job kills every process the child started, even if the caller crashes.
	const FUniqueHandle Job(CreateJobObjectW(nullptr, nullptr));
	JOBOBJECT_EXTENDED_LIMIT_INFORMATION Limits{};
	Limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
	if (!Job || !SetInformationJobObject(Job.get(), JobObjectExtendedLimitInformation, &Limits, sizeof(Limits)))
	{
		return Failure(EProcessErrorCode::LaunchFailed, "Cannot create a process job");
	}

	STARTUPINFOEXW Startup{};
	Startup.StartupInfo.cb = sizeof(Startup);
	Startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
	Startup.StartupInfo.hStdInput = Input.get();
	Startup.StartupInfo.hStdOutput = Output.get();
	Startup.StartupInfo.hStdError = ErrorOutput.get();
	Startup.lpAttributeList = Attributes;
	PROCESS_INFORMATION Information{};
	if (!CreateProcessW(Request.Executable.c_str(), CommandLine.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT, nullptr, nullptr, &Startup.StartupInfo, &Information))
	{
		const DWORD Error = GetLastError();
		const std::u8string Executable = Request.Executable.u8string();
		return std::unexpected(FProcessError{EProcessErrorCode::LaunchFailed, std::format("Cannot start '{}' (Windows error {})", std::string_view(reinterpret_cast<const char*>(Executable.data()), Executable.size()), Error)});
	}
	const FUniqueHandle Process(Information.hProcess);
	const FUniqueHandle Thread(Information.hThread);
	if (!AssignProcessToJobObject(Job.get(), Process.get()))
	{
		TerminateProcess(Process.get(), 1);
		return Failure(EProcessErrorCode::LaunchFailed, "Cannot assign the process to its job");
	}
	ResumeThread(Thread.get());

	const auto Started = std::chrono::steady_clock::now();
	while (true)
	{
		const DWORD Wait = WaitForSingleObject(Process.get(), 20);
		if (Wait == WAIT_OBJECT_0)
		{
			break;
		}
		std::optional<FProcessError> Stop;
		if (Wait != WAIT_TIMEOUT)
		{
			Stop = FProcessError{EProcessErrorCode::WaitFailed, std::format("Waiting for the process failed (Windows error {})", GetLastError())};
		}
		else if (Request.ShouldCancel && Request.ShouldCancel())
		{
			Stop = FProcessError{EProcessErrorCode::Cancelled, "The process was cancelled"};
		}
		else if (Request.Timeout.count() > 0 && std::chrono::steady_clock::now() - Started > Request.Timeout)
		{
			Stop = FProcessError{EProcessErrorCode::TimedOut, std::format("The process exceeded its {} ms timeout", Request.Timeout.count())};
		}
		if (Stop)
		{
			TerminateJobObject(Job.get(), 1);
			WaitForSingleObject(Process.get(), INFINITE);
			return std::unexpected(std::move(*Stop));
		}
	}

	DWORD ExitCode = 0;
	if (!GetExitCodeProcess(Process.get(), &ExitCode))
	{
		return Failure(EProcessErrorCode::WaitFailed, "Cannot read the process exit code");
	}
	return FProcessResult{static_cast<int>(ExitCode), ReadCapture(Output.get(), Request.MaximumOutputBytes), ReadCapture(ErrorOutput.get(), Request.MaximumOutputBytes)};
}

std::filesystem::path GetExecutablePath()
{
	std::wstring Buffer(MAX_PATH, L'\0');
	while (true)
	{
		const DWORD Length = GetModuleFileNameW(nullptr, Buffer.data(), static_cast<DWORD>(Buffer.size()));
		if (Length == 0)
		{
			return {};
		}
		if (Length < Buffer.size())
		{
			Buffer.resize(Length);
			return Buffer;
		}
		Buffer.resize(Buffer.size() * 2);
	}
}
}
#else
namespace Herta
{
namespace
{
class FFileDescriptor final
{
public:
	explicit FFileDescriptor(const int InValue = -1) noexcept
	    : Value(InValue)
	{
	}
	~FFileDescriptor()
	{
		if (Value >= 0)
		{
			close(Value);
		}
	}
	FFileDescriptor(const FFileDescriptor&) = delete;
	FFileDescriptor& operator=(const FFileDescriptor&) = delete;

	[[nodiscard]] int Get() const noexcept
	{
		return Value;
	}

private:
	int Value;
};

[[nodiscard]] std::unexpected<FProcessError> Failure(const EProcessErrorCode Code, const std::string_view Message, const int Error)
{
	return std::unexpected(FProcessError{Code, std::format("{}: {}", Message, std::strerror(Error))});
}

// Unlinked immediately, so the capture never outlives the descriptors.
[[nodiscard]] int CreateCaptureFile()
{
	std::error_code Error;
	std::string Template = (std::filesystem::temp_directory_path(Error) / "Herta-XXXXXX").string();
	if (Error)
	{
		return -1;
	}
	const int Descriptor = mkostemp(Template.data(), O_CLOEXEC);
	if (Descriptor >= 0)
	{
		unlink(Template.c_str());
	}
	return Descriptor;
}

[[nodiscard]] std::string ReadCapture(const int Descriptor, const std::size_t MaximumBytes)
{
	std::string Text;
	if (lseek(Descriptor, 0, SEEK_SET) != 0)
	{
		return Text;
	}
	std::array<char, 4096> Buffer{};
	while (Text.size() < MaximumBytes)
	{
		const ssize_t Read = read(Descriptor, Buffer.data(), Buffer.size());
		if (Read < 0 && errno == EINTR)
		{
			continue;
		}
		if (Read <= 0)
		{
			break;
		}
		Text.append(Buffer.data(), std::min<std::size_t>(static_cast<std::size_t>(Read), MaximumBytes - Text.size()));
	}
	return Text;
}

[[nodiscard]] int ToExitCode(const int Status) noexcept
{
	if (WIFEXITED(Status))
	{
		return WEXITSTATUS(Status);
	}
	return WIFSIGNALED(Status) ? 128 + WTERMSIG(Status) : -1;
}
}

std::expected<FProcessResult, FProcessError> RunProcess(const FProcessRequest& Request)
{
	if (Request.Executable.empty() || Request.Timeout.count() < 0)
	{
		return std::unexpected(FProcessError{EProcessErrorCode::InvalidRequest, "A process requires an executable and a nonnegative timeout"});
	}

	const FFileDescriptor Output(CreateCaptureFile());
	const FFileDescriptor ErrorOutput(CreateCaptureFile());
	if (Output.Get() < 0 || ErrorOutput.Get() < 0)
	{
		return Failure(EProcessErrorCode::LaunchFailed, "Cannot create process output files", errno);
	}

	posix_spawn_file_actions_t Actions;
	posix_spawn_file_actions_init(&Actions);
	posix_spawn_file_actions_addopen(&Actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
	posix_spawn_file_actions_adddup2(&Actions, Output.Get(), STDOUT_FILENO);
	posix_spawn_file_actions_adddup2(&Actions, ErrorOutput.Get(), STDERR_FILENO);
	// Descriptors the editor opened without O_CLOEXEC must not leak into the child.
	posix_spawn_file_actions_addclosefrom_np(&Actions, STDERR_FILENO + 1);

	// A new process group lets cancellation kill every process the child started.
	posix_spawnattr_t Attributes;
	posix_spawnattr_init(&Attributes);
	posix_spawnattr_setflags(&Attributes, POSIX_SPAWN_SETPGROUP);
	posix_spawnattr_setpgroup(&Attributes, 0);

	const std::string Executable = Request.Executable.string();
	std::vector<char*> Arguments;
	Arguments.push_back(const_cast<char*>(Executable.c_str()));
	for (const std::string& Argument : Request.Arguments)
	{
		Arguments.push_back(const_cast<char*>(Argument.c_str()));
	}
	Arguments.push_back(nullptr);

	pid_t Process = 0;
	const int SpawnError = posix_spawn(&Process, Executable.c_str(), &Actions, &Attributes, Arguments.data(), environ);
	posix_spawn_file_actions_destroy(&Actions);
	posix_spawnattr_destroy(&Attributes);
	if (SpawnError != 0)
	{
		return Failure(EProcessErrorCode::LaunchFailed, std::format("Cannot start '{}'", Executable), SpawnError);
	}

	const auto Started = std::chrono::steady_clock::now();
	int Status = 0;
	while (true)
	{
		const pid_t Waited = waitpid(Process, &Status, WNOHANG);
		if (Waited == Process)
		{
			break;
		}
		std::optional<FProcessError> Stop;
		if (Waited < 0 && errno != EINTR)
		{
			Stop = FProcessError{EProcessErrorCode::WaitFailed, std::format("Waiting for the process failed: {}", std::strerror(errno))};
		}
		else if (Request.ShouldCancel && Request.ShouldCancel())
		{
			Stop = FProcessError{EProcessErrorCode::Cancelled, "The process was cancelled"};
		}
		else if (Request.Timeout.count() > 0 && std::chrono::steady_clock::now() - Started > Request.Timeout)
		{
			Stop = FProcessError{EProcessErrorCode::TimedOut, std::format("The process exceeded its {} ms timeout", Request.Timeout.count())};
		}
		if (Stop)
		{
			kill(-Process, SIGKILL);
			while (waitpid(Process, &Status, 0) < 0 && errno == EINTR)
			{
			}
			return std::unexpected(std::move(*Stop));
		}
		// ponytail: 20 ms polling; switch to pidfd_open and poll if launch latency matters.
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}

	return FProcessResult{ToExitCode(Status), ReadCapture(Output.Get(), Request.MaximumOutputBytes), ReadCapture(ErrorOutput.Get(), Request.MaximumOutputBytes)};
}

std::filesystem::path GetExecutablePath()
{
	std::error_code Error;
	return std::filesystem::read_symlink("/proc/self/exe", Error);
}
}
#endif
