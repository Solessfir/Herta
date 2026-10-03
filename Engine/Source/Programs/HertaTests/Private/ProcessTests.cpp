#include "Herta/Platform/Process.h"
#include "TestFiles.h"

#include <doctest/doctest.h>

#include <chrono>

#ifndef HERTA_PLATFORM_WINDOWS
	#include <pthread.h>
	#include <signal.h>
	#include <sys/wait.h>
	#include <unistd.h>

	#include <cstdlib>
	#include <fstream>
	#include <optional>
	#include <thread>
#endif

namespace Herta
{
namespace
{
std::string TrimLineEnd(std::string Text)
{
	while (!Text.empty() && (Text.back() == '\n' || Text.back() == '\r'))
	{
		Text.pop_back();
	}

	return Text;
}

FProcessRequest MakeLongRunningRequest()
{
#ifdef HERTA_PLATFORM_WINDOWS
	return {.Executable = "C:\\Windows\\System32\\ping.exe", .Arguments = {"-n", "30", "127.0.0.1"}};
#else
	return {.Executable = "/bin/sleep", .Arguments = {"30"}};
#endif
}

#ifndef HERTA_PLATFORM_WINDOWS
inline constexpr const char* NestedProcessDirectory = "HERTA_PROCESS_TEST_DIRECTORY";

[[nodiscard]] pid_t ReadProcessId(const std::filesystem::path& Path)
{
	std::ifstream Stream(Path);
	pid_t Process = 0;
	Stream >> Process;
	return Process;
}

[[nodiscard]] bool IsProcessRunning(const pid_t Process)
{
	if (Process <= 0)
	{
		return false;
	}
	std::ifstream Stream(std::filesystem::path("/proc") / std::to_string(Process) / "stat");
	std::string Status;
	std::getline(Stream, Status);
	const std::size_t NameEnd = Status.rfind(')');
	return NameEnd != std::string::npos && NameEnd + 2 < Status.size() && Status[NameEnd + 2] != 'Z';
}

[[nodiscard]] bool WaitForProcessExit(const pid_t Process)
{
	const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (IsProcessRunning(Process) && std::chrono::steady_clock::now() < Deadline)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	return !IsProcessRunning(Process);
}

class FNestedProcessFiles final
{
public:
	explicit FNestedProcessFiles(const std::string_view Mode)
	    : Scratch("HertaNestedProcess")
	{
		if (const char* const Previous = std::getenv(NestedProcessDirectory))
		{
			PreviousDirectory = Previous;
		}
		Tests::WriteText(Scratch.GetPath() / "Mode", Mode);
		(void)setenv(NestedProcessDirectory, Scratch.GetPath().c_str(), 1);
	}

	~FNestedProcessFiles()
	{
		for (const std::string_view Name : {"Worker", "Child"})
		{
			const pid_t Process = ReadProcessId(Scratch.GetPath() / Name);
			if (IsProcessRunning(Process))
			{
				kill(-Process, SIGKILL);
				kill(Process, SIGKILL);
			}
		}
		if (PreviousDirectory)
		{
			(void)setenv(NestedProcessDirectory, PreviousDirectory->c_str(), 1);
		}
		else
		{
			(void)unsetenv(NestedProcessDirectory);
		}
	}

	FNestedProcessFiles(const FNestedProcessFiles&) = delete;
	FNestedProcessFiles& operator=(const FNestedProcessFiles&) = delete;
	FNestedProcessFiles(FNestedProcessFiles&&) = delete;
	FNestedProcessFiles& operator=(FNestedProcessFiles&&) = delete;

	[[nodiscard]] FProcessRequest MakeRequest() const
	{
		return {.Executable = GetExecutablePath(), .Arguments = {"--test-case=Nested process test child", "--no-skip=true"}, .Timeout = std::chrono::seconds(5)};
	}

	[[nodiscard]] pid_t GetChild() const
	{
		return ReadProcessId(Scratch.GetPath() / "Child");
	}

	[[nodiscard]] pid_t GetWorker() const
	{
		return ReadProcessId(Scratch.GetPath() / "Worker");
	}

private:
	Tests::FScratchDirectory Scratch;
	std::optional<std::string> PreviousDirectory;
};
#endif
}

TEST_CASE("The executable path names the running test binary")
{
	CHECK(GetExecutablePath().stem() == "HertaTests");
}

TEST_CASE("Processes receive UTF-8 arguments without shell interpretation")
{
	const std::vector<std::string> Arguments{"echo", "a b", "quote\"inside", "back\\slash\\", "trailing\\\\", "\xc3\xbcnicode", "$HOME", "%PATH%"};
	const auto Result = RunProcess({.Executable = Tests::GetSiblingExecutable("HertaEditorCmd"), .Arguments = Arguments});
	REQUIRE(Result);
	CHECK(Result->ExitCode == 0);
	CHECK(TrimLineEnd(Result->StandardOutput) == "a b quote\"inside back\\slash\\ trailing\\\\ \xc3\xbcnicode $HOME %PATH%");
	CHECK(Result->StandardError.empty());
}

TEST_CASE("Process results report exit codes and standard error")
{
	const auto Result = RunProcess({.Executable = Tests::GetSiblingExecutable("HertaEditorCmd"), .Arguments = {"definitely.unknown"}});
	REQUIRE(Result);
	CHECK(Result->ExitCode == 1);
	CHECK(Result->StandardError.find("Unknown command") != std::string::npos);
}

TEST_CASE("Processes are killed on timeout and cancellation")
{
	FProcessRequest TimedOut = MakeLongRunningRequest();
	TimedOut.Timeout = std::chrono::milliseconds(200);
	const auto Started = std::chrono::steady_clock::now();
	const auto TimeoutResult = RunProcess(TimedOut);
	REQUIRE_FALSE(TimeoutResult);
	CHECK(TimeoutResult.error().Code == EProcessErrorCode::TimedOut);
	CHECK(std::chrono::steady_clock::now() - Started < std::chrono::seconds(10));

	FProcessRequest Cancelled = MakeLongRunningRequest();
	int Polls = 0;
	Cancelled.ShouldCancel = [&Polls]
	{
		return ++Polls > 2;
	};

	const auto CancelResult = RunProcess(Cancelled);
	REQUIRE_FALSE(CancelResult);
	CHECK(CancelResult.error().Code == EProcessErrorCode::Cancelled);
}

TEST_CASE("Missing executables fail to launch")
{
	const auto Result = RunProcess({.Executable = Tests::GetSiblingExecutable("HertaMissingProgram"), .Arguments = {}});
	REQUIRE_FALSE(Result);
	CHECK(Result.error().Code == EProcessErrorCode::LaunchFailed);
	CHECK_FALSE(RunProcess({{}, {}}));
}

TEST_CASE("Shell requests run the line through the user's shell, keeping its quoting and operators")
{
	// && and quoted arguments are shell syntax in both cmd.exe and POSIX shells; fish accepts them too.
	const auto Result = RunProcess(MakeShellRequest("echo first&& echo \"two words\""));
	REQUIRE(Result);
	CHECK(Result->ExitCode == 0);
	CHECK(Result->StandardOutput.find("first") != std::string::npos);
	CHECK(Result->StandardOutput.find("two words") != std::string::npos);

	const auto Failed = RunProcess(MakeShellRequest("exit 3"));
	REQUIRE(Failed);
	CHECK(Failed->ExitCode == 3);
}

#ifndef HERTA_PLATFORM_WINDOWS
TEST_CASE("Nested process test child" * doctest::skip())
{
	// Only the lifetime tests opt into this subprocess helper.
	const char* const Directory = std::getenv(NestedProcessDirectory);
	if (Directory == nullptr)
	{
		return;
	}
	const std::filesystem::path Root(Directory);
	std::ifstream ModeStream(Root / "Mode");
	std::string Mode;
	ModeStream >> Mode;
	Tests::WriteText(Root / "Worker", std::to_string(getpid()));
	FProcessRequest Child{.Executable = "/bin/sh", .Arguments = {"-c", "echo $$ > \"$1\"; exec /bin/sleep 30", "sh", (Root / "Child").string()}, .Timeout = std::chrono::seconds(10)};
	if (Mode == "inner-timeout")
	{
		Child.Timeout = std::chrono::milliseconds(200);
	}
	else if (Mode == "inner-cancel")
	{
		Child.ShouldCancel = [&Root]
		{
			return ReadProcessId(Root / "Child") > 0;
		};
	}
	const auto Result = RunProcess(Child);
	REQUIRE_FALSE(Result);
	CHECK(Result.error().Code == (Mode == "inner-cancel" ? EProcessErrorCode::Cancelled : EProcessErrorCode::TimedOut));
}

TEST_CASE("Cancelling or timing out a process kills nested RunProcess launches")
{
	FNestedProcessFiles Files("outer");
	FProcessRequest Request = Files.MakeRequest();
	EProcessErrorCode Expected = EProcessErrorCode::Cancelled;
	SUBCASE("Cancellation")
	{
		Request.ShouldCancel = [&Files]
		{
			return Files.GetChild() > 0;
		};
	}
	SUBCASE("Timeout")
	{
		Request.Timeout = std::chrono::seconds(2);
		Expected = EProcessErrorCode::TimedOut;
	}
	const auto Result = RunProcess(Request);
	REQUIRE_FALSE(Result);
	CHECK(Result.error().Code == Expected);
	REQUIRE(Files.GetChild() > 0);
	CHECK(WaitForProcessExit(Files.GetChild()));
}

TEST_CASE("Nested process timeouts and cancellation leave their caller alive")
{
	std::string_view Mode;
	SUBCASE("Timeout")
	{
		Mode = "inner-timeout";
	}
	SUBCASE("Cancellation")
	{
		Mode = "inner-cancel";
	}
	FNestedProcessFiles Files(Mode);
	const auto Result = RunProcess(Files.MakeRequest());
	REQUIRE(Result);
	INFO(Result->StandardOutput);
	CHECK(Result->ExitCode == 0);
	REQUIRE(Files.GetChild() > 0);
	CHECK(WaitForProcessExit(Files.GetChild()));
}

TEST_CASE("Exiting the spawning thread kills its process and nested launches")
{
	FNestedProcessFiles Files("thread-exit");
	FProcessRequest Request = Files.MakeRequest();
	Request.ShouldCancel = [&Files]() -> bool
	{
		if (Files.GetChild() > 0)
		{
			pthread_exit(nullptr);
		}
		return false;
	};
	std::thread Thread([&Request]
	{
		(void)RunProcess(Request);
	});
	Thread.join();
	REQUIRE(Files.GetChild() > 0);
	REQUIRE(Files.GetWorker() > 0);
	CHECK(WaitForProcessExit(Files.GetChild()));
	CHECK(WaitForProcessExit(Files.GetWorker()));
	(void)waitpid(Files.GetWorker(), nullptr, WNOHANG);
}
#endif
}
