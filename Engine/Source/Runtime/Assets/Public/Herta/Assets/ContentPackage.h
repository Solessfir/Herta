#pragma once

#include "Herta/Assets/AssetRegistry.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Herta
{
// A content package holds a staged build's files in one archive: a fixed header, the files' chunks, then an index sorted by path. Each chunk is
// stored whole or as one Zstandard frame and named in the index by its path, sizes, and the BLAKE2b hash of its stored bytes. The header holds
// the index's hash and, when signed, an Ed25519 signature over the header, so the signature covers every chunk through the index.
inline constexpr std::array<char, 8> ContentPackageMagic{'H', 'E', 'R', 'T', 'A', 'P', 'A', 'K'};
inline constexpr std::uint32_t ContentPackageFormatVersion = 1;
inline constexpr std::size_t ContentPackageHeaderSize = 144;
inline constexpr std::size_t ContentPackageChunkAlignment = 16;
// Larger than any staged file: cooked assets are capped at 1 GiB.
inline constexpr std::uint64_t MaximumContentPackageEntrySize = std::uint64_t{1} << 30;
inline constexpr std::uint32_t MaximumContentPackageEntries = std::uint32_t{1} << 20;
inline constexpr std::size_t MaximumContentPackagePathLength = 512;

using FPackageHash = std::array<std::uint8_t, 32>;
using FPackageSignature = std::array<std::uint8_t, 64>;

enum class EPackageCodec : std::uint8_t
{
	None,
	Zstd,
};

struct FPackageEntry
{
	bool operator==(const FPackageEntry&) const = default;

	std::string Path{};
	EPackageCodec Codec = EPackageCodec::None;
	std::uint64_t Offset = 0;
	std::uint64_t StoredSize = 0;
	std::uint64_t Size = 0;
	FPackageHash Hash{};
};

struct FPackageHeader
{
	bool operator==(const FPackageHeader&) const = default;

	std::uint32_t Version = ContentPackageFormatVersion;
	bool bSigned = false;
	std::uint64_t IndexOffset = 0;
	std::uint64_t IndexSize = 0;
	std::uint32_t EntryCount = 0;
	// Which publisher key signed it, so keys can be rotated: players trust every version they ship with.
	std::uint32_t SigningKey = 0;
	FPackageHash IndexHash{};
	FPackageSignature Signature{};
};

struct FPackagePublicKey
{
	bool operator==(const FPackagePublicKey&) const = default;

	std::uint32_t Id = 0;
	std::array<std::uint8_t, 32> Key{};
};

struct FPackageOpenOptions
{
	// Keys whose signatures the package may carry. A signed package naming one of them must verify against it.
	std::span<const FPackagePublicKey> TrustedKeys{};
	// Refuse packages not signed by a trusted key, as a Shipping game does with its base content.
	bool bRequireSignature = false;
};

// Paths are relative and '/'-separated, with no empty, '.', or '..' parts, backslashes, drive colons, or control characters.
[[nodiscard]] bool IsValidPackagePath(std::string_view Path) noexcept;
// BLAKE2b with a 32-byte digest.
[[nodiscard]] FPackageHash HashPackageBytes(std::span<const std::byte> Bytes) noexcept;
[[nodiscard]] std::array<std::byte, ContentPackageHeaderSize> EncodePackageHeader(const FPackageHeader& Header) noexcept;
// What a signature signs: the header with its signature zeroed.
[[nodiscard]] std::array<std::byte, ContentPackageHeaderSize> GetPackageSignedBytes(const FPackageHeader& Header) noexcept;
[[nodiscard]] std::vector<std::byte> EncodePackageIndex(std::span<const FPackageEntry> Entries);
[[nodiscard]] std::expected<FPackageHeader, FAssetError> DecodePackageHeader(std::span<const std::byte> Bytes);
// Entries must be sorted by path without duplicates, and their chunks must lie between the header and DataEnd without overlapping.
[[nodiscard]] std::expected<std::vector<FPackageEntry>, FAssetError> DecodePackageIndex(std::span<const std::byte> Bytes, std::uint32_t EntryCount, std::uint64_t DataEnd);
// Public keys are written "<id>:<64 hex digits>"; a list separates them with commas, spaces, or line breaks.
[[nodiscard]] std::string FormatPackagePublicKey(const FPackagePublicKey& Key);
[[nodiscard]] std::expected<std::vector<FPackagePublicKey>, FAssetError> ParsePackagePublicKeys(std::string_view Text);

// An opened package. Its header, index, and signature are verified on opening; each chunk's hash is verified when it is read, before it is
// decompressed. Reads may run on any thread.
class FContentPackage final
{
public:
	[[nodiscard]] static std::expected<std::unique_ptr<FContentPackage>, FAssetError> Open(const std::filesystem::path& File, const FPackageOpenOptions& Options = {});
	// A package stored inside a larger file, such as a section of an executable.
	[[nodiscard]] static std::expected<std::unique_ptr<FContentPackage>, FAssetError> OpenRange(const std::filesystem::path& File, std::uint64_t Offset, std::uint64_t Size, const FPackageOpenOptions& Options = {});
	// A package in memory that outlives it, such as a resource mapped with the executable.
	[[nodiscard]] static std::expected<std::unique_ptr<FContentPackage>, FAssetError> OpenMemory(std::span<const std::byte> Bytes, const FPackageOpenOptions& Options = {});

	~FContentPackage();

	FContentPackage(const FContentPackage&) = delete;
	FContentPackage& operator=(const FContentPackage&) = delete;
	FContentPackage(FContentPackage&&) = delete;
	FContentPackage& operator=(FContentPackage&&) = delete;

	[[nodiscard]] const FPackageEntry* Find(std::string_view Path) const noexcept;
	[[nodiscard]] std::expected<std::vector<std::byte>, FAssetError> Read(std::string_view Path) const;
	[[nodiscard]] std::span<const FPackageEntry> GetEntries() const noexcept;
	[[nodiscard]] const FPackageHeader& GetHeader() const noexcept;
	// Signed by one of the trusted keys it was opened with.
	[[nodiscard]] bool IsTrusted() const noexcept;
	// Where it was read from, for diagnostics.
	[[nodiscard]] const std::string& GetName() const noexcept;

private:
	struct FImplementation;
	explicit FContentPackage(std::unique_ptr<FImplementation> InImplementation) noexcept;
	[[nodiscard]] static std::expected<std::unique_ptr<FImplementation>, FAssetError> OpenFile(const std::filesystem::path& File, std::uint64_t Offset, std::optional<std::uint64_t> Size);
	// Checks the header, the signature against the trusted keys, and the index against its hash.
	[[nodiscard]] static std::expected<std::unique_ptr<FImplementation>, FAssetError> Verify(std::unique_ptr<FImplementation> Package, const FPackageOpenOptions& Options);

	std::unique_ptr<FImplementation> Implementation;
};
}
