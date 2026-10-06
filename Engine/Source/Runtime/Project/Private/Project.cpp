#include "Herta/Project/Project.h"

#include "Herta/Assets/AssetId.h"
#include "Herta/Level/LevelSerialization.h"

#include <simdjson.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <set>
#include <system_error>

#ifdef _WIN32
	#include <Windows.h>
#else
	#include <fcntl.h>
	#include <sys/syscall.h>
	#include <unistd.h>
#endif

namespace Herta
{
namespace
{
constexpr std::size_t MaximumFileBytes = 1024 * 1024;

std::unexpected<FProjectError> ProjectError(const std::string_view Message)
{
	return std::unexpected(FProjectError{std::string(Message)});
}

std::filesystem::path Utf8Path(const std::string_view Text)
{
	return std::filesystem::path(std::u8string(Text.begin(), Text.end()));
}

bool ValidText(const std::string_view Text)
{
	return !Text.empty() && Text.size() <= 128 && simdjson::validate_utf8(Text.data(), Text.size()) && std::ranges::none_of(Text, [](const unsigned char Byte)
	{
		return Byte < 0x20 || Byte == 0x7f;
	});
}

bool Identifier(const std::string_view Text)
{
	const auto Letter = [](const char Byte)
	{
		return (Byte >= 'a' && Byte <= 'z') || (Byte >= 'A' && Byte <= 'Z');
	};
	return !Text.empty() && Text.size() <= 64 && Letter(Text.front()) && std::ranges::all_of(Text, [&](const char Byte)
	{
		return Letter(Byte) || (Byte >= '0' && Byte <= '9') || Byte == '_';
	});
}

std::string Lowercase(std::string Text)
{
	std::ranges::transform(Text, Text.begin(), [](const char Byte)
	{
		return Byte >= 'A' && Byte <= 'Z' ? static_cast<char>(Byte - 'A' + 'a') : Byte;
	});
	return Text;
}

bool PortableFilename(const std::string_view Text)
{
	if (Text.empty() || Text == "." || Text == ".." || Text.back() == '.' || Text.back() == ' ')
	{
		return false;
	}

	const auto Base = Lowercase(std::string(Text.substr(0, Text.find('.'))));
	return Base != "con" && Base != "prn" && Base != "aux" && Base != "nul" && !(Base.size() == 4 && (Base.starts_with("com") || Base.starts_with("lpt")) && Base.back() >= '1' && Base.back() <= '9');
}

bool ModuleName(const std::string_view Text)
{
	constexpr std::array<std::string_view, 36> EngineModules{"Core", "Math", "Assets", "Level", "Project", "Physics", "Platform", "Tasks", "EditorCore", "Application", "ToolUI", "AssetPipeline", "EditorFramework", "RHI", "RenderGraph", "Renderer", "NvrhiVulkan", "ShaderCompiler", "HertaEditor", "HertaEditorCmd", "HertaTests", "HertaAssetWorker", "HertaShaderWorker", "HertaShaders", "Spdlog", "SimdJson", "NVRHI", "NVRHIVulkanBackend", "MeshOptimizer", "Jolt", "FreeType", "FastGltf", "GLFW", "EnkiTS", "ImGui", "Im3d"};
	return Identifier(Text) && PortableFilename(Text) && std::ranges::none_of(EngineModules, [&](const std::string_view Name)
	{
		return Lowercase(std::string(Name)) == Lowercase(std::string(Text));
	});
}

bool RelativePath(const std::string_view Text)
{
	if (!ValidText(Text) || Text.find_first_of("\\:*?\"<>|") != std::string_view::npos || Text.find("//") != std::string_view::npos || Text.front() == '/' || Text.back() == '/')
	{
		return false;
	}

	const auto Path = Utf8Path(Text);
	return !Path.has_root_path() && std::ranges::all_of(Path, [](const auto& Part)
	{
		const auto Name = Part.generic_string();
		return PortableFilename(Name);
	});
}

template <std::size_t N> std::expected<std::array<simdjson::dom::element, N>, FProjectError> Fields(const simdjson::dom::element Element, const std::array<std::string_view, N>& Names)
{
	simdjson::dom::object Object;
	if (Element.get_object().get(Object))
	{
		return ProjectError("Expected project JSON object");
	}

	std::array<simdjson::dom::element, N> Values{};
	std::uint64_t Seen = 0;

	for (const auto Field : Object)
	{
		const auto Iterator = std::ranges::find(Names, Field.key);
		if (Iterator == Names.end())
		{
			return ProjectError("Unknown project JSON field");
		}

		const auto Index = static_cast<std::size_t>(Iterator - Names.begin());
		const auto Bit = std::uint64_t{1} << Index;
		if ((Seen & Bit) != 0)
		{
			return ProjectError("Duplicate project JSON field");
		}

		Seen |= Bit;
		Values[Index] = Field.value;
	}

	if (Seen != (std::uint64_t{1} << N) - 1)
	{
		return ProjectError("Missing required project JSON field");
	}

	return Values;
}

bool String(const simdjson::dom::element Element, std::string& Output)
{
	std::string_view Value;
	if (Element.get_string().get(Value))
	{
		return false;
	}

	Output = Value;
	return true;
}

bool Strings(const simdjson::dom::element Element, std::vector<std::string>& Output)
{
	simdjson::dom::array Array;
	if (Element.get_array().get(Array) || Array.size() > 64)
	{
		return false;
	}

	std::set<std::string> Unique;

	for (const auto Value : Array)
	{
		std::string Text;
		if (!String(Value, Text) || !Identifier(Text) || !Unique.insert(Text).second)
		{
			return false;
		}

		Output.push_back(std::move(Text));
	}

	return true;
}

std::expected<void, FProjectError> Validate(const FProjectDescriptor& Project)
{
	if (!FAssetId::Parse(Project.Id) || !ValidText(Project.Name) || !Identifier(Project.EngineAssociation) || !RelativePath(Project.StartingLevel)
	    || Project.Modules.size() > 64 || Project.Targets.size() > 64 || Project.ContentRoots.size() != 1 || Project.ContentRoots[0].Name != "Game" || !RelativePath(Project.ContentRoots[0].Path) || !Project.Features.empty())
	{
		return ProjectError("Invalid project identity, paths, content roots, or unsupported features");
	}

	std::set<std::string> Names;

	for (const auto& Module : Project.Modules)
	{
		if (!ModuleName(Module.Name) || Module.Source != "Source/" + Module.Name || !RelativePath(Module.Source) || !Names.insert(Module.Name).second || Module.Dependencies.empty())
		{
			return ProjectError("Invalid or duplicate project module");
		}

		std::set<std::string> Dependencies;

		for (const auto& Dependency : Module.Dependencies)
		{
			if ((Dependency != "Core" && Dependency != "Math" && Dependency != "Assets" && Dependency != "Level") || !Dependencies.insert(Dependency).second)
			{
				return ProjectError("Unsupported or duplicate project module dependency");
			}
		}
	}

	std::set<std::string> Targets;

	for (const auto& Target : Project.Targets)
	{
		if (!Identifier(Target.Name) || Target.Type != "Editor" || Target.Modules.empty() || !Targets.insert(Target.Name).second)
		{
			return ProjectError("Invalid or unsupported project target");
		}

		std::set<std::string> References;

		for (const auto& Module : Target.Modules)
		{
			if (!Names.contains(Module) || !References.insert(Module).second)
			{
				return ProjectError("Unknown or duplicate target module");
			}
		}
	}

	return {};
}

void Quote(std::string& Output, const std::string_view Text)
{
	Output += '"';

	for (const char Byte : Text)
	{
		if (Byte == '\\' || Byte == '"')
		{
			Output += '\\';
		}

		Output += Byte;
	}

	Output += '"';
}

void WriteStrings(std::string& Output, std::vector<std::string> Values)
{
	std::ranges::sort(Values);
	Output += '[';

	for (std::size_t Index = 0; Index < Values.size(); ++Index)
	{
		if (Index != 0)
		{
			Output += ", ";
		}

		Quote(Output, Values[Index]);
	}

	Output += ']';
}

std::expected<std::string, FProjectError> ReadFile(const std::filesystem::path& Path)
{
	std::error_code Error;
	const auto Size = std::filesystem::file_size(Path, Error);
	if (Error || Size > MaximumFileBytes)
	{
		return ProjectError("Cannot read project file or file exceeds 1 MiB limit");
	}

	std::ifstream Input(Path, std::ios::binary);
	std::string Text(static_cast<std::size_t>(Size), '\0');
	if (!Input || !Input.read(Text.data(), static_cast<std::streamsize>(Text.size())) || Input.peek() != std::char_traits<char>::eof())
	{
		return ProjectError("Cannot read complete project file");
	}

	return Text;
}

std::expected<std::filesystem::path, FProjectError> Resolve(const std::filesystem::path& Root, const std::string_view Relative)
{
	if (!RelativePath(Relative))
	{
		return ProjectError("Project paths must be portable relative paths without traversal");
	}

	std::error_code Error;
	const auto Resolved = std::filesystem::canonical(Root / Utf8Path(Relative), Error);
	const auto Local = Resolved.lexically_relative(Root);
	if (Error || Local.empty() || Local.is_absolute() || *Local.begin() == "..")
	{
		return ProjectError("Project path is missing or escapes its root through a symbolic link");
	}

	return Resolved;
}

struct FStagingCleanup
{
	~FStagingCleanup();
	FStagingCleanup(const FStagingCleanup&) = delete;
	FStagingCleanup& operator=(const FStagingCleanup&) = delete;
	FStagingCleanup(FStagingCleanup&&) = delete;
	FStagingCleanup& operator=(FStagingCleanup&&) = delete;
	explicit FStagingCleanup(std::filesystem::path InPath);

	std::filesystem::path Path;
};

FStagingCleanup::FStagingCleanup(std::filesystem::path InPath)
    : Path(std::move(InPath))
{
}

FStagingCleanup::~FStagingCleanup()
{
	if (Path.empty())
	{
		return;
	}

	std::error_code Error;
	std::filesystem::remove_all(Path, Error);
}

std::expected<std::string, FProjectError> Expand(std::string Text, const std::array<std::pair<std::string_view, std::string>, 4>& Substitutions)
{
	std::size_t Position = 0;

	while ((Position = Text.find("{{", Position)) != std::string::npos)
	{
		const auto End = Text.find("}}", Position + 2);
		if (End == std::string::npos)
		{
			return ProjectError("Unterminated template substitution");
		}

		const std::string_view Token(Text.data() + Position + 2, End - Position - 2);
		const auto Found = std::ranges::find(Substitutions, Token, &std::pair<std::string_view, std::string>::first);
		if (Found == Substitutions.end())
		{
			return ProjectError("Undeclared template substitution");
		}

		Text.replace(Position, End + 2 - Position, Found->second);
		Position += Found->second.size();
	}

	return Text;
}
}

std::expected<FProjectDescriptor, FProjectError> ParseProject(const std::string_view Text)
{
	if (Text.size() > MaximumFileBytes || !simdjson::validate_utf8(Text.data(), Text.size()))
	{
		return ProjectError("Project descriptor must be UTF-8 and at most 1 MiB");
	}

	simdjson::dom::parser Parser;
	simdjson::dom::element Document;
	if (Parser.parse(Text.data(), Text.size()).get(Document))
	{
		return ProjectError("Invalid project JSON");
	}

	std::uint64_t Schema = 0;
	if (Document["schemaVersion"].get_uint64().get(Schema) || (Schema != 1 && Schema != 2))
	{
		return ProjectError("Unsupported project schema version; supported schemas are 1 through 2");
	}

	const auto Values = Fields(Document, std::array<std::string_view, 11>{"format", "formatVersion", "id", "name", "engineAssociation", "modules", "targets", "contentRoots", "features", Schema == 1 ? "startingScene" : "startingLevel", "schemaVersion"});
	if (!Values)
	{
		return std::unexpected(Values.error());
	}

	std::string Format;
	std::uint64_t Version = 0;
	FProjectDescriptor Project;
	if (!String((*Values)[0], Format) || Format != "HertaProject" || (*Values)[1].get_uint64().get(Version) || Version != 1
	    || !String((*Values)[2], Project.Id) || !String((*Values)[3], Project.Name) || !String((*Values)[4], Project.EngineAssociation) || !Strings((*Values)[8], Project.Features) || !String((*Values)[9], Project.StartingLevel))
	{
		return ProjectError("Invalid or unsupported project format, schema, or fields");
	}

	simdjson::dom::array Modules;
	simdjson::dom::array Targets;
	simdjson::dom::array ContentRoots;
	if ((*Values)[5].get_array().get(Modules) || Modules.size() > 64 || (*Values)[6].get_array().get(Targets) || Targets.size() > 64 || (*Values)[7].get_array().get(ContentRoots) || ContentRoots.size() != 1)
	{
		return ProjectError("Invalid project module, target, or content root arrays");
	}

	for (const auto Value : Modules)
	{
		const auto Parts = Fields(Value, std::array<std::string_view, 3>{"name", "source", "dependencies"});
		FProjectModule Module;
		if (!Parts || !String((*Parts)[0], Module.Name) || !String((*Parts)[1], Module.Source) || !Strings((*Parts)[2], Module.Dependencies))
		{
			return ProjectError("Invalid project module fields");
		}

		if (Schema == 1)
		{
			std::ranges::replace(Module.Dependencies, "Scene", "Level");
		}

		Project.Modules.push_back(std::move(Module));
	}

	for (const auto Value : Targets)
	{
		const auto Parts = Fields(Value, std::array<std::string_view, 3>{"name", "type", "modules"});
		FProjectTarget Target;
		if (!Parts || !String((*Parts)[0], Target.Name) || !String((*Parts)[1], Target.Type) || !Strings((*Parts)[2], Target.Modules))
		{
			return ProjectError("Invalid project target fields");
		}

		Project.Targets.push_back(std::move(Target));
	}

	for (const auto Value : ContentRoots)
	{
		const auto Parts = Fields(Value, std::array<std::string_view, 2>{"name", "path"});
		FProjectContentRoot Root;
		if (!Parts || !String((*Parts)[0], Root.Name) || !String((*Parts)[1], Root.Path))
		{
			return ProjectError("Invalid project content root fields");
		}

		Project.ContentRoots.push_back(std::move(Root));
	}

	if (const auto Result = Validate(Project); !Result)
	{
		return std::unexpected(Result.error());
	}

	return Project;
}

std::expected<std::string, FProjectError> SerializeProject(const FProjectDescriptor& Descriptor)
{
	if (const auto Result = Validate(Descriptor); !Result)
	{
		return std::unexpected(Result.error());
	}

	std::string Output = "{\n  \"format\": \"HertaProject\",\n  \"formatVersion\": 1,\n  \"schemaVersion\": 2,\n  \"id\": ";
	Quote(Output, Descriptor.Id);
	Output += ",\n  \"name\": ";
	Quote(Output, Descriptor.Name);
	Output += ",\n  \"engineAssociation\": ";
	Quote(Output, Descriptor.EngineAssociation);
	Output += ",\n  \"modules\": [";
	auto Modules = Descriptor.Modules;
	std::ranges::sort(Modules, {}, &FProjectModule::Name);

	for (std::size_t Index = 0; Index < Modules.size(); ++Index)
	{
		Output += Index == 0 ? "\n" : ",\n";
		Output += "    {\"name\": ";
		Quote(Output, Modules[Index].Name);
		Output += ", \"source\": ";
		Quote(Output, Modules[Index].Source);
		Output += ", \"dependencies\": ";
		WriteStrings(Output, Modules[Index].Dependencies);
		Output += '}';
	}

	Output += Modules.empty() ? "],\n  \"targets\": [" : "\n  ],\n  \"targets\": [";
	auto Targets = Descriptor.Targets;
	std::ranges::sort(Targets, {}, &FProjectTarget::Name);

	for (std::size_t Index = 0; Index < Targets.size(); ++Index)
	{
		Output += Index == 0 ? "\n" : ",\n";
		Output += "    {\"name\": ";
		Quote(Output, Targets[Index].Name);
		Output += ", \"type\": ";
		Quote(Output, Targets[Index].Type);
		Output += ", \"modules\": ";
		WriteStrings(Output, Targets[Index].Modules);
		Output += '}';
	}

	Output += Targets.empty() ? "],\n  \"contentRoots\": [{\"name\": \"Game\", \"path\": " : "\n  ],\n  \"contentRoots\": [{\"name\": \"Game\", \"path\": ";
	Quote(Output, Descriptor.ContentRoots[0].Path);
	Output += "}],\n  \"features\": [],\n  \"startingLevel\": ";
	Quote(Output, Descriptor.StartingLevel);
	Output += "\n}\n";
	return Output;
}

std::expected<FLoadedProject, FProjectError> LoadProject(const std::filesystem::path& DescriptorPath)
{
	if (DescriptorPath.extension() != ".hertaproject")
	{
		return ProjectError("Expected a .hertaproject descriptor");
	}

	const auto Text = ReadFile(DescriptorPath);
	if (!Text)
	{
		return std::unexpected(Text.error());
	}

	auto Descriptor = ParseProject(*Text);
	if (!Descriptor)
	{
		return std::unexpected(Descriptor.error());
	}

	if (Descriptor->EngineAssociation != "Herta")
	{
		return ProjectError("Project engine association is not supported by this engine");
	}

	std::error_code Error;
	const auto Path = std::filesystem::canonical(DescriptorPath, Error);
	if (Error)
	{
		return ProjectError("Cannot resolve project descriptor");
	}

	const auto Root = Path.parent_path();
	const auto Content = Resolve(Root, Descriptor->ContentRoots[0].Path);
	const auto Level = Resolve(Root, Descriptor->StartingLevel);
	if (!Content || !Level || !std::filesystem::is_directory(*Content, Error) || Error || !std::filesystem::is_regular_file(*Level, Error) || Error)
	{
		return ProjectError("Project content directory or starting level is missing or unsafe");
	}

	for (const auto& Module : Descriptor->Modules)
	{
		const auto Source = Resolve(Root, Module.Source);
		if (!Source || !std::filesystem::is_directory(*Source, Error) || Error)
		{
			return ProjectError("Project module source directory is missing or unsafe");
		}
	}

	return FLoadedProject{.Descriptor = std::move(*Descriptor), .DescriptorPath = Path, .Root = Root, .ContentRoot = *Content, .StartingLevel = *Level};
}

std::expected<FLoadedProject, FProjectError> CreateProject(const FCreateProjectRequest& Request)
{
	if (!Identifier(Request.Name) || !PortableFilename(Request.Name) || !ModuleName(Request.ModuleName) || Request.EngineAssociation != "Herta" || Request.StopToken.stop_requested())
	{
		return ProjectError("Project creation cancelled or name/module/engine identifier is invalid");
	}

	std::error_code Error;
	const auto Absolute = std::filesystem::absolute(Request.Destination, Error).lexically_normal();
	if (Error || Absolute.filename().empty() || Absolute == Absolute.root_path())
	{
		return ProjectError("Invalid project destination");
	}

	const auto Parent = std::filesystem::canonical(Absolute.parent_path(), Error);
	if (Error || !std::filesystem::is_directory(Parent, Error) || Error)
	{
		return ProjectError("Project destination parent directory must exist");
	}

	const auto Destination = Parent / Absolute.filename();
	if (!RelativePath(Destination.filename().generic_string()) || Destination.native().size() > 180 || std::filesystem::symlink_status(Destination, Error).type() != std::filesystem::file_type::not_found)
	{
		return ProjectError("Project destination exists or path exceeds the portable creation limit");
	}

	Error.clear();
	const auto TemplateRoot = std::filesystem::canonical(Request.TemplateRoot, Error);
	if (Error)
	{
		return ProjectError("Cannot resolve project template directory");
	}

	const auto ManifestPath = Resolve(TemplateRoot, "Template.json");
	if (!ManifestPath)
	{
		return std::unexpected(ManifestPath.error());
	}

	const auto Text = ReadFile(*ManifestPath);
	if (!Text)
	{
		return std::unexpected(Text.error());
	}

	simdjson::dom::parser Parser;
	simdjson::dom::element Document;
	if (Parser.parse(*Text).get(Document))
	{
		return ProjectError("Invalid template JSON");
	}

	const auto Manifest = Fields(Document, std::array<std::string_view, 5>{"id", "version", "projectVersion", "substitutions", "files"});
	std::string Id;
	std::uint64_t Version = 0;
	std::uint64_t ProjectVersion = 0;
	std::vector<std::string> Allowed;
	simdjson::dom::array Files;
	if (!Manifest || !String((*Manifest)[0], Id) || Id != "Game" || (*Manifest)[1].get_uint64().get(Version) || Version != 1 || (*Manifest)[2].get_uint64().get(ProjectVersion) || ProjectVersion != 1
	    || !Strings((*Manifest)[3], Allowed) || (*Manifest)[4].get_array().get(Files) || Files.size() == 0 || Files.size() > 64)
	{
		return ProjectError("Unsupported template identity, version, or manifest fields");
	}

	std::ranges::sort(Allowed);
	if (Allowed != std::vector<std::string>{"EngineAssociation", "ModuleName", "ProjectId", "ProjectName"})
	{
		return ProjectError("Template substitution whitelist must match the Game template contract");
	}

	const auto ProjectId = FAssetId::Generate().ToString();
	const std::array<std::pair<std::string_view, std::string>, 4> Substitutions{{{"ProjectName", Request.Name}, {"ModuleName", Request.ModuleName}, {"ProjectId", ProjectId}, {"EngineAssociation", Request.EngineAssociation}}};
	const auto Staging = Parent / Utf8Path(".herta-create-" + ProjectId);
	if (!std::filesystem::create_directory(Staging, Error) || Error)
	{
		return ProjectError("Cannot create project staging directory");
	}

	FStagingCleanup Cleanup(Staging);
	std::set<std::string> Outputs;

	for (const auto File : Files)
	{
		if (Request.StopToken.stop_requested())
		{
			return ProjectError("Project creation cancelled");
		}

		const auto Entry = Fields(File, std::array<std::string_view, 3>{"source", "destination", "type"});
		std::string Source;
		std::string OutputName;
		std::string Type;
		if (!Entry || !String((*Entry)[0], Source) || !String((*Entry)[1], OutputName) || !String((*Entry)[2], Type) || (Type != "text" && Type != "binary"))
		{
			return ProjectError("Invalid template file entry");
		}

		const auto SourcePath = Resolve(TemplateRoot, Source);
		const auto ExpandedName = Expand(OutputName, Substitutions);
		if (!SourcePath || !ExpandedName || !RelativePath(*ExpandedName) || !Outputs.insert(Lowercase(*ExpandedName)).second || (Destination / Utf8Path(*ExpandedName)).native().size() > 240)
		{
			return ProjectError("Unsafe, duplicate, or overlong template file path");
		}

		auto Bytes = ReadFile(*SourcePath);
		if (!Bytes)
		{
			return std::unexpected(Bytes.error());
		}

		if (Type == "text")
		{
			if (!simdjson::validate_utf8(Bytes->data(), Bytes->size()))
			{
				return ProjectError("Template text file is not UTF-8");
			}

			Bytes = Expand(std::move(*Bytes), Substitutions);
			if (!Bytes)
			{
				return std::unexpected(Bytes.error());
			}
		}

		const auto OutputPath = Staging / Utf8Path(*ExpandedName);
		std::filesystem::create_directories(OutputPath.parent_path(), Error);
		if (Error)
		{
			return ProjectError("Cannot create staged project directories");
		}

		std::ofstream Output(OutputPath, std::ios::binary | std::ios::trunc);
		if (!Output || !Output.write(Bytes->data(), static_cast<std::streamsize>(Bytes->size())) || !Output.flush())
		{
			return ProjectError("Cannot write staged project file");
		}
	}

	const auto DescriptorName = Utf8Path(Request.Name + ".hertaproject");
	const auto Project = LoadProject(Staging / DescriptorName);
	if (!Project)
	{
		return std::unexpected(Project.error());
	}

	const auto Canonical = SerializeProject(Project->Descriptor);
	if (!Canonical || Project->Descriptor.Id != ProjectId || Project->Descriptor.Name != Request.Name || Project->Descriptor.EngineAssociation != Request.EngineAssociation
	    || Project->Descriptor.Modules.size() != 1 || Project->Descriptor.Modules[0].Name != Request.ModuleName)
	{
		return ProjectError("Staged template does not match the requested project identity");
	}

	if (!LoadLevel(Project->StartingLevel))
	{
		return ProjectError("Staged project starting level is invalid");
	}

	{
		std::ofstream Output(Staging / DescriptorName, std::ios::binary | std::ios::trunc);
		if (!Output || !Output.write(Canonical->data(), static_cast<std::streamsize>(Canonical->size())) || !Output.flush())
		{
			return ProjectError("Cannot write canonical staged project descriptor");
		}
	}

	if (Request.StopToken.stop_requested())
	{
		return ProjectError("Project creation cancelled");
	}

#ifdef _WIN32
	const bool bPublished = MoveFileExW(Staging.c_str(), Destination.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE;
#else
	const bool bPublished = syscall(SYS_renameat2, AT_FDCWD, Staging.c_str(), AT_FDCWD, Destination.c_str(), RENAME_NOREPLACE) == 0;
#endif
	if (!bPublished)
	{
		return ProjectError("Cannot publish project atomically into an absent destination");
	}

	Cleanup.Path.clear();

	return LoadProject(Destination / DescriptorName);
}
}
