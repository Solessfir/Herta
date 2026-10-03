#pragma once

#include "Herta/Core/Log.h"
#include "Herta/EditorFramework/LogTextSelection.h"

#include <array>
#include <cstddef>
#include <expected>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Herta
{
class FEditorCommandRegistry;

struct FOutputLogError
{
	std::string Message;
};

struct FOutputLogLine
{
	FLogRecord Record;
	std::string Text;
	std::size_t CategoryBegin = 0;
	std::size_t CategoryEnd = 0;
	std::size_t MessageBegin = 0;
	std::size_t TimeEnd = 0;
};

struct FOutputLogOptions
{
	std::size_t MaximumRetainedRecords = 50'000;
	std::size_t MaximumCommandHistory = 64;
	bool bAutoScroll = true;
	bool bColorizeCategories = true;
};

[[nodiscard]] constexpr std::uint32_t HashOutputLogCategory(const std::string_view Category) noexcept
{
	std::uint32_t Hash = 2166136261u;
	for (const char Character : Category)
	{
		Hash ^= static_cast<unsigned char>(Character);
		Hash *= 16777619u;
	}
	return Hash;
}

[[nodiscard]] constexpr bool HasOutputLogLevelColorOverride(const ELogLevel Level) noexcept
{
	return Level == ELogLevel::Warning || Level == ELogLevel::Error || Level == ELogLevel::Critical;
}

[[nodiscard]] constexpr bool ShouldScrollOutputLog(const bool bReceivedRecords, const bool bAutoScroll, const bool bWasAtBottom, const bool bExplicitRequest) noexcept
{
	return bExplicitRequest || (bReceivedRecords && bAutoScroll && bWasAtBottom);
}

class FOutputLogModel final
{
public:
	struct FImplementation;

	[[nodiscard]] static std::expected<std::unique_ptr<FOutputLogModel>, FOutputLogError> Create(FLogService& Log, FEditorCommandRegistry& Commands, FOutputLogOptions Options = {});

	~FOutputLogModel();

	FOutputLogModel(const FOutputLogModel&) = delete;
	FOutputLogModel& operator=(const FOutputLogModel&) = delete;
	FOutputLogModel(FOutputLogModel&&) = delete;
	FOutputLogModel& operator=(FOutputLogModel&&) = delete;

	[[nodiscard]] std::expected<bool, FOutputLogError> Synchronize();
	void Clear() noexcept;
	void SetPaused(bool bPaused) noexcept;
	void SetAutoScroll(bool bAutoScroll) noexcept;
	void SetCategoryColorization(bool bEnabled) noexcept;
	[[nodiscard]] std::expected<void, FOutputLogError> SetSearch(std::string Search);
	[[nodiscard]] std::expected<void, FOutputLogError> SetLevelVisible(ELogLevel Level, bool bVisible);

	[[nodiscard]] bool IsPaused() const noexcept;
	[[nodiscard]] bool IsAutoScroll() const noexcept;
	[[nodiscard]] bool IsCategoryColorizationEnabled() const noexcept;
	[[nodiscard]] bool IsLevelVisible(ELogLevel Level) const noexcept;
	[[nodiscard]] std::string_view GetSearch() const noexcept;
	[[nodiscard]] std::span<const FOutputLogLine> GetVisibleLines() const noexcept;
	[[nodiscard]] std::span<const std::string> GetVisibleText() const noexcept;
	[[nodiscard]] FLogTextSelection& GetSelection() noexcept;
	[[nodiscard]] const FLogTextSelection& GetSelection() const noexcept;
	[[nodiscard]] std::expected<std::string, FOutputLogError> CopySelectionOrVisible() const;

	// Lines starting with '!' go to the shell runner instead of the command registry.
	[[nodiscard]] std::expected<void, FOutputLogError> SubmitCommand(std::string_view CommandLine);
	void SetShellRunner(std::function<void(std::string)> Runner);
	[[nodiscard]] std::expected<std::vector<std::string>, FOutputLogError> CompleteCommand(std::string_view Prefix, std::size_t MaximumResults = 8) const;
	[[nodiscard]] std::expected<std::string, FOutputLogError> NavigateHistory(int Direction);
	[[nodiscard]] std::span<const std::string> GetCommandHistory() const noexcept;

	[[nodiscard]] bool HasTailRequest() const noexcept;
	void AcknowledgeTailRequest() noexcept;

private:
	explicit FOutputLogModel(std::unique_ptr<FImplementation> Implementation) noexcept;
	void RebuildVisibleLines();

	std::unique_ptr<FImplementation> Implementation;
};
}
