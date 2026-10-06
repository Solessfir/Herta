#include "PreviewAssets.h"

#include "Herta/AssetPipeline/AssetCommands.h"
#include "Herta/AssetPipeline/AssetCooker.h"
#include "Herta/AssetPipeline/ContentRoot.h"
#include "Herta/Core/Log.h"
#include "Herta/Tasks/TaskSystem.h"

#include <algorithm>
#include <array>
#include <format>
#include <set>
#include <system_error>
#include <utility>

#ifdef _WIN32
	#include <Windows.h>
#else
	#include <fcntl.h>
	#include <sys/syscall.h>
	#include <unistd.h>
#endif

namespace Herta
{
namespace
{
inline constexpr FLogCategory AssetLog{.Name = "Assets"};
inline constexpr std::chrono::seconds PollInterval{1};

std::string PathToUtf8(const std::filesystem::path& Path)
{
	const auto Text = Path.generic_u8string();
	return {Text.begin(), Text.end()};
}

std::filesystem::path Utf8Path(const std::string_view Text)
{
	return std::filesystem::path(std::u8string(Text.begin(), Text.end()));
}

constexpr char FoldAsciiCase(const char Character)
{
	return Character >= 'A' && Character <= 'Z' ? static_cast<char>(Character - 'A' + 'a') : Character;
}

bool IsValidFolderText(const std::string_view Text)
{
	for (std::size_t Index = 0; Index < Text.size();)
	{
		const auto Lead = static_cast<unsigned char>(Text[Index++]);
		if (Lead < 0x80)
		{
			continue;
		}

		const unsigned Count = Lead >= 0xc2 && Lead <= 0xdf ? 1 : Lead >= 0xe0 && Lead <= 0xef ? 2
		                                                      : Lead >= 0xf0 && Lead <= 0xf4   ? 3
		                                                                                       : 0;
		if (Count == 0 || Index + Count > Text.size())
		{
			return false;
		}

		unsigned CodePoint = Lead & (0x7f >> (Count + 1));

		for (unsigned Remaining = Count; Remaining != 0; --Remaining)
		{
			const auto Byte = static_cast<unsigned char>(Text[Index++]);
			if ((Byte & 0xc0) != 0x80)
			{
				return false;
			}

			CodePoint = (CodePoint << 6) | (Byte & 0x3f);
		}

		if ((Count == 2 && CodePoint < 0x800) || (Count == 3 && CodePoint < 0x10000) || CodePoint > 0x10ffff || (CodePoint >= 0xd800 && CodePoint <= 0xdfff) || CodePoint <= 0x9f)
		{
			return false;
		}
	}

	return true;
}

bool IsReservedFolderName(const std::string_view Name)
{
	std::string Base(Name.substr(0, Name.find('.')));
	std::ranges::transform(Base, Base.begin(), FoldAsciiCase);
	constexpr std::array<std::string_view, 6> Reserved{"con", "prn", "aux", "nul", "conin$", "conout$"};
	if (std::ranges::find(Reserved, Base) != Reserved.end())
	{
		return true;
	}

	if (!Base.starts_with("com") && !Base.starts_with("lpt"))
	{
		return false;
	}

	const auto Suffix = std::string_view(Base).substr(3);
	return (Suffix.size() == 1 && Suffix.front() >= '1' && Suffix.front() <= '9') || Suffix == "\xc2\xb9" || Suffix == "\xc2\xb2" || Suffix == "\xc2\xb3";
}

std::expected<void, FAssetError> ValidateFolderName(const std::string_view Name)
{
	if (Name.empty() || Name.size() > 255 || Name.contains('/') || Name.starts_with('.') || !IsValidAssetPath(Name) || !IsValidFolderText(Name) || IsReservedFolderName(Name))
	{
		return std::unexpected(FAssetError{"Use one portable UTF-8 folder name without separators, reserved names, or trailing dots/spaces"});
	}

	return {};
}

std::expected<std::filesystem::path, FAssetError> ResolveContentFolder(const std::filesystem::path& ContentRoot, const std::string_view Mount, const std::string_view MountedFolder)
{
	if (ContentRoot.empty() || (MountedFolder != Mount && !(MountedFolder.starts_with(Mount) && MountedFolder.size() > Mount.size() && MountedFolder[Mount.size()] == '/')))
	{
		return std::unexpected(FAssetError{"Folder does not belong to this content mount"});
	}

	if (!IsValidAssetPath(MountedFolder) || !IsValidFolderText(MountedFolder))
	{
		return std::unexpected(FAssetError{"Folder path must be a portable mounted content path"});
	}

	const auto Relative = MountedFolder == Mount ? std::string_view{} : MountedFolder.substr(Mount.size() + 1);
	std::error_code Error;
	const auto Root = std::filesystem::canonical(ContentRoot, Error);
	if (Error)
	{
		return std::unexpected(FAssetError{"Content root is unavailable"});
	}

	const auto Requested = Relative.empty() ? Root : Root / Utf8Path(Relative);
	const auto Folder = std::filesystem::canonical(Requested, Error);
	const auto Local = Folder.lexically_relative(Root);
	if (Error || Local.empty() || !Local.is_relative() || *Local.begin() == ".." || Folder != Requested.lexically_normal() || !std::filesystem::is_directory(Folder, Error) || Error)
	{
		return std::unexpected(FAssetError{"Folder must exist inside its content mount without linked directories"});
	}

	return Folder;
}

std::string FoldFolderName(const std::string_view Name)
{
	std::string Result(Name);
	std::ranges::transform(Result, Result.begin(), FoldAsciiCase);
	return Result;
}

std::expected<std::set<std::string>, FAssetError> ReadSiblingNames(const std::filesystem::path& Parent)
{
	std::set<std::string> Names;
	std::error_code Error;
	std::filesystem::directory_iterator Iterator(Parent, Error);

	for (; !Error && Iterator != std::filesystem::directory_iterator(); Iterator.increment(Error))
	{
		Names.insert(FoldFolderName(PathToUtf8(Iterator->path().filename())));
	}

	if (Error)
	{
		return std::unexpected(FAssetError{"Cannot inspect the folder parent"});
	}

	return Names;
}

std::vector<std::string> DiscoverFolders(const std::filesystem::path& Root, const std::string_view Mount)
{
	std::vector<std::string> Folders{std::string(Mount)};
	std::error_code Error;
	const auto CanonicalRoot = std::filesystem::canonical(Root, Error);
	if (Error)
	{
		return Folders;
	}

	std::filesystem::recursive_directory_iterator Iterator(Root, Error);

	for (; !Error && Iterator != std::filesystem::recursive_directory_iterator(); Iterator.increment(Error))
	{
		const auto& Entry = *Iterator;
		const auto Filename = PathToUtf8(Entry.path().filename());
		if (Filename.starts_with('.') || Entry.is_symlink(Error))
		{
			Iterator.disable_recursion_pending();
			continue;
		}

		if (Entry.is_directory(Error))
		{
			const auto RelativePath = Entry.path().lexically_relative(Root);
			const auto CanonicalDirectory = std::filesystem::canonical(Entry.path(), Error);
			if (Error || CanonicalDirectory != (CanonicalRoot / RelativePath).lexically_normal())
			{
				Iterator.disable_recursion_pending();
				Error.clear();
				continue;
			}

			const auto Relative = PathToUtf8(RelativePath);
			if (IsValidAssetPath(Relative) && IsValidFolderText(Relative))
			{
				Folders.push_back(std::format("{}/{}", Mount, Relative));
			}
		}
	}

	std::ranges::sort(Folders);
	return Folders;
}
}

struct FPreviewAssets::FMeshLoad
{
	std::expected<FCookedModel, FAssetError> Model = std::unexpected(FAssetError{"The cook task did not run"});
	std::optional<FPreviewAssetMetadata> Metadata;
	std::vector<std::string> Warnings;
	FHash128 Key;
	bool bCacheHit = false;
};

FPreviewAssetMetadata GetPreviewAssetMetadata(const FCookedAsset& Asset)
{
	if (const auto* Texture = std::get_if<FCookedTexture>(&Asset))
	{
		return FPreviewTextureMetadata{.Width = Texture->Mips.empty() ? 0 : Texture->Mips.front().Width, .Height = Texture->Mips.empty() ? 0 : Texture->Mips.front().Height, .Mips = Texture->Mips.size(), .ColorSpace = Texture->ColorSpace};
	}

	const auto& Model = std::get<FCookedModel>(Asset);
	FPreviewModelMetadata Metadata{.Vertices = Model.Vertices.size(), .Triangles = Model.Indices.size() / 3, .Materials = Model.Materials.size()};
	if (!Model.Vertices.empty())
	{
		const auto& Position = Model.Vertices.front().Position;
		Metadata.BoundsMinimum = Metadata.BoundsMaximum = FVector3{Position[0], Position[1], Position[2]};
	}

	for (const auto& Vertex : Model.Vertices)
	{
		Metadata.BoundsMinimum.X = std::min(Metadata.BoundsMinimum.X, Vertex.Position[0]);
		Metadata.BoundsMinimum.Y = std::min(Metadata.BoundsMinimum.Y, Vertex.Position[1]);
		Metadata.BoundsMinimum.Z = std::min(Metadata.BoundsMinimum.Z, Vertex.Position[2]);
		Metadata.BoundsMaximum.X = std::max(Metadata.BoundsMaximum.X, Vertex.Position[0]);
		Metadata.BoundsMaximum.Y = std::max(Metadata.BoundsMaximum.Y, Vertex.Position[1]);
		Metadata.BoundsMaximum.Z = std::max(Metadata.BoundsMaximum.Z, Vertex.Position[2]);
	}

	return Metadata;
}

FPreviewAssets::FPreviewAssets(FTaskSystem& InTasks, IGraphicsDevice& InDevice, FLogService& InLog, FEditorAssetPaths InPaths, const std::size_t ObjectCount, std::unique_ptr<FTaskScope> InScope, FEditorAssetThumbnailRenderer InRenderThumbnail)
    : Tasks(InTasks)
    , Device(InDevice)
    , Log(InLog)
    , Paths(std::move(InPaths))
    , Slots(ObjectCount)
    , RenderThumbnail(std::move(InRenderThumbnail))
    , Scope(std::move(InScope))
{
	if (!Paths.EngineContentRoot.empty())
	{
		Mounts.push_back({.Name = "Engine", .Root = Paths.EngineContentRoot});
	}

	if (!Paths.ContentRoot.empty())
	{
		Mounts.push_back({.Name = "Game", .Root = Paths.ContentRoot});
	}

	for (const auto& Mount : Mounts)
	{
		Folders.push_back(Mount.Name);
	}

	ScanCaches.resize(Mounts.size());
}

std::unique_ptr<FPreviewAssets> FPreviewAssets::Create(FTaskSystem& Tasks, IGraphicsDevice& Device, FLogService& Log, FEditorAssetPaths Paths, const std::size_t ObjectCount, FEditorAssetThumbnailRenderer RenderThumbnail)
{
	std::expected<std::unique_ptr<FTaskScope>, FTaskError> Scope = Tasks.CreateScope("Preview assets");
	if (!Scope)
	{
		HERTA_LOG_ERROR(Log, AssetLog, "Preview assets are unavailable: {}", Scope.error().Message);
		return nullptr;
	}

	std::unique_ptr<FPreviewAssets> Assets(new FPreviewAssets(Tasks, Device, Log, std::move(Paths), ObjectCount, std::move(*Scope), std::move(RenderThumbnail)));

	if (const auto Registered = RegisterAssetCommands(Assets->AssetCommands, {.DefaultContentRoot = Assets->Paths.ContentRoot, .DerivedDataRoot = Assets->Paths.DerivedDataRoot, .WorkerPath = Assets->Paths.WorkerPath, .TargetPlatform = Assets->Paths.TargetPlatform}); !Registered)
	{
		HERTA_LOG_ERROR(Log, AssetLog, "Dropped files cannot be imported: {}", Registered.error().Message);
	}

	Assets->RequestScan();
	Assets->CheckForChanges();
	return Assets;
}

FPreviewAssets::~FPreviewAssets()
{
	// Cancellation kills running workers, and cancelled continuations return before touching this object.
	Scope->RequestCancellation();
	Scope->Wait();
}

void FPreviewAssets::RequestScan()
{
	if (bScanning)
	{
		return;
	}

	bScanning = true;

	std::vector<std::filesystem::path> Roots;
	Roots.reserve(Mounts.size());

	for (const FMount& Mount : Mounts)
	{
		Roots.push_back(Mount.Root);
	}

	auto Results = std::make_shared<std::vector<std::expected<FContentScanResult, FAssetError>>>();
	auto ScannedFolders = std::make_shared<std::vector<std::string>>();
	std::vector<std::string> MountNames;

	for (const auto& Mount : Mounts)
	{
		MountNames.push_back(Mount.Name);
	}

	const auto Generation = FolderGeneration;
	std::expected<FTaskHandle, FTaskError> Scan = Tasks.Submit(*Scope, {.Name = "Scan content", .Lane = ETaskLane::BlockingIo}, [Results, ScannedFolders, MountNames = std::move(MountNames), Roots = std::move(Roots), Caches = &ScanCaches](FTaskContext&)
	{
		for (std::size_t Mount = 0; Mount < Roots.size(); ++Mount)
		{
			Results->push_back(ScanContentRoot(Roots[Mount], &(*Caches)[Mount]));
			auto Found = DiscoverFolders(Roots[Mount], MountNames[Mount]);
			ScannedFolders->insert(ScannedFolders->end(), std::make_move_iterator(Found.begin()), std::make_move_iterator(Found.end()));
		}
	});

	std::expected<FTaskHandle, FTaskError> Publish = Scan ? Tasks.ContinueOnMainThread(*Scope, *Scan, "Publish content scan", [this, Results, ScannedFolders, Generation](FTaskContext& Context)
	{
		if (Context.IsCancellationRequested())
		{
			return;
		}

		PublishScan(*Results);
		if (FolderGeneration == Generation)
		{
			PublishFolders(std::move(*ScannedFolders));
		}
	})
	                                                      : std::unexpected(Scan.error());

	if (!Publish)
	{
		bScanning = false;
		HERTA_LOG_ERROR(Log, AssetLog, "Could not scan content: {}", Publish.error().Message);
	}
}

void FPreviewAssets::Tick()
{
	if (std::exchange(bThumbnailRequestsChanged, false))
	{
		PruneCache();
	}

	for (const FAssetId& Asset : ThumbnailAssets)
	{
		GetOrLoadMesh(Asset, true);
		UpdateThumbnail(MeshCache.at(Asset));
	}

	const std::chrono::steady_clock::time_point Now = std::chrono::steady_clock::now();
	if (Now >= NextPoll)
	{
		NextPoll = Now + PollInterval;
		CheckForChanges();
	}
}

void FPreviewAssets::CheckForChanges()
{
	if (bPolling)
	{
		return;
	}

	bPolling = true;

	std::vector<std::filesystem::path> Roots;
	Roots.reserve(Mounts.size());

	for (const FMount& Mount : Mounts)
	{
		Roots.push_back(Mount.Root);
	}

	auto Taken = std::make_shared<std::vector<FContentSnapshot>>();
	auto TakenFolders = std::make_shared<std::vector<std::string>>();
	std::vector<std::string> MountNames;

	for (const auto& Mount : Mounts)
	{
		MountNames.push_back(Mount.Name);
	}

	const auto Generation = FolderGeneration;
	std::expected<FTaskHandle, FTaskError> Poll = Tasks.Submit(*Scope, {.Name = "Poll content", .Lane = ETaskLane::BlockingIo}, [Taken, TakenFolders, MountNames = std::move(MountNames), Roots = std::move(Roots)](FTaskContext&)
	{
		for (std::size_t Mount = 0; Mount < Roots.size(); ++Mount)
		{
			Taken->push_back(TakeContentSnapshot(Roots[Mount]));
			auto Found = DiscoverFolders(Roots[Mount], MountNames[Mount]);
			TakenFolders->insert(TakenFolders->end(), std::make_move_iterator(Found.begin()), std::make_move_iterator(Found.end()));
		}
	});

	std::expected<FTaskHandle, FTaskError> Compare = Poll ? Tasks.ContinueOnMainThread(*Scope, *Poll, "Compare content", [this, Taken, TakenFolders, Generation](FTaskContext& Context)
	{
		if (Context.IsCancellationRequested())
		{
			return;
		}

		bPolling = false;
		if (FolderGeneration == Generation)
		{
			PublishFolders(std::move(*TakenFolders));
		}

		if (*Taken == Snapshots)
		{
			return;
		}

		const bool bBaseline = Snapshots.empty();
		Snapshots = std::move(*Taken);

		if (bBaseline)
		{
			return;
		}

		HERTA_LOG_INFO(Log, AssetLog, "Content changed; reimporting shown assets");
		ContentChanged();
	})
	                                                      : std::unexpected(Poll.error());

	if (!Compare)
	{
		bPolling = false;
		HERTA_LOG_ERROR(Log, AssetLog, "Could not poll content: {}", Compare.error().Message);
	}
}

std::uint64_t FPreviewAssets::GetOptionsGeneration() const noexcept
{
	return OptionsGeneration;
}

std::span<const std::string> FPreviewAssets::GetFolders() const noexcept
{
	return Folders;
}

void FPreviewAssets::PublishFolders(std::vector<std::string> NewFolders)
{
	std::ranges::sort(NewFolders);
	if (NewFolders != Folders)
	{
		Folders = std::move(NewFolders);
		++FolderGeneration;
		++OptionsGeneration;
	}
}

std::expected<std::string, FAssetError> FPreviewAssets::CreateFolder(const std::string_view MountedParent, const std::string_view Name, const bool bUniqueName)
{
	if (const auto Valid = ValidateFolderName(Name); !Valid)
	{
		return std::unexpected(Valid.error());
	}

	const auto Parent = ResolveContentFolder(Paths.ContentRoot, "Game", MountedParent);
	if (!Parent)
	{
		return std::unexpected(Parent.error());
	}

	const auto Siblings = ReadSiblingNames(*Parent);
	if (!Siblings)
	{
		return std::unexpected(Siblings.error());
	}

	std::string UniqueName(Name);
	if (Siblings->contains(FoldFolderName(UniqueName)))
	{
		if (!bUniqueName)
		{
			return std::unexpected(FAssetError{"A file or folder with that name already exists"});
		}

		for (std::size_t Suffix = 2; Suffix <= Siblings->size() + 2; ++Suffix)
		{
			UniqueName = std::format("{} {}", Name, Suffix);
			if (!Siblings->contains(FoldFolderName(UniqueName)))
			{
				break;
			}
		}
	}

	if (const auto Valid = ValidateFolderName(UniqueName); !Valid)
	{
		return std::unexpected(Valid.error());
	}

	std::error_code Error;
	if (!std::filesystem::create_directory(*Parent / Utf8Path(UniqueName), Error) || Error)
	{
		return std::unexpected(FAssetError{std::format("Cannot create folder: {}", Error ? Error.message() : "destination already exists")});
	}

	const auto Created = std::format("{}/{}", MountedParent, UniqueName);
	auto Updated = Folders;
	Updated.push_back(Created);
	PublishFolders(std::move(Updated));
	RequestScan();
	return Created;
}

std::expected<std::string, FAssetError> FPreviewAssets::RenameFolder(const std::string_view MountedFolder, const std::string_view NewName)
{
	if (const auto Valid = ValidateFolderName(NewName); !Valid)
	{
		return std::unexpected(Valid.error());
	}

	const auto Separator = MountedFolder.find_last_of('/');
	if (Separator == std::string_view::npos)
	{
		return std::unexpected(FAssetError{"Content mount roots cannot be renamed"});
	}

	const auto Source = ResolveContentFolder(Paths.ContentRoot, "Game", MountedFolder);
	if (!Source)
	{
		return std::unexpected(Source.error());
	}

	std::error_code Error;
	if (!std::filesystem::is_empty(*Source, Error) || Error)
	{
		return std::unexpected(FAssetError{"Only empty content folders can be renamed"});
	}

	if (MountedFolder.substr(Separator + 1) == NewName)
	{
		return std::string(MountedFolder);
	}

	const auto Siblings = ReadSiblingNames(Source->parent_path());
	if (!Siblings)
	{
		return std::unexpected(Siblings.error());
	}

	if (Siblings->contains(FoldFolderName(NewName)))
	{
		return std::unexpected(FAssetError{"A file or folder with that name already exists"});
	}

	const auto Destination = Source->parent_path() / Utf8Path(NewName);
#ifdef _WIN32
	const bool bRenamed = MoveFileExW(Source->c_str(), Destination.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE;
#else
	const bool bRenamed = syscall(SYS_renameat2, AT_FDCWD, Source->c_str(), AT_FDCWD, Destination.c_str(), RENAME_NOREPLACE) == 0;
#endif
	if (!bRenamed)
	{
		return std::unexpected(FAssetError{"Cannot rename folder without replacing an existing destination"});
	}

	const auto Renamed = std::format("{}/{}", MountedFolder.substr(0, Separator), NewName);
	auto Updated = Folders;
	std::erase(Updated, MountedFolder);
	Updated.push_back(Renamed);
	PublishFolders(std::move(Updated));
	RequestScan();
	return Renamed;
}

std::expected<std::filesystem::path, FAssetError> FPreviewAssets::GetFolderPath(const std::string_view MountedFolder) const
{
	for (const auto& Mount : Mounts)
	{
		if (MountedFolder == Mount.Name || (MountedFolder.starts_with(Mount.Name) && MountedFolder.size() > Mount.Name.size() && MountedFolder[Mount.Name.size()] == '/'))
		{
			return ResolveContentFolder(Mount.Root, Mount.Name, MountedFolder);
		}
	}

	return std::unexpected(FAssetError{"Unknown content mount"});
}

void FPreviewAssets::ImportFiles(std::vector<std::filesystem::path> Files, std::string Destination)
{
	if (Files.empty())
	{
		return;
	}

	if (!Destination.empty() && !IsValidAssetPath(Destination))
	{
		HERTA_LOG_ERROR(Log, AssetLog, "Invalid import destination: {}", Destination);
		return;
	}

	struct FImportOutcome
	{
		bool bSucceeded = false;
		std::string Message;
	};

	auto Outcomes = std::make_shared<std::vector<FImportOutcome>>();
	// ponytail: asset.import is not cancellable, so closing the editor waits for the file being imported; thread ShouldCancel through the command if that hurts.
	std::expected<FTaskHandle, FTaskError> Import = Tasks.Submit(*Scope, {.Name = "Import dropped files", .Lane = ETaskLane::BlockingIo}, [this, Outcomes, Files = std::move(Files), Destination = std::move(Destination)](FTaskContext& Context)
	{
		for (const std::filesystem::path& File : Files)
		{
			if (Context.IsCancellationRequested())
			{
				return;
			}

			const std::u8string Utf8 = File.generic_u8string();
			const std::string Path(Utf8.begin(), Utf8.end());
			const std::optional<std::string_view> Importer = FindImporterForSource(File);
			if (!Importer)
			{
				Outcomes->push_back({.bSucceeded = false, .Message = std::format("Skipped '{}': Herta cannot import this file type", Path)});
				continue;
			}

			std::string Quoted;

			for (const char Character : Path)
			{
				if (Character == '"' || Character == '\\')
				{
					Quoted.push_back('\\');
				}

				Quoted.push_back(Character);
			}

			std::string Folder = Destination.empty() ? (*Importer == "Texture" ? "Textures" : "Models") : Destination;
			std::string QuotedFolder;
			for (const char Character : Folder)
			{
				if (Character == '"' || Character == '\\')
				{
					QuotedFolder.push_back('\\');
				}

				QuotedFolder.push_back(Character);
			}

			const auto Result = AssetCommands.Execute(std::format("asset.import \"{}\" --destination \"{}\"", Quoted, QuotedFolder));
			if (Result && Result->ExitCode == 0)
			{
				Outcomes->push_back({.bSucceeded = true, .Message = Result->Message});
			}
			else
			{
				Outcomes->push_back({.bSucceeded = false, .Message = std::format("Could not import '{}': {}", Path, Result ? Result->Message : Result.error().Message)});
			}
		}
	});

	std::expected<FTaskHandle, FTaskError> Report = Import ? Tasks.ContinueOnMainThread(*Scope, *Import, "Report imports", [this, Outcomes](FTaskContext& Context)
	{
		if (Context.IsCancellationRequested())
		{
			return;
		}

		--ImportsInFlight;

		for (const FImportOutcome& Outcome : *Outcomes)
		{
			if (Outcome.bSucceeded)
			{
				HERTA_LOG_INFO(Log, AssetLog, "{}", Outcome.Message);
			}
			else
			{
				HERTA_LOG_ERROR(Log, AssetLog, "{}", Outcome.Message);
			}
		}

		CheckForChanges();
	})
	                                                       : std::unexpected(Import.error());

	if (!Report)
	{
		HERTA_LOG_ERROR(Log, AssetLog, "Could not import dropped files: {}", Report.error().Message);
		return;
	}

	++ImportsInFlight;
}

void FPreviewAssets::PublishScan(const std::vector<std::expected<FContentScanResult, FAssetError>>& Results)
{
	bScanning = false;
	Options.clear();
	Locations.clear();
	++OptionsGeneration;

	for (std::size_t Mount = 0; Mount < Results.size(); ++Mount)
	{
		const std::expected<FContentScanResult, FAssetError>& Result = Results[Mount];
		if (!Result)
		{
			HERTA_LOG_ERROR(Log, AssetLog, "{} content scan failed: {}", Mounts[Mount].Name, Result.error().Message);
			continue;
		}

		if (!Result->Errors.empty())
		{
			HERTA_LOG_WARNING(Log, AssetLog, "{} content has {} files with errors. Run asset.validate for details", Mounts[Mount].Name, Result->Errors.size());
		}

		for (const FAssetRecord& Record : Result->Registry.GetRecords())
		{
			// Random IDs do not collide in practice; if one ever does, the Engine mount wins.
			if (Locations.emplace(Record.Id, FLocation{.Mount = Mount, .SourcePath = Record.SourcePath}).second)
			{
				Options.push_back({.Id = Record.Id, .Label = std::format("{}/{}", Mounts[Mount].Name, Record.SourcePath), .Importer = Record.Importer});
			}
		}
	}

	// The first scan releases requests made before it, such as the default preview meshes at startup.
	if (!std::exchange(bScanned, true))
	{
		for (const auto& [Asset, Cached] : MeshCache)
		{
			if (Asset.IsValid() && Cached.Slot.bLoading)
			{
				StartLoad(Asset);
			}
		}
	}

	if (std::exchange(bReimportAfterScan, false))
	{
		ReimportShownAssets();
	}
}

void FPreviewAssets::RequestMesh(const std::size_t Object, const FAssetId& Asset)
{
	FPreviewMeshSlot& Slot = Slots.at(Object);
	if (!Asset.IsValid())
	{
		Slot = {};
		PruneCache();
		return;
	}

	const FPreviewMeshSlot Previous = Slot;
	const auto Existing = MeshCache.find(Asset);
	if (bScanned && !bReimportAfterScan && Existing != MeshCache.end() && !Existing->second.Slot.bLoading && !Existing->second.Slot.Error.empty() && Existing->second.RequestContentGeneration == ContentGeneration)
	{
		StartLoad(Asset);
	}

	const FPreviewMeshSlot& Cached = GetOrLoadMesh(Asset);
	Slot = Cached;

	// Keep the old selection visible until its replacement finishes, without caching it under the new asset.
	if (Previous.Asset != Asset && Cached.bLoading && (!Cached.Mesh || Cached.ContentGeneration != ContentGeneration))
	{
		Slot.Mesh = Previous.Mesh;
		Slot.Key = Previous.Key;
	}

	PruneCache();
}

void FPreviewAssets::RebindObjects(const std::span<const FAssetId> Assets)
{
	std::vector<FPreviewMeshSlot> Rebound;
	Rebound.reserve(Assets.size());

	for (const FAssetId& Asset : Assets)
	{
		if (!Asset.IsValid())
		{
			Rebound.emplace_back();
			continue;
		}

		FPreviewMeshSlot Slot = GetOrLoadMesh(Asset);
		if (Slot.bLoading)
		{
			const auto Previous = std::ranges::find_if(Slots, [&Asset](const FPreviewMeshSlot& View)
			{
				return View.Asset == Asset && View.Mesh;
			});

			if (Previous != Slots.end())
			{
				Slot.Mesh = Previous->Mesh;
				Slot.Key = Previous->Key;
			}
		}

		Rebound.push_back(std::move(Slot));
	}

	Slots = std::move(Rebound);
	PruneCache();
}

void FPreviewAssets::SetThumbnailAssets(const std::span<const FAssetId> Assets)
{
	if (!RenderThumbnail)
	{
		return;
	}

	std::set<FAssetId> Requested;

	for (const FAssetId& Asset : Assets)
	{
		if (Asset.IsValid())
		{
			Requested.insert(Asset);
		}

		if (Requested.size() == 64)
		{
			break;
		}
	}

	if (Requested != ThumbnailAssets)
	{
		ThumbnailAssets = std::move(Requested);
		bThumbnailRequestsChanged = true;
	}
}

FEditorAssetThumbnail FPreviewAssets::GetThumbnail(const FAssetId& Asset) const noexcept
{
	const auto Cached = MeshCache.find(Asset);
	return Cached == MeshCache.end() ? nullptr : Cached->second.Slot.Thumbnail;
}

const FPreviewMeshSlot* FPreviewAssets::GetCachedAsset(const FAssetId& Asset) const noexcept
{
	const auto Cached = MeshCache.find(Asset);
	return Cached == MeshCache.end() ? nullptr : &Cached->second.Slot;
}

FPreviewMeshSlot& FPreviewAssets::GetOrLoadMesh(const FAssetId& Asset, const bool bThumbnailOnly)
{
	auto [Entry, bInserted] = MeshCache.try_emplace(Asset);
	FCachedMesh& Cached = Entry->second;
	if (bInserted)
	{
		Cached.Slot.Asset = Asset;
		Cached.Slot.Label = Asset.ToString();
		Cached.Slot.bLoading = true;
		Cached.RequestContentGeneration = ContentGeneration;
		Cached.bThumbnailOnly = bThumbnailOnly;
	}
	else if (!bThumbnailOnly)
	{
		Cached.bThumbnailOnly = false;
	}

	if (bScanned && !bReimportAfterScan && (bInserted || Cached.RequestContentGeneration != ContentGeneration))
	{
		StartLoad(Asset);
	}
	else if (Cached.RequestContentGeneration != ContentGeneration)
	{
		Cached.Slot.bLoading = true;
	}

	return Cached.Slot;
}

void FPreviewAssets::ContentChanged()
{
	++ContentGeneration;
	PruneCache();
	bReimportAfterScan = true;
	RequestScan();
}

void FPreviewAssets::StartLoad(const FAssetId& Asset)
{
	FCachedMesh& Cached = MeshCache.at(Asset);
	FPreviewMeshSlot& Slot = Cached.Slot;
	Cached.RequestContentGeneration = ContentGeneration;
	Slot.Generation = ++RequestGeneration;
	if (RequestGeneration == 0)
	{
		std::terminate();
	}

	Slot.bLoading = true;
	Slot.Error.clear();

	const auto Location = Locations.find(Slot.Asset);
	if (Location == Locations.end())
	{
		Slot.bLoading = false;
		Slot.Error = Slot.Mesh ? std::format("{} is no longer registered; keeping the previous version", Slot.Label) : std::format("Asset {} is not registered in content", Slot.Asset.ToString());
		HERTA_LOG_ERROR(Log, AssetLog, "{}", Slot.Error);
		PublishSlots(Slot);
		return;
	}

	Slot.Label = std::format("{}/{}", Mounts[Location->second.Mount].Name, Location->second.SourcePath);

	for (FPreviewMeshSlot& View : Slots)
	{
		if (View.Asset == Asset)
		{
			View.Generation = Slot.Generation;
			View.bLoading = true;
			View.Error.clear();
		}
	}

	const bool bSubmitted = SubmitCook(Location->second, Slot.Label, [this, Asset, Generation = Slot.Generation](FMeshLoad& Load)
	{
		PublishMesh(Asset, Generation, Load);
	});

	if (!bSubmitted)
	{
		Slot.bLoading = false;
		Slot.Error = std::format("Could not schedule cooking {}", Slot.Label);
		HERTA_LOG_ERROR(Log, AssetLog, "{}", Slot.Error);
		PublishSlots(Slot);
	}
}

bool FPreviewAssets::SubmitCook(const FLocation& Location, const std::string& Label, std::function<void(FMeshLoad&)> Publish)
{
	auto Load = std::make_shared<FMeshLoad>();
	FAssetCookRequest Request{.ContentRoot = Mounts[Location.Mount].Root, .DerivedDataRoot = Paths.DerivedDataRoot, .SourcePath = Location.SourcePath, .TargetPlatform = Paths.TargetPlatform, .bForce = false};
	std::expected<FTaskHandle, FTaskError> Cook = Tasks.Submit(*Scope, {.Name = "Cook " + Label, .Lane = ETaskLane::BlockingIo}, [Load, Request, Worker = Paths.WorkerPath](FTaskContext& Context)
	{
		std::expected<FAssetCookResult, FAssetError> Cooked = CookAssetInWorker(Request, {.WorkerPath = Worker, .ShouldCancel = [&Context]
		{
			return Context.IsCancellationRequested();
		}});

		if (!Cooked)
		{
			Load->Model = std::unexpected(std::move(Cooked.error()));
			return;
		}

		Load->Warnings = std::move(Cooked->Warnings);
		Load->Key = Cooked->Key;
		Load->bCacheHit = Cooked->bCacheHit;
		std::expected<FCookedAsset, FAssetError> Asset = LoadCookedAsset(Request.DerivedDataRoot, Cooked->Key);
		if (!Asset)
		{
			Load->Model = std::unexpected(std::move(Asset.error()));
			return;
		}

		Load->Metadata = GetPreviewAssetMetadata(*Asset);
		if (auto* const Texture = std::get_if<FCookedTexture>(&*Asset))
		{
			Load->Model = CreateTexturedCubeModel(std::move(*Texture));
		}
		else
		{
			Load->Model = std::move(std::get<FCookedModel>(*Asset));
		}
	});

	std::expected<FTaskHandle, FTaskError> Published = Cook ? Tasks.ContinueOnMainThread(*Scope, *Cook, "Publish preview mesh", [this, Load, Generation = ContentGeneration, Publish = std::move(Publish)](FTaskContext& Context)
	{
		if (!Context.IsCancellationRequested() && Generation == ContentGeneration)
		{
			Publish(*Load);
		}
	})
	                                                        : std::unexpected(Cook.error());

	return Published.has_value();
}

void FPreviewAssets::ReimportShownAssets()
{
	// One cook per shown asset, including failed loads that an edit may have fixed.
	std::set<FAssetId> Shown = ThumbnailAssets;

	for (const FPreviewMeshSlot& Slot : Slots)
	{
		if (Slot.Asset.IsValid() && MeshCache.contains(Slot.Asset))
		{
			Shown.insert(Slot.Asset);
		}
	}

	for (const FAssetId& Asset : Shown)
	{
		if (MeshCache.contains(Asset))
		{
			StartLoad(Asset);
		}
	}
}

void FPreviewAssets::PublishSlots(const FPreviewMeshSlot& Slot)
{
	for (FPreviewMeshSlot& View : Slots)
	{
		if (View.Asset == Slot.Asset)
		{
			View = Slot;
		}
	}
}

void FPreviewAssets::PruneCache()
{
	for (auto& [Asset, Cached] : MeshCache)
	{
		if (!ThumbnailAssets.contains(Asset))
		{
			Cached.Slot.Thumbnail.reset();
			Cached.ThumbnailKey.reset();
		}
	}

	for (FPreviewMeshSlot& Slot : Slots)
	{
		if (!ThumbnailAssets.contains(Slot.Asset))
		{
			Slot.Thumbnail.reset();
		}
	}

	std::erase_if(MeshCache, [this](const auto& Entry)
	{
		const auto& [Asset, Cached] = Entry;
		return !ThumbnailAssets.contains(Asset) && (Cached.bThumbnailOnly || !Cached.Slot.bLoading || Cached.Slot.Generation == 0 || Cached.RequestContentGeneration != ContentGeneration) && std::ranges::none_of(Slots, [&Asset](const FPreviewMeshSlot& Slot)
		{
			return Slot.Asset == Asset;
		});
	});
}

void FPreviewAssets::PublishMesh(const FAssetId& Asset, const std::uint64_t Generation, FMeshLoad& Load)
{
	const auto Entry = MeshCache.find(Asset);
	if (Entry == MeshCache.end() || Entry->second.Slot.Generation != Generation)
	{
		return;
	}

	if (!ThumbnailAssets.contains(Asset) && std::ranges::none_of(Slots, [&Asset](const FPreviewMeshSlot& Slot)
	{
		return Slot.Asset == Asset;
	}))
	{
		MeshCache.erase(Entry);
		return;
	}

	FPreviewMeshSlot& Slot = Entry->second.Slot;

	Slot.bLoading = false;

	for (const std::string& Warning : Load.Warnings)
	{
		HERTA_LOG_WARNING(Log, AssetLog, "{}: {}", Slot.Label, Warning);
	}

	if (!Load.Model)
	{
		Slot.Error = Slot.Mesh ? std::format("Reimport failed; keeping the previous version: {}", Load.Model.error().Message) : Load.Model.error().Message;
		HERTA_LOG_ERROR(Log, AssetLog, "{}", Slot.Error);
		PublishSlots(Slot);
		return;
	}

	if (Slot.Mesh && Slot.Key == Load.Key)
	{
		Slot.Metadata = Load.Metadata;
		Slot.ContentGeneration = ContentGeneration;
		UpdateThumbnail(Entry->second);
		PublishSlots(Slot);
		return;
	}

	std::expected<std::shared_ptr<const FRenderMesh>, FPresentationError> Mesh = FRenderMesh::Create(Device, *Load.Model, Slot.Label);
	if (!Mesh)
	{
		Slot.Error = Slot.Mesh ? std::format("Reimport failed; keeping the previous version: {}", Mesh.error().Message) : Mesh.error().Message;
		HERTA_LOG_ERROR(Log, AssetLog, "Could not upload {}: {}", Slot.Label, Slot.Error);
		PublishSlots(Slot);
		return;
	}

	Slot.Mesh = std::move(*Mesh);
	Slot.Metadata = Load.Metadata;
	Slot.Key = Load.Key;
	Slot.ContentGeneration = ContentGeneration;
	UpdateThumbnail(Entry->second);
	PublishSlots(Slot);
	HERTA_LOG_INFO(Log, AssetLog, "Loaded {} ({})", Slot.Label, Load.bCacheHit ? "cached" : "cooked");
}

void FPreviewAssets::UpdateThumbnail(FCachedMesh& Cached)
{
	FPreviewMeshSlot& Slot = Cached.Slot;
	if (!RenderThumbnail || !ThumbnailAssets.contains(Slot.Asset) || !Slot.Mesh || Cached.ThumbnailKey == Slot.Key)
	{
		return;
	}

	Cached.ThumbnailKey = Slot.Key;
	auto Thumbnail = RenderThumbnail(*Slot.Mesh);
	if (!Thumbnail)
	{
		HERTA_LOG_WARNING(Log, AssetLog, "Could not render thumbnail for {}: {}", Slot.Label, Thumbnail.error().Message);
		return;
	}

	Slot.Thumbnail = std::move(*Thumbnail);
}
}
