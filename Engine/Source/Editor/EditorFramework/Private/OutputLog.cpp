#include "Herta/EditorFramework/OutputLog.h"

#include "Herta/EditorCore/CommandRegistry.h"

#include <algorithm>
#include <format>
#include <iterator>
#include <limits>
#include <ranges>
#include <utility>

namespace Herta
{
namespace
{
inline constexpr FLogCategory CommandCategory{"Command"};

[[nodiscard]] constexpr char FoldAscii(const char Character) noexcept
{
	return Character >= 'A' && Character <= 'Z' ? static_cast<char>(Character + ('a' - 'A')) : Character;
}

[[nodiscard]] bool ContainsCaseInsensitive(const std::string_view Text, const std::string_view Search)
{
	if (Search.empty())
	{
		return true;
	}

	return std::ranges::search(Text, Search, [](const char Left, const char Right)
	                           {
		                           return FoldAscii(Left) == FoldAscii(Right);
	                           })
	           .begin() != Text.end();
}

[[nodiscard]] bool MatchesSearch(const FLogRecord& Record, const std::string_view Search)
{
	return ContainsCaseInsensitive(Record.Category, Search) || ContainsCaseInsensitive(Record.Message, Search) || ContainsCaseInsensitive(GetLogLevelName(Record.Level), Search);
}

[[nodiscard]] std::string_view TrimWhitespace(std::string_view Text) noexcept
{
	const auto IsAsciiWhitespace = [](const char Character) constexpr
	{
		return Character == ' ' || Character == '\t' || Character == '\n' || Character == '\r' || Character == '\f' || Character == '\v';
	};
	while (!Text.empty() && IsAsciiWhitespace(Text.front()))
	{
		Text.remove_prefix(1);
	}
	while (!Text.empty() && IsAsciiWhitespace(Text.back()))
	{
		Text.remove_suffix(1);
	}
	return Text;
}

[[nodiscard]] std::vector<FOutputLogLine> FormatRecordLines(const FLogRecord& Record)
{
	const std::string Time = std::format("{:7.3f}", Record.ElapsedSeconds);
	const std::string Category = std::format("{:<14}", Record.Category);
	const std::size_t TimeEnd = Time.size();
	const std::size_t CategoryBegin = TimeEnd + 2;
	const std::size_t CategoryEnd = CategoryBegin + Record.Category.size();
	const std::size_t MessageBegin = CategoryBegin + Category.size() + 2;
	const std::string Prefix = std::format("{}  {}  ", Time, Category);
	std::vector<FOutputLogLine> Lines;
	std::size_t FirstByte = 0;
	do
	{
		const std::size_t Newline = Record.Message.find('\n', FirstByte);
		const std::size_t LastByte = Newline == std::string::npos ? Record.Message.size() : Newline;
		std::string Line = Lines.empty() ? Prefix : std::string(MessageBegin, ' ');
		Line.append(Record.Message, FirstByte, LastByte - FirstByte);
		Lines.emplace_back(FOutputLogLine{Record, std::move(Line), CategoryBegin, CategoryEnd, MessageBegin, TimeEnd});
		if (Newline == std::string::npos)
		{
			break;
		}
		FirstByte = Newline + 1;
	} while (FirstByte <= Record.Message.size());
	return Lines;
}

[[nodiscard]] std::string JoinLines(const std::span<const std::string> Lines)
{
	std::string Text;
	for (std::size_t Index = 0; Index < Lines.size(); ++Index)
	{
		if (Index > 0)
		{
			Text.push_back('\n');
		}
		Text += Lines[Index];
	}
	return Text;
}
}

struct FOutputLogModel::FImplementation
{
	FLogService* Log = nullptr;
	FEditorCommandRegistry* Commands = nullptr;
	FOutputLogOptions Options;
	FLogCursor Cursor;
	std::vector<FLogRecord> Records;
	std::vector<FOutputLogLine> VisibleLines;
	std::vector<std::string> VisibleText;
	std::vector<std::string> CommandHistory;
	FLogTextSelection Selection;
	std::array<bool, static_cast<std::size_t>(ELogLevel::Off) + 1> LevelVisible = {true, true, true, true, true, true, false};
	std::string Search;
	std::size_t CommandHistoryIndex = 0;
	bool bPaused = false;
	bool bTailRequested = true;
};

std::expected<std::unique_ptr<FOutputLogModel>, FOutputLogError> FOutputLogModel::Create(FLogService& Log, FEditorCommandRegistry& Commands, FOutputLogOptions Options)
{
	if (Options.MaximumRetainedRecords == 0 || Options.MaximumCommandHistory == 0)
	{
		return std::unexpected(FOutputLogError{"Output Log capacities must be greater than zero"});
	}

	try
	{
		auto Implementation = std::make_unique<FImplementation>();
		Implementation->Log = &Log;
		Implementation->Commands = &Commands;
		Implementation->Options = Options;
		return std::unique_ptr<FOutputLogModel>(new FOutputLogModel(std::move(Implementation)));
	}
	catch (const std::exception& Exception)
	{
		return std::unexpected(FOutputLogError{Exception.what()});
	}
}

FOutputLogModel::FOutputLogModel(std::unique_ptr<FImplementation> Implementation) noexcept
    : Implementation(std::move(Implementation))
{
}

FOutputLogModel::~FOutputLogModel() = default;

std::expected<bool, FOutputLogError> FOutputLogModel::Synchronize()
{
	if (Implementation->bPaused)
	{
		return false;
	}

	std::expected<FLogReadResult, FLogError> ReadResult = Implementation->Log->ReadEditorBuffer(Implementation->Cursor);
	if (!ReadResult)
	{
		return std::unexpected(FOutputLogError{std::move(ReadResult.error().Message)});
	}

	try
	{
		Implementation->Cursor = ReadResult->NextCursor;
		if (ReadResult->bGenerationReset || ReadResult->bHistoryTruncated)
		{
			Implementation->Records.clear();
			Implementation->Selection.Clear();
		}
		if (ReadResult->Records.empty())
		{
			if (ReadResult->bGenerationReset || ReadResult->bHistoryTruncated)
			{
				RebuildVisibleLines();
			}
			return false;
		}

		Implementation->Records.insert(Implementation->Records.end(), std::make_move_iterator(ReadResult->Records.begin()), std::make_move_iterator(ReadResult->Records.end()));
		if (Implementation->Records.size() > Implementation->Options.MaximumRetainedRecords)
		{
			const std::size_t RemoveCount = Implementation->Records.size() - Implementation->Options.MaximumRetainedRecords;
			Implementation->Records.erase(Implementation->Records.begin(), Implementation->Records.begin() + static_cast<std::ptrdiff_t>(RemoveCount));
			Implementation->Selection.Clear();
		}
		RebuildVisibleLines();
		return true;
	}
	catch (const std::exception& Exception)
	{
		return std::unexpected(FOutputLogError{Exception.what()});
	}
}

void FOutputLogModel::Clear() noexcept
{
	Implementation->Log->ClearEditorBuffer();
	Implementation->Cursor = {};
	Implementation->Records.clear();
	Implementation->VisibleLines.clear();
	Implementation->VisibleText.clear();
	Implementation->Selection.Clear();
	Implementation->bTailRequested = false;
}

void FOutputLogModel::SetPaused(const bool bPaused) noexcept
{
	Implementation->bPaused = bPaused;
}

void FOutputLogModel::SetAutoScroll(const bool bAutoScroll) noexcept
{
	Implementation->Options.bAutoScroll = bAutoScroll;
}

void FOutputLogModel::SetCategoryColorization(const bool bEnabled) noexcept
{
	Implementation->Options.bColorizeCategories = bEnabled;
}

std::expected<void, FOutputLogError> FOutputLogModel::SetSearch(std::string Search)
{
	try
	{
		Implementation->Search = std::move(Search);
		Implementation->Selection.Clear();
		RebuildVisibleLines();
		return {};
	}
	catch (const std::exception& Exception)
	{
		return std::unexpected(FOutputLogError{Exception.what()});
	}
}

std::expected<void, FOutputLogError> FOutputLogModel::SetLevelVisible(const ELogLevel Level, const bool bVisible)
{
	const std::size_t LevelIndex = static_cast<std::size_t>(Level);
	try
	{
		if (LevelIndex < Implementation->LevelVisible.size())
		{
			Implementation->LevelVisible[LevelIndex] = bVisible;
			Implementation->Selection.Clear();
			RebuildVisibleLines();
		}
		return {};
	}
	catch (const std::exception& Exception)
	{
		return std::unexpected(FOutputLogError{Exception.what()});
	}
}

bool FOutputLogModel::IsPaused() const noexcept
{
	return Implementation->bPaused;
}

bool FOutputLogModel::IsAutoScroll() const noexcept
{
	return Implementation->Options.bAutoScroll;
}

bool FOutputLogModel::IsCategoryColorizationEnabled() const noexcept
{
	return Implementation->Options.bColorizeCategories;
}

bool FOutputLogModel::IsLevelVisible(const ELogLevel Level) const noexcept
{
	const std::size_t LevelIndex = static_cast<std::size_t>(Level);
	return LevelIndex < Implementation->LevelVisible.size() && Implementation->LevelVisible[LevelIndex];
}

std::string_view FOutputLogModel::GetSearch() const noexcept
{
	return Implementation->Search;
}

std::span<const FOutputLogLine> FOutputLogModel::GetVisibleLines() const noexcept
{
	return Implementation->VisibleLines;
}

std::span<const std::string> FOutputLogModel::GetVisibleText() const noexcept
{
	return Implementation->VisibleText;
}

FLogTextSelection& FOutputLogModel::GetSelection() noexcept
{
	return Implementation->Selection;
}

const FLogTextSelection& FOutputLogModel::GetSelection() const noexcept
{
	return Implementation->Selection;
}

std::expected<std::string, FOutputLogError> FOutputLogModel::CopySelectionOrVisible() const
{
	try
	{
		if (Implementation->Selection.HasSelection())
		{
			return Implementation->Selection.Copy(Implementation->VisibleText);
		}
		return JoinLines(Implementation->VisibleText);
	}
	catch (const std::exception& Exception)
	{
		return std::unexpected(FOutputLogError{Exception.what()});
	}
}

std::expected<void, FOutputLogError> FOutputLogModel::SubmitCommand(const std::string_view CommandLine)
{
	const std::string_view Command = TrimWhitespace(CommandLine);
	if (Command.empty())
	{
		return std::unexpected(FOutputLogError{"Command line is empty"});
	}

	try
	{
		Implementation->Log->LogText(CommandCategory, ELogLevel::Info, std::format("> {}", Command));
		std::expected<FEditorCommandResult, FEditorCommandError> Result = Implementation->Commands->Execute(Command);
		if (Result)
		{
			if (!Result->Message.empty())
			{
				Implementation->Log->LogText(CommandCategory, Result->ExitCode == 0 ? ELogLevel::Info : ELogLevel::Warning, Result->Message);
			}
		}
		else
		{
			Implementation->Log->LogText(CommandCategory, ELogLevel::Warning, Result.error().Message);
		}

		if (Implementation->CommandHistory.empty() || Implementation->CommandHistory.back() != Command)
		{
			Implementation->CommandHistory.emplace_back(Command);
			if (Implementation->CommandHistory.size() > Implementation->Options.MaximumCommandHistory)
			{
				Implementation->CommandHistory.erase(Implementation->CommandHistory.begin());
			}
		}
		Implementation->CommandHistoryIndex = Implementation->CommandHistory.size();
		Implementation->bTailRequested = Implementation->Options.bAutoScroll;
		return {};
	}
	catch (const std::exception& Exception)
	{
		return std::unexpected(FOutputLogError{Exception.what()});
	}
}

std::expected<std::vector<std::string>, FOutputLogError> FOutputLogModel::CompleteCommand(const std::string_view Prefix, const std::size_t MaximumResults) const
{
	std::expected<std::vector<std::string>, FEditorCommandError> Result = Implementation->Commands->Complete(Prefix, MaximumResults);
	if (!Result)
	{
		return std::unexpected(FOutputLogError{std::move(Result.error().Message)});
	}
	return std::move(*Result);
}

std::expected<std::string, FOutputLogError> FOutputLogModel::NavigateHistory(const int Direction)
{
	if (Implementation->CommandHistory.empty() || Direction == 0)
	{
		return std::string{};
	}

	if (Direction < 0 && Implementation->CommandHistoryIndex > 0)
	{
		--Implementation->CommandHistoryIndex;
	}
	else if (Direction > 0 && Implementation->CommandHistoryIndex < Implementation->CommandHistory.size())
	{
		++Implementation->CommandHistoryIndex;
	}
	try
	{
		return Implementation->CommandHistoryIndex < Implementation->CommandHistory.size() ? Implementation->CommandHistory[Implementation->CommandHistoryIndex] : std::string{};
	}
	catch (const std::exception& Exception)
	{
		return std::unexpected(FOutputLogError{Exception.what()});
	}
}

std::span<const std::string> FOutputLogModel::GetCommandHistory() const noexcept
{
	return Implementation->CommandHistory;
}

bool FOutputLogModel::HasTailRequest() const noexcept
{
	return Implementation->bTailRequested;
}

void FOutputLogModel::AcknowledgeTailRequest() noexcept
{
	Implementation->bTailRequested = false;
}

void FOutputLogModel::RebuildVisibleLines()
{
	Implementation->VisibleLines.clear();
	Implementation->VisibleText.clear();
	Implementation->VisibleLines.reserve(Implementation->Records.size());
	Implementation->VisibleText.reserve(Implementation->Records.size());
	for (const FLogRecord& Record : Implementation->Records)
	{
		const std::size_t LevelIndex = static_cast<std::size_t>(Record.Level);
		if (LevelIndex >= Implementation->LevelVisible.size() || !Implementation->LevelVisible[LevelIndex] || !MatchesSearch(Record, Implementation->Search))
		{
			continue;
		}

		for (FOutputLogLine& Line : FormatRecordLines(Record))
		{
			Implementation->VisibleText.emplace_back(Line.Text);
			Implementation->VisibleLines.emplace_back(std::move(Line));
		}
	}
	Implementation->Selection.ClampTo(Implementation->VisibleText);
}
}
