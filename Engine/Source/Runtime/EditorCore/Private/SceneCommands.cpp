#include "Herta/EditorCore/SceneCommands.h"

#include "Herta/Scene/SceneSerialization.h"

#include <format>

namespace Herta
{
namespace
{
std::filesystem::path Utf8Path(const std::string_view Text)
{
	return std::filesystem::path(std::u8string(Text.begin(), Text.end()));
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

	return Commands.Register({.Name = "scene.canonicalize", .Description = "Write a scene in canonical text. Usage: scene.canonicalize <source> <destination>", .Handler = [](const std::span<const std::string_view> Arguments) -> std::expected<FEditorCommandResult, FEditorCommandError>
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
}
}
