#include "Herta/AssetPipeline/AssetCommands.h"
#include "Herta/AnimationGraph/AnimationGraph.h"
#include "Herta/AssetPipeline/AssetMetadata.h"
#include "Herta/AudioGraph/AudioGraph.h"
#include "Herta/StateTreeCompiler/StateTreeCompiler.h"
#include "Herta/VfxGraph/VfxGraph.h"

#include "FileUtilities.h"
#include "Herta/AssetPipeline/AssetCooker.h"
#include "Herta/AssetPipeline/ContentRoot.h"
#include "Herta/AssetPipeline/GameStaging.h"
#include "Herta/AssetPipeline/MaterialShaderCompiler.h"
#include "Herta/AssetPipeline/NavigationCook.h"
#include "Herta/Assets/CookedContent.h"
#include "Herta/Level/LevelSerialization.h"
#include "Herta/Project/Project.h"

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

	if (Result)
	{
		Result = Registry.Register(FEditorCommandDescriptor{
		    .Name = "material.compile",
		    .Description = "Compile a material shader or Material Graph to the cooked shader stages a staged game loads",
		    .Handler = [Options](const std::span<const std::string_view> Arguments) -> std::expected<FEditorCommandResult, FEditorCommandError>
		{
			std::expected<FParsedArguments, FEditorCommandError> Parsed = ParseArguments(Arguments, Options, false);
			if (!Parsed)
			{
				return std::unexpected(std::move(Parsed.error()));
			}

			const auto Usage = "material.compile <Game/... or Engine/... shader[#Switch=1,...]> <output-directory> [--content-root <path>]";
			if (Parsed->Positional.size() != 2 || !(Parsed->Positional[0].starts_with("Game/") || Parsed->Positional[0].starts_with("Engine/")))
			{
				return UsageError(Usage);
			}

			const std::string Shader(Parsed->Positional[0]);
			if (const auto Key = ParseMaterialShaderKey(Shader); !Key || !IsValidAssetPath(Key->first))
			{
				return UsageError(Usage);
			}

			const std::array Roots{Options.EngineContentRoot.parent_path() / "Shaders", Options.EngineContentRoot, Parsed->ContentRoot};
			const auto Sources = CollectMaterialShaderSources(Roots, Shader);
			if (!Sources)
			{
				return ExecutionError(Sources.error());
			}

			std::filesystem::path Worker = Options.WorkerPath;
			Worker.replace_filename(Options.WorkerPath.has_extension() ? "HertaShaderWorker" + PathToUtf8(Options.WorkerPath.extension()) : "HertaShaderWorker");
			const auto Stages = CompileMaterialShader(Worker, *Sources, HashMaterialShaderSources(*Sources), false, {});
			if (!Stages)
			{
				return ExecutionError(Stages.error());
			}

			const std::filesystem::path Output = Utf8ToPath(Parsed->Positional[1]);
			const std::string_view Relative = std::string_view(Shader).substr(Shader.find('/') + 1);
			std::string Message = std::format("Compiled {}", Shader);
			for (std::size_t Stage = 0; Stage < Stages->size(); ++Stage)
			{
				const std::filesystem::path Path = GetStagedMaterialShaderPath(Output, Relative, MaterialShaderStageNames[Stage]);
				if (auto Written = WriteFileAtomically(Path, (*Stages)[Stage]); !Written)
				{
					return ExecutionError(Written.error());
				}

				Message += std::format("\n{} ({} bytes)", GenericPathToUtf8(Path), (*Stages)[Stage].size());
			}

			const std::string Table = SerializeMaterialShaderParameters(Sources->Parameters);
			if (auto Written = WriteFileAtomically(GetStagedMaterialShaderParametersPath(Output, Relative), std::as_bytes(std::span(Table))); !Written)
			{
				return ExecutionError(Written.error());
			}

			Message += std::format("\n{} parameters", Sources->Parameters.size());

			return FEditorCommandResult{.ExitCode = 0, .Message = std::move(Message)};
		}});
	}

	if (Result)
	{
		Result = Registry.Register(FEditorCommandDescriptor{
		    .Name = "animation.compile",
		    .Description = "Compile an animation graph to check it, listing what the state machine holds or what stops it compiling",
		    .Handler = [](const std::span<const std::string_view> Arguments) -> std::expected<FEditorCommandResult, FEditorCommandError>
		{
			if (Arguments.size() != 1 || !Arguments[0].ends_with(AnimationGraphExtension))
			{
				return UsageError("animation.compile <graph.hanimgraph>");
			}

			const auto Bytes = ReadWholeFile(Utf8ToPath(Arguments[0]), 64 * 1024 * 1024);
			if (!Bytes || !*Bytes)
			{
				return ExecutionError(Bytes ? FAssetError{std::format("{} does not exist", Arguments[0])} : Bytes.error());
			}

			const auto Graph = DeserializeGraph(std::string_view(reinterpret_cast<const char*>((*Bytes)->data()), (*Bytes)->size()), GetAnimationGraphSchema());
			if (!Graph)
			{
				return ExecutionError(FAssetError{Graph.error().Message});
			}

			const auto Compiled = CompileAnimationGraph(*Graph);
			if (!Compiled)
			{
				std::string Message = std::format("{} does not compile", Arguments[0]);
				for (const FGraphDiagnostic& Diagnostic : Compiled.error())
				{
					Message += std::format("\nnode {}: {}", Diagnostic.Node.Value, Diagnostic.Message);
				}

				return ExecutionError(FAssetError{std::move(Message)});
			}

			const FAnimationStateMachineDesc& Machine = Compiled->StateMachine;
			return FEditorCommandResult{.ExitCode = 0, .Message = std::format("Compiled {}: {} parameters, {} states, {} transitions, starting in {}", Arguments[0], Machine.Parameters.size(), Machine.States.size(), Machine.Transitions.size(), Machine.States[Machine.Entry].Name)};
		}});
	}

	if (Result)
	{
		Result = Registry.Register(FEditorCommandDescriptor{
		    .Name = "audio.compile",
		    .Description = "Compile an Audio Graph to check it, listing its inputs and waves or what stops it compiling",
		    .Handler = [](const std::span<const std::string_view> Arguments) -> std::expected<FEditorCommandResult, FEditorCommandError>
		{
			if (Arguments.size() != 1 || !Arguments[0].ends_with(AudioGraphExtension))
			{
				return UsageError("audio.compile <graph.haudiograph>");
			}

			const std::filesystem::path Path = Utf8ToPath(Arguments[0]);
			const auto Bytes = ReadWholeFile(Path, 64 * 1024 * 1024);
			if (!Bytes || !*Bytes)
			{
				return ExecutionError(Bytes ? FAssetError{std::format("{} does not exist", Arguments[0])} : Bytes.error());
			}

			const auto Graph = DeserializeGraph(std::string_view(reinterpret_cast<const char*>((*Bytes)->data()), (*Bytes)->size()), GetAudioGraphSchema());
			if (!Graph)
			{
				return ExecutionError(FAssetError{Graph.error().Message});
			}

			// Waves are content paths; outside a project the content root is whichever folder above the graph holds them.
			const auto Resolve = [&Path](const std::string_view Wave) -> std::optional<FAssetId>
			{
				for (std::filesystem::path Root = std::filesystem::absolute(Path).parent_path(); !Root.empty(); Root = Root.parent_path())
				{
					if (auto Metadata = LoadAssetMetadata(Root / Utf8ToPath(Wave)); Metadata && Metadata->Importer == "Sound")
					{
						return Metadata->Id;
					}

					if (Root == Root.parent_path())
					{
						break;
					}
				}

				return std::nullopt;
			};

			const auto Compiled = CompileAudioGraph(*Graph, Resolve);
			if (!Compiled)
			{
				std::string Message = std::format("{} does not compile", Arguments[0]);
				for (const FGraphDiagnostic& Diagnostic : Compiled.error())
				{
					Message += std::format("\nnode {}: {}", Diagnostic.Node.Value, Diagnostic.Message);
				}

				return ExecutionError(FAssetError{std::move(Message)});
			}

			std::string Inputs;
			for (const FSoundParameterDesc& Parameter : Compiled->Graph.Parameters)
			{
				Inputs += (Inputs.empty() ? "" : ", ") + Parameter.Name;
			}

			return FEditorCommandResult{.ExitCode = 0, .Message = std::format("Compiled {}: {} nodes, {} waves, inputs: {}", Arguments[0], Compiled->Graph.Nodes.size(), GetSoundGraphWaves(Compiled->Graph).size(), Inputs.empty() ? "none" : Inputs)};
		}});
	}

	if (Result)
	{
		Result = Registry.Register(FEditorCommandDescriptor{
		    .Name = "statetree.compile",
		    .Description = "Compile a StateTree to check it, listing its states or what stops it compiling",
		    .Handler = [](const std::span<const std::string_view> Arguments) -> std::expected<FEditorCommandResult, FEditorCommandError>
		{
			if (Arguments.size() != 1 || !Arguments[0].ends_with(StateTreeExtension))
			{
				return UsageError("statetree.compile <tree.hstatetree>");
			}

			const auto Bytes = ReadWholeFile(Utf8ToPath(Arguments[0]), 64 * 1024 * 1024);
			if (!Bytes || !*Bytes)
			{
				return ExecutionError(Bytes ? FAssetError{std::format("{} does not exist", Arguments[0])} : Bytes.error());
			}

			const auto Graph = DeserializeGraph(std::string_view(reinterpret_cast<const char*>((*Bytes)->data()), (*Bytes)->size()), GetStateTreeSchema());
			if (!Graph)
			{
				return ExecutionError(FAssetError{Graph.error().Message});
			}

			const auto Compiled = CompileStateTree(*Graph);
			if (!Compiled)
			{
				std::string Message = std::format("{} does not compile", Arguments[0]);
				for (const FGraphDiagnostic& Diagnostic : Compiled.error())
				{
					Message += std::format(" (node {}: {})", Diagnostic.Node.Value, Diagnostic.Message);
				}

				return ExecutionError(FAssetError{std::move(Message)});
			}

			std::string States;
			for (const FStateTreeStateDesc& State : Compiled->Tree.States)
			{
				States += (States.empty() ? "" : ", ") + State.Name;
			}

			std::string Warnings;
			for (const FGraphDiagnostic& Diagnostic : Compiled->Diagnostics)
			{
				Warnings += std::format("; warning at node {}: {}", Diagnostic.Node.Value, Diagnostic.Message);
			}

			return FEditorCommandResult{.ExitCode = 0, .Message = std::format("Compiled {}: {} states ({}), {} tasks, {} transitions{}", Arguments[0], Compiled->Tree.States.size(), States, Compiled->Tree.Tasks.size(), Compiled->Tree.Transitions.size(), Warnings)};
		}});
	}

	if (Result)
	{
		Result = Registry.Register(FEditorCommandDescriptor{
		    .Name = "nav.build",
		    .Description = "Build a project level's navigation, with its regions' tiles, into a .hnav file",
		    .Handler = [Options](const std::span<const std::string_view> Arguments) -> std::expected<FEditorCommandResult, FEditorCommandError>
		{
			if (Arguments.size() < 2 || Arguments.size() > 3)
			{
				return UsageError("nav.build <project.hertaproject> <output.hnav> [<level.hlevel>]");
			}

			const auto Project = LoadProject(Utf8ToPath(Arguments[0]));
			if (!Project)
			{
				return ExecutionError(FAssetError{std::format("Cannot load project: {}", Project.error().Message)});
			}

			const std::filesystem::path LevelPath = Arguments.size() == 3 ? Utf8ToPath(Arguments[2]) : Project->StartingLevel;
			const auto Level = LoadLevel(LevelPath);
			if (!Level)
			{
				return ExecutionError(FAssetError{std::format("Cannot load {}: {}", GenericPathToUtf8(LevelPath), Level.error().Message)});
			}

			const auto Navigation = CookLevelNavigation(*Level, {
			                                                        .LevelPath = LevelPath,
			                                                        .EngineContentRoot = Options.EngineContentRoot,
			                                                        .GameContentRoot = Project->ContentRoot,
			                                                        .DerivedDataRoot = Options.DerivedDataRoot,
			                                                        .WorkerPath = Options.WorkerPath,
			                                                        .TargetPlatform = Options.TargetPlatform,
			                                                    });
			if (!Navigation)
			{
				return ExecutionError(Navigation.error());
			}

			if (!*Navigation)
			{
				return ExecutionError(FAssetError{std::format("{} and its regions have no Navigation Bounds", GenericPathToUtf8(LevelPath))});
			}

			if (auto Written = WriteFileAtomically(Utf8ToPath(Arguments[1]), SerializeNavigationFile(**Navigation)); !Written)
			{
				return ExecutionError(Written.error());
			}

			std::size_t PatchLayers = 0;
			for (const FNavigationPatch& Patch : (*Navigation)->Patches)
			{
				PatchLayers += Patch.Data.Layers.size();
			}

			return FEditorCommandResult{.ExitCode = 0, .Message = std::format("Built navigation for {}: {} level layers and {} region patches with {} layers", GenericPathToUtf8(LevelPath), (*Navigation)->Base.Layers.size(), (*Navigation)->Patches.size(), PatchLayers)};
		}});
	}

	if (Result)
	{
		Result = Registry.Register(FEditorCommandDescriptor{
		    .Name = "vfx.compile",
		    .Description = "Compile a VFX Graph to check it, listing its emitters or what stops it compiling",
		    .Handler = [](const std::span<const std::string_view> Arguments) -> std::expected<FEditorCommandResult, FEditorCommandError>
		{
			if (Arguments.size() != 1 || !Arguments[0].ends_with(VfxGraphExtension))
			{
				return UsageError("vfx.compile <graph.hvfxgraph>");
			}

			const auto Bytes = ReadWholeFile(Utf8ToPath(Arguments[0]), 64 * 1024 * 1024);
			if (!Bytes || !*Bytes)
			{
				return ExecutionError(Bytes ? FAssetError{std::format("{} does not exist", Arguments[0])} : Bytes.error());
			}

			const auto Graph = DeserializeGraph(std::string_view(reinterpret_cast<const char*>((*Bytes)->data()), (*Bytes)->size()), GetVfxGraphSchema());
			if (!Graph)
			{
				return ExecutionError(FAssetError{Graph.error().Message});
			}

			const auto Compiled = CompileVfxGraph(*Graph);
			if (!Compiled)
			{
				std::string Message = std::format("{} does not compile", Arguments[0]);
				for (const FGraphDiagnostic& Diagnostic : Compiled.error())
				{
					Message += std::format(" (node {}: {})", Diagnostic.Node.Value, Diagnostic.Message);
				}

				return ExecutionError(FAssetError{std::move(Message)});
			}

			std::string Emitters;
			std::uint32_t Particles = 0;
			for (const FParticleEmitterDesc& Emitter : Compiled->System.Emitters)
			{
				Emitters += (Emitters.empty() ? "" : ", ") + Emitter.Name;
				Particles += Emitter.MaximumParticles;
			}

			return FEditorCommandResult{.ExitCode = 0, .Message = std::format("Compiled {}: emitters {}, up to {} particles", Arguments[0], Emitters, Particles)};
		}});
	}

	if (Result)
	{
		Result = Registry.Register(FEditorCommandDescriptor{
		    .Name = "game.stage",
		    .Description = "Cook a project's starting level and its assets into a relocatable loose-content directory",
		    .Handler = [Options](const std::span<const std::string_view> Arguments) -> std::expected<FEditorCommandResult, FEditorCommandError>
		{
			if (Arguments.size() != 2)
			{
				return UsageError("game.stage <project.hertaproject> <output-directory>");
			}

			const auto Staged = StageGameContent({
			    .ProjectPath = Utf8ToPath(Arguments[0]),
			    .OutputRoot = Utf8ToPath(Arguments[1]),
			    .EngineContentRoot = Options.EngineContentRoot,
			    .EngineShaderRoot = Options.EngineContentRoot.parent_path() / "Shaders",
			    .DerivedDataRoot = Options.DerivedDataRoot,
			    .WorkerPath = Options.WorkerPath,
			    .TargetPlatform = Options.TargetPlatform,
			});

			if (!Staged)
			{
				return ExecutionError(Staged.error());
			}

			return FEditorCommandResult{.ExitCode = 0, .Message = std::format("Staged {} assets ({:.1f} MiB) for {}", Staged->Assets, static_cast<double>(Staged->Bytes) / (1024. * 1024.), GenericPathToUtf8(Staged->ProjectPath))};
		}});
	}

	return Result;
}
}
