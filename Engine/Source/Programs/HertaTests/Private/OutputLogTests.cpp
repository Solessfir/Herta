#include "Herta/EditorCore/CommandRegistry.h"
#include "Herta/EditorFramework/OutputLog.h"
#include "OutputLogTextLayout.h"

#include <doctest/doctest.h>
#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <ctime>
#include <format>
#include <limits>
#include <string>
#include <string_view>

namespace Herta
{
namespace
{
struct FOutputLogLayoutTestContext
{
	ImGuiContext* PreviousContext = ImGui::GetCurrentContext();
	ImGuiContext* Context = ImGui::CreateContext();
	bool bRobotoLoaded = false;

	FOutputLogLayoutTestContext()
	{
		ImGui::SetCurrentContext(Context);
		ImGuiIO& Io = ImGui::GetIO();
		Io.DisplaySize = {640.f, 480.f};
		Io.DisplayFramebufferScale = {1.f, 1.f};
		Io.DeltaTime = 1.f / 60.f;
		Io.IniFilename = nullptr;
		ImFontConfig FontConfig;
		FontConfig.PixelSnapH = false;
		FontConfig.RasterizerDensity = 1.25f;
		bRobotoLoaded = Io.Fonts->AddFontFromFileTTF("Engine/Content/Editor/Fonts/Roboto/Roboto-Regular.ttf", 15.f, &FontConfig) != nullptr;
		if (!bRobotoLoaded)
		{
			Io.Fonts->AddFontDefault();
		}

		ImGui::GetStyle().FontScaleMain = 1.13f;
		unsigned char* Pixels = nullptr;
		int Width = 0;
		int Height = 0;
		Io.Fonts->GetTexDataAsRGBA32(&Pixels, &Width, &Height);
		ImGui::NewFrame();
		ImGui::SetNextWindowPos({0.f, 0.f}, ImGuiCond_Always);
		ImGui::SetNextWindowSize(Io.DisplaySize, ImGuiCond_Always);
		ImGui::Begin("Output Log layout test", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings);
	}

	~FOutputLogLayoutTestContext()
	{
		ImGui::End();
		ImGui::Render();
		ImGui::DestroyContext(Context);
		ImGui::SetCurrentContext(PreviousContext);
	}

	FOutputLogLayoutTestContext(const FOutputLogLayoutTestContext&) = delete;
	FOutputLogLayoutTestContext& operator=(const FOutputLogLayoutTestContext&) = delete;
	FOutputLogLayoutTestContext(FOutputLogLayoutTestContext&&) = delete;
	FOutputLogLayoutTestContext& operator=(FOutputLogLayoutTestContext&&) = delete;
};

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

void CheckOutputLogHitRoundTrips(const FOutputLogLine& Line, const FOutputLogColumns Columns)
{
	const std::string_view Text = Line.Text;
	for (std::size_t ByteOffset = 0; ByteOffset < Text.size();)
	{
		const std::size_t NextByteOffset = GetNextOutputLogUtf8Boundary(Text, ByteOffset);
		const float Left = MeasureOutputLogTextPrefix(Line, ByteOffset, Columns);
		const float Right = MeasureOutputLogTextPrefix(Line, NextByteOffset, Columns);
		CHECK(Right > Left);
		CHECK(FindOutputLogByteAtX(Line, Left + (Right - Left) * 0.25f, Columns) == ByteOffset);
		CHECK(FindOutputLogByteAtX(Line, Left + (Right - Left) * 0.75f, Columns) == NextByteOffset);
		ByteOffset = NextByteOffset;
	}
}
}

TEST_CASE("Output Log filters records and preserves severity overrides")
{
	std::unique_ptr<FLogService> Log = CreateOutputLogTestService();
	FEditorCommandRegistry Commands;
	REQUIRE(RegisterCoreEditorCommands(Commands).has_value());
	std::expected<std::unique_ptr<FOutputLogModel>, FOutputLogError> Model = FOutputLogModel::Create(*Log, Commands);
	REQUIRE(Model.has_value());
	constexpr FLogCategory Renderer{.Name = "Renderer"};
	Log->LogText(Renderer, ELogLevel::Info, "Created device");
	Log->LogText(Renderer, ELogLevel::Warning, "Fallback format");
	(*Model)->Synchronize();
	CHECK((*Model)->GetVisibleLines().size() == 2);
	CHECK((*Model)->GetVisibleLines()[0].Text.find("Renderer        Created device") != std::string::npos);
	CHECK((*Model)->GetVisibleLines()[0].Text.find("[Info]") == std::string::npos);
	CHECK(HasOutputLogLevelColorOverride((*Model)->GetVisibleLines()[1].Record.Level));

	(*Model)->SetSearch("device");
	REQUIRE((*Model)->GetVisibleLines().size() == 1);
	CHECK((*Model)->GetVisibleLines().front().Text.find("Created device") != std::string::npos);
	(*Model)->SetSearch({});
	(*Model)->SetLevelVisible(ELogLevel::Info, false);
	REQUIRE((*Model)->GetVisibleLines().size() == 1);
	CHECK((*Model)->GetVisibleLines().front().Record.Level == ELogLevel::Warning);
}

TEST_CASE("Output Log selection copies continuously across lines")
{
	const std::array Lines = {std::string("first"), std::string("second"), std::string("third")};
	FLogTextSelection Selection;
	Selection.Begin({.Line = 0, .Byte = 2}, false);
	Selection.Update({.Line = 2, .Byte = 2});
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
	constexpr FLogCategory Editor{.Name = "Editor"};
	Log->LogText(Editor, ELogLevel::Info, "first\nsecond");
	(*Model)->Synchronize();
	REQUIRE((*Model)->GetVisibleLines().size() == 2);
	CHECK((*Model)->GetVisibleLines()[0].Text.ends_with("first"));
	CHECK((*Model)->GetVisibleLines()[1].Text.ends_with("second"));
}

TEST_CASE("Output Log exposes byte offsets for padded and long category columns")
{
	std::unique_ptr<FLogService> Log = CreateOutputLogTestService();
	FEditorCommandRegistry Commands;
	REQUIRE(RegisterCoreEditorCommands(Commands).has_value());
	std::expected<std::unique_ptr<FOutputLogModel>, FOutputLogError> Model = FOutputLogModel::Create(*Log, Commands);
	REQUIRE(Model.has_value());
	constexpr FLogCategory ShortCategory{.Name = "Renderer"};
	constexpr FLogCategory LongUtf8Category{.Name = "渲染BackendLongCategoryName🌙"};
	Log->LogText(ShortCategory, ELogLevel::Info, "short message");
	Log->LogText(LongUtf8Category, ELogLevel::Info, "long category message");
	(*Model)->Synchronize();
	REQUIRE((*Model)->GetVisibleLines().size() == 2);
	for (const FOutputLogLine& Line : (*Model)->GetVisibleLines())
	{
		const auto Timestamp = std::chrono::system_clock::to_time_t(Line.Record.Timestamp);
		std::tm LocalTime{};
#ifdef _WIN32
		REQUIRE(localtime_s(&LocalTime, &Timestamp) == 0);
#else
		REQUIRE(localtime_r(&Timestamp, &LocalTime) != nullptr);
#endif
		const std::string FormattedTime = std::format("{:02}:{:02}:{:02}", LocalTime.tm_hour, LocalTime.tm_min, LocalTime.tm_sec);
		CHECK(FormattedTime.size() == 8);
		CHECK(Line.TimeEnd == FormattedTime.size());
		CHECK(Line.CategoryBegin == Line.TimeEnd + 2);
		CHECK(Line.CategoryEnd == Line.CategoryBegin + Line.Record.Category.size());
		CHECK(Line.MessageBegin == std::format("{}  {:<14}  ", FormattedTime, Line.Record.Category).size());
		CHECK(std::string_view(Line.Text).substr(0, Line.TimeEnd) == FormattedTime);
		CHECK(std::string_view(Line.Text).substr(Line.CategoryBegin, Line.Record.Category.size()) == Line.Record.Category);
		CHECK(std::string_view(Line.Text).substr(Line.MessageBegin) == Line.Record.Message);
	}
	CHECK((*Model)->GetVisibleLines()[1].MessageBegin > (*Model)->GetVisibleLines()[1].CategoryEnd);
}

TEST_CASE("Output Log multiline continuation keeps byte offsets for aligned blank columns")
{
	std::unique_ptr<FLogService> Log = CreateOutputLogTestService();
	FEditorCommandRegistry Commands;
	REQUIRE(RegisterCoreEditorCommands(Commands).has_value());
	std::expected<std::unique_ptr<FOutputLogModel>, FOutputLogError> Model = FOutputLogModel::Create(*Log, Commands);
	REQUIRE(Model.has_value());
	constexpr FLogCategory Category{.Name = "Editor"};
	Log->LogText(Category, ELogLevel::Info, "first\nsecond");
	(*Model)->Synchronize();
	REQUIRE((*Model)->GetVisibleLines().size() == 2);
	const FOutputLogLine& FirstLine = (*Model)->GetVisibleLines()[0];
	const FOutputLogLine& Continuation = (*Model)->GetVisibleLines()[1];
	CHECK(Continuation.CategoryBegin == FirstLine.CategoryBegin);
	CHECK(Continuation.CategoryEnd == FirstLine.CategoryEnd);
	CHECK(Continuation.MessageBegin == FirstLine.MessageBegin);
	CHECK(Continuation.TimeEnd == FirstLine.TimeEnd);
	CHECK(std::string_view(Continuation.Text).substr(0, Continuation.MessageBegin).find_first_not_of(' ') == std::string_view::npos);
	CHECK(std::string_view(Continuation.Text).substr(Continuation.MessageBegin) == "second");
}

TEST_CASE("Output Log text layout aligns RHI and Editor columns and round-trips UTF-8 byte hits")
{
	FOutputLogLayoutTestContext ImGuiContext;
	CHECK(ImGuiContext.bRobotoLoaded);
	constexpr char FontProbe[] = "Wi";
	const float FontProbeWidth = ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), std::numeric_limits<float>::max(), 0.f, FontProbe, FontProbe + 2).x;
	CHECK(std::abs(FontProbeWidth - std::round(FontProbeWidth)) > 0.01f);
	std::unique_ptr<FLogService> Log = CreateOutputLogTestService();
	FEditorCommandRegistry Commands;
	REQUIRE(RegisterCoreEditorCommands(Commands).has_value());
	std::expected<std::unique_ptr<FOutputLogModel>, FOutputLogError> Model = FOutputLogModel::Create(*Log, Commands);
	REQUIRE(Model.has_value());
	constexpr FLogCategory RhiCategory{.Name = "RHI"};
	constexpr FLogCategory EditorCategory{.Name = "Editor"};
	Log->LogText(RhiCategory, ELogLevel::Info, "device α😊 ready");
	Log->LogText(EditorCategory, ELogLevel::Info, "preview λ🧪 updated\ncontinuation 🌙");
	(*Model)->Synchronize();
	REQUIRE((*Model)->GetVisibleLines().size() == 3);
	const FOutputLogColumns Columns{.CategoryX = 90.f, .MessageX = 310.f};
	for (const FOutputLogLine& Line : (*Model)->GetVisibleLines())
	{
		CHECK(MeasureOutputLogTextPrefix(Line, Line.CategoryBegin, Columns) == doctest::Approx(Columns.CategoryX));
		CHECK(MeasureOutputLogTextPrefix(Line, Line.MessageBegin, Columns) == doctest::Approx(Columns.MessageX));
		CheckOutputLogHitRoundTrips(Line, Columns);
	}

	CHECK((*Model)->GetVisibleLines()[2].Text.substr((*Model)->GetVisibleLines()[2].MessageBegin) == "continuation 🌙");
	FLogRecord Record;
	Record.ElapsedSeconds = 1234.567;
	Record.Category = "渲染BackendLongCategoryName🌙";
	Record.Message = "long prefix α🧪";
	const std::string Time = std::format("{:7.3f}", Record.ElapsedSeconds);
	const std::string Category = std::format("{:<14}", Record.Category);
	const std::size_t CategoryBegin = Time.size() + 2;
	const std::size_t CategoryEnd = CategoryBegin + Record.Category.size();
	const std::size_t MessageBegin = CategoryBegin + Category.size() + 2;
	FOutputLogLine Line{.Record = Record, .Text = std::format("{}  {}  {}", Time, Category, Record.Message), .CategoryBegin = CategoryBegin, .CategoryEnd = CategoryEnd, .MessageBegin = MessageBegin, .TimeEnd = Time.size()};
	FOutputLogLine Continuation{.Record = Record, .Text = std::string(MessageBegin, ' ') + "second 🌌", .CategoryBegin = CategoryBegin, .CategoryEnd = CategoryEnd, .MessageBegin = MessageBegin, .TimeEnd = Time.size()};
	const FOutputLogColumns LongColumns{.CategoryX = 135.f, .MessageX = 850.f};
	CHECK(Line.TimeEnd > std::format("{:7.3f}", 1.0).size());
	CHECK(Line.CategoryEnd - Line.CategoryBegin > 14);
	for (const FOutputLogLine* const Current : {&Line, &Continuation})
	{
		CHECK(MeasureOutputLogTextPrefix(*Current, Current->CategoryBegin, LongColumns) == doctest::Approx(LongColumns.CategoryX));
		CHECK(MeasureOutputLogTextPrefix(*Current, Current->MessageBegin, LongColumns) == doctest::Approx(LongColumns.MessageX));
		CheckOutputLogHitRoundTrips(*Current, LongColumns);
	}
}

TEST_CASE("Output Log sends lines starting with ! to the shell runner instead of the command registry")
{
	std::unique_ptr<FLogService> Log = CreateOutputLogTestService();
	FEditorCommandRegistry Commands;
	REQUIRE(RegisterCoreEditorCommands(Commands).has_value());
	std::expected<std::unique_ptr<FOutputLogModel>, FOutputLogError> Model = FOutputLogModel::Create(*Log, Commands);
	REQUIRE(Model.has_value());

	// Without a runner, the line is rejected instead of reaching the registry as an unknown command.
	REQUIRE((*Model)->SubmitCommand("!git status").has_value());
	(*Model)->Synchronize();
	CHECK((*Model)->GetVisibleLines().back().Text.ends_with("Shell commands are not available here"));

	std::vector<std::string> Ran;
	(*Model)->SetShellRunner([&Ran](std::string Command)
	{
		Ran.push_back(std::move(Command));
	});

	REQUIRE((*Model)->SubmitCommand("  !  git commit -m \"Fixed tabs\"  ").has_value());
	REQUIRE((*Model)->SubmitCommand("!").has_value());
	REQUIRE(Ran.size() == 1);
	CHECK(Ran[0] == "git commit -m \"Fixed tabs\"");
	(*Model)->Synchronize();
	CHECK((*Model)->GetVisibleLines().back().Text.ends_with("Type a shell command after !"));
	CHECK((*Model)->NavigateHistory(-1) == "!");
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
	(*Model)->Synchronize();
	REQUIRE((*Model)->GetVisibleLines().size() == 2);
	CHECK((*Model)->GetVisibleLines()[0].Text.ends_with("> echo hello"));
	CHECK((*Model)->GetVisibleLines()[1].Text.ends_with("hello"));
	CHECK((*Model)->NavigateHistory(-1) == "echo hello");
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
	CHECK((*Model)->NavigateHistory(-1) == "echo ascii");
}
}
