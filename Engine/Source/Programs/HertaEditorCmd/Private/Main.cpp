#include "Herta/AssetPipeline/AssetCommands.h"
#include "Herta/EditorCore/CommandRegistry.h"
#include "Herta/Platform/Platform.h"
#include "Herta/Platform/Process.h"

#include <cstdint>
#include <exception>
#include <filesystem>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <vector>

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
[[nodiscard]] std::filesystem::path FindRepositoryRoot(const std::filesystem::path& ExecutablePath)
{
	std::error_code PathError;
	for (std::filesystem::path Start : {std::filesystem::current_path(PathError), ExecutablePath.parent_path()})
	{
		for (int Parent = 0; Parent < 8 && !Start.empty(); ++Parent)
		{
			if (std::filesystem::is_directory(Start / "Engine/Content", PathError))
			{
				return Start;
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

[[nodiscard]] std::string BuildCommandLine(const std::span<const std::string> Arguments)
{
	std::string CommandLine;
	for (const std::string& Argument : Arguments)
	{
		if (!CommandLine.empty())
		{
			CommandLine.push_back(' ');
		}

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

// Arguments exclude the executable name and are UTF-8 on every platform.
int Run(const std::span<const std::string> Arguments)
{
	const bool bJson = !Arguments.empty() && Arguments.front() == "--json";
	const std::span<const std::string> CommandArguments = Arguments.subspan(bJson ? 1 : 0);
	try
	{
		Herta::FEditorCommandRegistry Registry;
		std::expected<void, Herta::FEditorCommandError> RegistrationResult = Herta::RegisterCoreEditorCommands(Registry);
		if (RegistrationResult)
		{
			const std::filesystem::path ExecutablePath = Herta::GetExecutablePath();
			const std::filesystem::path RepositoryRoot = FindRepositoryRoot(ExecutablePath);
			const std::string_view Platform = Herta::GetPlatformName(Herta::GetCurrentPlatform());
			Herta::FAssetCommandOptions AssetOptions;
			if (!RepositoryRoot.empty())
			{
				AssetOptions.DefaultContentRoot = RepositoryRoot / "Games/Sandbox/Content";
				AssetOptions.DerivedDataRoot = RepositoryRoot / "DerivedDataCache" / Platform;
			}

			AssetOptions.WorkerPath = ExecutablePath.parent_path() / "HertaAssetWorker";
			AssetOptions.WorkerPath += ExecutablePath.extension();
			AssetOptions.TargetPlatform = Platform;
			RegistrationResult = Herta::RegisterAssetCommands(Registry, AssetOptions);
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

		const std::string CommandLine = CommandArguments.empty() ? "help" : BuildCommandLine(CommandArguments);
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
}

#ifdef HERTA_PLATFORM_WINDOWS
// Narrow main receives arguments in the ANSI code page, which cannot represent every content path.
int wmain(const int ArgumentCount, wchar_t** const Arguments)
{
	std::vector<std::string> Utf8Arguments;
	try
	{
		for (int Index = 1; Index < ArgumentCount; ++Index)
		{
			const std::u8string Text = std::filesystem::path(Arguments[Index]).u8string();
			Utf8Arguments.emplace_back(Text.begin(), Text.end());
		}
	}
	catch (...)
	{
		ReportFailure("Command-line arguments are not valid Unicode");
		return HostFailureExitCode;
	}

	return Run(Utf8Arguments);
}
#else
int main(const int ArgumentCount, char** const Arguments)
{
	try
	{
		return Run(std::vector<std::string>(Arguments + 1, Arguments + ArgumentCount));
	}
	catch (...)
	{
		ReportFailure("Could not read command-line arguments");
		return HostFailureExitCode;
	}
}
#endif
