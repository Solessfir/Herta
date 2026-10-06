#include "Herta/EditorCore/ProjectCommands.h"

#include "Herta/Project/Project.h"

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

std::expected<void, FEditorCommandError> RegisterProjectCommands(FEditorCommandRegistry& Registry, const std::filesystem::path& EngineRoot)
{
	if (const auto Result = Registry.Register({.Name = "project.validate", .Description = "Validate a project. Usage: project.validate <descriptor>", .Handler = [](const std::span<const std::string_view> Arguments) -> std::expected<FEditorCommandResult, FEditorCommandError>
	{
		if (Arguments.size() != 1)
		{
			return std::unexpected(FEditorCommandError{.Message = "Usage: project.validate <descriptor>"});
		}

		const auto Project = LoadProject(Utf8Path(Arguments[0]));
		if (!Project)
		{
			return std::unexpected(FEditorCommandError{.Message = Project.error().Message});
		}

		return FEditorCommandResult{.Message = std::format("Project '{}' is valid", Project->Descriptor.Name)};
	}});
	    !Result)
	{
		return Result;
	}

	return Registry.Register({.Name = "project.create", .Description = "Create a Game project. Usage: project.create <name> <module> <destination>", .Handler = [EngineRoot](const std::span<const std::string_view> Arguments) -> std::expected<FEditorCommandResult, FEditorCommandError>
	{
		if (Arguments.size() != 3)
		{
			return std::unexpected(FEditorCommandError{.Message = "Usage: project.create <name> <module> <destination>"});
		}

		const auto Project = CreateProject({.TemplateRoot = EngineRoot / "Templates/Projects/Game", .Destination = Utf8Path(Arguments[2]), .Name = std::string(Arguments[0]), .ModuleName = std::string(Arguments[1])});
		if (!Project)
		{
			return std::unexpected(FEditorCommandError{.Message = Project.error().Message});
		}

		const auto Path = Project->DescriptorPath.generic_u8string();
		return FEditorCommandResult{.Message = "Created project: " + std::string(Path.begin(), Path.end())};
	}});
}
}
