#include "Herta/Core/Log.h"

#include <spdlog/logger.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <algorithm>
#include <atomic>
#include <deque>
#include <exception>
#include <functional>
#include <mutex>
#include <system_error>
#include <thread>

#ifdef HERTA_PLATFORM_WINDOWS
	#include <spdlog/sinks/msvc_sink.h>
#endif

namespace Herta
{
namespace
{
[[nodiscard]] constexpr spdlog::level::level_enum ToSpdlogLevel(const ELogLevel Level) noexcept
{
	switch (Level)
	{
		case ELogLevel::Trace:
			return spdlog::level::trace;
		case ELogLevel::Debug:
			return spdlog::level::debug;
		case ELogLevel::Info:
			return spdlog::level::info;
		case ELogLevel::Warning:
			return spdlog::level::warn;
		case ELogLevel::Error:
			return spdlog::level::err;
		case ELogLevel::Critical:
			return spdlog::level::critical;
		case ELogLevel::Off:
			return spdlog::level::off;
	}

	return spdlog::level::off;
}

[[nodiscard]] constexpr bool IsEnabled(const ELogLevel Level, const ELogLevel MinimumLevel) noexcept
{
	return Level != ELogLevel::Off && MinimumLevel != ELogLevel::Off && Level >= MinimumLevel;
}

[[nodiscard]] std::string PathToUtf8(const std::filesystem::path& Path)
{
	const std::u8string Text = Path.generic_u8string();
	return {reinterpret_cast<const char*>(Text.data()), Text.size()};
}
}

struct FLogService::FImplementation
{
	explicit FImplementation(FLogOptions InOptions)
	    : Options(std::move(InOptions))
	    , StartTime(std::chrono::steady_clock::now())
	{
		std::vector<spdlog::sink_ptr> Sinks;

		if (Options.bConsoleOutput)
		{
			Sinks.emplace_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
		}

#ifdef HERTA_PLATFORM_WINDOWS
		if (Options.bDebuggerOutput)
		{
			Sinks.emplace_back(std::make_shared<spdlog::sinks::msvc_sink_mt>());
		}
#endif

		if (Options.bFileOutput)
		{
			Sinks.emplace_back(std::make_shared<spdlog::sinks::rotating_file_sink_mt>(Options.FilePath.native(), Options.MaximumFileSize, Options.MaximumFileCount));
		}

		BackendLogger = std::make_shared<spdlog::logger>("Herta", Sinks.begin(), Sinks.end());
		BackendLogger->set_level(spdlog::level::trace);
		BackendLogger->set_pattern("%^[%H:%M:%S.%e] [%l] %v%$");
		BackendLogger->flush_on(spdlog::level::err);

		BackendLogger->set_error_handler([this](const std::string&)
		{
			SinkFailures.fetch_add(1, std::memory_order_relaxed);
		});
	}

	FLogOptions Options;
	std::chrono::steady_clock::time_point StartTime;
	mutable std::mutex BufferMutex;
	std::deque<FLogRecord> Records;
	std::uint64_t NextSequence = 1;
	std::uint64_t Generation = 1;
	std::atomic_uint64_t DroppedRecords = 0;
	std::atomic_uint64_t FormattingFailures = 0;
	std::atomic_uint64_t SinkFailures = 0;
	std::shared_ptr<spdlog::logger> BackendLogger;
};

std::string_view GetLogLevelName(const ELogLevel Level) noexcept
{
	switch (Level)
	{
		case ELogLevel::Trace:
			return "Trace";
		case ELogLevel::Debug:
			return "Debug";
		case ELogLevel::Info:
			return "Info";
		case ELogLevel::Warning:
			return "Warning";
		case ELogLevel::Error:
			return "Error";
		case ELogLevel::Critical:
			return "Critical";
		case ELogLevel::Off:
			return "Off";
	}

	return "Unknown";
}

std::expected<std::unique_ptr<FLogService>, FLogError> FLogService::Create(FLogOptions Options)
{
	if (Options.bFileOutput)
	{
		if (Options.FilePath.empty())
		{
			return std::unexpected(FLogError{"A log file path is required when file output is enabled"});
		}

		if (Options.MaximumFileSize == 0 || Options.MaximumFileCount == 0)
		{
			return std::unexpected(FLogError{"Rotating log file size and count must be greater than zero"});
		}

		const std::filesystem::path ParentPath = Options.FilePath.parent_path();
		if (!ParentPath.empty())
		{
			std::error_code DirectoryError;
			std::filesystem::create_directories(ParentPath, DirectoryError);
			if (DirectoryError)
			{
				return std::unexpected(FLogError{std::format("Could not create log directory '{}': {}", PathToUtf8(ParentPath), DirectoryError.message())});
			}
		}
	}

	try
	{
		return std::unique_ptr<FLogService>(new FLogService(std::make_unique<FImplementation>(std::move(Options))));
	}
	catch (const std::exception& Exception)
	{
		return std::unexpected(FLogError{std::format("Could not initialize logging: {}", Exception.what())});
	}
	catch (...)
	{
		return std::unexpected(FLogError{"Could not initialize logging due to an unknown error"});
	}
}

FLogService::FLogService(std::unique_ptr<FImplementation> InImplementation) noexcept
    : Implementation(std::move(InImplementation))
{
}

FLogService::~FLogService()
{
	Flush();
}

bool FLogService::ShouldLog(const FLogCategory& Category, const ELogLevel Level) const noexcept
{
	const ELogLevel EffectiveMinimum = std::max(Implementation->Options.MinimumLevel, Category.MinimumLevel);
	return IsEnabled(Level, EffectiveMinimum);
}

void FLogService::LogText(const FLogCategory& Category, const ELogLevel Level, const std::string_view Message, const std::source_location& Location) noexcept
{
	if (!ShouldLog(Category, Level))
	{
		return;
	}

	try
	{
		WriteRecord(Category, Level, std::string(Message), Location);
	}
	catch (...)
	{
		Implementation->DroppedRecords.fetch_add(1, std::memory_order_relaxed);
	}
}

std::expected<FLogReadResult, FLogError> FLogService::ReadEditorBuffer(const FLogCursor Cursor, const std::size_t MaximumRecords) const
{
	try
	{
		std::scoped_lock Lock(Implementation->BufferMutex);

		FLogReadResult Result;
		Result.NextCursor.Generation = Implementation->Generation;
		Result.bGenerationReset = Cursor.Generation != 0 && Cursor.Generation != Implementation->Generation;

		const std::uint64_t FirstAvailableSequence = Implementation->Records.empty() ? Implementation->NextSequence : Implementation->Records.front().Sequence;
		std::uint64_t RequestedSequence = Cursor.NextSequence == 0 ? FirstAvailableSequence : Cursor.NextSequence;

		if (Result.bGenerationReset)
		{
			RequestedSequence = FirstAvailableSequence;
		}
		else if (RequestedSequence < FirstAvailableSequence)
		{
			RequestedSequence = FirstAvailableSequence;
			Result.bHistoryTruncated = true;
		}

		for (const FLogRecord& Record : Implementation->Records)
		{
			if (Record.Sequence < RequestedSequence)
			{
				continue;
			}

			if (Result.Records.size() == MaximumRecords)
			{
				break;
			}

			Result.Records.push_back(Record);
		}

		Result.NextCursor.NextSequence = Result.Records.empty() ? std::min(RequestedSequence, Implementation->NextSequence) : Result.Records.back().Sequence + 1;
		return Result;
	}
	catch (const std::exception& Exception)
	{
		return std::unexpected(FLogError{std::format("Could not read the editor log buffer: {}", Exception.what())});
	}
	catch (...)
	{
		return std::unexpected(FLogError{"Could not read the editor log buffer due to an unknown error"});
	}
}

void FLogService::ClearEditorBuffer() noexcept
{
	std::scoped_lock Lock(Implementation->BufferMutex);
	Implementation->Records.clear();
	++Implementation->Generation;
}

void FLogService::Flush() noexcept
{
	try
	{
		Implementation->BackendLogger->flush();
	}
	catch (...)
	{
		Implementation->SinkFailures.fetch_add(1, std::memory_order_relaxed);
	}
}

FLogStatistics FLogService::GetStatistics() const noexcept
{
	return {
	    .DroppedRecords = Implementation->DroppedRecords.load(std::memory_order_relaxed),
	    .FormattingFailures = Implementation->FormattingFailures.load(std::memory_order_relaxed),
	    .SinkFailures = Implementation->SinkFailures.load(std::memory_order_relaxed),
	};
}

void FLogService::WriteRecord(const FLogCategory& Category, const ELogLevel Level, std::string Message, const std::source_location& Location) noexcept
{
	try
	{
		FLogRecord Record{
		    .Sequence = 0,
		    .Timestamp = {},
		    .ElapsedSeconds = 0.0,
		    .ThreadId = static_cast<std::uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id())),
		    .Category = std::string(Category.Name),
		    .Level = Level,
		    .Message = std::move(Message),
		    .Source = std::nullopt,
		};

		if (Implementation->Options.bCaptureSourceLocation)
		{
			Record.Source = FLogSourceLocation{
			    .FileName = Location.file_name(),
			    .FunctionName = Location.function_name(),
			    .Line = Location.line(),
			};
		}

		{
			std::scoped_lock Lock(Implementation->BufferMutex);
			const auto SteadyNow = std::chrono::steady_clock::now();
			Record.Timestamp = std::chrono::system_clock::now();
			Record.ElapsedSeconds = std::chrono::duration<double>(SteadyNow - Implementation->StartTime).count();
			Record.Sequence = Implementation->NextSequence++;

			if (Implementation->Options.EditorBufferCapacity > 0)
			{
				if (Implementation->Records.size() == Implementation->Options.EditorBufferCapacity)
				{
					Implementation->Records.pop_front();
				}

				Implementation->Records.push_back(Record);
			}
		}

		const spdlog::source_loc BackendLocation(Record.Source ? Record.Source->FileName.c_str() : "", Record.Source ? static_cast<int>(Record.Source->Line) : 0, Record.Source ? Record.Source->FunctionName.c_str() : "");
		Implementation->BackendLogger->log(BackendLocation, ToSpdlogLevel(Level), "[{}] {}", Record.Category, Record.Message);
	}
	catch (...)
	{
		Implementation->DroppedRecords.fetch_add(1, std::memory_order_relaxed);
	}
}

void FLogService::ReportFormattingFailure(const FLogCategory& Category, const std::source_location& Location) noexcept
{
	Implementation->FormattingFailures.fetch_add(1, std::memory_order_relaxed);
	WriteRecord(Category, ELogLevel::Error, "Log message formatting failed", Location);
}
}
