#include "Herta/EditorCore/CommandRegistry.h"
#include "Herta/EditorFramework/OutputLog.h"

#include <array>
#include <doctest/doctest.h>

namespace Herta
{
namespace
{
[[nodiscard]] std::unique_ptr<FLogService> CreateOutputLogTestService()
{
	FLogOptions Options;
	Options.EditorBufferCapacity = 16;
	Options.bConsoleOutput = false;
	Options.bDebuggerOutput = false;
	Options.bFileOutput = false;
	std::expected<std::unique_ptr<FLogService>, FLogError> Log = FLogService::Create(std::move(Options));
	REQUIRE(Log.has_value());
	return std::move(*Log);
}
}

TEST_CASE("Output Log filters records and preserves severity overrides")
{
	std::unique_ptr<FLogService> Log = CreateOutputLogTestService();
	FEditorCommandRegistry Commands;
	REQUIRE(RegisterCoreEditorCommands(Commands).has_value());
	std::expected<std::unique_ptr<FOutputLogModel>, FOutputLogError> Model = FOutputLogModel::Create(*Log, Commands);
	REQUIRE(Model.has_value());
	constexpr FLogCategory Renderer{"Renderer"};
	Log->LogText(Renderer, ELogLevel::Info, "Created device");
	Log->LogText(Renderer, ELogLevel::Warning, "Fallback format");
	REQUIRE((*Model)->Synchronize().has_value());
	CHECK((*Model)->GetVisibleLines().size() == 2);
	CHECK(HasOutputLogLevelColorOverride((*Model)->GetVisibleLines()[1].Record.Level));

	REQUIRE((*Model)->SetSearch("device").has_value());
	REQUIRE((*Model)->GetVisibleLines().size() == 1);
	CHECK((*Model)->GetVisibleLines().front().Text.find("Created device") != std::string::npos);
	REQUIRE((*Model)->SetSearch({}).has_value());
	REQUIRE((*Model)->SetLevelVisible(ELogLevel::Info, false).has_value());
	REQUIRE((*Model)->GetVisibleLines().size() == 1);
	CHECK((*Model)->GetVisibleLines().front().Record.Level == ELogLevel::Warning);
}

TEST_CASE("Output Log selection copies continuously across lines")
{
	const std::array Lines = {std::string("first"), std::string("second"), std::string("third")};
	FLogTextSelection Selection;
	Selection.Begin({0, 2}, false);
	Selection.Update({2, 2});
	CHECK(Selection.Copy(Lines) == "rst\nsecond\nth");
	Selection.SelectAll(Lines);
	CHECK(Selection.Copy(Lines) == "first\nsecond\nthird");
}

TEST_CASE("Output Log expands multiline records into selectable display lines")
{
	std::unique_ptr<FLogService> Log = CreateOutputLogTestService();
	FEditorCommandRegistry Commands;
	REQUIRE(RegisterCoreEditorCommands(Commands).has_value());
	std::expected<std::unique_ptr<FOutputLogModel>, FOutputLogError> Model = FOutputLogModel::Create(*Log, Commands);
	REQUIRE(Model.has_value());
	constexpr FLogCategory Editor{"Editor"};
	Log->LogText(Editor, ELogLevel::Info, "first\nsecond");
	REQUIRE((*Model)->Synchronize().has_value());
	REQUIRE((*Model)->GetVisibleLines().size() == 2);
	CHECK((*Model)->GetVisibleLines()[0].Text.ends_with("first"));
	CHECK((*Model)->GetVisibleLines()[1].Text.ends_with("second"));
}

TEST_CASE("Output Log command submission captures results and history before tail acknowledgment")
{
	std::unique_ptr<FLogService> Log = CreateOutputLogTestService();
	FEditorCommandRegistry Commands;
	REQUIRE(RegisterCoreEditorCommands(Commands).has_value());
	std::expected<std::unique_ptr<FOutputLogModel>, FOutputLogError> Model = FOutputLogModel::Create(*Log, Commands);
	REQUIRE(Model.has_value());
	REQUIRE((*Model)->SubmitCommand("echo hello").has_value());
	CHECK((*Model)->HasTailRequest());
	REQUIRE((*Model)->Synchronize().has_value());
	REQUIRE((*Model)->GetVisibleLines().size() == 2);
	CHECK((*Model)->GetVisibleLines()[0].Text.ends_with("> echo hello"));
	CHECK((*Model)->GetVisibleLines()[1].Text.ends_with("hello"));
	std::expected<std::string, FOutputLogError> History = (*Model)->NavigateHistory(-1);
	REQUIRE(History.has_value());
	CHECK(*History == "echo hello");
	(*Model)->AcknowledgeTailRequest();
	CHECK_FALSE((*Model)->HasTailRequest());
}

TEST_CASE("Output Log scroll policy follows only an owned tail")
{
	CHECK(ShouldScrollOutputLog(true, true, true, false));
	CHECK_FALSE(ShouldScrollOutputLog(true, true, false, false));
	CHECK_FALSE(ShouldScrollOutputLog(true, false, true, false));
	CHECK(ShouldScrollOutputLog(false, false, false, true));
}

TEST_CASE("Output Log command trimming uses the fixed ASCII grammar")
{
	std::unique_ptr<FLogService> Log = CreateOutputLogTestService();
	FEditorCommandRegistry Commands;
	REQUIRE(RegisterCoreEditorCommands(Commands).has_value());
	std::expected<std::unique_ptr<FOutputLogModel>, FOutputLogError> Model = FOutputLogModel::Create(*Log, Commands);
	REQUIRE(Model.has_value());
	REQUIRE((*Model)->SubmitCommand("\t\v echo ascii \f\r\n").has_value());
	std::expected<std::string, FOutputLogError> History = (*Model)->NavigateHistory(-1);
	REQUIRE(History.has_value());
	CHECK(*History == "echo ascii");
}
}
