#include "BlenderImporter.h"

#include "FileUtilities.h"
#include "Herta/Platform/Process.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <format>
#include <optional>
#include <ranges>
#include <string_view>
#include <system_error>
#include <utility>

namespace Herta
{
namespace
{
// Bump BlenderImporterVersion when this preset changes. Images keep their source encoding where glTF allows it.
constexpr std::string_view ExportScript = R"PY(import os
import sys

import bpy

output, manifest = sys.argv[sys.argv.index("--") + 1:]
dependencies = set()
for image in bpy.data.images:
    if image.users and image.source in {"FILE", "SEQUENCE", "TILED"} and image.packed_file is None and image.filepath:
        dependencies.add(os.path.normpath(bpy.path.abspath(image.filepath, library=image.library)))
for library in bpy.data.libraries:
    dependencies.add(os.path.normpath(bpy.path.abspath(library.filepath)))
with open(manifest, "w", encoding="utf-8", newline="\n") as file:
    file.writelines(path + "\n" for path in sorted(dependencies))
bpy.ops.export_scene.gltf(filepath=output, export_format="GLB", export_apply=True, export_yup=True, export_image_format="AUTO", export_cameras=False, export_lights=False, export_animations=False, export_extras=False)
)PY";

// A wedged Blender inside the worker must not outlive it on Linux, where it runs in its own process group.
constexpr std::chrono::minutes ExportTimeout{5};
constexpr std::uint64_t MaximumGlbSize = std::uint64_t{1} << 30;

[[nodiscard]] std::optional<std::string> ReadEnvironment(const char* const Name)
{
#ifdef HERTA_PLATFORM_WINDOWS
	char* Value = nullptr;
	std::size_t Length = 0;
	std::optional<std::string> Result;
	if (_dupenv_s(&Value, &Length, Name) == 0 && Value != nullptr && *Value != '\0')
	{
		Result = Value;
	}

	std::free(Value);
	return Result;
#else
	const char* const Value = std::getenv(Name);
	return Value != nullptr && *Value != '\0' ? std::optional<std::string>(Value) : std::nullopt;
#endif
}

// "5.2.2" becomes {5, 2, 2}; text that is not a dotted number yields an empty list.
[[nodiscard]] std::vector<int> ParseVersion(const std::string_view Text)
{
	std::vector<int> Parts;
	for (const auto Part : std::views::split(Text, '.'))
	{
		const std::string_view Digits(Part.begin(), Part.end());
		int Value = 0;
		const auto [End, Error] = std::from_chars(Digits.data(), Digits.data() + Digits.size(), Value);
		if (Digits.empty() || Error != std::errc{} || End != Digits.data() + Digits.size())
		{
			return {};
		}

		Parts.push_back(Value);
	}

	return Parts;
}

[[nodiscard]] std::vector<std::filesystem::path> GetCandidateExecutables()
{
	if (std::optional<std::string> Override = ReadEnvironment("HERTA_BLENDER"))
	{
		return {Utf8ToPath(*Override)};
	}

	std::vector<std::filesystem::path> Candidates;
	std::error_code Error;
#ifdef HERTA_PLATFORM_WINDOWS
	std::vector<std::pair<std::vector<int>, std::filesystem::path>> Installs;
	for (const char* const Variable : {"ProgramW6432", "ProgramFiles"})
	{
		const std::optional<std::string> Root = ReadEnvironment(Variable);
		if (!Root)
		{
			continue;
		}

		for (const std::filesystem::directory_entry& Entry : std::filesystem::directory_iterator(Utf8ToPath(*Root) / "Blender Foundation", Error))
		{
			const std::filesystem::path Executable = Entry.path() / "blender.exe";
			if (std::filesystem::is_regular_file(Executable, Error))
			{
				// Install folders are named like "Blender 5.2".
				const std::string Name = PathToUtf8(Entry.path().filename());
				Installs.emplace_back(ParseVersion(Name.starts_with("Blender ") ? std::string_view(Name).substr(8) : std::string_view()), Executable);
			}
		}
	}

	std::ranges::sort(Installs, std::ranges::greater{});
	for (auto& [Version, Executable] : Installs)
	{
		if (std::ranges::find(Candidates, Executable) == Candidates.end())
		{
			Candidates.push_back(std::move(Executable));
		}
	}
#else
	if (const std::optional<std::string> Path = ReadEnvironment("PATH"))
	{
		for (const auto Part : std::views::split(std::string_view(*Path), ':'))
		{
			const std::filesystem::path Executable = Utf8ToPath(std::string_view(Part.begin(), Part.end())) / "blender";
			if (!Executable.parent_path().empty() && std::filesystem::is_regular_file(Executable, Error))
			{
				Candidates.push_back(Executable);
			}
		}
	}
#endif
	return Candidates;
}

[[nodiscard]] std::string GetOutputTail(const FProcessResult& Result)
{
	const std::string& Output = Result.StandardError.empty() ? Result.StandardOutput : Result.StandardError;
	constexpr std::size_t MaximumTail = 1024;
	return Output.size() > MaximumTail ? Output.substr(Output.size() - MaximumTail) : Output;
}

class FScratchDirectory final
{
public:
	[[nodiscard]] static std::expected<FScratchDirectory, FAssetError> Create()
	{
		static std::atomic<std::uint64_t> Counter{0};
		std::error_code Error;
		const std::filesystem::path Base = std::filesystem::temp_directory_path(Error);
		if (Error)
		{
			return std::unexpected(FAssetError{std::format("Cannot find a temporary directory for Blender export: {}", Error.message())});
		}

		const std::filesystem::path Path = Base / std::format("HertaBlender-{}-{}", std::chrono::steady_clock::now().time_since_epoch().count(), Counter.fetch_add(1));
		if (!std::filesystem::create_directory(Path, Error))
		{
			return std::unexpected(FAssetError{std::format("Cannot create '{}' for Blender export", PathToUtf8(Path))});
		}

		return FScratchDirectory(Path);
	}

	FScratchDirectory(FScratchDirectory&& Other) noexcept
	    : Path(std::exchange(Other.Path, {}))
	{
	}

	FScratchDirectory(const FScratchDirectory&) = delete;
	FScratchDirectory& operator=(const FScratchDirectory&) = delete;
	FScratchDirectory& operator=(FScratchDirectory&&) = delete;

	~FScratchDirectory()
	{
		if (!Path.empty())
		{
			std::error_code Error;
			std::filesystem::remove_all(Path, Error);
		}
	}

	[[nodiscard]] const std::filesystem::path& GetPath() const noexcept
	{
		return Path;
	}

private:
	explicit FScratchDirectory(std::filesystem::path InPath)
	    : Path(std::move(InPath))
	{
	}

	std::filesystem::path Path;
};
}

std::expected<FBlenderInstallation, FAssetError> FindBlender()
{
	for (const std::filesystem::path& Executable : GetCandidateExecutables())
	{
		const std::expected<FProcessResult, FProcessError> Result = RunProcess({.Executable = Executable, .Arguments = {"--version"}, .Timeout = std::chrono::seconds(30)});
		if (!Result || Result->ExitCode != 0)
		{
			continue;
		}

		// The first line reads like "Blender 5.2.2 LTS".
		const std::string_view Output = Result->StandardOutput;
		const std::size_t Start = Output.find("Blender ");
		if (Start == std::string_view::npos)
		{
			continue;
		}

		const std::string_view Rest = Output.substr(Start + 8);
		const std::string_view Version = Rest.substr(0, Rest.find_first_of(" \r\n"));
		if (ParseVersion(Version).size() >= 2)
		{
			return FBlenderInstallation{.Executable = Executable, .Version = std::string(Version)};
		}
	}

	return std::unexpected(FAssetError{"Blender was not found. Install Blender, or set HERTA_BLENDER to its executable, to import .blend files"});
}

std::expected<FBlenderExport, FAssetError> ExportBlend(const FBlenderInstallation& Blender, const std::filesystem::path& ContentRoot, const std::string& SourcePath)
{
	std::expected<FScratchDirectory, FAssetError> Scratch = FScratchDirectory::Create();
	if (!Scratch)
	{
		return std::unexpected(std::move(Scratch.error()));
	}

	const std::filesystem::path Script = Scratch->GetPath() / "HertaExport.py";
	const std::filesystem::path Glb = Scratch->GetPath() / "Export.glb";
	const std::filesystem::path Manifest = Scratch->GetPath() / "Dependencies.txt";
	if (std::expected<void, FAssetError> Written = WriteFileAtomically(Script, std::as_bytes(std::span(ExportScript))); !Written)
	{
		return std::unexpected(std::move(Written.error()));
	}

	// Factory startup ignores user add-ons and preferences; disabling autoexec keeps scripts embedded in the .blend from running.
	const std::filesystem::path Source = ContentRoot / Utf8ToPath(SourcePath);
	const std::expected<FProcessResult, FProcessError> Result = RunProcess({.Executable = Blender.Executable, .Arguments = {"--background", "--factory-startup", "--disable-autoexec", "-noaudio", PathToUtf8(Source), "--python-exit-code", "1", "--python", PathToUtf8(Script), "--", PathToUtf8(Glb), PathToUtf8(Manifest)}, .Timeout = ExportTimeout});
	if (!Result)
	{
		return std::unexpected(FAssetError{std::format("Blender {} could not export '{}': {}", Blender.Version, SourcePath, Result.error().Message)});
	}

	if (Result->ExitCode != 0)
	{
		return std::unexpected(FAssetError{std::format("Blender {} failed to export '{}' with exit code {}: {}", Blender.Version, SourcePath, Result->ExitCode, GetOutputTail(*Result))});
	}

	FBlenderExport Export;
	std::expected<std::optional<std::vector<std::byte>>, FAssetError> Bytes = ReadWholeFile(Glb, MaximumGlbSize);
	if (!Bytes || !*Bytes)
	{
		return std::unexpected(Bytes ? FAssetError{std::format("Blender {} wrote no GLB for '{}'", Blender.Version, SourcePath)} : std::move(Bytes.error()));
	}

	Export.Glb = std::move(**Bytes);

	std::expected<std::optional<std::vector<std::byte>>, FAssetError> ManifestBytes = ReadWholeFile(Manifest, MaximumGlbSize);
	if (!ManifestBytes || !*ManifestBytes)
	{
		return std::unexpected(ManifestBytes ? FAssetError{std::format("Blender {} wrote no dependency list for '{}'", Blender.Version, SourcePath)} : std::move(ManifestBytes.error()));
	}

	std::error_code Error;
	const std::filesystem::path Root = std::filesystem::weakly_canonical(ContentRoot, Error);
	if (Error)
	{
		return std::unexpected(FAssetError{std::format("Cannot resolve content root '{}': {}", PathToUtf8(ContentRoot), Error.message())});
	}

	const std::string_view Lines(reinterpret_cast<const char*>((*ManifestBytes)->data()), (*ManifestBytes)->size());
	for (const auto Line : std::views::split(Lines, '\n'))
	{
		const std::string_view Text(Line.begin(), Line.end());
		if (Text.empty())
		{
			continue;
		}

		const std::filesystem::path Dependency = std::filesystem::weakly_canonical(Utf8ToPath(Text), Error);
		const std::string Relative = GenericPathToUtf8(Dependency.lexically_relative(Root));
		if (Error || Relative.empty() || Relative.starts_with("..") || !IsValidAssetPath(Relative))
		{
			return std::unexpected(FAssetError{std::format("'{}' references '{}' outside the content root", SourcePath, Text)});
		}

		if (!std::filesystem::is_regular_file(Dependency, Error))
		{
			Export.Warnings.push_back(std::format("'{}' references missing '{}'", SourcePath, Relative));
		}

		Export.Dependencies.push_back(Relative);
	}

	std::ranges::sort(Export.Dependencies);
	return Export;
}
}
