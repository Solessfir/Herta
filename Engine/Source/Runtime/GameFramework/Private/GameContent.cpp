#include "Herta/GameFramework/GameContent.h"

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

std::expected<FGameContent, FGameError> MountGameContent(const FGameContentOptions& Options)
{
	// A developer's --project plays a staged folder elsewhere, laid out as it would be beside the executable.
	if (!Options.ProjectPath.empty())
	{
		const std::filesystem::path Project = std::filesystem::absolute(Options.ProjectPath).lexically_normal();
		auto FileSystem = std::make_shared<FVirtualFileSystem>(Project.parent_path().parent_path());
		FileSystem->MountDirectory(FileSystem->GetRoot());
		return FGameContent{.FileSystem = std::move(FileSystem), .ProjectPath = Project};
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

	return FGameContent{.FileSystem = std::move(FileSystem), .ProjectPath = Projects.front()};
}
}
