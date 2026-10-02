#include "Herta/AssetPipeline/AssetCommands.h"
#include "Herta/EditorCore/CommandRegistry.h"

#include <cstdint>
#include <exception>
#include <filesystem>
#include <print>
#include <string>
#include <string_view>

namespace
{
inline constexpr int CommandFailureExitCode = 1;
inline constexpr int HostFailureExitCode = 2;

void ReportFailure(const std::string_view Message) noexcept
{
	try
	{
		std::println(stderr, "HertaEditorCmd: {}", Message);
	}
	catch (...) // NOLINT(bugprone-empty-catch)
	{
		// Reporting must not replace the original host failure with a formatting exception.
	}
}

void AppendJsonString(std::string& Output, const std::string_view Value)
{
	static constexpr std::string_view HexDigits = "0123456789abcdef";
	Output.push_back('"');
	for (const unsigned char Character : Value)
	{
		switch (Character)
		{
			case '"':
				Output.append("\\\"");
				break;
			case '\\':
				Output.append("\\\\");
				break;
			case '\b':
				Output.append("\\b");
				break;
			case '\f':
				Output.append("\\f");
				break;
			case '\n':
				Output.append("\\n");
				break;
			case '\r':
				Output.append("\\r");
				break;
			case '\t':
				Output.append("\\t");
				break;
			default:
				if (Character < 0x20)
				{
					Output.append("\\u00");
					Output.push_back(HexDigits[Character >> 4]);
					Output.push_back(HexDigits[Character & 0x0f]);
				}
				else
				{
					Output.push_back(static_cast<char>(Character));
				}
				break;
		}
	}
	Output.push_back('"');
}

void ReportJsonDiagnostic(const std::string_view Status, const std::string_view Code, const int ExitCode, const std::string_view Message) noexcept
{
	try
	{
		std::string Output = R"({"schema":1,"status":)";
		AppendJsonString(Output, Status);
		Output.append(R"(,"code":)");
		AppendJsonString(Output, Code);
		Output.append(R"(,"exit_code":)");
		Output.append(std::to_string(ExitCode));
		Output.append(R"(,"message":)");
		AppendJsonString(Output, Message);
		Output.push_back('}');
		std::println("{}", Output);
	}
	catch (...)
	{
		ReportFailure(Message);
	}
}

// The working directory wins so Herta checkouts can share one installed binary.
[[nodiscard]] std::filesystem::path FindDefaultContentRoot(const std::filesystem::path& ExecutablePath)
{
	std::error_code PathError;
	for (std::filesystem::path Start : {std::filesystem::current_path(PathError), std::filesystem::absolute(ExecutablePath, PathError).parent_path()})
	{
		for (int Parent = 0; Parent < 8 && !Start.empty(); ++Parent)
		{
			if (std::filesystem::is_directory(Start / "Engine/Content", PathError))
			{
				return Start / "Games/Sandbox/Content";
			}
			if (Start == Start.parent_path())
			{
				break;
			}
			Start = Start.parent_path();
		}
	}
	return {};
}

[[nodiscard]] std::string BuildCommandLine(const int ArgumentCount, const char* const* const Arguments, const int FirstCommandArgument)
{
	std::string CommandLine;
	for (int Index = FirstCommandArgument; Index < ArgumentCount; ++Index)
	{
		if (!CommandLine.empty())
		{
			CommandLine.push_back(' ');
		}

		const std::string_view Argument = Arguments[Index];
		CommandLine.push_back('"');
		for (const char Character : Argument)
		{
			if (Character == '"' || Character == '\\')
			{
				CommandLine.push_back('\\');
			}
			CommandLine.push_back(Character);
		}
		CommandLine.push_back('"');
	}
	return CommandLine;
}
}

int main(const int ArgumentCount, const char* const* const Arguments)
{
	const bool bJson = ArgumentCount > 1 && std::string_view(Arguments[1]) == "--json";
	const int FirstCommandArgument = bJson ? 2 : 1;
	try
	{
		Herta::FEditorCommandRegistry Registry;
		std::expected<void, Herta::FEditorCommandError> RegistrationResult = Herta::RegisterCoreEditorCommands(Registry);
		if (RegistrationResult)
		{
			const std::filesystem::path ExecutablePath = ArgumentCount > 0 && Arguments[0] != nullptr ? Arguments[0] : "HertaEditorCmd";
			RegistrationResult = Herta::RegisterAssetCommands(Registry, {FindDefaultContentRoot(ExecutablePath)});
		}
		if (!RegistrationResult)
		{
			if (bJson)
			{
				ReportJsonDiagnostic("error", Herta::GetEditorCommandErrorCodeName(RegistrationResult.error().Code), HostFailureExitCode, RegistrationResult.error().Message);
			}
			else
			{
				ReportFailure(RegistrationResult.error().Message);
			}
			return HostFailureExitCode;
		}

		const std::string CommandLine = ArgumentCount > FirstCommandArgument ? BuildCommandLine(ArgumentCount, Arguments, FirstCommandArgument) : "help";
		const std::expected<Herta::FEditorCommandResult, Herta::FEditorCommandError> Result = Registry.Execute(CommandLine);
		if (!Result)
		{
			if (bJson)
			{
				ReportJsonDiagnostic("error", Herta::GetEditorCommandErrorCodeName(Result.error().Code), CommandFailureExitCode, Result.error().Message);
			}
			else
			{
				ReportFailure(Result.error().Message);
			}
			return CommandFailureExitCode;
		}

		if (bJson)
		{
			ReportJsonDiagnostic("success", "ok", static_cast<int>(Result->ExitCode), Result->Message);
		}
		else if (!Result->Message.empty())
		{
			std::println("{}", Result->Message);
		}
		return static_cast<int>(Result->ExitCode);
	}
	catch (const std::exception& Exception)
	{
		if (bJson)
		{
			ReportJsonDiagnostic("error", "internal_error", HostFailureExitCode, Exception.what());
		}
		else
		{
			ReportFailure(Exception.what());
		}
	}
	catch (...)
	{
		if (bJson)
		{
			ReportJsonDiagnostic("error", "internal_error", HostFailureExitCode, "An unknown failure crossed the headless editor boundary");
		}
		else
		{
			ReportFailure("An unknown failure crossed the headless editor boundary");
		}
	}
	return HostFailureExitCode;
}
