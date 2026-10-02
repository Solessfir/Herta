#include "Herta/AssetPipeline/BuildKey.h"

#include <algorithm>
#include <cstddef>
#include <string_view>
#include <tuple>

namespace Herta
{
namespace
{
// Bump when the key encoding changes so stale derived data cannot be reused.
inline constexpr std::string_view BuildKeySchema = "HertaAssetBuildKey/1";

void AppendInteger(std::vector<std::byte>& Bytes, const std::uint64_t Value)
{
	for (std::size_t Index = 0; Index < 8; ++Index)
	{
		Bytes.push_back(static_cast<std::byte>((Value >> (Index * 8)) & 0xff));
	}
}

// Length prefixes keep adjacent fields from aliasing, such as "ab" + "c" and "a" + "bc".
void AppendString(std::vector<std::byte>& Bytes, const std::string_view Value)
{
	AppendInteger(Bytes, Value.size());
	for (const char Character : Value)
	{
		Bytes.push_back(static_cast<std::byte>(Character));
	}
}

void AppendHash(std::vector<std::byte>& Bytes, const FHash128& Hash)
{
	AppendInteger(Bytes, Hash.High);
	AppendInteger(Bytes, Hash.Low);
}
}

FHash128 ComputeAssetBuildKey(const FAssetBuildKeyInput& Input)
{
	std::vector<std::byte> Bytes;
	AppendString(Bytes, BuildKeySchema);
	AppendHash(Bytes, Input.SourceHash);
	AppendString(Bytes, Input.SourcePath);
	AppendString(Bytes, Input.Importer);
	AppendInteger(Bytes, Input.ImporterVersion);
	AppendInteger(Bytes, Input.Settings.size());
	for (const auto& [Name, Value] : Input.Settings)
	{
		AppendString(Bytes, Name);
		AppendString(Bytes, Value);
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
	AppendInteger(Bytes, Dependencies.size());
	for (const FAssetBuildDependency* Dependency : Dependencies)
	{
		AppendString(Bytes, Dependency->Path);
		AppendHash(Bytes, Dependency->ContentHash);
	}

	AppendString(Bytes, Input.TargetPlatform);
	AppendInteger(Bytes, Input.CookedFormatVersion);
	return HashBytes(Bytes);
}
}
