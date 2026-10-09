#include "Herta/EditorCore/CommandRegistry.h"

#include <doctest/doctest.h>

namespace Herta
{
TEST_CASE("Editor commands are validated and sorted")
{
	FEditorCommandRegistry Registry;
	CHECK(Registry.Register({"zeta", "Last command", [](std::span<const std::string_view>)
	{
		return FEditorCommandResult{};
	}}).has_value());

	CHECK(Registry.Register({"alpha", "First command", [](std::span<const std::string_view>)
	{
		return FEditorCommandResult{};
	}}).has_value());

	const std::vector<FEditorCommandInfo> Commands = Registry.List();
	REQUIRE(Commands.size() == 2);
	CHECK(Commands[0].Name == "alpha");
	CHECK(Commands[1].Name == "zeta");

	CHECK_FALSE(Registry.Register({"alpha", "Duplicate", [](std::span<const std::string_view>)
	{
		return FEditorCommandResult{};
	}}).has_value());

	CHECK_FALSE(Registry.Register({"bad name", "Invalid", [](std::span<const std::string_view>)
	{
		return FEditorCommandResult{};
	}}).has_value());
}

TEST_CASE("Editor command parsing preserves quoted and escaped arguments")
{
	FEditorCommandRegistry Registry;
	CHECK(RegisterCoreEditorCommands(Registry).has_value());

	const std::expected<FEditorCommandResult, FEditorCommandError> Result = Registry.Execute(R"(echo "hello world" escaped\ value)");

	REQUIRE(Result.has_value());
	CHECK(Result->Message == "hello world escaped value");
	CHECK_FALSE(Registry.Execute(R"(echo "unterminated)").has_value());
}

TEST_CASE("Editor command parsing preserves empty quoted arguments")
{
	FEditorCommandRegistry Registry;
	CHECK(Registry.Register({"capture",
	                            "Capture arguments",
	                            [](const std::span<const std::string_view> Arguments) -> std::expected<FEditorCommandResult, FEditorCommandError>
	{
		if (Arguments.size() != 2 || !Arguments.front().empty() || Arguments.back() != "tail")
		{
			return std::unexpected(FEditorCommandError{.Code = EEditorCommandErrorCode::ExecutionFailed, .Message = "Empty quoted argument was not preserved"});
		}

		return FEditorCommandResult{};
	}})
	          .has_value());

	CHECK(Registry.Execute(R"(capture "" tail)").has_value());
}

TEST_CASE("Editor command completion is prefix based and bounded")
{
	FEditorCommandRegistry Registry;
	CHECK(RegisterCoreEditorCommands(Registry).has_value());

	const std::vector<std::string> Result = Registry.Complete("e", 1);
	REQUIRE(Result.size() == 1);
	CHECK(Result.front() == "echo");
	CHECK(Registry.Complete("", 0).empty());
}

TEST_CASE("Core editor command registration is transactional")
{
	FEditorCommandRegistry Registry;
	CHECK(Registry.Register({"help", "Existing help", [](std::span<const std::string_view>)
	{
		return FEditorCommandResult{};
	}}).has_value());

	const std::expected<void, FEditorCommandError> Result = RegisterCoreEditorCommands(Registry);
	REQUIRE_FALSE(Result.has_value());
	CHECK_FALSE(Registry.Execute("echo partial registration").has_value());

	const std::vector<FEditorCommandInfo> Commands = Registry.List();
	REQUIRE(Commands.size() == 1);
	CHECK(Commands.front().Name == "help");
}

TEST_CASE("Core editor help and unknown command diagnostics share one registry")
{
	FEditorCommandRegistry Registry;
	CHECK(RegisterCoreEditorCommands(Registry).has_value());

	const std::expected<FEditorCommandResult, FEditorCommandError> Help = Registry.Execute("help");
	REQUIRE(Help.has_value());
	CHECK(Help->Message.find("echo") != std::string::npos);
	CHECK(Help->Message.find("help") != std::string::npos);

	const std::expected<FEditorCommandResult, FEditorCommandError> Unknown = Registry.Execute("missing");
	REQUIRE_FALSE(Unknown.has_value());
	CHECK(Unknown.error().Code == EEditorCommandErrorCode::UnknownCommand);
	CHECK(std::string(GetEditorCommandErrorCodeName(Unknown.error().Code)) == "unknown_command");
	CHECK(Unknown.error().Message.find("Unknown command") != std::string::npos);
}
}
