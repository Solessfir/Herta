#include "Herta/GameFramework/GameSave.h"

#include "Herta/Core/Hash.h"
#include "Herta/Platform/FileSystem.h"

#include <charconv>
#include <cmath>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <system_error>

namespace Herta
{
namespace
{
constexpr std::string_view Magic = "HertaSave";
constexpr std::size_t MaximumSaveBytes = std::size_t{16} * 1024 * 1024;
constexpr std::size_t MaximumListEntries = 65536;

[[nodiscard]] std::unexpected<FGameSaveError> Fail(std::string Message)
{
	return std::unexpected(FGameSaveError{std::move(Message)});
}

[[nodiscard]] std::string Checksum(const std::string_view Text)
{
	return ToString(HashBytes(std::as_bytes(std::span(Text.data(), Text.size()))));
}

template <typename T>
[[nodiscard]] std::optional<T> ParseNumber(const std::string_view Text)
{
	T Value{};
	const auto [End, Error] = std::from_chars(Text.data(), Text.data() + Text.size(), Value);
	if (Error != std::errc{} || End != Text.data() + Text.size())
	{
		return std::nullopt;
	}

	if constexpr (std::is_floating_point_v<T>)
	{
		if (!std::isfinite(Value))
		{
			return std::nullopt;
		}
	}

	return Value;
}

[[nodiscard]] std::vector<std::string_view> Split(const std::string_view Text, const char Separator)
{
	std::vector<std::string_view> Parts;
	if (Text.empty())
	{
		return Parts;
	}

	std::size_t Start = 0;
	while (Parts.size() <= MaximumListEntries)
	{
		const std::size_t End = std::min(Text.find(Separator, Start), Text.size());
		Parts.push_back(Text.substr(Start, End - Start));
		if (End == Text.size())
		{
			break;
		}

		Start = End + 1;
	}

	return Parts;
}

// Bonds pack four to a hex digit, lowest bond in the lowest bit.
[[nodiscard]] std::string EncodeBits(const std::vector<bool>& Bits)
{
	std::string Text = std::format("{}:", Bits.size());
	for (std::size_t Index = 0; Index < Bits.size(); Index += 4)
	{
		unsigned Digit = 0;
		for (std::size_t Bit = 0; Bit < 4 && Index + Bit < Bits.size(); ++Bit)
		{
			Digit |= Bits[Index + Bit] ? 1u << Bit : 0u;
		}

		Text += "0123456789abcdef"[Digit];
	}

	return Text;
}

[[nodiscard]] std::optional<std::vector<bool>> DecodeBits(const std::string_view Text)
{
	const std::size_t Colon = Text.find(':');
	const auto Count = Colon == std::string_view::npos ? std::nullopt : ParseNumber<std::size_t>(Text.substr(0, Colon));
	const std::string_view Digits = Colon == std::string_view::npos ? std::string_view{} : Text.substr(Colon + 1);
	if (!Count || *Count > MaximumListEntries * 4 || Digits.size() != (*Count + 3) / 4)
	{
		return std::nullopt;
	}

	std::vector<bool> Bits(*Count);
	for (std::size_t Index = 0; Index < Bits.size(); ++Index)
	{
		const char Character = Digits[Index / 4];
		const int Digit = Character >= '0' && Character <= '9' ? Character - '0' : Character >= 'a' && Character <= 'f' ? Character - 'a' + 10
		                                                                                                                : -1;
		if (Digit < 0)
		{
			return std::nullopt;
		}

		Bits[Index] = (Digit >> (Index % 4) & 1) != 0;
	}

	return Bits;
}
}

std::string SerializeGameSave(const FGameSave& Save)
{
	std::string Text = std::format("{}\nversion={}\nlevel={}\nobjective={}\nelapsed={}\nbest={}\nrespawns={}\nfoot={},{},{}\nyaw={}\n", Magic, GameSaveVersion, Save.Level.ToString(), Save.ObjectiveState, Save.ElapsedSeconds, Save.BestSeconds ? std::format("{}", *Save.BestSeconds) : std::string(), Save.Respawns, Save.PlayerFoot.X, Save.PlayerFoot.Y, Save.PlayerFoot.Z, Save.PlayerYaw);
	for (const FGameSaveDestructible& Destructible : Save.Destructibles)
	{
		Text += std::format("destructible={} {}\n", Destructible.Object.ToString(), EncodeBits(Destructible.IntactBonds));
	}

	for (const FObjectId& Character : Save.DeadCharacters)
	{
		Text += std::format("dead={}\n", Character.ToString());
	}

	return Text + std::format("checksum={}\n", Checksum(Text));
}

std::expected<FGameSave, FGameSaveError> ParseGameSave(const std::string_view Text)
{
	const std::size_t ChecksumAt = Text.rfind("checksum=");
	if (!Text.starts_with(Magic) || ChecksumAt == std::string_view::npos || !Text.ends_with('\n'))
	{
		return Fail("Not a save file, or it ends early");
	}

	const std::string_view Body = Text.substr(0, ChecksumAt);
	if (Text.substr(ChecksumAt + 9, Text.size() - ChecksumAt - 10) != Checksum(Body))
	{
		return Fail("The save is damaged: its checksum does not match");
	}

	FGameSave Save;
	std::map<std::string_view, std::string_view> Fields;
	for (const std::string_view Line : Split(Body.substr(Magic.size() + 1), '\n'))
	{
		if (Line.empty())
		{
			continue;
		}

		const std::size_t Equals = Line.find('=');
		if (Equals == std::string_view::npos)
		{
			return Fail(std::format("Unreadable save line '{}'", Line));
		}

		const std::string_view Key = Line.substr(0, Equals);
		const std::string_view Value = Line.substr(Equals + 1);
		if (Key == "destructible")
		{
			const std::size_t Space = Value.find(' ');
			const auto Object = FObjectId::Parse(Value.substr(0, Space));
			auto Bits = Space == std::string_view::npos ? std::nullopt : DecodeBits(Value.substr(Space + 1));
			if (!Object || !Bits || Save.Destructibles.size() >= MaximumListEntries)
			{
				return Fail("A saved destructible is unreadable");
			}

			Save.Destructibles.push_back({.Object = *Object, .IntactBonds = std::move(*Bits)});
		}
		else if (Key == "dead")
		{
			const auto Object = FObjectId::Parse(Value);
			if (!Object || Save.DeadCharacters.size() >= MaximumListEntries)
			{
				return Fail("A saved character is unreadable");
			}

			Save.DeadCharacters.push_back(*Object);
		}
		else if (!Fields.emplace(Key, Value).second)
		{
			return Fail(std::format("The save repeats '{}'", Key));
		}
	}

	const auto Field = [&Fields](const std::string_view Key) -> std::optional<std::string_view>
	{
		const auto Found = Fields.find(Key);
		return Found == Fields.end() ? std::nullopt : std::optional(Found->second);
	};

	const auto Version = Field("version").and_then(ParseNumber<std::uint32_t>);
	if (!Version || *Version != GameSaveVersion)
	{
		return Fail(std::format("Save version {} is not one this game reads", Field("version").value_or("missing")));
	}

	const auto Level = Field("level").and_then(FObjectId::Parse);
	const auto Objective = Field("objective").and_then(ParseNumber<std::uint32_t>);
	const auto Elapsed = Field("elapsed").and_then(ParseNumber<double>);
	const auto Best = Field("best");
	const auto Respawns = Field("respawns").and_then(ParseNumber<std::uint32_t>);
	const auto Yaw = Field("yaw").and_then(ParseNumber<float>);
	const auto Foot = Field("foot").transform([](const std::string_view Value)
	{
		return Split(Value, ',');
	});

	if (!Level || !Objective || *Objective > 3 || !Elapsed || *Elapsed < 0. || !Best || !Respawns || !Yaw || !Foot || Foot->size() != 3)
	{
		return Fail("The save is missing progress it needs");
	}

	const auto X = ParseNumber<float>((*Foot)[0]);
	const auto Y = ParseNumber<float>((*Foot)[1]);
	const auto Z = ParseNumber<float>((*Foot)[2]);
	const auto BestSeconds = Best->empty() ? std::optional<double>() : ParseNumber<double>(*Best);
	if (!X || !Y || !Z || (!Best->empty() && !BestSeconds))
	{
		return Fail("The save's player or best time is unreadable");
	}

	Save.Level = *Level;
	Save.ObjectiveState = *Objective;
	Save.ElapsedSeconds = *Elapsed;
	Save.BestSeconds = BestSeconds;
	Save.Respawns = *Respawns;
	Save.PlayerFoot = {*X, *Y, *Z};
	Save.PlayerYaw = *Yaw;
	return Save;
}

std::filesystem::path GetGameSaveBackupPath(const std::filesystem::path& File)
{
	std::filesystem::path Backup = File;
	Backup += ".previous";
	return Backup;
}

std::expected<void, FGameSaveError> WriteGameSave(const std::filesystem::path& File, const FGameSave& Save)
{
	std::error_code Error;
	std::filesystem::create_directories(File.parent_path(), Error);
	std::filesystem::path Temporary = File;
	Temporary += ".tmp";
	{
		std::ofstream Stream(Temporary, std::ios::binary | std::ios::trunc);
		const std::string Text = SerializeGameSave(Save);
		Stream.write(Text.data(), static_cast<std::streamsize>(Text.size()));
		Stream.flush();
		if (!Stream)
		{
			return Fail(std::format("Cannot write the save to {}", Temporary.generic_string()));
		}
	}

	// The current save becomes the backup only once the new one is fully on disk.
	if (std::filesystem::exists(File, Error))
	{
		if (auto Kept = MoveAtomically(File, GetGameSaveBackupPath(File), EMoveMode::ReplaceExisting); !Kept)
		{
			return Fail(std::format("Cannot keep the previous save: {}", Kept.error().message()));
		}
	}

	if (auto Moved = MoveAtomically(Temporary, File, EMoveMode::ReplaceExisting); !Moved)
	{
		return Fail(std::format("Cannot publish the save: {}", Moved.error().message()));
	}

	return {};
}

std::expected<FGameSaveLoad, FGameSaveError> LoadGameSave(const std::filesystem::path& File)
{
	const auto Read = [](const std::filesystem::path& Path) -> std::expected<FGameSave, FGameSaveError>
	{
		std::error_code Error;
		const auto Size = std::filesystem::file_size(Path, Error);
		if (Error)
		{
			return Fail("No save");
		}

		if (Size > MaximumSaveBytes)
		{
			return Fail("The save is too large to be one");
		}

		std::ifstream Stream(Path, std::ios::binary);
		const std::string Text{std::istreambuf_iterator<char>(Stream), std::istreambuf_iterator<char>()};
		return ParseGameSave(Text);
	};

	auto Main = Read(File);
	if (Main)
	{
		return FGameSaveLoad{.Save = std::move(*Main)};
	}

	auto Backup = Read(GetGameSaveBackupPath(File));
	if (Backup)
	{
		return FGameSaveLoad{.Save = std::move(*Backup), .bRecovered = true, .Problem = Main.error().Message};
	}

	return std::unexpected(Main.error());
}
}
