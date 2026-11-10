#include "SoundCooker.h"

#include "FileUtilities.h"
#include "Herta/AudioGraph/AudioGraph.h"
#include "Herta/Audio/SoundDecoder.h"
#include "Herta/Core/Hash.h"

#include <charconv>
#include <cmath>
#include <format>

namespace Herta
{
namespace
{
[[nodiscard]] std::unexpected<FAssetError> Fail(std::string Message)
{
	return std::unexpected(FAssetError{std::move(Message)});
}

[[nodiscard]] std::optional<float> ParseFloat(const std::string_view Text)
{
	float Value = 0.f;
	const auto [End, Error] = std::from_chars(Text.data(), Text.data() + Text.size(), Value);
	return Error == std::errc{} && End == Text.data() + Text.size() && std::isfinite(Value) ? std::optional(Value) : std::nullopt;
}

[[nodiscard]] std::optional<bool> ParseBool(const std::string_view Text)
{
	return Text == "true" ? std::optional(true) : Text == "false" ? std::optional(false) : std::nullopt;
}
}

std::expected<FSoundSettings, FAssetError> ParseSoundSettings(const FAssetImportSettings& Settings, const bool bWave)
{
	FSoundSettings Result;
	FSoundPlayback& Playback = Result.Playback;
	for (const auto& [Key, Value] : Settings)
	{
		const auto Invalid = [&Key, &Value](const std::string_view Expected)
		{
			return Fail(std::format("Sound setting {} '{}' must be {}", Key, Value, Expected));
		};

		if (Key == "Bus")
		{
			if (Value.empty() || Value.size() > 64)
			{
				return Invalid("a bus name");
			}

			Playback.Bus = Value;
		}
		else if (Key == "Concurrency")
		{
			if (Value == "StealOldest")
			{
				Playback.Concurrency = ESoundConcurrency::StealOldest;
			}
			else if (Value == "StealQuietest")
			{
				Playback.Concurrency = ESoundConcurrency::StealQuietest;
			}
			else if (Value == "Reject")
			{
				Playback.Concurrency = ESoundConcurrency::Reject;
			}
			else
			{
				return Invalid("StealOldest, StealQuietest, or Reject");
			}
		}
		else if (Key == "MaximumInstances")
		{
			std::uint32_t Count = 0;
			const auto [End, Error] = std::from_chars(Value.data(), Value.data() + Value.size(), Count);
			if (Error != std::errc{} || End != Value.data() + Value.size())
			{
				return Invalid("a whole number");
			}

			Playback.MaximumInstances = Count;
		}
		else if (Key == "Volume" || Key == "MinimumDistance" || Key == "MaximumDistance")
		{
			const auto Number = ParseFloat(Value);
			if (!Number)
			{
				return Invalid("a number");
			}

			(Key == "Volume" ? Playback.Volume : Key == "MinimumDistance" ? Playback.MinimumDistance : Playback.MaximumDistance) = *Number;
		}
		else if (bWave && (Key == "Loop" || Key == "Stream"))
		{
			const auto Flag = ParseBool(Value);
			if (!Flag)
			{
				return Invalid("true or false");
			}

			(Key == "Loop" ? Result.bLoop : Result.bStream) = *Flag;
		}
		else
		{
			return Fail(std::format("Unknown sound setting '{}'; sounds take Bus, Volume, MaximumInstances, Concurrency, MinimumDistance, and MaximumDistance{}", Key, bWave ? ", and waves also Loop and Stream" : ""));
		}
	}

	if (auto Valid = ValidateSoundPlayback(Playback); !Valid)
	{
		return std::unexpected(std::move(Valid.error()));
	}

	return Result;
}

std::expected<FCookedSound, FAssetError> CookSound(const std::span<const std::byte> Source, const FSoundSettings& Settings)
{
	auto Decoded = DecodeSound(Source);
	if (!Decoded)
	{
		return Fail(std::move(Decoded.error().Message));
	}

	FCookedSound Sound{.Playback = Settings.Playback, .SampleRate = Decoded->SampleRate, .Channels = Decoded->Channels, .bLoop = Settings.bLoop, .bStream = Settings.bStream, .Samples = std::move(Decoded->Samples)};
	if (auto Valid = ValidateCookedSound(Sound); !Valid)
	{
		return std::unexpected(std::move(Valid.error()));
	}

	return Sound;
}

std::expected<FCookedSoundGraph, FAssetError> CookSoundGraph(const std::filesystem::path& ContentRoot, const std::string_view SourcePath, const std::span<const std::byte> Source, const FSoundSettings& Settings, std::vector<FAssetBuildDependency>& Dependencies)
{
	auto Graph = DeserializeGraph(std::string_view(reinterpret_cast<const char*>(Source.data()), Source.size()), GetAudioGraphSchema());
	if (!Graph)
	{
		return Fail(std::format("{}: {}", SourcePath, Graph.error().Message));
	}

	// Waves are named by content path; their sidecars hold the IDs the cooked graph plays.
	std::string MetadataError;
	const auto Resolve = [&ContentRoot, &Dependencies, &MetadataError](const std::string_view Path) -> std::optional<FAssetId>
	{
		if (!IsValidAssetPath(Path))
		{
			return std::nullopt;
		}

		const std::filesystem::path Wave = ContentRoot / Utf8ToPath(Path);
		auto Metadata = LoadAssetMetadata(Wave);
		if (!Metadata || Metadata->Importer != "Sound")
		{
			MetadataError = Metadata ? std::format("'{}' is not imported as a Sound", Path) : Metadata.error().Message;
			return std::nullopt;
		}

		auto Text = ReadWholeFile(GetAssetMetadataPath(Wave), 1 << 20);
		if (Text && *Text)
		{
			Dependencies.push_back({.Path = std::string(Path) + std::string(AssetMetadataExtension), .ContentHash = HashBytes(**Text)});
		}

		return Metadata->Id;
	};

	auto Compiled = CompileAudioGraph(*Graph, Resolve);
	if (!Compiled)
	{
		std::string Message = std::format("{} does not compile:", SourcePath);
		for (const FGraphDiagnostic& Diagnostic : Compiled.error())
		{
			Message += " " + Diagnostic.Message + ".";
		}

		if (!MetadataError.empty())
		{
			Message += " " + MetadataError + ".";
		}

		return Fail(std::move(Message));
	}

	return FCookedSoundGraph{.Playback = Settings.Playback, .Graph = std::move(Compiled->Graph)};
}
}
