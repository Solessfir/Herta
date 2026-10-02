#include "Herta/AssetPipeline/AssetMetadata.h"

#include <algorithm>
#include <charconv>
#include <format>
#include <optional>
#include <ranges>
#include <vector>

namespace Herta
{
namespace
{
inline constexpr std::string_view FormatName = "HertaAssetMetadata";
inline constexpr std::string_view SettingPrefix = "Setting.";
inline constexpr std::string_view Separator = " = ";
inline constexpr std::size_t MaximumMetadataSize = std::size_t{64} * 1024;

[[nodiscard]] constexpr bool IsIdentifier(const std::string_view Text) noexcept
{
	return !Text.empty() && std::ranges::all_of(Text, [](const char Character)
	                                            {
		                                            return (Character >= 'A' && Character <= 'Z') || (Character >= 'a' && Character <= 'z') || (Character >= '0' && Character <= '9') || Character == '_';
	                                            });
}

[[nodiscard]] constexpr bool IsValidValue(const std::string_view Value) noexcept
{
	if (!Value.empty() && (Value.front() == ' ' || Value.back() == ' '))
	{
		return false;
	}

	return std::ranges::none_of(Value, [](const char Character)
	                            {
		                            const auto Byte = static_cast<unsigned char>(Character);
		                            return Byte < 0x20 || Byte == 0x7f;
	                            });
}

struct FLine
{
	std::string_view Key;
	std::string_view Value;
};

[[nodiscard]] std::optional<FLine> SplitLine(const std::string_view Line) noexcept
{
	const std::size_t Position = Line.find(Separator);
	if (Position == std::string_view::npos)
	{
		return std::nullopt;
	}
	return FLine{Line.substr(0, Position), Line.substr(Position + Separator.size())};
}

[[nodiscard]] std::expected<void, FAssetError> ValidateMetadata(const FAssetMetadata& Metadata)
{
	if (!Metadata.Id.IsValid())
	{
		return std::unexpected(FAssetError{"Asset metadata requires a valid ID"});
	}
	if (!IsIdentifier(Metadata.Importer))
	{
		return std::unexpected(FAssetError{std::format("Invalid importer name '{}'", Metadata.Importer)});
	}
	for (const auto& [Name, Value] : Metadata.Settings)
	{
		if (!IsIdentifier(Name) || !IsValidValue(Value))
		{
			return std::unexpected(FAssetError{std::format("Invalid import setting '{}'", Name)});
		}
	}
	return {};
}
}

std::expected<FAssetMetadata, FAssetError> ParseAssetMetadata(const std::string_view Text)
{
	if (Text.size() > MaximumMetadataSize)
	{
		return std::unexpected(FAssetError{"Asset metadata exceeds 64 KiB"});
	}

	std::vector<std::string_view> Lines;
	for (const auto Range : std::views::split(Text, '\n'))
	{
		std::string_view Line(Range.begin(), Range.end());
		// Tolerate CRLF checkouts even though Herta always writes LF.
		if (Line.ends_with('\r'))
		{
			Line.remove_suffix(1);
		}
		Lines.push_back(Line);
	}
	if (!Lines.empty() && Lines.back().empty())
	{
		Lines.pop_back();
	}

	const auto LineError = [](const std::size_t Index, const std::string_view Message)
	{
		return std::unexpected(FAssetError{std::format("Asset metadata line {}: {}", Index + 1, Message)});
	};

	if (Lines.size() < 2)
	{
		return std::unexpected(FAssetError{"Asset metadata is missing its format header"});
	}

	const std::optional<FLine> Format = SplitLine(Lines[0]);
	if (!Format || Format->Key != "Format" || Format->Value != FormatName)
	{
		return LineError(0, "expected 'Format = HertaAssetMetadata'");
	}

	const std::optional<FLine> Version = SplitLine(Lines[1]);
	std::uint32_t VersionNumber = 0;
	if (!Version || Version->Key != "Version" ||
	    std::from_chars(Version->Value.data(), Version->Value.data() + Version->Value.size(), VersionNumber).ptr != Version->Value.data() + Version->Value.size())
	{
		return LineError(1, "expected 'Version = <number>'");
	}
	if (VersionNumber != AssetMetadataVersion)
	{
		return LineError(1, std::format("unsupported metadata version {}", VersionNumber));
	}

	FAssetMetadata Metadata;
	bool bHasId = false;
	bool bHasImporter = false;
	for (std::size_t Index = 2; Index < Lines.size(); ++Index)
	{
		const std::optional<FLine> Line = SplitLine(Lines[Index]);
		if (!Line || !IsValidValue(Line->Value))
		{
			return LineError(Index, "expected 'Key = Value'");
		}

		if (Line->Key == "Id")
		{
			const std::optional<FAssetId> Id = FAssetId::Parse(Line->Value);
			if (bHasId || !Id)
			{
				return LineError(Index, "invalid or duplicate ID");
			}
			Metadata.Id = *Id;
			bHasId = true;
		}
		else if (Line->Key == "Importer")
		{
			if (bHasImporter)
			{
				return LineError(Index, "duplicate importer");
			}
			Metadata.Importer = Line->Value;
			bHasImporter = true;
		}
		else if (Line->Key.starts_with(SettingPrefix))
		{
			const std::string_view Name = Line->Key.substr(SettingPrefix.size());
			if (!IsIdentifier(Name) || !Metadata.Settings.emplace(Name, Line->Value).second)
			{
				return LineError(Index, std::format("invalid or duplicate setting '{}'", Name));
			}
		}
		else
		{
			return LineError(Index, std::format("unknown key '{}'", Line->Key));
		}
	}

	if (!bHasId || !bHasImporter)
	{
		return std::unexpected(FAssetError{"Asset metadata requires Id and Importer"});
	}

	if (std::expected<void, FAssetError> Valid = ValidateMetadata(Metadata); !Valid)
	{
		return std::unexpected(std::move(Valid.error()));
	}
	return Metadata;
}

std::expected<std::string, FAssetError> SerializeAssetMetadata(const FAssetMetadata& Metadata)
{
	if (std::expected<void, FAssetError> Valid = ValidateMetadata(Metadata); !Valid)
	{
		return std::unexpected(std::move(Valid.error()));
	}

	std::string Text = std::format("Format = {}\nVersion = {}\nId = {}\nImporter = {}\n", FormatName, AssetMetadataVersion, Metadata.Id.ToString(), Metadata.Importer);
	for (const auto& [Name, Value] : Metadata.Settings)
	{
		Text.append(std::format("{}{} = {}\n", SettingPrefix, Name, Value));
	}
	return Text;
}

std::filesystem::path GetAssetMetadataPath(const std::filesystem::path& SourcePath)
{
	std::filesystem::path Path = SourcePath;
	Path += AssetMetadataExtension;
	return Path;
}
}
