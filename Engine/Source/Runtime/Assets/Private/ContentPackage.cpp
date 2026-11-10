#include "Herta/Assets/ContentPackage.h"

#include <monocypher-ed25519.h>
#include <monocypher.h>
#include <zstd.h>

#include <algorithm>
#include <bit>
#include <charconv>
#include <cstring>
#include <format>
#include <fstream>
#include <limits>
#include <optional>
#include <system_error>
#include <utility>

namespace Herta
{
namespace
{
constexpr std::uint32_t SignedFlag = 1;
// Header fields, by byte offset.
constexpr std::size_t VersionOffset = 8;
constexpr std::size_t FlagsOffset = 12;
constexpr std::size_t IndexOffsetOffset = 16;
constexpr std::size_t IndexSizeOffset = 24;
constexpr std::size_t EntryCountOffset = 32;
constexpr std::size_t SigningKeyOffset = 36;
constexpr std::size_t IndexHashOffset = 40;
constexpr std::size_t SignatureOffset = 72;
constexpr std::size_t ReservedOffset = 136;
// Path length, codec, offset, stored size, size, and hash after the path.
constexpr std::size_t EntryFixedSize = 2 + 1 + 8 + 8 + 8 + 32;
// A larger index than any real package could need, so a corrupt size cannot make the reader allocate gigabytes.
constexpr std::uint64_t MaximumIndexSize = std::uint64_t{MaximumContentPackageEntries} * (EntryFixedSize + 128);

template <typename T> void Put(std::span<std::byte> Bytes, const std::size_t Offset, const T Value) noexcept
{
	for (std::size_t Index = 0; Index < sizeof(T); ++Index)
	{
		Bytes[Offset + Index] = static_cast<std::byte>(static_cast<std::uint64_t>(Value) >> (8 * Index));
	}
}

template <typename T> [[nodiscard]] T Get(std::span<const std::byte> Bytes, const std::size_t Offset) noexcept
{
	std::uint64_t Value = 0;
	for (std::size_t Index = 0; Index < sizeof(T); ++Index)
	{
		Value |= static_cast<std::uint64_t>(Bytes[Offset + Index]) << (8 * Index);
	}

	return static_cast<T>(Value);
}

[[nodiscard]] FAssetError PackageError(const std::string_view Name, const std::string_view Message)
{
	return FAssetError{std::format("Content package {}: {}", Name, Message)};
}
}

bool IsValidPackagePath(const std::string_view Path) noexcept
{
	if (Path.empty() || Path.size() > MaximumContentPackagePathLength || Path.front() == '/' || Path.back() == '/')
	{
		return false;
	}

	std::size_t Start = 0;
	while (Start <= Path.size())
	{
		const std::size_t End = std::min(Path.find('/', Start), Path.size());
		const std::string_view Part = Path.substr(Start, End - Start);
		if (Part.empty() || Part == "." || Part == "..")
		{
			return false;
		}

		Start = End + 1;
	}

	return std::ranges::none_of(Path, [](const char Character)
	{
		const auto Code = static_cast<unsigned char>(Character);
		return Code < 0x20 || Code == 0x7f || Character == '\\' || Character == ':';
	});
}

FPackageHash HashPackageBytes(const std::span<const std::byte> Bytes) noexcept
{
	FPackageHash Hash{};
	crypto_blake2b(Hash.data(), Hash.size(), reinterpret_cast<const std::uint8_t*>(Bytes.data()), Bytes.size());
	return Hash;
}

std::array<std::byte, ContentPackageHeaderSize> EncodePackageHeader(const FPackageHeader& Header) noexcept
{
	std::array<std::byte, ContentPackageHeaderSize> Bytes{};
	std::ranges::transform(ContentPackageMagic, Bytes.begin(), [](const char Character)
	{
		return static_cast<std::byte>(Character);
	});

	Put(Bytes, VersionOffset, Header.Version);
	Put(Bytes, FlagsOffset, Header.bSigned ? SignedFlag : 0u);
	Put(Bytes, IndexOffsetOffset, Header.IndexOffset);
	Put(Bytes, IndexSizeOffset, Header.IndexSize);
	Put(Bytes, EntryCountOffset, Header.EntryCount);
	Put(Bytes, SigningKeyOffset, Header.SigningKey);
	std::ranges::transform(Header.IndexHash, Bytes.begin() + IndexHashOffset, [](const std::uint8_t Value)
	{
		return static_cast<std::byte>(Value);
	});

	std::ranges::transform(Header.Signature, Bytes.begin() + SignatureOffset, [](const std::uint8_t Value)
	{
		return static_cast<std::byte>(Value);
	});

	return Bytes;
}

std::array<std::byte, ContentPackageHeaderSize> GetPackageSignedBytes(const FPackageHeader& Header) noexcept
{
	FPackageHeader Unsigned = Header;
	Unsigned.Signature = {};
	return EncodePackageHeader(Unsigned);
}

std::vector<std::byte> EncodePackageIndex(const std::span<const FPackageEntry> Entries)
{
	std::vector<std::byte> Bytes;
	for (const FPackageEntry& Entry : Entries)
	{
		const std::size_t Start = Bytes.size();
		Bytes.resize(Start + EntryFixedSize + Entry.Path.size());
		const std::span<std::byte> Out(Bytes.data() + Start, EntryFixedSize + Entry.Path.size());
		Put(Out, 0, static_cast<std::uint16_t>(Entry.Path.size()));
		std::ranges::transform(Entry.Path, Out.begin() + 2, [](const char Character)
		{
			return static_cast<std::byte>(Character);
		});

		std::size_t Offset = 2 + Entry.Path.size();
		Put(Out, Offset, static_cast<std::uint8_t>(Entry.Codec));
		Put(Out, Offset + 1, Entry.Offset);
		Put(Out, Offset + 9, Entry.StoredSize);
		Put(Out, Offset + 17, Entry.Size);
		Offset += 25;
		std::ranges::transform(Entry.Hash, Out.begin() + static_cast<std::ptrdiff_t>(Offset), [](const std::uint8_t Value)
		{
			return static_cast<std::byte>(Value);
		});
	}

	return Bytes;
}

std::expected<FPackageHeader, FAssetError> DecodePackageHeader(const std::span<const std::byte> Bytes)
{
	if (Bytes.size() < ContentPackageHeaderSize || !std::ranges::equal(Bytes.first(ContentPackageMagic.size()), ContentPackageMagic, [](const std::byte Byte, const char Character)
	{
		return Byte == static_cast<std::byte>(Character);
	}))
	{
		return std::unexpected(FAssetError{"not a Herta content package"});
	}

	FPackageHeader Header;
	Header.Version = Get<std::uint32_t>(Bytes, VersionOffset);
	const auto Flags = Get<std::uint32_t>(Bytes, FlagsOffset);
	Header.bSigned = (Flags & SignedFlag) != 0;
	Header.IndexOffset = Get<std::uint64_t>(Bytes, IndexOffsetOffset);
	Header.IndexSize = Get<std::uint64_t>(Bytes, IndexSizeOffset);
	Header.EntryCount = Get<std::uint32_t>(Bytes, EntryCountOffset);
	Header.SigningKey = Get<std::uint32_t>(Bytes, SigningKeyOffset);
	for (std::size_t Index = 0; Index < Header.IndexHash.size(); ++Index)
	{
		Header.IndexHash[Index] = static_cast<std::uint8_t>(Bytes[IndexHashOffset + Index]);
	}

	for (std::size_t Index = 0; Index < Header.Signature.size(); ++Index)
	{
		Header.Signature[Index] = static_cast<std::uint8_t>(Bytes[SignatureOffset + Index]);
	}

	const bool bReservedClear = std::ranges::all_of(Bytes.subspan(ReservedOffset, ContentPackageHeaderSize - ReservedOffset), [](const std::byte Byte)
	{
		return Byte == std::byte{0};
	});

	if (Header.Version != ContentPackageFormatVersion)
	{
		return std::unexpected(FAssetError{std::format("format version {} is not the supported {}", Header.Version, ContentPackageFormatVersion)});
	}

	if ((Flags & ~SignedFlag) != 0 || !bReservedClear || Header.EntryCount > MaximumContentPackageEntries || Header.IndexSize > MaximumIndexSize || Header.IndexOffset < ContentPackageHeaderSize)
	{
		return std::unexpected(FAssetError{"the header is corrupt"});
	}

	if (!Header.bSigned && (Header.SigningKey != 0 || std::ranges::any_of(Header.Signature, [](const std::uint8_t Value)
	{
		return Value != 0;
	})))
	{
		return std::unexpected(FAssetError{"an unsigned header carries a signature"});
	}

	return Header;
}

std::expected<std::vector<FPackageEntry>, FAssetError> DecodePackageIndex(const std::span<const std::byte> Bytes, const std::uint32_t EntryCount, const std::uint64_t DataEnd)
{
	std::vector<FPackageEntry> Entries;
	Entries.reserve(std::min<std::size_t>(EntryCount, Bytes.size() / EntryFixedSize));
	std::size_t Cursor = 0;
	std::uint64_t ChunkEnd = ContentPackageHeaderSize;
	for (std::uint32_t Index = 0; Index < EntryCount; ++Index)
	{
		if (Bytes.size() - Cursor < 2)
		{
			return std::unexpected(FAssetError{"the index is truncated"});
		}

		const auto PathLength = Get<std::uint16_t>(Bytes, Cursor);
		if (Bytes.size() - Cursor < EntryFixedSize + PathLength)
		{
			return std::unexpected(FAssetError{"the index is truncated"});
		}

		FPackageEntry Entry;
		Entry.Path.resize(PathLength);
		std::ranges::transform(Bytes.subspan(Cursor + 2, PathLength), Entry.Path.begin(), [](const std::byte Byte)
		{
			return static_cast<char>(Byte);
		});

		std::size_t Offset = Cursor + 2 + PathLength;
		const auto Codec = Get<std::uint8_t>(Bytes, Offset);
		Entry.Offset = Get<std::uint64_t>(Bytes, Offset + 1);
		Entry.StoredSize = Get<std::uint64_t>(Bytes, Offset + 9);
		Entry.Size = Get<std::uint64_t>(Bytes, Offset + 17);
		Offset += 25;
		for (std::size_t Hash = 0; Hash < Entry.Hash.size(); ++Hash)
		{
			Entry.Hash[Hash] = static_cast<std::uint8_t>(Bytes[Offset + Hash]);
		}

		Cursor = Offset + Entry.Hash.size();
		if (!IsValidPackagePath(Entry.Path) || (!Entries.empty() && Entries.back().Path >= Entry.Path))
		{
			return std::unexpected(FAssetError{std::format("entry {} has an invalid, duplicate, or unsorted path", Index)});
		}

		// Chunks follow each other in index order, so each must start after the last ends and stay before the index.
		const bool bCodecValid = Codec <= static_cast<std::uint8_t>(EPackageCodec::Zstd);
		const bool bSizesValid = Entry.Size <= MaximumContentPackageEntrySize && Entry.StoredSize <= MaximumContentPackageEntrySize && (Codec != static_cast<std::uint8_t>(EPackageCodec::None) || Entry.StoredSize == Entry.Size);
		if (!bCodecValid || !bSizesValid || Entry.Offset < ChunkEnd || Entry.Offset > DataEnd || Entry.StoredSize > DataEnd - Entry.Offset)
		{
			return std::unexpected(FAssetError{std::format("entry {} has an unknown codec or a chunk outside the package", Entry.Path)});
		}

		Entry.Codec = static_cast<EPackageCodec>(Codec);
		ChunkEnd = Entry.Offset + Entry.StoredSize;
		Entries.push_back(std::move(Entry));
	}

	if (Cursor != Bytes.size())
	{
		return std::unexpected(FAssetError{"the index has trailing bytes"});
	}

	return Entries;
}

std::string FormatPackagePublicKey(const FPackagePublicKey& Key)
{
	std::string Text = std::format("{}:", Key.Id);
	for (const std::uint8_t Value : Key.Key)
	{
		Text += std::format("{:02x}", Value);
	}

	return Text;
}

std::expected<std::vector<FPackagePublicKey>, FAssetError> ParsePackagePublicKeys(const std::string_view Text)
{
	std::vector<FPackagePublicKey> Keys;
	std::size_t Start = 0;
	while (Start < Text.size())
	{
		const std::size_t End = std::min(Text.find_first_of(", \t\r\n", Start), Text.size());
		const std::string_view Item = Text.substr(Start, End - Start);
		Start = End + 1;
		if (Item.empty())
		{
			continue;
		}

		const std::size_t Colon = Item.find(':');
		FPackagePublicKey Key;
		const std::string_view Id = Colon == std::string_view::npos ? std::string_view{} : Item.substr(0, Colon);
		const std::string_view Hex = Colon == std::string_view::npos ? std::string_view{} : Item.substr(Colon + 1);
		const auto [IdEnd, IdError] = std::from_chars(Id.data(), Id.data() + Id.size(), Key.Id);
		bool bValid = !Id.empty() && IdError == std::errc{} && IdEnd == Id.data() + Id.size() && Hex.size() == Key.Key.size() * 2;
		for (std::size_t Index = 0; bValid && Index < Key.Key.size(); ++Index)
		{
			const auto [ByteEnd, ByteError] = std::from_chars(Hex.data() + Index * 2, Hex.data() + Index * 2 + 2, Key.Key[Index], 16);
			bValid = ByteError == std::errc{} && ByteEnd == Hex.data() + Index * 2 + 2;
		}

		if (!bValid || std::ranges::contains(Keys, Key.Id, &FPackagePublicKey::Id))
		{
			return std::unexpected(FAssetError{std::format("'{}' is not a package public key \"<id>:<64 hex digits>\" with a unique id", Item)});
		}

		Keys.push_back(Key);
	}

	return Keys;
}

struct FContentPackage::FImplementation
{
	[[nodiscard]] std::expected<std::vector<std::byte>, FAssetError> ReadRange(const std::uint64_t Offset, const std::uint64_t Size) const
	{
		if (Offset > Length || Size > Length - Offset)
		{
			return std::unexpected(PackageError(Name, "a read runs past its end"));
		}

		std::vector<std::byte> Bytes(static_cast<std::size_t>(Size));
		if (!Memory.empty())
		{
			std::memcpy(Bytes.data(), Memory.data() + Offset, Bytes.size());
			return Bytes;
		}

		// Each read opens its own stream, so reads on several threads never share a position.
		std::ifstream Stream(File, std::ios::binary);
		if (!Stream || !Stream.seekg(static_cast<std::streamoff>(Base + Offset)) || !Stream.read(reinterpret_cast<char*>(Bytes.data()), static_cast<std::streamsize>(Bytes.size())))
		{
			return std::unexpected(PackageError(Name, "cannot be read"));
		}

		return Bytes;
	}

	std::string Name;
	std::filesystem::path File;
	std::uint64_t Base = 0;
	std::uint64_t Length = 0;
	std::span<const std::byte> Memory;
	FPackageHeader Header;
	std::vector<FPackageEntry> Entries;
	bool bTrusted = false;
};

std::expected<std::unique_ptr<FContentPackage::FImplementation>, FAssetError> FContentPackage::Verify(std::unique_ptr<FImplementation> Package, const FPackageOpenOptions& Options)
{
	const std::string& Name = Package->Name;
	auto HeaderBytes = Package->ReadRange(0, std::min<std::uint64_t>(ContentPackageHeaderSize, Package->Length));
	if (!HeaderBytes)
	{
		return std::unexpected(HeaderBytes.error());
	}

	auto Header = DecodePackageHeader(*HeaderBytes);
	if (!Header)
	{
		return std::unexpected(PackageError(Name, Header.error().Message));
	}

	if (Header->IndexOffset > Package->Length || Header->IndexSize != Package->Length - Header->IndexOffset)
	{
		return std::unexpected(PackageError(Name, "the index does not end the package"));
	}

	// The signature is checked before the index is trusted, then the index against the hash it signs.
	const auto Key = std::ranges::find(Options.TrustedKeys, Header->SigningKey, &FPackagePublicKey::Id);
	const bool bKnownKey = Header->bSigned && Key != Options.TrustedKeys.end();
	if (bKnownKey)
	{
		const auto Signed = GetPackageSignedBytes(*Header);
		if (crypto_ed25519_check(Header->Signature.data(), Key->Key.data(), reinterpret_cast<const std::uint8_t*>(Signed.data()), Signed.size()) != 0)
		{
			return std::unexpected(PackageError(Name, "its signature does not verify"));
		}
	}
	else if (Options.bRequireSignature)
	{
		return std::unexpected(PackageError(Name, Header->bSigned ? std::format("it is signed with key {}, which is not trusted", Header->SigningKey) : std::string("it is not signed")));
	}

	auto IndexBytes = Package->ReadRange(Header->IndexOffset, Header->IndexSize);
	if (!IndexBytes)
	{
		return std::unexpected(IndexBytes.error());
	}

	if (HashPackageBytes(*IndexBytes) != Header->IndexHash)
	{
		return std::unexpected(PackageError(Name, "its index does not match its hash"));
	}

	auto Entries = DecodePackageIndex(*IndexBytes, Header->EntryCount, Header->IndexOffset);
	if (!Entries)
	{
		return std::unexpected(PackageError(Name, Entries.error().Message));
	}

	Package->Header = *Header;
	Package->Entries = std::move(*Entries);
	Package->bTrusted = bKnownKey;
	return Package;
}

std::expected<std::unique_ptr<FContentPackage::FImplementation>, FAssetError> FContentPackage::OpenFile(const std::filesystem::path& File, const std::uint64_t Offset, const std::optional<std::uint64_t> Size)
{
	auto Package = std::make_unique<FImplementation>();
	Package->Name = File.generic_string();
	std::error_code Error;
	const std::uintmax_t FileSize = std::filesystem::file_size(File, Error);
	if (Error)
	{
		return std::unexpected(PackageError(Package->Name, "cannot be opened"));
	}

	if (Offset > FileSize || (Size && *Size > FileSize - Offset))
	{
		return std::unexpected(PackageError(Package->Name, "lies outside its file"));
	}

	Package->File = File;
	Package->Base = Offset;
	Package->Length = Size.value_or(FileSize - Offset);
	return Package;
}

FContentPackage::FContentPackage(std::unique_ptr<FImplementation> InImplementation) noexcept
    : Implementation(std::move(InImplementation))
{
}

FContentPackage::~FContentPackage() = default;

std::expected<std::unique_ptr<FContentPackage>, FAssetError> FContentPackage::Open(const std::filesystem::path& File, const FPackageOpenOptions& Options)
{
	return OpenFile(File, 0, std::nullopt).and_then([&Options](auto Package)
	{
		return Verify(std::move(Package), Options);
	}).transform([](auto Package)
	{
		return std::unique_ptr<FContentPackage>(new FContentPackage(std::move(Package)));
	});
}

std::expected<std::unique_ptr<FContentPackage>, FAssetError> FContentPackage::OpenRange(const std::filesystem::path& File, const std::uint64_t Offset, const std::uint64_t Size, const FPackageOpenOptions& Options)
{
	return OpenFile(File, Offset, Size).and_then([&Options](auto Package)
	{
		return Verify(std::move(Package), Options);
	}).transform([](auto Package)
	{
		return std::unique_ptr<FContentPackage>(new FContentPackage(std::move(Package)));
	});
}

std::expected<std::unique_ptr<FContentPackage>, FAssetError> FContentPackage::OpenMemory(const std::span<const std::byte> Bytes, const FPackageOpenOptions& Options, std::string Name)
{
	auto Package = std::make_unique<FImplementation>();
	Package->Name = std::move(Name);
	Package->Memory = Bytes;
	Package->Length = Bytes.size();
	return Verify(std::move(Package), Options).transform([](auto Verified)
	{
		return std::unique_ptr<FContentPackage>(new FContentPackage(std::move(Verified)));
	});
}

const FPackageEntry* FContentPackage::Find(const std::string_view Path) const noexcept
{
	const auto Found = std::ranges::lower_bound(Implementation->Entries, Path, {}, &FPackageEntry::Path);
	return Found != Implementation->Entries.end() && Found->Path == Path ? &*Found : nullptr;
}

std::expected<std::vector<std::byte>, FAssetError> FContentPackage::Read(const std::string_view Path) const
{
	const FPackageEntry* const Entry = Find(Path);
	if (!Entry)
	{
		return std::unexpected(PackageError(Implementation->Name, std::format("has no {}", Path)));
	}

	auto Stored = Implementation->ReadRange(Entry->Offset, Entry->StoredSize);
	if (!Stored)
	{
		return Stored;
	}

	// The hash is checked before decompressing, so a tampered chunk never reaches the decompressor.
	if (HashPackageBytes(*Stored) != Entry->Hash)
	{
		return std::unexpected(PackageError(Implementation->Name, std::format("{} does not match its hash", Path)));
	}

	if (Entry->Codec == EPackageCodec::None)
	{
		return Stored;
	}

	const unsigned long long FrameSize = ZSTD_getFrameContentSize(Stored->data(), Stored->size());
	if (FrameSize != Entry->Size)
	{
		return std::unexpected(PackageError(Implementation->Name, std::format("{} does not decompress to its size", Path)));
	}

	std::vector<std::byte> Bytes(static_cast<std::size_t>(Entry->Size));
	const std::size_t Written = ZSTD_decompress(Bytes.data(), Bytes.size(), Stored->data(), Stored->size());
	if (ZSTD_isError(Written) || Written != Bytes.size())
	{
		return std::unexpected(PackageError(Implementation->Name, std::format("{} cannot be decompressed", Path)));
	}

	return Bytes;
}

std::span<const FPackageEntry> FContentPackage::GetEntries() const noexcept
{
	return Implementation->Entries;
}

const FPackageHeader& FContentPackage::GetHeader() const noexcept
{
	return Implementation->Header;
}

bool FContentPackage::IsTrusted() const noexcept
{
	return Implementation->bTrusted;
}

const std::string& FContentPackage::GetName() const noexcept
{
	return Implementation->Name;
}
}
