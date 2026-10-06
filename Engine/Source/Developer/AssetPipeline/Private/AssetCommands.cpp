#include "Herta/AssetPipeline/AssetCommands.h"

#include "FileUtilities.h"
#include "Herta/AssetPipeline/AssetCooker.h"
#include "Herta/AssetPipeline/ContentRoot.h"

#include <format>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Herta
{
namespace
{
struct FParsedArguments
{
	std::filesystem::path ContentRoot;
	std::string Destination;
	bool bForce = false;
	std::vector<std::string_view> Positional;
};

[[nodiscard]] std::expected<FParsedArguments, FEditorCommandError> ParseArguments(const std::span<const std::string_view> Arguments, const FAssetCommandOptions& Options, const bool bAllowDestination, const bool bAllowForce = false)
{
	FParsedArguments Parsed;
	Parsed.ContentRoot = Options.DefaultContentRoot;
	for (std::size_t Index = 0; Index < Arguments.size(); ++Index)
	{
		const std::string_view Argument = Arguments[Index];
		if (!Argument.starts_with("--"))
		{
			Parsed.Positional.push_back(Argument);
			continue;
		}

		if (bAllowForce && Argument == "--force")
		{
			Parsed.bForce = true;
			continue;
		}

		const bool bContentRoot = Argument == "--content-root";
		const bool bDestination = bAllowDestination && Argument == "--destination";
		if ((!bContentRoot && !bDestination) || Index + 1 == Arguments.size())
		{
			return std::unexpected(FEditorCommandError{.Code = EEditorCommandErrorCode::ParseError, .Message = std::format("Unknown option or missing value for '{}'", Argument)});
		}

		const std::string_view Value = Arguments[++Index];
		if (bContentRoot)
		{
			Parsed.ContentRoot = Utf8ToPath(Value);
		}
		else
		{
			Parsed.Destination = Value;
		}
	}

	if (Parsed.ContentRoot.empty())
	{
		return std::unexpected(FEditorCommandError{.Code = EEditorCommandErrorCode::ParseError, .Message = "No content root. Pass --content-root <path>"});
	}

	return Parsed;
}

[[nodiscard]] std::unexpected<FEditorCommandError> ExecutionError(FAssetError Error)
{
	return std::unexpected(FEditorCommandError{.Code = EEditorCommandErrorCode::ExecutionFailed, .Message = std::move(Error.Message)});
}

[[nodiscard]] std::unexpected<FEditorCommandError> UsageError(const std::string_view Usage)
{
	return std::unexpected(FEditorCommandError{.Code = EEditorCommandErrorCode::ParseError, .Message = std::format("Usage: {}", Usage)});
}

[[nodiscard]] std::expected<std::string, FAssetError> Cook(const FAssetCommandOptions& Options, const std::filesystem::path& ContentRoot, const std::string& SourcePath, const bool bForce)
{
	if (Options.WorkerPath.empty() || Options.DerivedDataRoot.empty() || Options.TargetPlatform.empty())
	{
		return std::unexpected(FAssetError{"Cooking requires an asset worker, derived data root, and target platform"});
	}

	std::expected<FAssetCookResult, FAssetError> Result = CookAssetInWorker({.ContentRoot = ContentRoot, .DerivedDataRoot = Options.DerivedDataRoot, .SourcePath = SourcePath, .TargetPlatform = Options.TargetPlatform, .bForce = bForce, .DependencyContentRoots = Options.DependencyContentRoots, .EngineContentRoot = Options.EngineContentRoot, .GameContentRoot = Options.GameContentRoot.empty() ? ContentRoot : Options.GameContentRoot}, {.WorkerPath = Options.WorkerPath});
	if (!Result)
	{
		return std::unexpected(std::move(Result.error()));
	}

	std::string Message = std::format("Cooked {} -> {} ({})", SourcePath, ToString(Result->Key), Result->bCacheHit ? "cache hit" : "cooked");
	for (const std::string& Warning : Result->Warnings)
	{
		Message.append(std::format("\nwarning: {}", Warning));
	}

	return Message;
}
}

std::expected<void, FEditorCommandError> RegisterAssetCommands(FEditorCommandRegistry& Registry, const FAssetCommandOptions& Options)
{
	std::expected<void, FEditorCommandError> Result = Registry.Register(FEditorCommandDescriptor{
	    .Name = "asset.validate",
	    .Description = "Validate content metadata and report unregistered sources",
	    .Handler = [Options](const std::span<const std::string_view> Arguments) -> std::expected<FEditorCommandResult, FEditorCommandError>
	{
		std::expected<FParsedArguments, FEditorCommandError> Parsed = ParseArguments(Arguments, Options, false);
		if (!Parsed)
		{
			return std::unexpected(std::move(Parsed.error()));
		}

		if (!Parsed->Positional.empty())
		{
			return UsageError("asset.validate [--content-root <path>]");
		}

		std::expected<FContentScanResult, FAssetError> Scan = ScanContentRoot(Parsed->ContentRoot);
		if (!Scan)
		{
			return ExecutionError(std::move(Scan.error()));
		}

		std::string Message;
		for (const FContentDiagnostic& Diagnostic : Scan->Errors)
		{
			Message.append(std::format("error: {}: {}\n", Diagnostic.Path, Diagnostic.Message));
		}

		for (const std::string& Source : Scan->UnregisteredSources)
		{
			Message.append(std::format("warning: {}: no metadata, run asset.import\n", Source));
		}

		Message.append(std::format("{} assets, {} errors, {} unregistered", Scan->Registry.GetRecords().size(), Scan->Errors.size(), Scan->UnregisteredSources.size()));
		return FEditorCommandResult{.ExitCode = static_cast<std::uint8_t>(Scan->Errors.empty() ? 0 : 1), .Message = std::move(Message)};
	}});

	if (Result)
	{
		Result = Registry.Register(FEditorCommandDescriptor{
		    .Name = "asset.list",
		    .Description = "List registered content assets",
		    .Handler = [Options](const std::span<const std::string_view> Arguments) -> std::expected<FEditorCommandResult, FEditorCommandError>
		{
			std::expected<FParsedArguments, FEditorCommandError> Parsed = ParseArguments(Arguments, Options, false);
			if (!Parsed)
			{
				return std::unexpected(std::move(Parsed.error()));
			}

			if (!Parsed->Positional.empty())
			{
				return UsageError("asset.list [--content-root <path>]");
			}

			std::expected<FContentScanResult, FAssetError> Scan = ScanContentRoot(Parsed->ContentRoot);
			if (!Scan)
			{
				return ExecutionError(std::move(Scan.error()));
			}

			std::string Message;
			for (const FAssetRecord& Record : Scan->Registry.GetRecords())
			{
				Message.append(std::format("{} {:<8} {}\n", Record.Id.ToString(), Record.Importer, Record.SourcePath));
			}

			if (!Message.empty())
			{
				Message.pop_back();
			}

			return FEditorCommandResult{.ExitCode = 0, .Message = std::move(Message)};
		}});
	}

	if (Result)
	{
		Result = Registry.Register(FEditorCommandDescriptor{
		    .Name = "asset.import",
		    .Description = "Register a source file as an asset, copying it into content when needed",
		    .Handler = [Options](const std::span<const std::string_view> Arguments) -> std::expected<FEditorCommandResult, FEditorCommandError>
		{
			std::expected<FParsedArguments, FEditorCommandError> Parsed = ParseArguments(Arguments, Options, true);
			if (!Parsed)
			{
				return std::unexpected(std::move(Parsed.error()));
			}

			if (Parsed->Positional.size() != 1)
			{
				return UsageError("asset.import <source> [--destination <content-directory>] [--content-root <path>]");
			}

			std::expected<FImportedSource, FAssetError> Imported = ImportSource(Parsed->ContentRoot, Utf8ToPath(Parsed->Positional.front()), Parsed->Destination);
			if (!Imported)
			{
				return ExecutionError(std::move(Imported.error()));
			}

			const std::string Registered = std::format("Imported {} as {} ({})", Imported->SourcePath, Imported->Metadata.Id.ToString(), Imported->Metadata.Importer);
			std::expected<std::string, FAssetError> Cooked = Cook(Options, Parsed->ContentRoot, Imported->SourcePath, false);
			if (!Cooked)
			{
				return ExecutionError(FAssetError{std::format("{}, but cooking failed. Fix the source and run asset.reimport.\n{}", Registered, Cooked.error().Message)});
			}

			return FEditorCommandResult{.ExitCode = 0, .Message = std::format("{}\n{}", Registered, *Cooked)};
		}});
	}

	if (Result)
	{
		Result = Registry.Register(FEditorCommandDescriptor{
		    .Name = "asset.reimport",
		    .Description = "Cook a registered asset by content path or ID",
		    .Handler = [Options](const std::span<const std::string_view> Arguments) -> std::expected<FEditorCommandResult, FEditorCommandError>
		{
			std::expected<FParsedArguments, FEditorCommandError> Parsed = ParseArguments(Arguments, Options, false, true);
			if (!Parsed)
			{
				return std::unexpected(std::move(Parsed.error()));
			}

			if (Parsed->Positional.size() != 1)
			{
				return UsageError("asset.reimport <content-path|asset-id> [--force] [--content-root <path>]");
			}

			std::string SourcePath(Parsed->Positional.front());
			if (const std::optional<FAssetId> Id = FAssetId::Parse(SourcePath))
			{
				std::expected<FContentScanResult, FAssetError> Scan = ScanContentRoot(Parsed->ContentRoot);
				if (!Scan)
				{
					return ExecutionError(std::move(Scan.error()));
				}

				const FAssetRecord* Record = Scan->Registry.Find(*Id);
				if (!Record)
				{
					return ExecutionError(FAssetError{std::format("No registered asset has ID {}", SourcePath)});
				}

				SourcePath = Record->SourcePath;
			}

			std::expected<std::string, FAssetError> Cooked = Cook(Options, Parsed->ContentRoot, SourcePath, Parsed->bForce);
			if (!Cooked)
			{
				return ExecutionError(std::move(Cooked.error()));
			}

			return FEditorCommandResult{.ExitCode = 0, .Message = std::move(*Cooked)};
		}});
	}

	return Result;
}
}
