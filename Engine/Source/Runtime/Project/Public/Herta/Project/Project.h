#pragma once

#include <expected>
#include <filesystem>
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
	std::string Name{};
	std::string Source{};
	std::vector<std::string> Dependencies{};
};

struct FProjectTarget
{
	std::string Name{};
	std::string Type{};
	std::vector<std::string> Modules{};
};

struct FProjectContentRoot
{
	std::string Name{};
	std::string Path{};
};

struct FProjectDescriptor
{
	std::string Id{};
	std::string Name{};
	std::string EngineAssociation{};
	std::vector<FProjectModule> Modules{};
	std::vector<FProjectTarget> Targets{};
	std::vector<FProjectContentRoot> ContentRoots{};
	std::vector<std::string> Features{};
	std::string StartingScene{};
};

struct FLoadedProject
{
	FProjectDescriptor Descriptor{};
	std::filesystem::path DescriptorPath{};
	std::filesystem::path Root{};
	std::filesystem::path ContentRoot{};
	std::filesystem::path StartingScene{};
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
[[nodiscard]] std::expected<FLoadedProject, FProjectError> CreateProject(const FCreateProjectRequest& Request);
}
