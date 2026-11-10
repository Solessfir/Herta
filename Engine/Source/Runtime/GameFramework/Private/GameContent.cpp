#include "Herta/GameFramework/GameContent.h"

#include "Herta/Assets/DataMod.h"
#include "Herta/Core/Build.h"
#include "Herta/Platform/Platform.h"

#include <algorithm>
#include <format>
#include <iterator>
#include <system_error>
#include <vector>

#ifndef HERTA_PACKAGE_PUBLIC_KEYS
	#define HERTA_PACKAGE_PUBLIC_KEYS ""
#endif

namespace Herta
{
std::span<const FPackagePublicKey> GetPublisherPackageKeys()
{
	// Generation already refused malformed keys, so a parse failure here leaves the build trusting none.
	static const std::vector<FPackagePublicKey> Keys = ParsePackagePublicKeys(HERTA_PACKAGE_PUBLIC_KEYS).value_or(std::vector<FPackagePublicKey>{});
	return Keys;
}

namespace
{
// Mods are untrusted data: their packages are checked like any other, and a mod is only ever files laid over the game's.
void MountMods(FGameContent& Content, const FGameContentOptions& Options, const FPackageOpenOptions& OpenOptions)
{
	FVirtualFileSystem& FileSystem = *Content.FileSystem;
	const std::filesystem::path Directory = Options.ModsDirectory.empty() ? FileSystem.GetRoot() / "Mods" : Options.ModsDirectory;
	std::error_code Error;
	if (!Options.bLoadMods || !std::filesystem::is_directory(Directory, Error))
	{
		return;
	}

	struct FFound
	{
		std::shared_ptr<const FContentPackage> Package{};
		std::filesystem::path Files{};
	};

	std::vector<std::filesystem::path> Folders;
	for (const auto& Entry : std::filesystem::directory_iterator(Directory, Error))
	{
		if (Entry.is_directory(Error))
		{
			Folders.push_back(Entry.path());
		}
	}

	std::ranges::sort(Folders);
	std::vector<FModCandidate> Candidates;
	std::vector<FFound> Found;
	for (const std::filesystem::path& Folder : Folders)
	{
		const std::string Source = Folder.generic_string();
		const auto Text = ReadDiskFile(Folder / DataModManifestName);
		auto Manifest = Text ? ParseModManifest(std::string_view(reinterpret_cast<const char*>(Text->data()), Text->size())) : std::unexpected(Text.error());
		if (!Manifest)
		{
			Content.ModDiagnostics.push_back(std::format("Mod in {} is not loaded: {}", Source, Manifest.error().Message));
			continue;
		}

		FModCandidate Candidate{.Manifest = std::move(*Manifest), .Source = Source};
		FFound Mount;
		const std::filesystem::path PackagePath = Folder / (Candidate.Manifest.Namespace + std::string(ContentPackageExtension));
		if (std::filesystem::is_regular_file(PackagePath, Error))
		{
			auto Package = FContentPackage::Open(PackagePath, {.TrustedKeys = OpenOptions.TrustedKeys});
			if (!Package)
			{
				Content.ModDiagnostics.push_back(std::format("Mod {} is not loaded: {}", Candidate.Manifest.Namespace, Package.error().Message));
				continue;
			}

			for (const FPackageEntry& Entry : (*Package)->GetEntries())
			{
				Candidate.Files.push_back(Entry.Path);
			}

			Mount.Package = std::move(*Package);
		}
		else
		{
			Mount.Files = Folder / "Files";
			FVirtualFileSystem Loose(Mount.Files);
			Loose.MountDirectory(Mount.Files);
			for (const std::filesystem::path& File : Loose.FindFiles("", 64))
			{
				const std::u8string Relative = File.lexically_relative(Loose.GetRoot()).generic_u8string();
				Candidate.Files.emplace_back(Relative.begin(), Relative.end());
			}
		}

		Candidates.push_back(std::move(Candidate));
		Found.push_back(std::move(Mount));
	}

	const std::filesystem::path Root = FileSystem.GetRoot();
	const FModResolution Resolution = ResolveMods(Candidates, [&FileSystem, &Root](const std::string_view Path)
	{
		return FileSystem.Exists(Root / std::filesystem::path(std::u8string(Path.begin(), Path.end())));
	});

	Content.ModDiagnostics.insert(Content.ModDiagnostics.end(), Resolution.Diagnostics.begin(), Resolution.Diagnostics.end());
	// Each mod mounts in front of everything before it, so the last loaded wins where mods replace the same file.
	for (const std::size_t Index : Resolution.LoadOrder)
	{
		const FModManifest& Manifest = Candidates[Index].Manifest;
		const bool bSigned = Found[Index].Package && Found[Index].Package->IsTrusted();
		if (Found[Index].Package)
		{
			FileSystem.MountPackage(Found[Index].Package, true);
		}
		else
		{
			FileSystem.MountDirectory(Found[Index].Files, true);
		}

		Content.Mods.push_back(std::format("{} {}.{}.{}, from {}, {}", Manifest.Namespace, Manifest.Version.Major, Manifest.Version.Minor, Manifest.Version.Patch, Candidates[Index].Source, bSigned ? "signed by the publisher" : "unsigned, installed by the user"));
	}
}
}

std::expected<FGameContent, FGameError> MountGameContent(const FGameContentOptions& Options)
{
	// A developer's --project plays a staged folder elsewhere, laid out as it would be beside the executable.
	if (!Options.ProjectPath.empty())
	{
		const std::filesystem::path Project = std::filesystem::absolute(Options.ProjectPath).lexically_normal();
		FGameContent Content{.FileSystem = std::make_shared<FVirtualFileSystem>(Project.parent_path().parent_path()), .ProjectPath = Project};
		Content.FileSystem->MountDirectory(Content.FileSystem->GetRoot());
		MountMods(Content, Options, {.TrustedKeys = Options.TrustedKeys});
		return Content;
	}

	auto FileSystem = std::make_shared<FVirtualFileSystem>(Options.ExecutableDirectory);
	std::vector<std::filesystem::path> Packages;
	std::error_code Error;
	for (const auto& Entry : std::filesystem::directory_iterator(FileSystem->GetRoot(), Error))
	{
		if (Entry.is_regular_file(Error) && Entry.path().extension() == ContentPackageExtension)
		{
			Packages.push_back(Entry.path());
		}
	}

	std::ranges::sort(Packages);
	std::vector<FPackagePublicKey> Trusted(Options.TrustedKeys.begin(), Options.TrustedKeys.end());
	std::ranges::copy(GetPublisherPackageKeys(), std::back_inserter(Trusted));
	const bool bRequireSignature = Options.bRequireSignature || (GetBuildConfiguration() == EBuildConfiguration::Shipping && !GetPublisherPackageKeys().empty());
	const FPackageOpenOptions OpenOptions{.TrustedKeys = Trusted, .bRequireSignature = bRequireSignature};
	// The base package a monolithic executable carries comes first, then the packages beside it.
	if (const std::optional<FEmbeddedData> Embedded = FindEmbeddedData("HERTAPAK"))
	{
		auto Package = !Embedded->Memory.empty() ? FContentPackage::OpenMemory(Embedded->Memory, OpenOptions, "embedded in the executable") : FContentPackage::OpenRange(Embedded->File, Embedded->Offset, Embedded->Size, OpenOptions);
		if (!Package)
		{
			return std::unexpected(FGameError{std::format("The executable's embedded content: {}", Package.error().Message)});
		}

		FileSystem->MountPackage(std::move(*Package));
	}

	for (const std::filesystem::path& Path : Packages)
	{
		auto Package = FContentPackage::Open(Path, OpenOptions);
		if (!Package)
		{
			return std::unexpected(FGameError{Package.error().Message});
		}

		FileSystem->MountPackage(std::move(*Package));
	}

	if (FileSystem->GetMountCount() == 0)
	{
		if (bRequireSignature)
		{
			return std::unexpected(FGameError{std::format("This game plays only its signed content packages, and there are none in {}", FileSystem->GetRoot().generic_string())});
		}

		FileSystem->MountDirectory(FileSystem->GetRoot());
	}

	const std::vector<std::filesystem::path> Projects = FileSystem->FindFiles(".hertaproject", 1);
	if (Projects.empty())
	{
		return std::unexpected(FGameError{std::format("No staged project in {}", FileSystem->GetRoot().generic_string())});
	}

	FGameContent Content{.FileSystem = std::move(FileSystem), .ProjectPath = Projects.front()};
	MountMods(Content, Options, OpenOptions);
	return Content;
}
}
