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

[[nodiscard]] std::expected<std::vector<std::string>, FEditorCommandError> TokenizeCommandLine(const std::string_view CommandLine)
{
	try
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
			return std::unexpected(FEditorCommandError{EEditorCommandErrorCode::ParseError, "Command line contains an unterminated quote"});
		}

		if (bTokenStarted)
		{
			Tokens.emplace_back(std::move(Current));
		}
		return Tokens;
	}
	catch (const std::exception& Exception)
	{
		return std::unexpected(FEditorCommandError{EEditorCommandErrorCode::ParseError, Exception.what()});
	}
	catch (...)
	{
		return std::unexpected(FEditorCommandError{EEditorCommandErrorCode::ParseError, "Could not parse the command line"});
	}
}
}

struct FEditorCommandRegistry::FImplementation
{
	mutable std::mutex Mutex;
	std::vector<std::shared_ptr<FStoredCommand>> Commands;
};

FEditorCommandRegistry::FEditorCommandRegistry() noexcept
{
	try
	{
		Implementation = std::make_unique<FImplementation>();
	}
	catch (...) // NOLINT(bugprone-empty-catch)
	{
		// The expected-returning API reports RegistryUnavailable after allocation failure.
	}
}

FEditorCommandRegistry::~FEditorCommandRegistry() = default;

std::expected<void, FEditorCommandError> FEditorCommandRegistry::Register(FEditorCommandDescriptor Descriptor)
{
	try
	{
		std::vector<FEditorCommandDescriptor> Descriptors;
		Descriptors.emplace_back(std::move(Descriptor));
		return RegisterBatch(std::move(Descriptors));
	}
	catch (const std::exception& Exception)
	{
		return std::unexpected(FEditorCommandError{EEditorCommandErrorCode::InternalError, Exception.what()});
	}
	catch (...)
	{
		return std::unexpected(FEditorCommandError{EEditorCommandErrorCode::InternalError, "Could not register the command"});
	}
}

std::expected<void, FEditorCommandError> FEditorCommandRegistry::RegisterBatch(std::vector<FEditorCommandDescriptor> Descriptors)
{
	if (!Implementation)
	{
		return std::unexpected(FEditorCommandError{EEditorCommandErrorCode::RegistryUnavailable, "The editor command registry could not initialize"});
	}

	try
	{
		std::vector<std::shared_ptr<FStoredCommand>> NewCommands;
		NewCommands.reserve(Descriptors.size());
		for (FEditorCommandDescriptor& Descriptor : Descriptors)
		{
			if (!IsValidCommandName(Descriptor.Name) || Descriptor.Description.empty() || !Descriptor.Handler)
			{
				return std::unexpected(FEditorCommandError{EEditorCommandErrorCode::InvalidDescriptor, "A command requires a valid name, description, and handler"});
			}

			NewCommands.emplace_back(std::make_shared<FStoredCommand>(FStoredCommand{std::move(Descriptor.Name), std::move(Descriptor.Description), std::move(Descriptor.Handler)}));
		}

		std::ranges::sort(NewCommands, {}, [](const std::shared_ptr<FStoredCommand>& Command) -> const std::string&
		                  {
			                  return Command->Name;
		                  });
		for (std::size_t Index = 1; Index < NewCommands.size(); ++Index)
		{
			if (NewCommands[Index - 1]->Name == NewCommands[Index]->Name)
			{
				return std::unexpected(FEditorCommandError{EEditorCommandErrorCode::AlreadyRegistered, std::format("Command '{}' is registered more than once in the same batch", NewCommands[Index]->Name)});
			}
		}

		std::scoped_lock Lock(Implementation->Mutex);
		for (const std::shared_ptr<FStoredCommand>& Command : NewCommands)
		{
			const auto Existing = std::ranges::lower_bound(Implementation->Commands, Command->Name, {}, [](const std::shared_ptr<FStoredCommand>& Candidate) -> const std::string&
			                                               {
				                                               return Candidate->Name;
			                                               });
			if (Existing != Implementation->Commands.end() && (*Existing)->Name == Command->Name)
			{
				return std::unexpected(FEditorCommandError{EEditorCommandErrorCode::AlreadyRegistered, std::format("Command '{}' is already registered", Command->Name)});
			}
		}

		std::vector<std::shared_ptr<FStoredCommand>> MergedCommands;
		MergedCommands.reserve(Implementation->Commands.size() + NewCommands.size());
		std::ranges::merge(Implementation->Commands, NewCommands, std::back_inserter(MergedCommands), {}, [](const std::shared_ptr<FStoredCommand>& Command) -> const std::string&
		                   {
			                   return Command->Name;
		                   },
		                   [](const std::shared_ptr<FStoredCommand>& Command) -> const std::string&
		                   {
			                   return Command->Name;
		                   });
		Implementation->Commands.swap(MergedCommands);
		return {};
	}
	catch (const std::exception& Exception)
	{
		return std::unexpected(FEditorCommandError{EEditorCommandErrorCode::InternalError, Exception.what()});
	}
	catch (...)
	{
		return std::unexpected(FEditorCommandError{EEditorCommandErrorCode::InternalError, "Could not register editor commands"});
	}
}

std::expected<FEditorCommandResult, FEditorCommandError> FEditorCommandRegistry::Execute(const std::string_view CommandLine) const
{
	if (!Implementation)
	{
		return std::unexpected(FEditorCommandError{EEditorCommandErrorCode::RegistryUnavailable, "The editor command registry could not initialize"});
	}

	std::expected<std::vector<std::string>, FEditorCommandError> Tokens = TokenizeCommandLine(CommandLine);
	if (!Tokens)
	{
		return std::unexpected(std::move(Tokens.error()));
	}

	if (Tokens->empty())
	{
		return std::unexpected(FEditorCommandError{EEditorCommandErrorCode::EmptyCommandLine, "Command line is empty"});
	}

	try
	{
		std::vector<std::string_view> Arguments;
		Arguments.reserve(Tokens->size() - 1);
		for (std::size_t Index = 1; Index < Tokens->size(); ++Index)
		{
			Arguments.emplace_back((*Tokens)[Index]);
		}

		std::shared_ptr<FStoredCommand> Command;
		{
			std::scoped_lock Lock(Implementation->Mutex);
			const auto Iterator = std::ranges::lower_bound(Implementation->Commands, Tokens->front(), {}, [](const std::shared_ptr<FStoredCommand>& Candidate) -> const std::string&
			                                               {
				                                               return Candidate->Name;
			                                               });
			if (Iterator == Implementation->Commands.end() || (*Iterator)->Name != Tokens->front())
			{
				return std::unexpected(FEditorCommandError{EEditorCommandErrorCode::UnknownCommand, std::format("Unknown command '{}'. Type help for available commands", Tokens->front())});
			}
			Command = *Iterator;
		}

		if (!Command)
		{
			return std::unexpected(FEditorCommandError{EEditorCommandErrorCode::InternalError, "Command lookup failed"});
		}

		return Command->Handler(Arguments);
	}
	catch (const std::exception& Exception)
	{
		return std::unexpected(FEditorCommandError{EEditorCommandErrorCode::ExecutionFailed, Exception.what()});
	}
	catch (...)
	{
		return std::unexpected(FEditorCommandError{EEditorCommandErrorCode::ExecutionFailed, "Command execution failed due to an unknown error"});
	}
}

std::expected<std::vector<std::string>, FEditorCommandError> FEditorCommandRegistry::Complete(const std::string_view Prefix, const std::size_t MaximumResults) const
{
	if (!Implementation)
	{
		return std::unexpected(FEditorCommandError{EEditorCommandErrorCode::RegistryUnavailable, "The editor command registry could not initialize"});
	}

	if (MaximumResults == 0)
	{
		return std::vector<std::string>{};
	}

	try
	{
		std::vector<std::string> Results;
		std::scoped_lock Lock(Implementation->Mutex);
		Results.reserve(std::min(MaximumResults, Implementation->Commands.size()));
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
	catch (const std::exception& Exception)
	{
		return std::unexpected(FEditorCommandError{EEditorCommandErrorCode::InternalError, Exception.what()});
	}
	catch (...)
	{
		return std::unexpected(FEditorCommandError{EEditorCommandErrorCode::InternalError, "Could not complete the command prefix"});
	}
}

std::expected<std::vector<FEditorCommandInfo>, FEditorCommandError> FEditorCommandRegistry::List() const
{
	if (!Implementation)
	{
		return std::unexpected(FEditorCommandError{EEditorCommandErrorCode::RegistryUnavailable, "The editor command registry could not initialize"});
	}

	try
	{
		std::vector<FEditorCommandInfo> Results;
		std::scoped_lock Lock(Implementation->Mutex);
		Results.reserve(Implementation->Commands.size());
		for (const std::shared_ptr<FStoredCommand>& Command : Implementation->Commands)
		{
			Results.emplace_back(FEditorCommandInfo{Command->Name, Command->Description});
		}
		return Results;
	}
	catch (const std::exception& Exception)
	{
		return std::unexpected(FEditorCommandError{EEditorCommandErrorCode::InternalError, Exception.what()});
	}
	catch (...)
	{
		return std::unexpected(FEditorCommandError{EEditorCommandErrorCode::InternalError, "Could not list editor commands"});
	}
}

std::expected<void, FEditorCommandError> RegisterCoreEditorCommands(FEditorCommandRegistry& Registry)
{
	try
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
			    return FEditorCommandResult{0, std::move(Message)};
		    }});
		Descriptors.emplace_back(FEditorCommandDescriptor{
		    .Name = "help",
		    .Description = "List registered editor commands",
		    .Handler = [&Registry](std::span<const std::string_view>) -> std::expected<FEditorCommandResult, FEditorCommandError>
		    {
			    std::expected<std::vector<FEditorCommandInfo>, FEditorCommandError> Commands = Registry.List();
			    if (!Commands)
			    {
				    return std::unexpected(std::move(Commands.error()));
			    }

			    std::string Message;
			    for (const FEditorCommandInfo& Command : *Commands)
			    {
				    Message.append(std::format("{:<16} {}\n", Command.Name, Command.Description));
			    }
			    if (!Message.empty())
			    {
				    Message.pop_back();
			    }
			    return FEditorCommandResult{0, std::move(Message)};
		    }});
		return Registry.RegisterBatch(std::move(Descriptors));
	}
	catch (const std::exception& Exception)
	{
		return std::unexpected(FEditorCommandError{EEditorCommandErrorCode::InternalError, Exception.what()});
	}
	catch (...)
	{
		return std::unexpected(FEditorCommandError{EEditorCommandErrorCode::InternalError, "Could not create the core editor commands"});
	}
}
}
