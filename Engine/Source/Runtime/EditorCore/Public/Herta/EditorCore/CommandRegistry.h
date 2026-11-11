#pragma once

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Herta
{
enum class EEditorCommandErrorCode : std::uint8_t
{
	InvalidDescriptor,
	AlreadyRegistered,
	ParseError,
	EmptyCommandLine,
	UnknownCommand,
	ExecutionFailed,
	ReadOnly
};

[[nodiscard]] constexpr std::string_view GetEditorCommandErrorCodeName(const EEditorCommandErrorCode Code) noexcept
{
	switch (Code)
	{
		case EEditorCommandErrorCode::InvalidDescriptor:
			return "invalid_descriptor";
		case EEditorCommandErrorCode::AlreadyRegistered:
			return "already_registered";
		case EEditorCommandErrorCode::ParseError:
			return "parse_error";
		case EEditorCommandErrorCode::EmptyCommandLine:
			return "empty_command_line";
		case EEditorCommandErrorCode::UnknownCommand:
			return "unknown_command";
		case EEditorCommandErrorCode::ExecutionFailed:
			return "execution_failed";
		case EEditorCommandErrorCode::ReadOnly:
			return "read_only";
	}

	return "unknown";
}

struct FEditorCommandError
{
	EEditorCommandErrorCode Code = EEditorCommandErrorCode::ExecutionFailed;
	std::string Message;
};

struct FEditorCommandResult
{
	std::uint8_t ExitCode = 0;
	std::string Message;
};

using FEditorCommandHandler = std::move_only_function<std::expected<FEditorCommandResult, FEditorCommandError>(std::span<const std::string_view>)>;

struct FEditorCommandDescriptor
{
	std::string Name;
	std::string Description;
	// Changes files, the open level, or other lasting state, so a read-only caller such as an automation session may not run it.
	bool bMutates = false;
	FEditorCommandHandler Handler;
};

struct FEditorCommandInfo
{
	std::string Name;
	std::string Description;
	bool bMutates = false;
};

class FEditorCommandRegistry final
{
public:
	struct FImplementation;

	FEditorCommandRegistry();
	~FEditorCommandRegistry();

	FEditorCommandRegistry(const FEditorCommandRegistry&) = delete;
	FEditorCommandRegistry& operator=(const FEditorCommandRegistry&) = delete;
	FEditorCommandRegistry(FEditorCommandRegistry&&) = delete;
	FEditorCommandRegistry& operator=(FEditorCommandRegistry&&) = delete;

	[[nodiscard]] std::expected<void, FEditorCommandError> Register(FEditorCommandDescriptor Descriptor);
	// Without bAllowMutation, commands that mutate are refused before they run.
	[[nodiscard]] std::expected<FEditorCommandResult, FEditorCommandError> Execute(std::string_view CommandLine, bool bAllowMutation = true) const;
	[[nodiscard]] std::vector<std::string> Complete(std::string_view Prefix, std::size_t MaximumResults = 8) const;
	[[nodiscard]] std::vector<FEditorCommandInfo> List() const;

private:
	[[nodiscard]] std::expected<void, FEditorCommandError> RegisterBatch(std::vector<FEditorCommandDescriptor> Descriptors);

	std::unique_ptr<FImplementation> Implementation;

	friend std::expected<void, FEditorCommandError> RegisterCoreEditorCommands(FEditorCommandRegistry& Registry);
};

[[nodiscard]] std::expected<void, FEditorCommandError> RegisterCoreEditorCommands(FEditorCommandRegistry& Registry);
}
