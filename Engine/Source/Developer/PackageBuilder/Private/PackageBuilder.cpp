#include "Herta/PackageBuilder/PackageBuilder.h"

#include "Herta/Platform/Platform.h"
#include "Herta/Platform/Process.h"

#include <monocypher-ed25519.h>
#include <monocypher.h>
#include <zstd.h>

#ifdef HERTA_PLATFORM_WINDOWS
	#include <windows.h>
#endif

#include <algorithm>
#include <charconv>
#include <format>
#include <fstream>
#include <memory>
#include <system_error>
#include <utility>

namespace Herta
{
namespace
{
[[nodiscard]] std::unexpected<FPackageBuildError> BuildError(std::string Message)
{
	return std::unexpected(FPackageBuildError{std::move(Message)});
}

struct FCompressorDeleter
{
	void operator()(ZSTD_CCtx* Context) const noexcept
	{
		ZSTD_freeCCtx(Context);
	}
};

// One Zstandard frame with its content size and no checksum, since the package hashes every chunk; single-threaded, so the bytes never
// depend on how work was split.
[[nodiscard]] std::expected<std::vector<std::byte>, FPackageBuildError> Compress(ZSTD_CCtx& Context, const std::span<const std::byte> Bytes, const int Level)
{
	ZSTD_CCtx_reset(&Context, ZSTD_reset_session_and_parameters);
	ZSTD_CCtx_setParameter(&Context, ZSTD_c_compressionLevel, Level);
	ZSTD_CCtx_setParameter(&Context, ZSTD_c_contentSizeFlag, 1);
	ZSTD_CCtx_setParameter(&Context, ZSTD_c_checksumFlag, 0);
	ZSTD_CCtx_setParameter(&Context, ZSTD_c_dictIDFlag, 0);
	std::vector<std::byte> Compressed(ZSTD_compressBound(Bytes.size()));
	const std::size_t Size = ZSTD_compress2(&Context, Compressed.data(), Compressed.size(), Bytes.data(), Bytes.size());
	if (ZSTD_isError(Size))
	{
		return BuildError(std::format("Zstandard failed: {}", ZSTD_getErrorName(Size)));
	}

	Compressed.resize(Size);
	return Compressed;
}

[[nodiscard]] std::string ToHex(const std::span<const std::uint8_t> Bytes)
{
	std::string Text;
	for (const std::uint8_t Value : Bytes)
	{
		Text += std::format("{:02x}", Value);
	}

	return Text;
}
}

std::expected<std::vector<FPackageInput>, FPackageBuildError> CollectPackageInputs(const std::filesystem::path& Root)
{
	std::vector<FPackageInput> Inputs;
	std::error_code Error;
	for (auto Iterator = std::filesystem::recursive_directory_iterator(Root, Error); !Error && Iterator != std::filesystem::recursive_directory_iterator(); Iterator.increment(Error))
	{
		if (!Iterator->is_regular_file(Error))
		{
			continue;
		}

		const std::u8string Relative = std::filesystem::relative(Iterator->path(), Root, Error).generic_u8string();
		FPackageInput Input{.Path = std::string(Relative.begin(), Relative.end())};
		std::ifstream File(Iterator->path(), std::ios::binary | std::ios::ate);
		const auto Size = File ? static_cast<std::streamoff>(File.tellg()) : std::streamoff{-1};
		if (Error || Size < 0 || static_cast<std::uint64_t>(Size) > MaximumContentPackageEntrySize)
		{
			return BuildError(std::format("Cannot read {} for packaging", Iterator->path().generic_string()));
		}

		Input.Bytes.resize(static_cast<std::size_t>(Size));
		File.seekg(0);
		if (!File.read(reinterpret_cast<char*>(Input.Bytes.data()), Size))
		{
			return BuildError(std::format("Cannot read {} for packaging", Iterator->path().generic_string()));
		}

		Inputs.push_back(std::move(Input));
	}

	if (Error)
	{
		return BuildError(std::format("Cannot list {}: {}", Root.generic_string(), Error.message()));
	}

	std::ranges::sort(Inputs, {}, &FPackageInput::Path);
	return Inputs;
}

// ponytail: builds the whole package in memory, fine for the Sandbox's 10 MB; stream chunks to the file once a game's packages outgrow memory.
std::expected<std::vector<std::byte>, FPackageBuildError> BuildPackage(std::vector<FPackageInput> Inputs, const FPackageBuildOptions& Options)
{
	std::ranges::sort(Inputs, {}, &FPackageInput::Path);
	for (std::size_t Index = 0; Index < Inputs.size(); ++Index)
	{
		if (!IsValidPackagePath(Inputs[Index].Path) || (Index > 0 && Inputs[Index - 1].Path == Inputs[Index].Path))
		{
			return BuildError(std::format("'{}' is not a valid, unique package path", Inputs[Index].Path));
		}

		if (Inputs[Index].Bytes.size() > MaximumContentPackageEntrySize)
		{
			return BuildError(std::format("{} is larger than a package entry may be", Inputs[Index].Path));
		}
	}

	if (Inputs.size() > MaximumContentPackageEntries)
	{
		return BuildError("A package holds at most 1048576 files");
	}

	const std::unique_ptr<ZSTD_CCtx, FCompressorDeleter> Context(ZSTD_createCCtx());
	if (!Context)
	{
		return BuildError("Cannot create a Zstandard compressor");
	}

	std::vector<std::byte> Package(ContentPackageHeaderSize);
	std::vector<FPackageEntry> Entries;
	Entries.reserve(Inputs.size());
	for (const FPackageInput& Input : Inputs)
	{
		auto Compressed = Compress(*Context, Input.Bytes, Options.CompressionLevel);
		if (!Compressed)
		{
			return std::unexpected(Compressed.error());
		}

		// Data that does not shrink, such as already compressed fonts or audio, is stored whole.
		const bool bCompress = Compressed->size() < Input.Bytes.size();
		const std::span<const std::byte> Stored = bCompress ? std::span<const std::byte>(*Compressed) : std::span<const std::byte>(Input.Bytes);
		Package.resize((Package.size() + ContentPackageChunkAlignment - 1) / ContentPackageChunkAlignment * ContentPackageChunkAlignment);
		Entries.push_back({
		    .Path = Input.Path,
		    .Codec = bCompress ? EPackageCodec::Zstd : EPackageCodec::None,
		    .Offset = Package.size(),
		    .StoredSize = Stored.size(),
		    .Size = Input.Bytes.size(),
		    .Hash = HashPackageBytes(Stored),
		});
		Package.insert(Package.end(), Stored.begin(), Stored.end());
	}

	const std::vector<std::byte> Index = EncodePackageIndex(Entries);
	FPackageHeader Header{.IndexOffset = Package.size(), .IndexSize = Index.size(), .EntryCount = static_cast<std::uint32_t>(Entries.size()), .IndexHash = HashPackageBytes(Index)};
	if (Options.SigningKey)
	{
		Header.bSigned = true;
		Header.SigningKey = Options.SigningKey->Id;
		const auto Signed = GetPackageSignedBytes(Header);
		crypto_ed25519_sign(Header.Signature.data(), Options.SigningKey->Secret.data(), reinterpret_cast<const std::uint8_t*>(Signed.data()), Signed.size());
	}

	Package.insert(Package.end(), Index.begin(), Index.end());
	const auto HeaderBytes = EncodePackageHeader(Header);
	std::ranges::copy(HeaderBytes, Package.begin());
	return Package;
}

std::expected<FPackageBuildStats, FPackageBuildError> WritePackage(const std::filesystem::path& Output, std::vector<FPackageInput> Inputs, const FPackageBuildOptions& Options)
{
	FPackageBuildStats Stats{.Entries = Inputs.size()};
	for (const FPackageInput& Input : Inputs)
	{
		Stats.Bytes += Input.Bytes.size();
	}

	auto Package = BuildPackage(std::move(Inputs), Options);
	if (!Package)
	{
		return std::unexpected(Package.error());
	}

	std::error_code Error;
	if (Output.has_parent_path())
	{
		std::filesystem::create_directories(Output.parent_path(), Error);
	}

	// Written beside the output and renamed over it, so a failed build never leaves a half-written package.
	std::filesystem::path Temporary = Output;
	Temporary += ".tmp";
	{
		std::ofstream File(Temporary, std::ios::binary | std::ios::trunc);
		if (!File || !File.write(reinterpret_cast<const char*>(Package->data()), static_cast<std::streamsize>(Package->size())) || !File.flush())
		{
			std::filesystem::remove(Temporary, Error);
			return BuildError(std::format("Cannot write {}", Temporary.generic_string()));
		}
	}

	std::filesystem::rename(Temporary, Output, Error);
	if (Error)
	{
		std::filesystem::remove(Temporary, Error);
		return BuildError(std::format("Cannot replace {}", Output.generic_string()));
	}

	Stats.PackageBytes = Package->size();
	return Stats;
}

std::expected<void, FPackageBuildError> EmbedPackage(const std::filesystem::path& Executable, const std::span<const std::byte> Package)
{
	if (Package.size() > MaximumContentPackageEntrySize * 2)
	{
		return BuildError("A package that large belongs beside the executable, not in it");
	}

#ifdef HERTA_PLATFORM_WINDOWS
	HANDLE Update = BeginUpdateResourceW(Executable.c_str(), FALSE);
	if (!Update)
	{
		return BuildError(std::format("Cannot open {} to embed a package", Executable.generic_string()));
	}

	std::wstring Name(EmbeddedPackageName.begin(), EmbeddedPackageName.end());
	const BOOL bUpdated = UpdateResourceW(Update, MAKEINTRESOURCEW(10), Name.c_str(), MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL), const_cast<std::byte*>(Package.data()), static_cast<DWORD>(Package.size()));
	if (!EndUpdateResourceW(Update, bUpdated ? FALSE : TRUE) || !bUpdated)
	{
		return BuildError(std::format("Cannot embed the package in {}", Executable.generic_string()));
	}

	return {};
#else
	std::filesystem::path Section = Executable;
	Section += ".hertapak";
	{
		std::ofstream File(Section, std::ios::binary | std::ios::trunc);
		if (!File || !File.write(reinterpret_cast<const char*>(Package.data()), static_cast<std::streamsize>(Package.size())))
		{
			return BuildError(std::format("Cannot write {}", Section.generic_string()));
		}
	}

	std::string SectionName = ".";
	for (const char Character : EmbeddedPackageName)
	{
		SectionName.push_back(static_cast<char>(Character - 'A' + 'a'));
	}

	// RunProcess starts programs by path, so objcopy is looked up on PATH as a shell would.
	std::filesystem::path Objcopy = "objcopy";
	const std::string Search = ReadEnvironment("PATH").value_or("/usr/bin:/bin");
	for (std::size_t Start = 0; Start <= Search.size();)
	{
		const std::size_t End = std::min(Search.find(':', Start), Search.size());
		std::error_code Error;
		if (const std::filesystem::path Candidate = std::filesystem::path(Search.substr(Start, End - Start)) / "objcopy"; End > Start && std::filesystem::is_regular_file(Candidate, Error))
		{
			Objcopy = Candidate;
			break;
		}

		Start = End + 1;
	}

	const auto Ran = RunProcess({.Executable = Objcopy, .Arguments = {"--remove-section=" + SectionName, "--add-section", SectionName + "=" + Section.string(), "--set-section-flags", SectionName + "=noload,readonly", Executable.string()}});
	std::error_code Error;
	std::filesystem::remove(Section, Error);
	if (!Ran || Ran->ExitCode != 0)
	{
		return BuildError(std::format("objcopy could not embed the package in {}: {}", Executable.generic_string(), Ran ? Ran->StandardError : Ran.error().Message));
	}

	return {};
#endif
}

std::expected<FPackageSigningKey, FPackageBuildError> GeneratePackageSigningKey(const std::uint32_t Id)
{
	std::array<std::byte, 32> Seed{};
	if (!FillSecureRandom(Seed))
	{
		return BuildError("The operating system's secure random generator failed");
	}

	FPackageSigningKey Key{.Id = Id};
	std::array<std::uint8_t, 32> Public{};
	// Monocypher wipes the seed once it has derived the key pair.
	crypto_ed25519_key_pair(Key.Secret.data(), Public.data(), reinterpret_cast<std::uint8_t*>(Seed.data()));
	return Key;
}

FPackagePublicKey GetPackagePublicKey(const FPackageSigningKey& Key) noexcept
{
	FPackagePublicKey Public{.Id = Key.Id};
	std::ranges::copy(std::span(Key.Secret).subspan(32), Public.Key.begin());
	return Public;
}

std::string FormatPackageSigningKey(const FPackageSigningKey& Key)
{
	return std::format("HertaPackageSigningKey {} {}\n", Key.Id, ToHex(Key.Secret));
}

std::expected<FPackageSigningKey, FPackageBuildError> ParsePackageSigningKey(std::string_view Text)
{
	constexpr std::string_view Prefix = "HertaPackageSigningKey ";
	while (!Text.empty() && (Text.back() == '\n' || Text.back() == '\r' || Text.back() == ' '))
	{
		Text.remove_suffix(1);
	}

	FPackageSigningKey Key;
	const std::size_t Space = Text.starts_with(Prefix) ? Text.find(' ', Prefix.size()) : std::string_view::npos;
	if (Space == std::string_view::npos)
	{
		return BuildError("Not a Herta package signing key");
	}

	const std::string_view Id = Text.substr(Prefix.size(), Space - Prefix.size());
	const std::string_view Hex = Text.substr(Space + 1);
	const auto [IdEnd, IdError] = std::from_chars(Id.data(), Id.data() + Id.size(), Key.Id);
	bool bValid = IdError == std::errc{} && IdEnd == Id.data() + Id.size() && Hex.size() == Key.Secret.size() * 2;
	for (std::size_t Index = 0; bValid && Index < Key.Secret.size(); ++Index)
	{
		const auto [End, Error] = std::from_chars(Hex.data() + Index * 2, Hex.data() + Index * 2 + 2, Key.Secret[Index], 16);
		bValid = Error == std::errc{} && End == Hex.data() + Index * 2 + 2;
	}

	if (!bValid)
	{
		return BuildError("Not a Herta package signing key");
	}

	return Key;
}
}
