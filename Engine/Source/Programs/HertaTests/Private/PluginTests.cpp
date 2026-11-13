#include "Herta/EditorCore/EditorPlugin.h"
#include "Herta/EditorCore/Plugins.h"
#include "Herta/Platform/DynamicLibrary.h"
#include "TestFiles.h"

// The first-party plugins' parsers are header-only, so their tests build them here without loading the plugins.
#include "../../../../Plugins/Git/Source/Private/GitStatus.h"
#include "../../../../Plugins/Terminal/Source/Private/TerminalText.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <format>
#include <string>
#include <vector>

using namespace Herta;

namespace
{
void WritePlugin(const std::filesystem::path& Root, const std::string_view Name, const std::string_view Dependencies, const bool bEnabled = true)
{
	Tests::WriteText(Root / Name / std::format("{}.hplugin", Name), std::format(R"({{"format": "HertaPlugin", "formatVersion": 1, "name": "{}", "dependencies": [{}], "enabledByDefault": {}}})", Name, Dependencies, bEnabled ? "true" : "false"));
}

[[nodiscard]] std::vector<std::string> Names(const FPluginResolution& Resolution)
{
	std::vector<std::string> Result;
	Result.reserve(Resolution.Load.size());
	for (const FPluginDescriptor* const Plugin : Resolution.Load)
	{
		Result.push_back(Plugin->Name);
	}

	return Result;
}

[[nodiscard]] bool Mentions(const std::vector<FPluginDiagnostic>& Diagnostics, const std::string_view Plugin, const std::string_view Text)
{
	return std::ranges::any_of(Diagnostics, [&](const FPluginDiagnostic& Diagnostic)
	{
		return Diagnostic.Plugin == Plugin && Diagnostic.Message.find(Text) != std::string::npos;
	});
}

class FRecordingHost final : public IEditorPluginHost
{
public:
	void Log(EEditorPluginLog, const std::string_view Message) override
	{
		Logged.emplace_back(Message);
	}

	[[nodiscard]] std::string GetProjectDirectory() const override
	{
		return {};
	}

	[[nodiscard]] std::string GetEngineDirectory() const override
	{
		return {};
	}

	void RunProcess(FEditorPluginProcess, std::function<void(const FEditorPluginProcessResult&)>) override
	{
	}

	std::vector<std::string> Logged;
};

class FRecordingRegistry final : public IEditorPluginRegistry
{
public:
	IEditorPluginHost& GetHost() override
	{
		return Host;
	}

	void AddPanel(const std::string_view Title, std::unique_ptr<IEditorPanel> Panel) override
	{
		Panels.emplace_back(Title, std::move(Panel));
	}

	void AddMenuItem(const std::string_view Label, std::function<void()> OnSelected) override
	{
		MenuItems.emplace_back(Label, std::move(OnSelected));
	}

	void AddCommand(const std::string_view Name, std::string_view, bool, FEditorPluginCommand Run) override
	{
		Commands.emplace_back(Name, std::move(Run));
	}

	void AddStatusText(std::function<std::string()> GetText) override
	{
		StatusTexts.push_back(std::move(GetText));
	}

	FRecordingHost Host;
	std::vector<std::pair<std::string, std::unique_ptr<IEditorPanel>>> Panels;
	std::vector<std::pair<std::string, std::function<void()>>> MenuItems;
	std::vector<std::pair<std::string, FEditorPluginCommand>> Commands;
	std::vector<std::function<std::string()>> StatusTexts;
};
}

TEST_CASE("Plugin descriptors name their plugin and module, and malformed ones are refused")
{
	const std::filesystem::path File = "Plugins/Git/Git.hplugin";
	const auto Plugin = ParsePluginDescriptor(R"({"format": "HertaPlugin", "formatVersion": 1, "name": "Git", "module": "HertaGit", "dependencies": ["Core_Tools"], "enabledByDefault": true})", File);
	REQUIRE(Plugin);
	CHECK(Plugin->Name == "Git");
	CHECK(Plugin->FriendlyName == "Git");
	CHECK(Plugin->Module == "HertaGit");
	CHECK(Plugin->Dependencies == std::vector<std::string>{"Core_Tools"});
	CHECK(Plugin->bEnabledByDefault);
	CHECK(Plugin->Directory == File.parent_path());

	CHECK_FALSE(ParsePluginDescriptor(R"({"format": "HertaPlugin", "formatVersion": 2, "name": "Git"})", File));
	CHECK_FALSE(ParsePluginDescriptor(R"({"format": "HertaPlugin", "formatVersion": 1, "name": "Other"})", File));
	CHECK_FALSE(ParsePluginDescriptor(R"({"format": "HertaPlugin", "formatVersion": 1, "name": "Git", "module": "../Evil"})", File));
	CHECK_FALSE(ParsePluginDescriptor(R"({"format": "HertaPlugin", "formatVersion": 1, "name": "Git", "dependencies": "Core"})", File));
	CHECK_FALSE(ParsePluginDescriptor("not json", File));
}

TEST_CASE("Plugins load after their dependencies, and missing, disabled, or cyclic dependencies keep a plugin out with a reason")
{
	const Tests::FScratchDirectory Scratch("Plugins");
	const std::filesystem::path Engine = Scratch.GetPath() / "Engine";
	const std::filesystem::path Project = Scratch.GetPath() / "Project";
	WritePlugin(Engine, "Base", "");
	WritePlugin(Engine, "Off", "", false);
	WritePlugin(Project, "Tools", R"("Base")");
	WritePlugin(Project, "Needy", R"("Missing")");
	WritePlugin(Project, "Lazy", R"("Off")");
	WritePlugin(Project, "Ping", R"("Pong")");
	WritePlugin(Project, "Pong", R"("Ping")");
	// A project plugin cannot replace an engine one.
	WritePlugin(Project, "Base", "");
	Tests::WriteText(Project / "Broken" / "Broken.hplugin", "{");

	const std::array Roots = {FPluginRoot{.Directory = Engine, .Source = EPluginSource::Engine}, FPluginRoot{.Directory = Project, .Source = EPluginSource::Project}};
	const FPluginCatalog Catalog = DiscoverPlugins(Roots);
	CHECK(Catalog.Plugins.size() == 7);
	CHECK(std::ranges::find(Catalog.Plugins, "Base", &FPluginDescriptor::Name)->Source == EPluginSource::Engine);
	CHECK(Mentions(Catalog.Diagnostics, "Base", "already"));
	CHECK(Mentions(Catalog.Diagnostics, "Broken", "JSON"));

	const FPluginResolution Resolution = ResolvePlugins(Catalog, {});
	CHECK(Names(Resolution) == std::vector<std::string>{"Base", "Tools"});
	CHECK(Mentions(Resolution.Diagnostics, "Needy", "Missing is not installed"));
	CHECK(Mentions(Resolution.Diagnostics, "Lazy", "Off is disabled"));
	CHECK(Mentions(Resolution.Diagnostics, "Ping", "cycle"));

	// Overrides turn defaults around, and a dependency enabled by one lets its users load.
	const FPluginOverrides Overrides{{"Off", true}, {"Tools", false}};
	CHECK(Names(ResolvePlugins(Catalog, Overrides)) == std::vector<std::string>{"Base", "Off", "Lazy"});

	const std::filesystem::path OverridesPath = Scratch.GetPath() / "Settings" / "Plugins.txt";
	CHECK(LoadPluginOverrides(OverridesPath) == FPluginOverrides{});
	REQUIRE(SavePluginOverrides(OverridesPath, Overrides));
	CHECK(LoadPluginOverrides(OverridesPath) == Overrides);
	Tests::WriteText(OverridesPath, "Off=maybe\n");
	CHECK_FALSE(LoadPluginOverrides(OverridesPath));
}

TEST_CASE("Editor plugins built from the public header register panels, menu items, and commands")
{
	const std::filesystem::path Library = Tests::GetSiblingExecutable("HertaTestPlugin").parent_path() / GetDynamicLibraryFileName("HertaTestPlugin");
	REQUIRE_MESSAGE(std::filesystem::exists(Library), Library.string());

	FRecordingRegistry Registry;
	const auto Loaded = LoadEditorPlugin(Library, Registry);
	REQUIRE_MESSAGE(Loaded, (Loaded ? std::string() : Loaded.error()));
	REQUIRE(Registry.Panels.size() == 1);
	CHECK(Registry.Panels[0].first == "Greeting");
	REQUIRE(Registry.MenuItems.size() == 1);
	Registry.MenuItems[0].second();
	CHECK(Registry.Host.Logged == std::vector<std::string>{"Hello"});

	REQUIRE(Registry.Commands.size() == 1);
	CHECK(Registry.Commands[0].first == "test.greet");
	const std::array<std::string_view, 1> Arguments{"world"};
	const FEditorPluginCommandResult Result = Registry.Commands[0].second(Arguments);
	CHECK(Result.bSucceeded);
	CHECK(Result.Message == "Hello world");

	CHECK_FALSE(LoadEditorPlugin(Library.parent_path() / GetDynamicLibraryFileName("HertaNoSuchPlugin"), Registry));
}

TEST_CASE("Git porcelain v2 status gives the branch, its distance from upstream, and each change with renames")
{
	using namespace std::string_view_literals;
	const std::string_view Output = "# branch.oid 0123456789abcdef\0# branch.head main\0# branch.upstream origin/main\0# branch.ab +2 -1\0"
	                                "1 .M N... 100644 100644 100644 aaaa bbbb Docs/With Space.md\0"
	                                "2 R. N... 100644 100644 100644 aaaa bbbb R100 New.cpp\0Old.cpp\0"
	                                "u UU N... 100644 100644 100644 100644 aaaa bbbb cccc Conflict.h\0"
	                                "? Untracked.txt\0"sv;

	const FGitStatus Status = ParseGitStatus(Output);
	CHECK(Status.Branch == "main");
	CHECK(Status.Upstream == "origin/main");
	CHECK(Status.Ahead == 2);
	CHECK(Status.Behind == 1);
	REQUIRE(Status.Changes.size() == 4);
	CHECK(Status.Changes[0].Path == "Docs/With Space.md");
	CHECK(Status.Changes[0].Unstaged == 'M');
	CHECK(Status.Changes[1].Path == "New.cpp");
	CHECK(Status.Changes[1].OriginalPath == "Old.cpp");
	CHECK(Status.Changes[1].Staged == 'R');
	CHECK(Status.Changes[2].Path == "Conflict.h");
	CHECK(Status.Changes[2].Staged == 'U');
	CHECK(Status.Changes[3].Path == "Untracked.txt");
	CHECK(Status.Changes[3].Unstaged == '?');
}

TEST_CASE("Terminal text drops escape sequences and follows carriage returns, backspaces, and line erases")
{
	FTerminalText Text(3);
	Text.Append("\x1b]0;Title\x07\x1b[?25l\x1b[32mgreen\x1b[0m\r\n");
	Text.Append("progress 10%\rprogress 99%\r\n");
	Text.Append("abc\b\bX\x1b[K\n");
	Text.Append("caf\xc3\xa9\b!");
	REQUIRE(Text.GetLines().size() == 3);
	// The oldest line went when a fourth arrived.
	CHECK(Text.GetLines()[0] == "progress 99%");
	CHECK(Text.GetLines()[1] == "aX");
	CHECK(Text.GetLines()[2] == "caf!");

	Text.Append("\x1b[2J");
	CHECK(Text.GetLines().size() == 1);
	CHECK(Text.GetLines()[0].empty());
}
