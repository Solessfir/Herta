#include "Herta/AssetPipeline/BuildKey.h"

#include "Herta/Core/BinaryStream.h"

#include <algorithm>
#include <string_view>
#include <tuple>

namespace Herta
{
namespace
{
// Bump when the key encoding changes so stale derived data cannot be reused.
inline constexpr std::string_view BuildKeySchema = "HertaAssetBuildKey/1";

void WriteHash(FBinaryWriter& Writer, const FHash128& Hash)
{
	Writer.Write(Hash.High);
	Writer.Write(Hash.Low);
}
}

FHash128 ComputeAssetBuildKey(const FAssetBuildKeyInput& Input)
{
	// Strings are length-prefixed so adjacent fields cannot alias, such as "ab" + "c" and "a" + "bc".
	FBinaryWriter Writer;
	Writer.WriteString(BuildKeySchema);
	WriteHash(Writer, Input.SourceHash);
	Writer.WriteString(Input.SourcePath);
	Writer.WriteString(Input.Importer);
	Writer.Write(Input.ImporterVersion);
	Writer.Write(static_cast<std::uint64_t>(Input.Settings.size()));
	for (const auto& [Name, Value] : Input.Settings)
	{
		Writer.WriteString(Name);
		Writer.WriteString(Value);
	}

	std::vector<const FAssetBuildDependency*> Dependencies;
	Dependencies.reserve(Input.Dependencies.size());
	for (const FAssetBuildDependency& Dependency : Input.Dependencies)
	{
		Dependencies.push_back(&Dependency);
	}

	std::ranges::sort(Dependencies, [](const FAssetBuildDependency* Left, const FAssetBuildDependency* Right)
	{
		return std::tie(Left->Path, Left->ContentHash) < std::tie(Right->Path, Right->ContentHash);
	});

	Writer.Write(static_cast<std::uint64_t>(Dependencies.size()));
	for (const FAssetBuildDependency* Dependency : Dependencies)
	{
		Writer.WriteString(Dependency->Path);
		WriteHash(Writer, Dependency->ContentHash);
	}

	Writer.WriteString(Input.TargetPlatform);
	Writer.Write(Input.CookedFormatVersion);
	return HashBytes(Writer.GetBytes());
}
}
