#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <limits>
#include <memory>
#include <optional>
#include <source_location>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Herta
{
enum class ELogLevel : std::uint8_t
{
	Trace,
	Debug,
	Info,
	Warning,
	Error,
	Critical,
	Off
};

[[nodiscard]] std::string_view GetLogLevelName(ELogLevel Level) noexcept;

struct FLogCategory
{
	std::string_view Name;
	ELogLevel MinimumLevel = ELogLevel::Trace;
};

struct FLogSourceLocation
{
	std::string FileName;
	std::string FunctionName;
	std::uint_least32_t Line = 0;
};

struct FLogRecord
{
	std::uint64_t Sequence = 0;
	std::chrono::system_clock::time_point Timestamp;
	double ElapsedSeconds = 0.0;
	std::uint64_t ThreadId = 0;
	std::string Category;
	ELogLevel Level = ELogLevel::Info;
	std::string Message;
	std::optional<FLogSourceLocation> Source;
};

struct FLogCursor
{
	std::uint64_t NextSequence = 0;
	std::uint64_t Generation = 0;
};

struct FLogReadResult
{
	std::vector<FLogRecord> Records;
	FLogCursor NextCursor;
	bool bHistoryTruncated = false;
	bool bGenerationReset = false;
};

struct FLogStatistics
{
	std::uint64_t DroppedRecords = 0;
	std::uint64_t FormattingFailures = 0;
	std::uint64_t SinkFailures = 0;
};

struct FLogError
{
	std::string Message;
};

struct FLogOptions
{
	std::filesystem::path FilePath = "Saved/Logs/Herta.log";
	std::size_t EditorBufferCapacity = 0;
	std::size_t MaximumFileSize = std::size_t{5} * 1024 * 1024;
	std::size_t MaximumFileCount = 3;
	ELogLevel MinimumLevel = ELogLevel::Trace;
	bool bConsoleOutput = true;
	bool bDebuggerOutput = true;
	bool bFileOutput = true;
	bool bCaptureSourceLocation = true;
};

class FLogService final
{
public:
	[[nodiscard]] static std::expected<std::unique_ptr<FLogService>, FLogError> Create(FLogOptions Options);

	~FLogService();

	FLogService(const FLogService&) = delete;
	FLogService& operator=(const FLogService&) = delete;
	FLogService(FLogService&&) = delete;
	FLogService& operator=(FLogService&&) = delete;

	[[nodiscard]] bool ShouldLog(const FLogCategory& Category, ELogLevel Level) const noexcept;

	template <typename... Arguments>
	void Log(const FLogCategory& Category, ELogLevel Level, const std::source_location& Location, std::format_string<Arguments...> Format, Arguments&&... Values) noexcept
	{
		if (!ShouldLog(Category, Level))
		{
			return;
		}

		try
		{
			WriteRecord(Category, Level, std::format(Format, std::forward<Arguments>(Values)...), Location);
		}
		catch (...)
		{
			ReportFormattingFailure(Category, Location);
		}
	}

	void LogText(const FLogCategory& Category, ELogLevel Level, std::string_view Message, const std::source_location& Location = std::source_location::current()) noexcept;
	[[nodiscard]] std::expected<FLogReadResult, FLogError> ReadEditorBuffer(FLogCursor Cursor = {}, std::size_t MaximumRecords = std::numeric_limits<std::size_t>::max()) const;
	void ClearEditorBuffer() noexcept;
	void Flush() noexcept;
	[[nodiscard]] FLogStatistics GetStatistics() const noexcept;

private:
	struct FImplementation;

	explicit FLogService(std::unique_ptr<FImplementation> Implementation) noexcept;
	void WriteRecord(const FLogCategory& Category, ELogLevel Level, std::string Message, const std::source_location& Location) noexcept;
	void ReportFormattingFailure(const FLogCategory& Category, const std::source_location& Location) noexcept;

	std::unique_ptr<FImplementation> Implementation;
};
}

#define HERTA_LOG(LogService, Category, Level, Format, ...) (LogService).Log((Category), (Level), std::source_location::current(), (Format)__VA_OPT__(, ) __VA_ARGS__)
#define HERTA_LOG_TRACE(LogService, Category, Format, ...) HERTA_LOG((LogService), (Category), ::Herta::ELogLevel::Trace, (Format)__VA_OPT__(, ) __VA_ARGS__)
#define HERTA_LOG_DEBUG(LogService, Category, Format, ...) HERTA_LOG((LogService), (Category), ::Herta::ELogLevel::Debug, (Format)__VA_OPT__(, ) __VA_ARGS__)
#define HERTA_LOG_INFO(LogService, Category, Format, ...) HERTA_LOG((LogService), (Category), ::Herta::ELogLevel::Info, (Format)__VA_OPT__(, ) __VA_ARGS__)
#define HERTA_LOG_WARNING(LogService, Category, Format, ...) HERTA_LOG((LogService), (Category), ::Herta::ELogLevel::Warning, (Format)__VA_OPT__(, ) __VA_ARGS__)
#define HERTA_LOG_ERROR(LogService, Category, Format, ...) HERTA_LOG((LogService), (Category), ::Herta::ELogLevel::Error, (Format)__VA_OPT__(, ) __VA_ARGS__)
#define HERTA_LOG_CRITICAL(LogService, Category, Format, ...) HERTA_LOG((LogService), (Category), ::Herta::ELogLevel::Critical, (Format)__VA_OPT__(, ) __VA_ARGS__)
