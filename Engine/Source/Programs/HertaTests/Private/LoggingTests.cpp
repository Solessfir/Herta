#include "Herta/Core/Log.h"

#include <doctest/doctest.h>
#include <filesystem>
#include <format>
#include <fstream>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

namespace
{
struct FFormatProbe
{
	int* FormatCount = nullptr;
};

struct FThrowingFormatProbe
{
};
}

template <>
struct std::formatter<FFormatProbe> : std::formatter<std::string_view>
{
	auto format(const FFormatProbe& Probe, std::format_context& Context) const
	{
		++*Probe.FormatCount;
		return std::formatter<std::string_view>::format("probe", Context);
	}
};

template <>
struct std::formatter<FThrowingFormatProbe> : std::formatter<std::string_view>
{
	auto format(const FThrowingFormatProbe&, std::format_context&) const -> std::format_context::iterator
	{
		throw std::runtime_error("formatter failure");
	}
};

namespace Herta
{
namespace
{
constexpr FLogCategory TestCategory{"Tests"};
constexpr FLogCategory InfoCategory{"InfoOnly", ELogLevel::Info};

[[nodiscard]] FLogOptions MakeTestLogOptions(const std::size_t Capacity)
{
	return {
	    .EditorBufferCapacity = Capacity,
	    .MinimumLevel = ELogLevel::Trace,
	    .bConsoleOutput = false,
	    .bDebuggerOutput = false,
	    .bFileOutput = false,
	    .bCaptureSourceLocation = true};
}

[[nodiscard]] std::unique_ptr<FLogService> CreateTestLog(const std::size_t Capacity)
{
	std::expected<std::unique_ptr<FLogService>, FLogError> Result = FLogService::Create(MakeTestLogOptions(Capacity));
	REQUIRE(Result.has_value());
	return std::move(*Result);
}
}

TEST_CASE("Log level names are stable")
{
	CHECK(GetLogLevelName(ELogLevel::Trace) == "Trace");
	CHECK(GetLogLevelName(ELogLevel::Debug) == "Debug");
	CHECK(GetLogLevelName(ELogLevel::Info) == "Info");
	CHECK(GetLogLevelName(ELogLevel::Warning) == "Warning");
	CHECK(GetLogLevelName(ELogLevel::Error) == "Error");
	CHECK(GetLogLevelName(ELogLevel::Critical) == "Critical");
	CHECK(GetLogLevelName(ELogLevel::Off) == "Off");
}

TEST_CASE("Log filtering happens before message formatting")
{
	const std::unique_ptr<FLogService> Log = CreateTestLog(4);
	int FormatCount = 0;

	HERTA_LOG_TRACE(*Log, InfoCategory, "{}", FFormatProbe{&FormatCount});
	CHECK(FormatCount == 0);

	HERTA_LOG_INFO(*Log, InfoCategory, "{}", FFormatProbe{&FormatCount});
	CHECK(FormatCount == 1);

	const std::expected<FLogReadResult, FLogError> ReadResult = Log->ReadEditorBuffer();
	REQUIRE(ReadResult.has_value());
	REQUIRE(ReadResult->Records.size() == 1);
	CHECK(ReadResult->Records.front().Message == "probe");
}

TEST_CASE("Log formatting failures become diagnostic records")
{
	const std::unique_ptr<FLogService> Log = CreateTestLog(4);
	HERTA_LOG_INFO(*Log, TestCategory, "{}", FThrowingFormatProbe{});

	const std::expected<FLogReadResult, FLogError> ReadResult = Log->ReadEditorBuffer();
	REQUIRE(ReadResult.has_value());
	REQUIRE(ReadResult->Records.size() == 1);
	CHECK(ReadResult->Records.front().Level == ELogLevel::Error);
	CHECK(ReadResult->Records.front().Message == "Log message formatting failed");
	CHECK(Log->GetStatistics().FormattingFailures == 1);
}

TEST_CASE("Editor log buffer supports bounded incremental reads")
{
	const std::unique_ptr<FLogService> Log = CreateTestLog(2);
	const std::expected<FLogReadResult, FLogError> InitialRead = Log->ReadEditorBuffer();
	REQUIRE(InitialRead.has_value());

	HERTA_LOG_INFO(*Log, TestCategory, "Record {}", 1);
	HERTA_LOG_WARNING(*Log, TestCategory, "Record {}", 2);
	HERTA_LOG_ERROR(*Log, TestCategory, "Record {}", 3);

	const std::expected<FLogReadResult, FLogError> TruncatedRead = Log->ReadEditorBuffer(InitialRead->NextCursor);
	REQUIRE(TruncatedRead.has_value());
	REQUIRE(TruncatedRead->Records.size() == 2);
	CHECK(TruncatedRead->bHistoryTruncated);
	CHECK_FALSE(TruncatedRead->bGenerationReset);
	CHECK(TruncatedRead->Records[0].Message == "Record 2");
	CHECK(TruncatedRead->Records[1].Message == "Record 3");
	CHECK(TruncatedRead->Records[0].Sequence + 1 == TruncatedRead->Records[1].Sequence);

	HERTA_LOG_INFO(*Log, TestCategory, "Record {}", 4);
	const std::expected<FLogReadResult, FLogError> IncrementalRead = Log->ReadEditorBuffer(TruncatedRead->NextCursor);
	REQUIRE(IncrementalRead.has_value());
	REQUIRE(IncrementalRead->Records.size() == 1);
	CHECK(IncrementalRead->Records.front().Message == "Record 4");
}

TEST_CASE("Clearing the editor log buffer invalidates old cursors")
{
	const std::unique_ptr<FLogService> Log = CreateTestLog(4);
	HERTA_LOG_INFO(*Log, TestCategory, "Before clear");

	const std::expected<FLogReadResult, FLogError> BeforeClear = Log->ReadEditorBuffer();
	REQUIRE(BeforeClear.has_value());
	Log->ClearEditorBuffer();
	HERTA_LOG_INFO(*Log, TestCategory, "After clear");

	const std::expected<FLogReadResult, FLogError> AfterClear = Log->ReadEditorBuffer(BeforeClear->NextCursor);
	REQUIRE(AfterClear.has_value());
	CHECK(AfterClear->bGenerationReset);
	CHECK_FALSE(AfterClear->bHistoryTruncated);
	REQUIRE(AfterClear->Records.size() == 1);
	CHECK(AfterClear->Records.front().Message == "After clear");
}

TEST_CASE("Log records own their structured fields")
{
	const std::unique_ptr<FLogService> Log = CreateTestLog(2);
	HERTA_LOG_INFO(*Log, TestCategory, "Value {}", 42);

	const std::expected<FLogReadResult, FLogError> ReadResult = Log->ReadEditorBuffer();
	REQUIRE(ReadResult.has_value());
	REQUIRE(ReadResult->Records.size() == 1);

	const FLogRecord& Record = ReadResult->Records.front();
	CHECK(Record.Category == "Tests");
	CHECK(Record.Level == ELogLevel::Info);
	CHECK(Record.Message == "Value 42");
	CHECK(Record.ElapsedSeconds >= 0.0);
	REQUIRE(Record.Source.has_value());
	const FLogSourceLocation Source = Record.Source.value_or(FLogSourceLocation{});
	CHECK(Source.FileName.find("LoggingTests.cpp") != std::string::npos);
	CHECK(Source.Line > 0);
}

TEST_CASE("Concurrent log producers receive one ordered sequence")
{
	constexpr std::size_t ThreadCount = 4;
	constexpr std::size_t RecordsPerThread = 100;
	constexpr std::size_t RecordCount = ThreadCount * RecordsPerThread;
	const std::unique_ptr<FLogService> Log = CreateTestLog(RecordCount);
	std::vector<std::thread> Threads;
	Threads.reserve(ThreadCount);

	for (std::size_t ThreadIndex = 0; ThreadIndex < ThreadCount; ++ThreadIndex)
	{
		Threads.emplace_back([&Log, ThreadIndex]
		                     {
			                     for (std::size_t RecordIndex = 0; RecordIndex < RecordsPerThread; ++RecordIndex)
			                     {
				                     HERTA_LOG_DEBUG(*Log, TestCategory, "{}:{}", ThreadIndex, RecordIndex);
			                     }
		                     });
	}

	for (std::thread& Thread : Threads)
	{
		Thread.join();
	}

	const std::expected<FLogReadResult, FLogError> ReadResult = Log->ReadEditorBuffer();
	REQUIRE(ReadResult.has_value());
	REQUIRE(ReadResult->Records.size() == RecordCount);

	for (std::size_t Index = 1; Index < ReadResult->Records.size(); ++Index)
	{
		CHECK(ReadResult->Records[Index - 1].Sequence + 1 == ReadResult->Records[Index].Sequence);
	}

	const FLogStatistics Statistics = Log->GetStatistics();
	CHECK(Statistics.DroppedRecords == 0);
	CHECK(Statistics.FormattingFailures == 0);
	CHECK(Statistics.SinkFailures == 0);
}

TEST_CASE("Logger rejects invalid rotating file settings")
{
	FLogOptions Options = MakeTestLogOptions(0);
	Options.bFileOutput = true;
	Options.FilePath.clear();

	const std::expected<std::unique_ptr<FLogService>, FLogError> Result = FLogService::Create(std::move(Options));
	CHECK_FALSE(Result.has_value());
}

TEST_CASE("Rotating file logging supports Unicode paths")
{
	const std::filesystem::path TestDirectory = std::filesystem::temp_directory_path() / std::filesystem::path(u8"Herta-Logging-Żółć");
	const std::filesystem::path LogPath = TestDirectory / "Herta.log";
	std::error_code CleanupError;
	std::filesystem::remove_all(TestDirectory, CleanupError);

	FLogOptions Options = MakeTestLogOptions(0);
	Options.bFileOutput = true;
	Options.FilePath = LogPath;
	{
		std::expected<std::unique_ptr<FLogService>, FLogError> LogResult = FLogService::Create(Options);
		REQUIRE(LogResult.has_value());
		HERTA_LOG_INFO(**LogResult, TestCategory, "Unicode path probe");
		(*LogResult)->Flush();
	}

	CHECK(std::filesystem::is_regular_file(LogPath));
	std::ifstream Stream(LogPath);
	const std::string Contents((std::istreambuf_iterator<char>(Stream)), std::istreambuf_iterator<char>());
	CHECK(Contents.find("Unicode path probe") != std::string::npos);

	std::filesystem::remove_all(TestDirectory, CleanupError);
}
}
