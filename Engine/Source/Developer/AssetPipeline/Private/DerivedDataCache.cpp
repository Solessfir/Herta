#include "Herta/AssetPipeline/DerivedDataCache.h"

#include "FileUtilities.h"

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

void AppendInteger(std::vector<std::byte>& Bytes, const std::uint64_t Value, const std::size_t Size)
{
	for (std::size_t Index = 0; Index < Size; ++Index)
	{
		Bytes.push_back(static_cast<std::byte>((Value >> (Index * 8)) & 0xff));
	}
}

[[nodiscard]] std::uint64_t ReadInteger(const std::span<const std::byte> Bytes, const std::size_t Offset, const std::size_t Size) noexcept
{
	std::uint64_t Value = 0;
	for (std::size_t Index = 0; Index < Size; ++Index)
	{
		Value |= static_cast<std::uint64_t>(Bytes[Offset + Index]) << (Index * 8);
	}
	return Value;
}

void AppendHash(std::vector<std::byte>& Bytes, const FHash128& Hash)
{
	AppendInteger(Bytes, Hash.High, 8);
	AppendInteger(Bytes, Hash.Low, 8);
}

[[nodiscard]] FHash128 ReadHash(const std::span<const std::byte> Bytes, const std::size_t Offset) noexcept
{
	return FHash128{ReadInteger(Bytes, Offset, 8), ReadInteger(Bytes, Offset + 8, 8)};
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
	if (ReadHash(Bytes, Bytes.size() - ChecksumSize) != HashBytes(Bytes.first(Bytes.size() - ChecksumSize)))
	{
		return Corrupt("checksum mismatch");
	}
	if (ReadInteger(Bytes, 0, 4) != EntryMagic || ReadInteger(Bytes, 4, 4) != EntryVersion)
	{
		return Corrupt("unsupported format or version");
	}
	if (ReadHash(Bytes, 8) != Key)
	{
		return Corrupt("key mismatch");
	}
	if (ReadInteger(Bytes, 24, 8) != Bytes.size() - HeaderSize - ChecksumSize)
	{
		return Corrupt("payload size mismatch");
	}

	return std::optional<std::vector<std::byte>>(std::vector<std::byte>(Bytes.begin() + HeaderSize, Bytes.end() - ChecksumSize));
}

std::expected<void, FAssetError> FDerivedDataCache::Put(const FHash128& Key, const std::span<const std::byte> Payload) const
{
	if (Payload.size() > MaximumPayloadSize)
	{
		return std::unexpected(FAssetError{"Derived data payload exceeds 4 GiB"});
	}

	std::vector<std::byte> Bytes;
	Bytes.reserve(HeaderSize + Payload.size() + ChecksumSize);
	AppendInteger(Bytes, EntryMagic, 4);
	AppendInteger(Bytes, EntryVersion, 4);
	AppendHash(Bytes, Key);
	AppendInteger(Bytes, Payload.size(), 8);
	Bytes.insert(Bytes.end(), Payload.begin(), Payload.end());
	AppendHash(Bytes, HashBytes(Bytes));

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
