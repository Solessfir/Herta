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
	RegistryUnavailable,
	InvalidDescriptor,
	AlreadyRegistered,
	ParseError,
	EmptyCommandLine,
	UnknownCommand,
	ExecutionFailed,
	InternalError
};

[[nodiscard]] constexpr std::string_view GetEditorCommandErrorCodeName(const EEditorCommandErrorCode Code) noexcept
{
	switch (Code)
	{
		case EEditorCommandErrorCode::RegistryUnavailable:
			return "registry_unavailable";
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
		case EEditorCommandErrorCode::InternalError:
			return "internal_error";
	}

	return "internal_error";
}

struct FEditorCommandError
{
	EEditorCommandErrorCode Code = EEditorCommandErrorCode::InternalError;
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

	FEditorCommandRegistry() noexcept;
	~FEditorCommandRegistry();

	FEditorCommandRegistry(const FEditorCommandRegistry&) = delete;
	FEditorCommandRegistry& operator=(const FEditorCommandRegistry&) = delete;
	FEditorCommandRegistry(FEditorCommandRegistry&&) = delete;
	FEditorCommandRegistry& operator=(FEditorCommandRegistry&&) = delete;

	[[nodiscard]] std::expected<void, FEditorCommandError> Register(FEditorCommandDescriptor Descriptor);
	[[nodiscard]] std::expected<FEditorCommandResult, FEditorCommandError> Execute(std::string_view CommandLine) const;
	[[nodiscard]] std::expected<std::vector<std::string>, FEditorCommandError> Complete(std::string_view Prefix, std::size_t MaximumResults = 8) const;
	[[nodiscard]] std::expected<std::vector<FEditorCommandInfo>, FEditorCommandError> List() const;

private:
	[[nodiscard]] std::expected<void, FEditorCommandError> RegisterBatch(std::vector<FEditorCommandDescriptor> Descriptors);

	std::unique_ptr<FImplementation> Implementation;

	friend std::expected<void, FEditorCommandError> RegisterCoreEditorCommands(FEditorCommandRegistry& Registry);
};

[[nodiscard]] std::expected<void, FEditorCommandError> RegisterCoreEditorCommands(FEditorCommandRegistry& Registry);
}
