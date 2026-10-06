#include "Herta/EditorCore/ProjectCommands.h"
#include "Herta/Project/Project.h"
#include "Herta/Scene/SceneSerialization.h"
#include "TestFiles.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <stop_token>

namespace Herta
{
namespace
{
std::filesystem::path EngineRoot()
{
	std::error_code Error;
	auto Path = std::filesystem::current_path(Error);

	while (!Path.empty())
	{
		if (std::filesystem::is_regular_file(Path / "Templates/Projects/Game/Template.json", Error))
		{
			return Path;
		}

		const auto Parent = Path.parent_path();
		if (Parent == Path)
		{
			break;
		}

		Path = Parent;
	}

	return {};
}

FProjectDescriptor MakeProject()
{
	return {.Id = "ec34e991-d3c7-41cb-aae7-d6659f062afa", .Name = "Test project", .EngineAssociation = "Herta", .Modules = {{.Name = "TestGame", .Source = "Source/TestGame", .Dependencies = {"Scene"}}}, .Targets = {{.Name = "TestGameEditor", .Type = "Editor", .Modules = {"TestGame"}}}, .ContentRoots = {{.Name = "Game", .Path = "Content"}}, .StartingScene = "Scenes/Main.hscene"};
}

std::string Replace(std::string Text, const std::string_view From, const std::string_view To)
{
	const auto Position = Text.find(From);
	REQUIRE(Position != std::string::npos);
	Text.replace(Position, From.size(), To);
	return Text;
}
}

TEST_CASE("Project JSON is strict, versioned, and canonical")
{
	const auto Text = SerializeProject(MakeProject());
	REQUIRE(Text);
	const auto Project = ParseProject(*Text);
	REQUIRE(Project);
	const auto Canonical = SerializeProject(*Project);
	REQUIRE(Canonical);
	CHECK(*Canonical == *Text);
	CHECK_FALSE(ParseProject(Replace(*Text, "\"formatVersion\": 1", "\"formatVersion\": 2")));
	CHECK_FALSE(ParseProject(Replace(*Text, "\"schemaVersion\": 1", "\"schemaVersion\": 2")));
	CHECK_FALSE(ParseProject(Replace(*Text, "\"features\": []", "\"features\": [\"Scripting\"]")));
	CHECK_FALSE(ParseProject(Replace(*Text, "\"name\": \"Test project\"", "\"name\": \"Test project\", \"name\": \"Duplicate\"")));
	CHECK_FALSE(ParseProject(Replace(*Text, "\"name\": \"Test project\"", "\"unknown\": 1, \"name\": \"Test project\"")));
	CHECK_FALSE(ParseProject(Replace(*Text, "Source/TestGame", "../TestGame")));
	CHECK_FALSE(ParseProject(Replace(*Text, "Scenes/Main.hscene", "C:/Main.hscene")));
	CHECK_FALSE(ParseProject(Replace(*Text, "\"Scene\"", "\"Unknown\"")));
	CHECK_FALSE(ParseProject(Replace(*Text, "\"modules\": [\"TestGame\"]", "\"modules\": [\"Missing\"]")));
	CHECK_FALSE(ParseProject(std::string("\xff", 1)));
}

TEST_CASE("Game project creation is transactional and produces a loadable source tree")
{
	const auto Root = EngineRoot();
	REQUIRE_FALSE(Root.empty());
	Tests::FScratchDirectory Scratch("herta-project");
	const auto Destination = Scratch.GetPath() / "Example";
	const FCreateProjectRequest Request{.TemplateRoot = Root / "Templates/Projects/Game", .Destination = Destination, .Name = "Example", .ModuleName = "ExampleGame"};
	const auto Project = CreateProject(Request);
	REQUIRE_MESSAGE(Project.has_value(), (Project ? "" : Project.error().Message));
	CHECK(Project->Descriptor.Name == "Example");
	std::error_code PathError;
	CHECK(std::filesystem::equivalent(Project->ContentRoot, Destination / "Content", PathError));
	CHECK_FALSE(PathError);
	CHECK(LoadScene(Project->StartingScene).has_value());
	CHECK(std::filesystem::is_regular_file(Destination / "Source/ExampleGame/Private/ExampleGameGame.cpp"));
	CHECK_FALSE(CreateProject(Request));
	CHECK(LoadProject(Destination / "Example.hertaproject").has_value());
	std::stop_source Cancellation;
	Cancellation.request_stop();
	auto Cancelled = Request;
	Cancelled.Destination = Scratch.GetPath() / "Cancelled";
	Cancelled.StopToken = Cancellation.get_token();
	CHECK_FALSE(CreateProject(Cancelled));
	CHECK_FALSE(std::filesystem::exists(Cancelled.Destination));
	auto Invalid = Request;
	Invalid.Destination = Scratch.GetPath() / "Invalid";
	Invalid.ModuleName = "../Escape";
	CHECK_FALSE(CreateProject(Invalid));
	CHECK_FALSE(std::filesystem::exists(Invalid.Destination));
	Invalid.ModuleName = "GLFW";
	CHECK_FALSE(CreateProject(Invalid));
	Invalid.ModuleName = "ExampleGame";
	Invalid.Name = "CON";
	CHECK_FALSE(CreateProject(Invalid));

	for (const auto& Entry : std::filesystem::directory_iterator(Scratch.GetPath()))
	{
		CHECK_FALSE(Entry.path().filename().string().starts_with(".herta-create-"));
	}
}

TEST_CASE("Project templates reject traversal, unknown substitutions, and duplicate output files")
{
	const auto Root = EngineRoot();
	REQUIRE_FALSE(Root.empty());
	Tests::FScratchDirectory Scratch("herta-project-invalid");
	const auto Template = Scratch.GetPath() / "Template";
	std::filesystem::copy(Root / "Templates/Projects/Game", Template, std::filesystem::copy_options::recursive);
	std::ifstream Input(Template / "Template.json", std::ios::binary);
	const std::string Manifest{std::istreambuf_iterator<char>(Input), std::istreambuf_iterator<char>()};
	Input.close();
	const FCreateProjectRequest Request{.TemplateRoot = Template, .Destination = Scratch.GetPath() / "Result", .Name = "Result", .ModuleName = "ResultGame"};

	for (const auto& Modified : {
	         Replace(Manifest, "\"destination\": \"{{ProjectName}}.hertaproject\"", "\"destination\": \"../escape.hertaproject\""),
	         Replace(Manifest, "{{ProjectName}}.hertaproject", "{{Unknown}}.hertaproject"),
	         Replace(Manifest, "\"destination\": \"README.md\"", "\"destination\": \"{{ProjectName}}.hertaproject\""),
	     })
	{
		Tests::WriteText(Template / "Template.json", Modified);
		CHECK_FALSE(CreateProject(Request));
		CHECK_FALSE(std::filesystem::exists(Request.Destination));
	}
}

TEST_CASE("Project headless commands share descriptor validation and creation")
{
	const auto Root = EngineRoot();
	REQUIRE_FALSE(Root.empty());
	FEditorCommandRegistry Registry;
	REQUIRE(RegisterProjectCommands(Registry, Root));
	CHECK(Registry.Execute("project.validate Games/Sandbox/Sandbox.hertaproject").has_value());
	CHECK_FALSE(Registry.Execute("project.create missing arguments"));
	CHECK_FALSE(Registry.Execute("project.validate missing.hertaproject"));
}
}
