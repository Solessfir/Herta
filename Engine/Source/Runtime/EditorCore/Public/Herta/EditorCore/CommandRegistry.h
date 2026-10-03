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
	ExecutionFailed
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
	FEditorCommandHandler Handler;
};

struct FEditorCommandInfo
{
	std::string Name;
	std::string Description;
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
	[[nodiscard]] std::expected<FEditorCommandResult, FEditorCommandError> Execute(std::string_view CommandLine) const;
	[[nodiscard]] std::vector<std::string> Complete(std::string_view Prefix, std::size_t MaximumResults = 8) const;
	[[nodiscard]] std::vector<FEditorCommandInfo> List() const;

private:
	[[nodiscard]] std::expected<void, FEditorCommandError> RegisterBatch(std::vector<FEditorCommandDescriptor> Descriptors);

	std::unique_ptr<FImplementation> Implementation;

	friend std::expected<void, FEditorCommandError> RegisterCoreEditorCommands(FEditorCommandRegistry& Registry);
};

[[nodiscard]] std::expected<void, FEditorCommandError> RegisterCoreEditorCommands(FEditorCommandRegistry& Registry);
}
