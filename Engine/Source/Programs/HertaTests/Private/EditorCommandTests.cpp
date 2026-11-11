#include "Herta/EditorCore/CommandRegistry.h"

#include <doctest/doctest.h>

namespace Herta
{
TEST_CASE("Editor commands are validated and sorted")
{
	FEditorCommandRegistry Registry;
	CHECK(Registry.Register({.Name = "zeta", .Description = "Last command", .Handler = [](std::span<const std::string_view>)
	{
		return FEditorCommandResult{};
	}}).has_value());

	CHECK(Registry.Register({.Name = "alpha", .Description = "First command", .Handler = [](std::span<const std::string_view>)
	{
		return FEditorCommandResult{};
	}}).has_value());

	const std::vector<FEditorCommandInfo> Commands = Registry.List();
	REQUIRE(Commands.size() == 2);
	CHECK(Commands[0].Name == "alpha");
	CHECK(Commands[1].Name == "zeta");

	CHECK_FALSE(Registry.Register({.Name = "alpha", .Description = "Duplicate", .Handler = [](std::span<const std::string_view>)
	{
		return FEditorCommandResult{};
	}}).has_value());

	CHECK_FALSE(Registry.Register({.Name = "bad name", .Description = "Invalid", .Handler = [](std::span<const std::string_view>)
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
	CHECK(Registry.Register({.Name = "capture",
	                            .Description = "Capture arguments",
	                            .Handler = [](const std::span<const std::string_view> Arguments) -> std::expected<FEditorCommandResult, FEditorCommandError>
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
	CHECK(Registry.Register({.Name = "help", .Description = "Existing help", .Handler = [](std::span<const std::string_view>)
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

TEST_CASE("Callers without the right to change state cannot run commands that mutate")
{
	FEditorCommandRegistry Registry;
	int Runs = 0;
	const auto Count = [&Runs](std::span<const std::string_view>)
	{
		++Runs;
		return FEditorCommandResult{};
	};

	REQUIRE(Registry.Register({.Name = "thing.write", .Description = "Writes", .bMutates = true, .Handler = Count}));
	REQUIRE(Registry.Register({.Name = "thing.read", .Description = "Reads", .Handler = Count}));
	const std::vector<FEditorCommandInfo> Listed = Registry.List();
	REQUIRE(Listed.size() == 2);
	CHECK_FALSE(Listed[0].bMutates);
	CHECK(Listed[1].bMutates);

	// Refused before the handler runs; reads still work, and a permitted caller can write.
	const auto Refused = Registry.Execute("thing.write", false);
	REQUIRE_FALSE(Refused);
	CHECK(Refused.error().Code == EEditorCommandErrorCode::ReadOnly);
	CHECK(std::string(GetEditorCommandErrorCodeName(Refused.error().Code)) == "read_only");
	CHECK(Runs == 0);
	CHECK(Registry.Execute("thing.read", false));
	CHECK(Registry.Execute("thing.write"));
	CHECK(Runs == 2);
}
}
