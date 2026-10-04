#include "Herta/EditorCore/SceneCommands.h"

#include "Herta/Scene/ScalingScene.h"
#include "Herta/Scene/SceneSerialization.h"

#include <charconv>
#include <format>
#include <system_error>

namespace Herta
{
namespace
{
std::filesystem::path Utf8Path(const std::string_view Text)
{
	return std::filesystem::path(std::u8string(Text.begin(), Text.end()));
}

std::expected<std::size_t, FEditorCommandError> ParseCubeCount(const std::string_view Text)
{
	std::size_t Count = 0;
	const auto [End, Error] = std::from_chars(Text.data(), Text.data() + Text.size(), Count);
	if (Error != std::errc{} || End != Text.data() + Text.size())
	{
		return std::unexpected(FEditorCommandError{.Message = "Cube count must be 1000, 5000, or 10000"});
	}

	return Count;
}
}

std::expected<void, FEditorCommandError> RegisterSceneFileCommands(FEditorCommandRegistry& Commands)
{
	if (auto Result = Commands.Register({.Name = "scene.validate", .Description = "Validate a scene file. Usage: scene.validate <path>", .Handler = [](const std::span<const std::string_view> Arguments) -> std::expected<FEditorCommandResult, FEditorCommandError>
	{
		if (Arguments.size() != 1)
		{
			return std::unexpected(FEditorCommandError{.Message = "Usage: scene.validate <path>"});
		}

		const auto Scene = LoadScene(Utf8Path(Arguments.front()));
		if (!Scene)
		{
			return std::unexpected(FEditorCommandError{.Message = Scene.error().Message});
		}

		return FEditorCommandResult{.Message = std::format("Scene '{}' is valid ({} entities)", Scene->Name, Scene->Entities.size())};
	}});
	    !Result)
	{
		return Result;
	}

	if (auto Result = Commands.Register({.Name = "scene.canonicalize", .Description = "Write a scene in canonical text. Usage: scene.canonicalize <source> <destination>", .Handler = [](const std::span<const std::string_view> Arguments) -> std::expected<FEditorCommandResult, FEditorCommandError>
	{
		if (Arguments.size() != 2)
		{
			return std::unexpected(FEditorCommandError{.Message = "Usage: scene.canonicalize <source> <destination>"});
		}

		const auto Scene = LoadScene(Utf8Path(Arguments[0]));
		if (!Scene)
		{
			return std::unexpected(FEditorCommandError{.Message = Scene.error().Message});
		}

		if (auto Result = SaveScene(Utf8Path(Arguments[1]), *Scene); !Result)
		{
			return std::unexpected(FEditorCommandError{.Message = Result.error().Message});
		}

		return FEditorCommandResult{.Message = "Canonical scene written"};
	}});
	    !Result)
	{
		return Result;
	}

	return Commands.Register({.Name = "scene.generate-scaling", .Description = "Generate a scaling fixture. Usage: scene.generate-scaling <rendering|dynamic> <1000|5000|10000> <destination>", .Handler = [](const std::span<const std::string_view> Arguments) -> std::expected<FEditorCommandResult, FEditorCommandError>
	{
		if (Arguments.size() != 3)
		{
			return std::unexpected(FEditorCommandError{.Message = "Usage: scene.generate-scaling <rendering|dynamic> <1000|5000|10000> <destination>"});
		}

		EScalingSceneWorkload Workload;
		if (Arguments[0] == "rendering")
		{
			Workload = EScalingSceneWorkload::Rendering;
		}
		else if (Arguments[0] == "dynamic")
		{
			Workload = EScalingSceneWorkload::DynamicBodies;
		}
		else
		{
			return std::unexpected(FEditorCommandError{.Message = "Scaling workload must be 'rendering' or 'dynamic'"});
		}

		const auto Count = ParseCubeCount(Arguments[1]);
		if (!Count)
		{
			return std::unexpected(Count.error());
		}

		const auto Scene = GenerateScalingScene({.CubeCount = *Count, .Workload = Workload});
		if (!Scene)
		{
			return std::unexpected(FEditorCommandError{.Message = Scene.error().Message});
		}

		if (auto Result = SaveScene(Utf8Path(Arguments[2]), *Scene); !Result)
		{
			return std::unexpected(FEditorCommandError{.Message = Result.error().Message});
		}

		return FEditorCommandResult{.Message = std::format("Generated {} scaling scene with {} cubes", Arguments[0], *Count)};
	}});
}
}
