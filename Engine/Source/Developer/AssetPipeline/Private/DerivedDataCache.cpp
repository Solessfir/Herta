#include "Herta/AssetPipeline/DerivedDataCache.h"

#include "FileUtilities.h"
#include "Herta/Core/BinaryStream.h"

#include <algorithm>
#include <cstdint>
#include <format>
#include <string>

namespace Herta
{
namespace
{
inline constexpr std::uint32_t EntryMagic = 0x43444448; // "HDDC"
inline constexpr std::uint32_t EntryVersion = 1;
inline constexpr std::size_t HeaderSize = 4 + 4 + 16 + 8;
inline constexpr std::size_t ChecksumSize = 16;
inline constexpr std::uint64_t MaximumPayloadSize = 4ull * 1024 * 1024 * 1024;

void WriteHash(FBinaryWriter& Writer, const FHash128& Hash)
{
	Writer.Write(Hash.High);
	Writer.Write(Hash.Low);
}

[[nodiscard]] FHash128 ReadHash(FBinaryReader& Reader) noexcept
{
	const auto High = Reader.Read<std::uint64_t>();
	return FHash128{High, Reader.Read<std::uint64_t>()};
}
}

FDerivedDataCache::FDerivedDataCache(std::filesystem::path InRoot)
    : Root(std::move(InRoot))
{
}

std::filesystem::path FDerivedDataCache::GetEntryPath(const FHash128& Key) const
{
	const std::string Name = ToString(Key);
	// Two-character fan-out keeps directories small enough for fast enumeration.
	return Root / Name.substr(0, 2) / (Name + ".hddc");
}

std::expected<std::optional<std::vector<std::byte>>, FAssetError> FDerivedDataCache::Get(const FHash128& Key) const
{
	const std::filesystem::path Path = GetEntryPath(Key);
	std::expected<std::optional<std::vector<std::byte>>, FAssetError> File = ReadWholeFile(Path, HeaderSize + MaximumPayloadSize + ChecksumSize);
	if (!File || !*File)
	{
		return File;
	}

	const std::span<const std::byte> Bytes = **File;
	const auto Corrupt = [&Path](const std::string_view Reason)
	{
		return std::unexpected(FAssetError{std::format("Corrupt derived data '{}': {}", PathToUtf8(Path), Reason)});
	};
	if (Bytes.size() < HeaderSize + ChecksumSize)
	{
		return Corrupt("truncated header");
	}
	FBinaryReader Checksum(Bytes.last(ChecksumSize));
	if (ReadHash(Checksum) != HashBytes(Bytes.first(Bytes.size() - ChecksumSize)))
	{
		return Corrupt("checksum mismatch");
	}

	FBinaryReader Reader(Bytes.first(Bytes.size() - ChecksumSize));
	if (Reader.Read<std::uint32_t>() != EntryMagic || Reader.Read<std::uint32_t>() != EntryVersion)
	{
		return Corrupt("unsupported format or version");
	}
	if (ReadHash(Reader) != Key)
	{
		return Corrupt("key mismatch");
	}
	const auto PayloadSize = Reader.Read<std::uint64_t>();
	const std::span<const std::byte> Payload = Reader.ReadBytes(PayloadSize <= MaximumPayloadSize ? static_cast<std::size_t>(PayloadSize) : 0);
	if (!Reader.IsValid() || !Reader.IsAtEnd())
	{
		return Corrupt("payload size mismatch");
	}
	return std::optional<std::vector<std::byte>>(std::vector<std::byte>(Payload.begin(), Payload.end()));
}

std::expected<void, FAssetError> FDerivedDataCache::Put(const FHash128& Key, const std::span<const std::byte> Payload) const
{
	if (Payload.size() > MaximumPayloadSize)
	{
		return std::unexpected(FAssetError{"Derived data payload exceeds 4 GiB"});
	}

	FBinaryWriter Writer;
	Writer.Write(EntryMagic);
	Writer.Write(EntryVersion);
	WriteHash(Writer, Key);
	Writer.Write(static_cast<std::uint64_t>(Payload.size()));
	Writer.WriteBytes(Payload);
	WriteHash(Writer, HashBytes(Writer.GetBytes()));
	const std::vector<std::byte> Bytes = Writer.TakeBytes();

	const std::filesystem::path Path = GetEntryPath(Key);
	std::expected<void, FAssetError> Written = WriteFileAtomically(Path, Bytes);
	if (!Written)
	{
		// Windows refuses to replace a file another process is reading. The key fixes the content, so a valid existing entry is a success.
		std::expected<std::optional<std::vector<std::byte>>, FAssetError> Existing = Get(Key);
		if (Existing && *Existing && std::ranges::equal(**Existing, Payload))
		{
			return {};
		}
	}
	return Written;
}
}
