#include "Herta/Platform/Process.h"
#include "TestFiles.h"

#include <chrono>
#include <doctest/doctest.h>

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
	return {"C:\\Windows\\System32\\ping.exe", {"-n", "30", "127.0.0.1"}};
#else
	return {"/bin/sleep", {"30"}};
#endif
}
}

TEST_CASE("The executable path names the running test binary")
{
	CHECK(GetExecutablePath().stem() == "HertaTests");
}

TEST_CASE("Processes receive UTF-8 arguments without shell interpretation")
{
	const std::vector<std::string> Arguments{"echo", "a b", "quote\"inside", "back\\slash\\", "trailing\\\\", "\xc3\xbcnicode", "$HOME", "%PATH%"};
	const auto Result = RunProcess({Tests::GetSiblingExecutable("HertaEditorCmd"), Arguments});
	REQUIRE(Result);
	CHECK(Result->ExitCode == 0);
	CHECK(TrimLineEnd(Result->StandardOutput) == "a b quote\"inside back\\slash\\ trailing\\\\ \xc3\xbcnicode $HOME %PATH%");
	CHECK(Result->StandardError.empty());
}

TEST_CASE("Process results report exit codes and standard error")
{
	const auto Result = RunProcess({Tests::GetSiblingExecutable("HertaEditorCmd"), {"definitely.unknown"}});
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
	const auto Result = RunProcess({Tests::GetSiblingExecutable("HertaMissingProgram"), {}});
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
}
