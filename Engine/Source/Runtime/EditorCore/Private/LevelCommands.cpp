#include "Herta/EditorCore/LevelCommands.h"

#include "Herta/Level/LevelSerialization.h"
#include "Herta/Level/ScalingLevel.h"

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

std::expected<void, FEditorCommandError> RegisterLevelFileCommands(FEditorCommandRegistry& Commands)
{
	if (auto Result = Commands.Register({.Name = "level.validate", .Description = "Validate a level file. Usage: level.validate <path>", .Handler = [](const std::span<const std::string_view> Arguments) -> std::expected<FEditorCommandResult, FEditorCommandError>
	{
		if (Arguments.size() != 1)
		{
			return std::unexpected(FEditorCommandError{.Message = "Usage: level.validate <path>"});
		}

		const auto Level = LoadLevel(Utf8Path(Arguments.front()));
		if (!Level)
		{
			return std::unexpected(FEditorCommandError{.Message = Level.error().Message});
		}

		return FEditorCommandResult{.Message = std::format("Level '{}' is valid ({} entities)", Level->Name, Level->Entities.size())};
	}});
	    !Result)
	{
		return Result;
	}

	if (auto Result = Commands.Register({.Name = "level.canonicalize", .Description = "Write a level in canonical text. Usage: level.canonicalize <source> <destination>", .Handler = [](const std::span<const std::string_view> Arguments) -> std::expected<FEditorCommandResult, FEditorCommandError>
	{
		if (Arguments.size() != 2)
		{
			return std::unexpected(FEditorCommandError{.Message = "Usage: level.canonicalize <source> <destination>"});
		}

		const auto Level = LoadLevel(Utf8Path(Arguments[0]));
		if (!Level)
		{
			return std::unexpected(FEditorCommandError{.Message = Level.error().Message});
		}

		if (auto Result = SaveLevel(Utf8Path(Arguments[1]), *Level); !Result)
		{
			return std::unexpected(FEditorCommandError{.Message = Result.error().Message});
		}

		return FEditorCommandResult{.Message = "Canonical level written"};
	}});
	    !Result)
	{
		return Result;
	}

	return Commands.Register({.Name = "level.generate-scaling", .Description = "Generate a scaling fixture. Usage: level.generate-scaling <rendering|dynamic> <1000|5000|10000> <destination>", .Handler = [](const std::span<const std::string_view> Arguments) -> std::expected<FEditorCommandResult, FEditorCommandError>
	{
		if (Arguments.size() != 3)
		{
			return std::unexpected(FEditorCommandError{.Message = "Usage: level.generate-scaling <rendering|dynamic> <1000|5000|10000> <destination>"});
		}

		EScalingLevelWorkload Workload;
		if (Arguments[0] == "rendering")
		{
			Workload = EScalingLevelWorkload::Rendering;
		}
		else if (Arguments[0] == "dynamic")
		{
			Workload = EScalingLevelWorkload::DynamicBodies;
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

		const auto Level = GenerateScalingLevel({.CubeCount = *Count, .Workload = Workload});
		if (!Level)
		{
			return std::unexpected(FEditorCommandError{.Message = Level.error().Message});
		}

		if (auto Result = SaveLevel(Utf8Path(Arguments[2]), *Level); !Result)
		{
			return std::unexpected(FEditorCommandError{.Message = Result.error().Message});
		}

		return FEditorCommandResult{.Message = std::format("Generated {} scaling level with {} cubes", Arguments[0], *Count)};
	}});
}
}
