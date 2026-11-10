#include "Herta/Assets/DataMod.h"

#include "Herta/Assets/ContentPackage.h"
#include "Herta/Assets/CookedAsset.h"

#include <simdjson.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <format>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <utility>

namespace Herta
{
namespace
{
constexpr std::size_t MaximumModListEntries = 4096;

[[nodiscard]] bool IsNamespace(const std::string_view Text) noexcept
{
	const auto IsLetter = [](const char Character)
	{
		return (Character >= 'a' && Character <= 'z') || (Character >= 'A' && Character <= 'Z');
	};

	return !Text.empty() && Text.size() <= 64 && IsLetter(Text.front()) && std::ranges::all_of(Text, [&IsLetter](const char Character)
	{
		return IsLetter(Character) || (Character >= '0' && Character <= '9') || Character == '_';
	});
}

[[nodiscard]] std::optional<FModVersion> ParseVersion(const std::string_view Text) noexcept
{
	FModVersion Version;
	std::array<std::uint32_t*, 3> Parts{&Version.Major, &Version.Minor, &Version.Patch};
	const char* Cursor = Text.data();
	const char* const End = Text.data() + Text.size();
	for (std::size_t Index = 0; Index < Parts.size(); ++Index)
	{
		const auto [Next, Error] = std::from_chars(Cursor, End, *Parts[Index]);
		if (Error != std::errc{} || Next == Cursor || (Index < 2 && (Next == End || *Next != '.')))
		{
			return std::nullopt;
		}

		Cursor = Index < 2 ? Next + 1 : Next;
	}

	return Cursor == End ? std::optional(Version) : std::nullopt;
}

[[nodiscard]] std::string FormatVersion(const FModVersion& Version)
{
	return std::format("{}.{}.{}", Version.Major, Version.Minor, Version.Patch);
}

[[nodiscard]] std::expected<std::vector<std::string>, FAssetError> ReadStrings(const simdjson::dom::element Element, const std::string_view Field, bool (*Valid)(std::string_view))
{
	simdjson::dom::array Array;
	if (Element.get_array().get(Array) || Array.size() > MaximumModListEntries)
	{
		return std::unexpected(FAssetError{std::format("'{}' must be a list of at most {} entries", Field, MaximumModListEntries)});
	}

	std::vector<std::string> Strings;
	for (const simdjson::dom::element Item : Array)
	{
		std::string_view Text;
		if (Item.get_string().get(Text) || !Valid(Text))
		{
			return std::unexpected(FAssetError{std::format("'{}' holds an invalid entry", Field)});
		}

		Strings.emplace_back(Text);
	}

	return Strings;
}
}

std::expected<FModManifest, FAssetError> ParseModManifest(const std::string_view Text)
{
	simdjson::dom::parser Parser;
	simdjson::dom::element Root;
	simdjson::dom::object Object;
	if (Parser.parse(Text.data(), Text.size()).get(Root) || Root.get_object().get(Object))
	{
		return std::unexpected(FAssetError{"A mod manifest is a JSON object"});
	}

	constexpr std::array<std::string_view, 9> Keys{"format", "formatVersion", "namespace", "version", "cookedFormat", "dependencies", "loadAfter", "loadBefore", "overrides"};
	std::array<simdjson::dom::element, Keys.size()> Fields{};
	std::array<bool, Keys.size()> bFound{};
	for (const auto Field : Object)
	{
		const auto Found = std::ranges::find(Keys, Field.key);
		if (Found == Keys.end() || bFound[static_cast<std::size_t>(Found - Keys.begin())])
		{
			return std::unexpected(FAssetError{std::format("A mod manifest has an unknown or repeated field '{}'", std::string_view(Field.key))});
		}

		bFound[static_cast<std::size_t>(Found - Keys.begin())] = true;
		Fields[static_cast<std::size_t>(Found - Keys.begin())] = Field.value;
	}

	if (!std::ranges::all_of(bFound, std::identity{}))
	{
		return std::unexpected(FAssetError{"A mod manifest needs format, formatVersion, namespace, version, cookedFormat, dependencies, loadAfter, loadBefore, and overrides"});
	}

	std::string_view Format;
	std::uint64_t FormatVersion = 0;
	std::string_view Namespace;
	std::string_view Version;
	std::uint64_t CookedFormat = 0;
	if (Fields[0].get_string().get(Format) || Format != "HertaMod" || Fields[1].get_uint64().get(FormatVersion) || FormatVersion != DataModFormatVersion)
	{
		return std::unexpected(FAssetError{std::format("Not a HertaMod manifest of format version {}", DataModFormatVersion)});
	}

	const auto ParsedVersion = Fields[3].get_string().get(Version) ? std::nullopt : ParseVersion(Version);
	if (Fields[2].get_string().get(Namespace) || !IsNamespace(Namespace) || !ParsedVersion || Fields[4].get_uint64().get(CookedFormat) || CookedFormat > UINT32_MAX)
	{
		return std::unexpected(FAssetError{"A mod's namespace is letters, digits, and underscores starting with a letter, its version is major.minor.patch, and its cookedFormat a number"});
	}

	FModManifest Manifest{.Namespace = std::string(Namespace), .Version = *ParsedVersion, .CookedFormat = static_cast<std::uint32_t>(CookedFormat)};
	simdjson::dom::array Dependencies;
	if (Fields[5].get_array().get(Dependencies) || Dependencies.size() > MaximumModListEntries)
	{
		return std::unexpected(FAssetError{"'dependencies' must be a list"});
	}

	for (const simdjson::dom::element Dependency : Dependencies)
	{
		std::string_view Name;
		std::string_view Minimum;
		const auto MinimumVersion = Dependency["minimumVersion"].get_string().get(Minimum) ? std::nullopt : ParseVersion(Minimum);
		simdjson::dom::object DependencyObject;
		if (Dependency.get_object().get(DependencyObject) || DependencyObject.size() != 2 || Dependency["namespace"].get_string().get(Name) || !IsNamespace(Name) || !MinimumVersion)
		{
			return std::unexpected(FAssetError{"A dependency is {\"namespace\": ..., \"minimumVersion\": \"major.minor.patch\"}"});
		}

		Manifest.Dependencies.push_back({.Namespace = std::string(Name), .MinimumVersion = *MinimumVersion});
	}

	auto LoadAfter = ReadStrings(Fields[6], "loadAfter", &IsNamespace);
	auto LoadBefore = ReadStrings(Fields[7], "loadBefore", &IsNamespace);
	auto Overrides = ReadStrings(Fields[8], "overrides", &IsValidPackagePath);
	if (!LoadAfter || !LoadBefore || !Overrides)
	{
		return std::unexpected(!LoadAfter ? LoadAfter.error() : !LoadBefore ? LoadBefore.error() : Overrides.error());
	}

	Manifest.LoadAfter = std::move(*LoadAfter);
	Manifest.LoadBefore = std::move(*LoadBefore);
	Manifest.Overrides = std::move(*Overrides);
	return Manifest;
}

FModResolution ResolveMods(const std::span<const FModCandidate> Mods, const std::function<bool(std::string_view)>& BaseHas)
{
	FModResolution Result;
	const auto Refuse = [&Result, &Mods](std::vector<bool>& Accepted, const std::size_t Index, const std::string_view Reason)
	{
		if (Accepted[Index])
		{
			Accepted[Index] = false;
			Result.Diagnostics.push_back(std::format("Mod {} ({}) is not loaded: {}", Mods[Index].Manifest.Namespace, Mods[Index].Source, Reason));
		}
	};

	std::vector<bool> Accepted(Mods.size(), true);
	std::map<std::string_view, std::vector<std::size_t>> ByNamespace;
	for (std::size_t Index = 0; Index < Mods.size(); ++Index)
	{
		ByNamespace[Mods[Index].Manifest.Namespace].push_back(Index);
	}

	for (const auto& [Namespace, Indices] : ByNamespace)
	{
		for (const std::size_t Index : Indices)
		{
			if (Indices.size() > 1)
			{
				Refuse(Accepted, Index, std::format("{} mods share the namespace", Indices.size()));
			}
			else if (Mods[Index].Manifest.CookedFormat != CookedAssetFormatVersion)
			{
				Refuse(Accepted, Index, std::format("it was cooked for asset format {}, and this game reads {}; cook it again", Mods[Index].Manifest.CookedFormat, CookedAssetFormatVersion));
			}
		}
	}

	const auto Find = [&](const std::string_view Namespace) -> std::optional<std::size_t>
	{
		const auto Found = ByNamespace.find(Namespace);
		return Found != ByNamespace.end() && Found->second.size() == 1 && Accepted[Found->second.front()] ? std::optional(Found->second.front()) : std::nullopt;
	};

	// Refusing one mod can strand the mods that depend on it, so dependencies are checked until nothing changes.
	for (bool bChanged = true; bChanged;)
	{
		bChanged = false;
		for (std::size_t Index = 0; Index < Mods.size(); ++Index)
		{
			for (const FModDependency& Dependency : Mods[Index].Manifest.Dependencies)
			{
				const auto Provider = Accepted[Index] ? Find(Dependency.Namespace) : std::nullopt;
				if (Accepted[Index] && (!Provider || Mods[*Provider].Manifest.Version < Dependency.MinimumVersion))
				{
					Refuse(Accepted, Index, Provider ? std::format("it needs {} {} or later, and {} is installed", Dependency.Namespace, FormatVersion(Dependency.MinimumVersion), FormatVersion(Mods[*Provider].Manifest.Version)) : std::format("it needs {}, which is missing or not loaded", Dependency.Namespace));
					bChanged = true;
				}
			}
		}
	}

	// Each mod loads after its dependencies and the mods it names in loadAfter, and before those in loadBefore; ties go by namespace.
	std::vector<std::set<std::size_t>> Before(Mods.size());
	for (std::size_t Index = 0; Index < Mods.size(); ++Index)
	{
		const FModManifest& Manifest = Mods[Index].Manifest;
		for (const FModDependency& Dependency : Manifest.Dependencies)
		{
			if (const auto Provider = Find(Dependency.Namespace))
			{
				Before[Index].insert(*Provider);
			}
		}

		for (const std::string& After : Manifest.LoadAfter)
		{
			if (const auto Other = Find(After))
			{
				Before[Index].insert(*Other);
			}
		}

		for (const std::string& BeforeName : Manifest.LoadBefore)
		{
			if (const auto Other = Find(BeforeName))
			{
				Before[*Other].insert(Index);
			}
		}
	}

	std::vector<std::size_t> Order;
	std::vector<bool> Placed(Mods.size(), false);
	for (bool bProgress = true; bProgress;)
	{
		bProgress = false;
		std::optional<std::size_t> Next;
		for (std::size_t Index = 0; Index < Mods.size(); ++Index)
		{
			const bool bReady = Accepted[Index] && !Placed[Index] && std::ranges::all_of(Before[Index], [&](const std::size_t Earlier)
			{
				return Placed[Earlier] || !Accepted[Earlier];
			});

			if (bReady && (!Next || Mods[Index].Manifest.Namespace < Mods[*Next].Manifest.Namespace))
			{
				Next = Index;
			}
		}

		if (Next)
		{
			Placed[*Next] = true;
			Order.push_back(*Next);
			bProgress = true;
		}
	}

	for (std::size_t Index = 0; Index < Mods.size(); ++Index)
	{
		if (Accepted[Index] && !Placed[Index])
		{
			Refuse(Accepted, Index, "its load order constraints form a cycle with other mods");
		}
	}

	// In load order, every file a mod provides that the game or an earlier mod already holds must be declared as an override.
	std::map<std::string_view, std::string_view> Owners;
	for (const std::size_t Index : Order)
	{
		const FModCandidate& Mod = Mods[Index];
		const bool bDependenciesLoaded = std::ranges::all_of(Mod.Manifest.Dependencies, [&](const FModDependency& Dependency)
		{
			return Find(Dependency.Namespace).has_value();
		});

		if (!bDependenciesLoaded)
		{
			Refuse(Accepted, Index, "a mod it depends on was refused");
			continue;
		}

		std::vector<std::string_view> Undeclared;
		std::vector<std::string> Warnings;
		for (const std::string& File : Mod.Files)
		{
			const auto Owner = Owners.find(File);
			const bool bTaken = Owner != Owners.end() || BaseHas(File);
			const bool bDeclared = std::ranges::contains(Mod.Manifest.Overrides, File);
			if (bTaken && !bDeclared)
			{
				Undeclared.push_back(File);
			}
			else if (Owner != Owners.end())
			{
				Warnings.push_back(std::format("Mod {} replaces {}, which mod {} also replaces", Mod.Manifest.Namespace, File, Owner->second));
			}
		}

		if (!Undeclared.empty())
		{
			Refuse(Accepted, Index, std::format("it replaces {} without listing it in its overrides", Undeclared.front()));
			continue;
		}

		for (const std::string& Override : Mod.Manifest.Overrides)
		{
			if (!std::ranges::contains(Mod.Files, Override) || (!Owners.contains(Override) && !BaseHas(Override)))
			{
				Warnings.push_back(std::format("Mod {} lists {} as an override, but {}", Mod.Manifest.Namespace, Override, std::ranges::contains(Mod.Files, Override) ? "nothing it replaces holds that file" : "it does not provide that file"));
			}
		}

		for (const std::string& File : Mod.Files)
		{
			Owners[File] = Mod.Manifest.Namespace;
		}

		std::ranges::move(Warnings, std::back_inserter(Result.Diagnostics));
		Result.LoadOrder.push_back(Index);
	}

	return Result;
}
}
