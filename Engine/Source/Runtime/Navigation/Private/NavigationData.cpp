#include "Herta/Navigation/NavigationData.h"

#include "Herta/Core/BinaryStream.h"

#include <DetourTileCacheBuilder.h>

#include <cmath>
#include <cstring>
#include <numbers>

namespace Herta
{
namespace
{
constexpr std::uint32_t NavigationMagic = 0x56414e48;
constexpr std::size_t MaximumBoxes = 4096;
constexpr std::size_t MaximumLayers = 1 << 20;
constexpr std::size_t MaximumLayerBytes = 1 << 20;
constexpr std::size_t MaximumPatches = 4096;

void WriteSettings(FBinaryWriter& Writer, const FNavigationSettings& Settings)
{
	Writer.WriteFloat(Settings.CellSize);
	Writer.WriteFloat(Settings.CellHeight);
	Writer.WriteFloat(Settings.AgentRadius);
	Writer.WriteFloat(Settings.AgentHeight);
	Writer.WriteFloat(Settings.AgentMaxClimb);
	Writer.WriteFloat(Settings.AgentMaxSlopeRadians);
	Writer.Write(Settings.TileCells);
}

void WriteBox(FBinaryWriter& Writer, const FNavigationBox& Box)
{
	for (const FVector3& Corner : {Box.Minimum, Box.Maximum})
	{
		Writer.WriteFloat(Corner.X);
		Writer.WriteFloat(Corner.Y);
		Writer.WriteFloat(Corner.Z);
	}
}

void WriteData(FBinaryWriter& Writer, const FNavigationData& Data)
{
	WriteSettings(Writer, Data.Settings);
	Writer.Write(Data.Source.High);
	Writer.Write(Data.Source.Low);
	Writer.Write(static_cast<std::uint32_t>(Data.Boxes.size()));
	for (const FNavigationBox& Box : Data.Boxes)
	{
		WriteBox(Writer, Box);
	}

	Writer.Write(static_cast<std::uint32_t>(Data.Layers.size()));
	for (const FNavigationLayer& Layer : Data.Layers)
	{
		Writer.Write(static_cast<std::uint32_t>(Layer.TileX));
		Writer.Write(static_cast<std::uint32_t>(Layer.TileZ));
		Writer.Write(static_cast<std::uint32_t>(Layer.Data.size()));
		Writer.WriteBytes(Layer.Data);
	}
}

[[nodiscard]] std::expected<FNavigationData, FNavigationError> ReadData(FBinaryReader& Reader)
{
	FNavigationData Data;
	Data.Settings.CellSize = Reader.ReadFloat();
	Data.Settings.CellHeight = Reader.ReadFloat();
	Data.Settings.AgentRadius = Reader.ReadFloat();
	Data.Settings.AgentHeight = Reader.ReadFloat();
	Data.Settings.AgentMaxClimb = Reader.ReadFloat();
	Data.Settings.AgentMaxSlopeRadians = Reader.ReadFloat();
	Data.Settings.TileCells = Reader.Read<std::uint32_t>();
	Data.Source.High = Reader.Read<std::uint64_t>();
	Data.Source.Low = Reader.Read<std::uint64_t>();
	const auto BoxCount = Reader.Read<std::uint32_t>();
	if (!Reader.IsValid() || BoxCount > MaximumBoxes)
	{
		return std::unexpected(FNavigationError{"Navigation data is truncated or has too many boxes"});
	}

	for (std::uint32_t Index = 0; Index < BoxCount; ++Index)
	{
		FNavigationBox& Box = Data.Boxes.emplace_back();
		for (FVector3* const Corner : {&Box.Minimum, &Box.Maximum})
		{
			Corner->X = Reader.ReadFloat();
			Corner->Y = Reader.ReadFloat();
			Corner->Z = Reader.ReadFloat();
		}
	}

	const auto LayerCount = Reader.Read<std::uint32_t>();
	if (!Reader.IsValid() || LayerCount > MaximumLayers)
	{
		return std::unexpected(FNavigationError{"Navigation data is truncated or has too many layers"});
	}

	for (std::uint32_t Index = 0; Index < LayerCount; ++Index)
	{
		FNavigationLayer& Layer = Data.Layers.emplace_back();
		Layer.TileX = static_cast<std::int32_t>(Reader.Read<std::uint32_t>());
		Layer.TileZ = static_cast<std::int32_t>(Reader.Read<std::uint32_t>());
		const auto Size = Reader.Read<std::uint32_t>();
		if (!Reader.IsValid() || Size > MaximumLayerBytes || Size < sizeof(dtTileCacheLayerHeader))
		{
			return std::unexpected(FNavigationError{"Navigation data has a truncated or oversized layer"});
		}

		const std::span<const std::byte> Payload = Reader.ReadBytes(Size);
		if (!Reader.IsValid())
		{
			return std::unexpected(FNavigationError{"Navigation data has a truncated layer"});
		}

		Layer.Data.assign(Payload.begin(), Payload.end());
		dtTileCacheLayerHeader Header{};
		std::memcpy(&Header, Layer.Data.data(), sizeof(Header));
		if (Header.magic != DT_TILECACHE_MAGIC || Header.version != DT_TILECACHE_VERSION || Header.tx != Layer.TileX || Header.ty != Layer.TileZ)
		{
			return std::unexpected(FNavigationError{"Navigation data has a layer that is not a tile cache layer of its tile"});
		}
	}

	if (auto Valid = ValidateNavigationSettings(Data.Settings); !Valid)
	{
		return std::unexpected(Valid.error());
	}

	return Data;
}
}

std::expected<void, FNavigationError> ValidateNavigationSettings(const FNavigationSettings& Settings)
{
	const auto Positive = [](const float Value)
	{
		return std::isfinite(Value) && Value > 0.f;
	};

	if (!Positive(Settings.CellSize) || !Positive(Settings.CellHeight) || !Positive(Settings.AgentRadius) || !Positive(Settings.AgentHeight) || !std::isfinite(Settings.AgentMaxClimb) || Settings.AgentMaxClimb < 0.f || !Positive(Settings.AgentMaxSlopeRadians)
	    || Settings.AgentMaxSlopeRadians >= std::numbers::pi_v<float> * 0.5f || Settings.TileCells < 16)
	{
		return std::unexpected(FNavigationError{"Navigation settings need positive cells and agent sizes, a slope under 90 degrees, and tiles of at least 16 cells"});
	}

	// Layers store their size in a byte, and a tile's build pads it by the agent's radius plus three cells on each side.
	if (Settings.TileCells + 2 * (static_cast<std::uint32_t>(std::ceil(Settings.AgentRadius / Settings.CellSize)) + 3) > 255)
	{
		return std::unexpected(FNavigationError{"Navigation tiles with their border must be at most 255 cells across"});
	}

	return {};
}

std::vector<std::byte> SerializeNavigationFile(const FNavigationFile& File)
{
	FBinaryWriter Writer;
	Writer.Write(NavigationMagic);
	Writer.Write(NavigationDataVersion);
	WriteData(Writer, File.Base);
	Writer.Write(static_cast<std::uint32_t>(File.Patches.size()));
	for (const FNavigationPatch& Patch : File.Patches)
	{
		Writer.Write(Patch.Key.High);
		Writer.Write(Patch.Key.Low);
		WriteData(Writer, Patch.Data);
	}

	return Writer.TakeBytes();
}

std::expected<FNavigationFile, FNavigationError> ParseNavigationFile(const std::span<const std::byte> Bytes)
{
	FBinaryReader Reader(Bytes);
	if (Reader.Read<std::uint32_t>() != NavigationMagic || Reader.Read<std::uint32_t>() != NavigationDataVersion)
	{
		return std::unexpected(FNavigationError{"Not navigation data of a supported version"});
	}

	FNavigationFile File;
	auto Base = ReadData(Reader);
	if (!Base)
	{
		return std::unexpected(Base.error());
	}

	File.Base = std::move(*Base);
	const auto PatchCount = Reader.Read<std::uint32_t>();
	if (!Reader.IsValid() || PatchCount > MaximumPatches)
	{
		return std::unexpected(FNavigationError{"Navigation data is truncated or has too many patches"});
	}

	for (std::uint32_t Index = 0; Index < PatchCount; ++Index)
	{
		FNavigationPatch& Patch = File.Patches.emplace_back();
		Patch.Key.High = Reader.Read<std::uint64_t>();
		Patch.Key.Low = Reader.Read<std::uint64_t>();
		auto Data = ReadData(Reader);
		if (!Data)
		{
			return std::unexpected(Data.error());
		}

		if (!(Data->Settings == File.Base.Settings))
		{
			return std::unexpected(FNavigationError{"A navigation patch was built with other settings than its level"});
		}

		Patch.Data = std::move(*Data);
	}

	if (!Reader.IsValid() || !Reader.IsAtEnd())
	{
		return std::unexpected(FNavigationError{"Navigation data is truncated or has trailing bytes"});
	}

	return File;
}
}
