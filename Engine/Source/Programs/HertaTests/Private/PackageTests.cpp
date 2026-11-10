#include "Herta/Assets/ContentPackage.h"
#include "Herta/Assets/VirtualFileSystem.h"
#include "Herta/GameFramework/GameContent.h"
#include "Herta/PackageBuilder/PackageBuilder.h"
#include "TestFiles.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <fstream>
#include <random>
#include <string>

namespace Herta
{
namespace
{
[[nodiscard]] std::vector<std::byte> Text(const std::string_view Value)
{
	std::vector<std::byte> Bytes(Value.size());
	std::ranges::transform(Value, Bytes.begin(), [](const char Character)
	{
		return static_cast<std::byte>(Character);
	});

	return Bytes;
}

[[nodiscard]] std::vector<FPackageInput> SampleInputs()
{
	// A compressible level, random bytes that do not shrink, an empty file, and a nested path.
	std::vector<std::byte> Noise(4096);
	std::mt19937 Random(7);
	std::ranges::generate(Noise, [&Random]
	{
		return static_cast<std::byte>(Random());
	});

	std::string Level;
	for (int Line = 0; Line < 200; ++Line)
	{
		Level += std::format("{{\"name\": \"Crate {}\", \"position\": [0, 1, {}]}}\n", Line, Line);
	}

	return {
	    {.Path = "Sandbox/Levels/Sandbox.hlevel", .Bytes = Text(Level)},
	    {.Path = "Sandbox/Content/Noise.hasset", .Bytes = Noise},
	    {.Path = "Engine/Content/Empty.txt", .Bytes = {}},
	    {.Path = "Sandbox/Sandbox.hertaproject", .Bytes = Text("{\"name\": \"Sandbox\"}")},
	};
}

void WriteBytes(const std::filesystem::path& Path, const std::span<const std::byte> Bytes)
{
	std::ofstream File(Path, std::ios::binary | std::ios::trunc);
	File.write(reinterpret_cast<const char*>(Bytes.data()), static_cast<std::streamsize>(Bytes.size()));
}
}

TEST_CASE("Content packages store files sorted and compressed where it helps, and read them back from a file, a range, or memory")
{
	const std::vector<FPackageInput> Inputs = SampleInputs();
	const auto Package = BuildPackage(Inputs);
	REQUIRE_MESSAGE(Package, (Package ? std::string() : Package.error().Message));

	const auto Opened = FContentPackage::OpenMemory(*Package);
	REQUIRE_MESSAGE(Opened, (Opened ? std::string() : Opened.error().Message));
	const std::span<const FPackageEntry> Entries = (*Opened)->GetEntries();
	REQUIRE(Entries.size() == Inputs.size());
	CHECK(std::ranges::is_sorted(Entries, {}, &FPackageEntry::Path));
	CHECK((*Opened)->Find("Sandbox/Levels/Sandbox.hlevel")->Codec == EPackageCodec::Zstd);
	CHECK((*Opened)->Find("Sandbox/Levels/Sandbox.hlevel")->StoredSize < Inputs[0].Bytes.size() / 4);
	CHECK((*Opened)->Find("Sandbox/Content/Noise.hasset")->Codec == EPackageCodec::None);
	CHECK((*Opened)->Find("Missing") == nullptr);
	CHECK_FALSE((*Opened)->IsTrusted());
	for (const FPackageInput& Input : Inputs)
	{
		const auto Read = (*Opened)->Read(Input.Path);
		REQUIRE_MESSAGE(Read, (Read ? std::string() : Read.error().Message));
		CHECK(*Read == Input.Bytes);
	}

	CHECK_FALSE((*Opened)->Read("Sandbox/Levels/Missing.hlevel"));

	// The same package opens from its own file and from inside a larger one.
	const Tests::FScratchDirectory Scratch("HertaPackages");
	const auto Stats = WritePackage(Scratch.GetPath() / "Base.hpak", Inputs);
	REQUIRE(Stats);
	CHECK(Stats->Entries == Inputs.size());
	CHECK(Stats->PackageBytes == Package->size());
	const auto FromFile = FContentPackage::Open(Scratch.GetPath() / "Base.hpak");
	REQUIRE(FromFile);
	CHECK((*FromFile)->Read("Sandbox/Sandbox.hertaproject") == Inputs[3].Bytes);
	std::vector<std::byte> Outer = Text("an executable's code before the package");
	const std::uint64_t Offset = Outer.size();
	Outer.insert(Outer.end(), Package->begin(), Package->end());
	Outer.insert(Outer.end(), 37, std::byte{0xcc});
	WriteBytes(Scratch.GetPath() / "Game.exe", Outer);
	const auto FromRange = FContentPackage::OpenRange(Scratch.GetPath() / "Game.exe", Offset, Package->size());
	REQUIRE_MESSAGE(FromRange, (FromRange ? std::string() : FromRange.error().Message));
	CHECK((*FromRange)->Read("Sandbox/Content/Noise.hasset") == Inputs[1].Bytes);
	CHECK_FALSE(FContentPackage::OpenRange(Scratch.GetPath() / "Game.exe", Offset, Outer.size()));

	// A directory collects every file under it by relative path.
	std::filesystem::create_directories(Scratch.GetPath() / "Staged/Sandbox/Levels");
	WriteBytes(Scratch.GetPath() / "Staged/Sandbox/Levels/Sandbox.hlevel", Inputs[0].Bytes);
	WriteBytes(Scratch.GetPath() / "Staged/Readme.txt", Text("hi"));
	const auto Collected = CollectPackageInputs(Scratch.GetPath() / "Staged");
	REQUIRE(Collected);
	REQUIRE(Collected->size() == 2);
	CHECK(Collected->front().Path == "Readme.txt");
	CHECK(Collected->back().Path == "Sandbox/Levels/Sandbox.hlevel");
}

TEST_CASE("Content packages build byte for byte the same from the same files in any order, and refuse unsafe paths")
{
	std::vector<FPackageInput> Inputs = SampleInputs();
	const auto First = BuildPackage(Inputs);
	std::ranges::reverse(Inputs);
	const auto Second = BuildPackage(Inputs);
	REQUIRE(First);
	REQUIRE(Second);
	CHECK(*First == *Second);

	for (const std::string_view Path : {"", "/abs", "C:/drive", "back\\slash", "a/../b", "./a", "a//b", "a/", "tab\there"})
	{
		CHECK_FALSE_MESSAGE(IsValidPackagePath(Path), Path);
		CHECK_FALSE(BuildPackage({{.Path = std::string(Path), .Bytes = {}}}));
	}

	CHECK(IsValidPackagePath("Sandbox/Content/0f1e.hasset"));
	CHECK_FALSE(BuildPackage({{.Path = "Same", .Bytes = {}}, {.Path = "Same", .Bytes = {}}}));
}

TEST_CASE("Signed content packages verify against trusted keys and reject tampering with the header, index, or chunks")
{
	const auto Key = GeneratePackageSigningKey(3);
	REQUIRE(Key);
	const FPackagePublicKey Public = GetPackagePublicKey(*Key);
	const std::array Trusted{Public};
	const auto Signed = BuildPackage(SampleInputs(), {.SigningKey = *Key});
	REQUIRE(Signed);

	// Its own key verifies it; a game that requires signatures refuses an unsigned package or an unknown key.
	const auto Verified = FContentPackage::OpenMemory(*Signed, {.TrustedKeys = Trusted, .bRequireSignature = true});
	REQUIRE_MESSAGE(Verified, (Verified ? std::string() : Verified.error().Message));
	CHECK((*Verified)->IsTrusted());
	CHECK((*Verified)->GetHeader().SigningKey == 3);
	const auto Unsigned = BuildPackage(SampleInputs());
	REQUIRE(Unsigned);
	CHECK_FALSE(FContentPackage::OpenMemory(*Unsigned, {.TrustedKeys = Trusted, .bRequireSignature = true}));
	const auto OtherKey = GeneratePackageSigningKey(3);
	REQUIRE(OtherKey);
	const std::array Other{GetPackagePublicKey(*OtherKey)};
	CHECK_FALSE(FContentPackage::OpenMemory(*Signed, {.TrustedKeys = Other}));
	CHECK_FALSE(FContentPackage::OpenMemory(*Signed, {.bRequireSignature = true}));
	// Without trusted keys it still opens, untrusted, for development.
	const auto Untrusted = FContentPackage::OpenMemory(*Signed);
	REQUIRE(Untrusted);
	CHECK_FALSE((*Untrusted)->IsTrusted());

	// Any changed byte in the header or index breaks the signature or the index hash; a changed chunk fails its read.
	const auto Header = DecodePackageHeader(*Signed);
	REQUIRE(Header);
	for (const std::size_t Offset : {std::size_t{16}, std::size_t{32}, std::size_t{45}, std::size_t{100}, static_cast<std::size_t>(Header->IndexOffset + 5), Signed->size() - 1})
	{
		std::vector<std::byte> Tampered = *Signed;
		Tampered[Offset] ^= std::byte{0x01};
		CHECK_FALSE_MESSAGE(FContentPackage::OpenMemory(Tampered, {.TrustedKeys = Trusted}), Offset);
	}

	std::vector<std::byte> Tampered = *Signed;
	const auto Opened = FContentPackage::OpenMemory(*Signed, {.TrustedKeys = Trusted});
	REQUIRE(Opened);
	const FPackageEntry* const Level = (*Opened)->Find("Sandbox/Levels/Sandbox.hlevel");
	REQUIRE(Level);
	Tampered[Level->Offset + 3] ^= std::byte{0x40};
	const auto Chunked = FContentPackage::OpenMemory(Tampered, {.TrustedKeys = Trusted});
	REQUIRE(Chunked);
	CHECK_FALSE((*Chunked)->Read("Sandbox/Levels/Sandbox.hlevel"));
	CHECK((*Chunked)->Read("Sandbox/Sandbox.hertaproject"));

	// Key files round trip.
	const auto Parsed = ParsePackageSigningKey(FormatPackageSigningKey(*Key));
	REQUIRE(Parsed);
	CHECK(Parsed->Secret == Key->Secret);
	CHECK(Parsed->Id == 3);
	CHECK_FALSE(ParsePackageSigningKey("HertaPackageSigningKey 3 00"));
	const auto Keys = ParsePackagePublicKeys(FormatPackagePublicKey(Public) + ", " + FormatPackagePublicKey(GetPackagePublicKey(*OtherKey)).replace(0, 1, "4"));
	REQUIRE(Keys);
	CHECK(Keys->size() == 2);
	CHECK(Keys->front() == Public);
	CHECK(ParsePackagePublicKeys("")->empty());
	CHECK_FALSE(ParsePackagePublicKeys("3:abc"));
	CHECK_FALSE(ParsePackagePublicKeys(FormatPackagePublicKey(Public) + " " + FormatPackagePublicKey(Public)));
}

TEST_CASE("Content package readers refuse corrupt indexes and survive random corruption")
{
	const auto Package = BuildPackage(SampleInputs());
	REQUIRE(Package);
	const auto Header = DecodePackageHeader(*Package);
	REQUIRE(Header);
	const std::span<const std::byte> Index = std::span(*Package).subspan(static_cast<std::size_t>(Header->IndexOffset));
	REQUIRE(DecodePackageIndex(Index, Header->EntryCount, Header->IndexOffset));
	CHECK_FALSE(DecodePackageIndex(Index, Header->EntryCount + 1, Header->IndexOffset));
	CHECK_FALSE(DecodePackageIndex(Index, Header->EntryCount - 1, Header->IndexOffset));
	CHECK_FALSE(DecodePackageIndex(Index, Header->EntryCount, ContentPackageHeaderSize));

	// Entries out of order, overlapping, or past the data are refused even with a matching hash.
	auto Entries = DecodePackageIndex(Index, Header->EntryCount, Header->IndexOffset);
	REQUIRE(Entries);
	std::vector<FPackageEntry> Swapped = *Entries;
	std::swap(Swapped[0], Swapped[1]);
	CHECK_FALSE(DecodePackageIndex(EncodePackageIndex(Swapped), Header->EntryCount, Header->IndexOffset));
	std::vector<FPackageEntry> Overlapping = *Entries;
	Overlapping[2].Offset = Overlapping[1].Offset;
	CHECK_FALSE(DecodePackageIndex(EncodePackageIndex(Overlapping), Header->EntryCount, Header->IndexOffset));
	std::vector<FPackageEntry> Huge = *Entries;
	Huge.back().Size = MaximumContentPackageEntrySize + 1;
	CHECK_FALSE(DecodePackageIndex(EncodePackageIndex(Huge), Header->EntryCount, Header->IndexOffset));
	std::vector<FPackageEntry> Escaping = *Entries;
	Escaping.back().Path = "Sandbox/../../Secrets";
	CHECK_FALSE(DecodePackageIndex(EncodePackageIndex(Escaping), Header->EntryCount, Header->IndexOffset));

	// Flipped bytes, truncation, and garbage never crash the reader; every read either fails or returns the original bytes.
	std::mt19937 Random(11);
	const std::vector<FPackageInput> Inputs = SampleInputs();
	for (int Trial = 0; Trial < 2000; ++Trial)
	{
		std::vector<std::byte> Corrupt = *Package;
		const int Flips = 1 + static_cast<int>(Random() % 4);
		for (int Flip = 0; Flip < Flips; ++Flip)
		{
			Corrupt[Random() % Corrupt.size()] ^= static_cast<std::byte>(1 + Random() % 255);
		}

		if (Trial % 7 == 0)
		{
			Corrupt.resize(Random() % Corrupt.size());
		}

		const auto Opened = FContentPackage::OpenMemory(Corrupt);
		if (!Opened)
		{
			continue;
		}

		for (const FPackageInput& Input : Inputs)
		{
			const auto Read = (*Opened)->Read(Input.Path);
			CHECK((!Read || *Read == Input.Bytes));
		}
	}
}

TEST_CASE("The virtual file system reads mounts in order, the disk outside its root, and finds staged files")
{
	const Tests::FScratchDirectory Scratch("HertaFileSystem");
	const std::filesystem::path Root = Scratch.GetPath() / "Game";
	std::filesystem::create_directories(Root / "Sandbox");
	WriteBytes(Root / "Sandbox/Overridden.txt", Text("loose"));
	WriteBytes(Root / "Sandbox/Loose.txt", Text("only loose"));
	WriteBytes(Scratch.GetPath() / "Outside.txt", Text("outside"));
	const auto Package = BuildPackage({{.Path = "Sandbox/Overridden.txt", .Bytes = Text("packaged")}, {.Path = "Sandbox/Sandbox.hertaproject", .Bytes = Text("{}")}});
	REQUIRE(Package);
	WriteBytes(Scratch.GetPath() / "Base.hpak", *Package);
	auto Opened = FContentPackage::Open(Scratch.GetPath() / "Base.hpak");
	REQUIRE(Opened);

	// The package is mounted first, so it wins where both hold a file; the loose folder fills in the rest.
	FVirtualFileSystem Files(Root);
	Files.MountPackage(std::move(*Opened));
	Files.MountDirectory(Root);
	CHECK(Files.Read(Root / "Sandbox/Overridden.txt") == Text("packaged"));
	CHECK(Files.Read(Root / "Sandbox/../Sandbox/Loose.txt") == Text("only loose"));
	CHECK(Files.Exists(Root / "Sandbox/Sandbox.hertaproject"));
	CHECK_FALSE(Files.Exists(Root / "Sandbox/Missing.txt"));
	CHECK_FALSE(Files.Read(Root / "Sandbox/Missing.txt"));
	CHECK(Files.Read(Scratch.GetPath() / "Outside.txt") == Text("outside"));
	CHECK(Files.FindFiles(".hertaproject", 1) == std::vector{Root / "Sandbox/Sandbox.hertaproject"});
	CHECK(Files.FindFiles(".txt", 0).empty());
	CHECK(Files.FindFiles(".txt", 1).size() == 2);
	CHECK(Files.DescribeMounts().size() == 2);

	// Without a root it is the disk.
	CHECK(FVirtualFileSystem().Read(Scratch.GetPath() / "Outside.txt") == Text("outside"));
}

TEST_CASE("Game content mounts packages beside the executable, refuses unsigned ones when signatures are required, and falls back to loose files")
{
	const Tests::FScratchDirectory Scratch("HertaGameContent");
	const std::filesystem::path Executables = Scratch.GetPath() / "Game";
	std::filesystem::create_directories(Executables / "Sandbox");
	const auto Key = GeneratePackageSigningKey(9);
	REQUIRE(Key);
	const std::array Trusted{GetPackagePublicKey(*Key)};

	// Loose: the staged folder beside the executable.
	WriteBytes(Executables / "Sandbox/Sandbox.hertaproject", Text("{}"));
	auto Loose = MountGameContent({.ExecutableDirectory = Executables});
	REQUIRE_MESSAGE(Loose, (Loose ? std::string() : Loose.error().Message));
	CHECK(Loose->ProjectPath == std::filesystem::absolute(Executables / "Sandbox/Sandbox.hertaproject").lexically_normal());
	CHECK(Loose->FileSystem->DescribeMounts().front().starts_with("directory"));
	CHECK_FALSE(MountGameContent({.ExecutableDirectory = Executables, .TrustedKeys = Trusted, .bRequireSignature = true}));

	// Packaged: the package is mounted instead of the loose files, which a signed game must never fall back to.
	std::filesystem::remove_all(Executables / "Sandbox");
	REQUIRE(WritePackage(Executables / "Sandbox.hpak", {{.Path = "Sandbox/Sandbox.hertaproject", .Bytes = Text("{\"packaged\": true}")}}));
	auto Packaged = MountGameContent({.ExecutableDirectory = Executables});
	REQUIRE(Packaged);
	CHECK(Packaged->FileSystem->Read(Packaged->ProjectPath) == Text("{\"packaged\": true}"));
	CHECK(Packaged->FileSystem->GetMountCount() == 1);
	CHECK_FALSE(MountGameContent({.ExecutableDirectory = Executables, .TrustedKeys = Trusted, .bRequireSignature = true}));
	REQUIRE(WritePackage(Executables / "Sandbox.hpak", {{.Path = "Sandbox/Sandbox.hertaproject", .Bytes = Text("{}")}}, {.SigningKey = *Key}));
	auto Signed = MountGameContent({.ExecutableDirectory = Executables, .TrustedKeys = Trusted, .bRequireSignature = true});
	REQUIRE_MESSAGE(Signed, (Signed ? std::string() : Signed.error().Message));
	CHECK(Signed->FileSystem->DescribeMounts().front().ends_with(", signed"));

	// A developer's --project reads that staged folder from the disk.
	std::filesystem::create_directories(Scratch.GetPath() / "Staged/Sandbox");
	WriteBytes(Scratch.GetPath() / "Staged/Sandbox/Sandbox.hertaproject", Text("{\"staged\": true}"));
	const auto Developer = MountGameContent({.ExecutableDirectory = Executables, .ProjectPath = Scratch.GetPath() / "Staged/Sandbox/Sandbox.hertaproject"});
	REQUIRE(Developer);
	CHECK(Developer->FileSystem->GetRoot() == std::filesystem::absolute(Scratch.GetPath() / "Staged").lexically_normal());
	CHECK(Developer->FileSystem->Read(Developer->ProjectPath) == Text("{\"staged\": true}"));
}
}
