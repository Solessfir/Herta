#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace Herta
{
enum class EProcessErrorCode : std::uint8_t
{
	InvalidRequest,
	LaunchFailed,
	WaitFailed,
	TimedOut,
	Cancelled
};

struct FProcessError
{
	EProcessErrorCode Code = EProcessErrorCode::LaunchFailed;
	std::string Message;
};

struct FProcessRequest
{
	std::filesystem::path Executable;
	// UTF-8 arguments, passed without shell interpretation.
	std::vector<std::string> Arguments;
	// Zero waits indefinitely.
	std::chrono::milliseconds Timeout{0};
	// Polled while the process runs. Returning true kills it.
	std::function<bool()> ShouldCancel{};
	// Captured output beyond this many bytes per stream is discarded.
	std::size_t MaximumOutputBytes = std::size_t{1} << 20;
	// Windows only: appended verbatim after Arguments for programs such as cmd.exe that parse their own command line. Must be empty on Linux.
	std::string RawArguments{};
};

struct FProcessResult
{
	// The exit status, or 128 plus the signal number for a process killed by a signal on Linux.
	int ExitCode = 0;
	std::string StandardOutput;
	std::string StandardError;
};

// Blocks the calling thread, so run it on a blocking-IO task. Standard input is empty.
// Timeout and cancellation kill the whole process tree: a job object on Windows, a process group on Linux.
[[nodiscard]] std::expected<FProcessResult, FProcessError> RunProcess(const FProcessRequest& Request);

[[nodiscard]] std::filesystem::path GetExecutablePath();

// Runs CommandLine through the user's shell: %COMSPEC% (cmd.exe) on Windows, $SHELL (or /bin/sh) on Linux.
[[nodiscard]] FProcessRequest MakeShellRequest(std::string_view CommandLine);
}
