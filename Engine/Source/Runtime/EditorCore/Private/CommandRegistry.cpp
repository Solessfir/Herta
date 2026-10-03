#include "Herta/EditorCore/CommandRegistry.h"

#include <algorithm>
#include <format>
#include <iterator>
#include <mutex>
#include <ranges>
#include <utility>

namespace Herta
{
namespace
{
struct FStoredCommand
{
	std::string Name;
	std::string Description;
	FEditorCommandHandler Handler;
};

[[nodiscard]] constexpr bool IsAsciiAlpha(const char Character) noexcept
{
	return (Character >= 'A' && Character <= 'Z') || (Character >= 'a' && Character <= 'z');
}

[[nodiscard]] constexpr bool IsAsciiDigit(const char Character) noexcept
{
	return Character >= '0' && Character <= '9';
}

[[nodiscard]] constexpr bool IsAsciiSpace(const char Character) noexcept
{
	return Character == ' ' || Character == '\t' || Character == '\n' || Character == '\r' || Character == '\f' || Character == '\v';
}

[[nodiscard]] bool IsValidCommandName(const std::string_view Name) noexcept
{
	if (Name.empty() || !IsAsciiAlpha(Name.front()))
	{
		return false;
	}

	return std::ranges::all_of(Name, [](const char Character)
	{
		return IsAsciiAlpha(Character) || IsAsciiDigit(Character) || Character == '.' || Character == '_' || Character == '-';
	});
}

[[nodiscard]] const std::string& GetCommandName(const std::shared_ptr<FStoredCommand>& Command) noexcept
{
	return Command->Name;
}

[[nodiscard]] std::expected<std::vector<std::string>, FEditorCommandError> TokenizeCommandLine(const std::string_view CommandLine)
{
	std::vector<std::string> Tokens;
	std::string Current;
	bool bQuoted = false;
	bool bEscaped = false;
	bool bTokenStarted = false;

	for (const char Character : CommandLine)
	{
		if (bEscaped)
		{
			if (Character != '"' && Character != '\\' && !IsAsciiSpace(Character))
			{
				Current.push_back('\\');
			}

			Current.push_back(Character);
			bEscaped = false;
			bTokenStarted = true;
			continue;
		}

		if (Character == '\\')
		{
			bEscaped = true;
			bTokenStarted = true;
			continue;
		}

		if (Character == '"')
		{
			bQuoted = !bQuoted;
			bTokenStarted = true;
			continue;
		}

		if (!bQuoted && IsAsciiSpace(Character))
		{
			if (bTokenStarted)
			{
				Tokens.emplace_back(std::move(Current));
				Current.clear();
				bTokenStarted = false;
			}

			continue;
		}

		Current.push_back(Character);
		bTokenStarted = true;
	}

	if (bEscaped)
	{
		Current.push_back('\\');
	}

	if (bQuoted)
	{
		return std::unexpected(FEditorCommandError{.Code = EEditorCommandErrorCode::ParseError, .Message = "Command line contains an unterminated quote"});
	}

	if (bTokenStarted)
	{
		Tokens.emplace_back(std::move(Current));
	}

	return Tokens;
}
}

struct FEditorCommandRegistry::FImplementation
{
	mutable std::mutex Mutex;
	std::vector<std::shared_ptr<FStoredCommand>> Commands;
};

FEditorCommandRegistry::FEditorCommandRegistry()
    : Implementation(std::make_unique<FImplementation>())
{
}

FEditorCommandRegistry::~FEditorCommandRegistry() = default;

std::expected<void, FEditorCommandError> FEditorCommandRegistry::Register(FEditorCommandDescriptor Descriptor)
{
	std::vector<FEditorCommandDescriptor> Descriptors;
	Descriptors.emplace_back(std::move(Descriptor));
	return RegisterBatch(std::move(Descriptors));
}

std::expected<void, FEditorCommandError> FEditorCommandRegistry::RegisterBatch(std::vector<FEditorCommandDescriptor> Descriptors)
{
	std::vector<std::shared_ptr<FStoredCommand>> NewCommands;
	NewCommands.reserve(Descriptors.size());
	for (FEditorCommandDescriptor& Descriptor : Descriptors)
	{
		if (!IsValidCommandName(Descriptor.Name) || Descriptor.Description.empty() || !Descriptor.Handler)
		{
			return std::unexpected(FEditorCommandError{.Code = EEditorCommandErrorCode::InvalidDescriptor, .Message = "A command requires a valid name, description, and handler"});
		}

		NewCommands.emplace_back(std::make_shared<FStoredCommand>(FStoredCommand{.Name = std::move(Descriptor.Name), .Description = std::move(Descriptor.Description), .Handler = std::move(Descriptor.Handler)}));
	}

	std::ranges::sort(NewCommands, {}, GetCommandName);

	for (std::size_t Index = 1; Index < NewCommands.size(); ++Index)
	{
		if (NewCommands[Index - 1]->Name == NewCommands[Index]->Name)
		{
			return std::unexpected(FEditorCommandError{.Code = EEditorCommandErrorCode::AlreadyRegistered, .Message = std::format("Command '{}' is registered more than once in the same batch", NewCommands[Index]->Name)});
		}
	}

	std::scoped_lock Lock(Implementation->Mutex);
	for (const std::shared_ptr<FStoredCommand>& Command : NewCommands)
	{
		const auto Existing = std::ranges::lower_bound(Implementation->Commands, Command->Name, {}, GetCommandName);
		if (Existing != Implementation->Commands.end() && (*Existing)->Name == Command->Name)
		{
			return std::unexpected(FEditorCommandError{.Code = EEditorCommandErrorCode::AlreadyRegistered, .Message = std::format("Command '{}' is already registered", Command->Name)});
		}
	}

	std::vector<std::shared_ptr<FStoredCommand>> MergedCommands;
	MergedCommands.reserve(Implementation->Commands.size() + NewCommands.size());
	std::ranges::merge(Implementation->Commands, NewCommands, std::back_inserter(MergedCommands), {}, GetCommandName, GetCommandName);
	Implementation->Commands.swap(MergedCommands);
	return {};
}

std::expected<FEditorCommandResult, FEditorCommandError> FEditorCommandRegistry::Execute(const std::string_view CommandLine) const
{
	std::expected<std::vector<std::string>, FEditorCommandError> Tokens = TokenizeCommandLine(CommandLine);
	if (!Tokens)
	{
		return std::unexpected(std::move(Tokens.error()));
	}

	if (Tokens->empty())
	{
		return std::unexpected(FEditorCommandError{.Code = EEditorCommandErrorCode::EmptyCommandLine, .Message = "Command line is empty"});
	}

	std::shared_ptr<FStoredCommand> Command;
	{
		std::scoped_lock Lock(Implementation->Mutex);
		const auto Iterator = std::ranges::lower_bound(Implementation->Commands, Tokens->front(), {}, GetCommandName);
		if (Iterator == Implementation->Commands.end() || (*Iterator)->Name != Tokens->front())
		{
			return std::unexpected(FEditorCommandError{.Code = EEditorCommandErrorCode::UnknownCommand, .Message = std::format("Unknown command '{}'. Type help for available commands", Tokens->front())});
		}

		Command = *Iterator;
	}

	std::vector<std::string_view> Arguments(std::next(Tokens->begin()), Tokens->end());

	// Handlers come from other modules, so their exceptions must not unwind through the registry.
	try
	{
		return Command->Handler(Arguments);
	}
	catch (const std::exception& Exception)
	{
		return std::unexpected(FEditorCommandError{.Code = EEditorCommandErrorCode::ExecutionFailed, .Message = Exception.what()});
	}
	catch (...)
	{
		return std::unexpected(FEditorCommandError{.Code = EEditorCommandErrorCode::ExecutionFailed, .Message = "Command execution failed due to an unknown error"});
	}
}

std::vector<std::string> FEditorCommandRegistry::Complete(const std::string_view Prefix, const std::size_t MaximumResults) const
{
	std::vector<std::string> Results;
	if (MaximumResults == 0)
	{
		return Results;
	}

	std::scoped_lock Lock(Implementation->Mutex);
	for (const std::shared_ptr<FStoredCommand>& Command : Implementation->Commands)
	{
		if (Command->Name.starts_with(Prefix))
		{
			Results.emplace_back(Command->Name);
			if (Results.size() == MaximumResults)
			{
				break;
			}
		}
	}

	return Results;
}

std::vector<FEditorCommandInfo> FEditorCommandRegistry::List() const
{
	std::vector<FEditorCommandInfo> Results;
	std::scoped_lock Lock(Implementation->Mutex);
	Results.reserve(Implementation->Commands.size());
	for (const std::shared_ptr<FStoredCommand>& Command : Implementation->Commands)
	{
		Results.emplace_back(FEditorCommandInfo{.Name = Command->Name, .Description = Command->Description});
	}

	return Results;
}

std::expected<void, FEditorCommandError> RegisterCoreEditorCommands(FEditorCommandRegistry& Registry)
{
	std::vector<FEditorCommandDescriptor> Descriptors;
	Descriptors.emplace_back(FEditorCommandDescriptor{
	    .Name = "echo",
	    .Description = "Print the provided arguments",
	    .Handler = [](const std::span<const std::string_view> Arguments) -> std::expected<FEditorCommandResult, FEditorCommandError>
	{
		std::string Message;
		for (const std::string_view Argument : Arguments)
		{
			if (!Message.empty())
			{
				Message.push_back(' ');
			}

			Message.append(Argument);
		}

		return FEditorCommandResult{.ExitCode = 0, .Message = std::move(Message)};
	},
	});

	Descriptors.emplace_back(FEditorCommandDescriptor{
	    .Name = "help",
	    .Description = "List registered editor commands",
	    .Handler = [&Registry](std::span<const std::string_view>) -> std::expected<FEditorCommandResult, FEditorCommandError>
	{
		std::string Message;
		for (const FEditorCommandInfo& Command : Registry.List())
		{
			Message.append(std::format("{:<16} {}\n", Command.Name, Command.Description));
		}

		if (!Message.empty())
		{
			Message.pop_back();
		}

		return FEditorCommandResult{.ExitCode = 0, .Message = std::move(Message)};
	},
	});

	return Registry.RegisterBatch(std::move(Descriptors));
}
}
