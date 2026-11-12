#pragma once

#include <expected>
#include <filesystem>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace Herta
{
struct FProjectError
{
	std::string Message{};
};

struct FProjectModule
{
	[[nodiscard]] bool operator==(const FProjectModule&) const = default;

	std::string Name{};
	std::string Source{};
	std::vector<std::string> Dependencies{};
};

struct FProjectTarget
{
	[[nodiscard]] bool operator==(const FProjectTarget&) const = default;

	std::string Name{};
	std::string Type{};
	std::vector<std::string> Modules{};
};

struct FProjectContentRoot
{
	[[nodiscard]] bool operator==(const FProjectContentRoot&) const = default;

	std::string Name{};
	std::string Path{};
};

struct FProjectDescriptor
{
	[[nodiscard]] bool operator==(const FProjectDescriptor&) const = default;

	std::string Id{};
	std::string Name{};
	std::string EngineAssociation{};
	std::vector<FProjectModule> Modules{};
	std::vector<FProjectTarget> Targets{};
	std::vector<FProjectContentRoot> ContentRoots{};
	std::vector<std::string> Features{};
	std::string StartingLevel{};
};

struct FLoadedProject
{
	FProjectDescriptor Descriptor{};
	std::filesystem::path DescriptorPath{};
	std::filesystem::path Root{};
	std::filesystem::path ContentRoot{};
	std::filesystem::path StartingLevel{};
};

struct FCreateProjectRequest
{
	std::filesystem::path TemplateRoot{};
	std::filesystem::path Destination{};
	std::string Name{};
	std::string ModuleName{};
	std::string EngineAssociation = "Herta";
	std::stop_token StopToken{};
};

[[nodiscard]] std::expected<FProjectDescriptor, FProjectError> ParseProject(std::string_view Text);
[[nodiscard]] std::expected<std::string, FProjectError> SerializeProject(const FProjectDescriptor& Descriptor);
[[nodiscard]] std::expected<FLoadedProject, FProjectError> LoadProject(const std::filesystem::path& DescriptorPath);
// A staged project read from Text, which may come from a package rather than the disk: its paths resolve beside DescriptorPath without
// looking for them, and it has no module sources. The game checks the files exist as it reads them.
[[nodiscard]] std::expected<FLoadedProject, FProjectError> LoadStagedProject(const std::filesystem::path& DescriptorPath, std::string_view Text);
// The nearest .hertaproject in the file's directory or an ancestor, choosing the first by name when a directory holds several.
[[nodiscard]] std::optional<std::filesystem::path> FindOwningProject(const std::filesystem::path& File);
// The .hertaproject in a staged build, which keeps one project folder next to its executables in Directory.
[[nodiscard]] std::optional<std::filesystem::path> FindStagedProject(const std::filesystem::path& Directory);
[[nodiscard]] std::expected<FLoadedProject, FProjectError> CreateProject(const FCreateProjectRequest& Request);
}
