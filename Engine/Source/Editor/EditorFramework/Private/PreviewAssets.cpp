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

using FMaterialTextureData = std::array<std::optional<FCookedTexture>, MaterialTextureSlotCount>;

std::expected<FMaterialTextureData, FAssetError> CookMaterialTextures(const FMaterialAsset& Material, const std::map<FAssetId, FAssetCookRequest>& Dependencies, const std::filesystem::path& Worker, const std::function<bool()>& ShouldCancel)
{
	FMaterialTextureData Textures;
	for (std::size_t Slot = 0; Slot < MaterialTextureSlotCount; ++Slot)
	{
		if (ShouldCancel())
		{
			return std::unexpected(FAssetError{"Material preview was superseded or cancelled"});
		}

		const FMaterialTextureBinding& Binding = Material.Textures[Slot];
		if (!Binding.Texture.IsValid())
		{
			continue;
		}

		const auto Dependency = Dependencies.find(Binding.Texture);
		if (Dependency == Dependencies.end())
		{
			return std::unexpected(FAssetError{std::format("Material texture {} is not registered in mounted content", Binding.Texture.ToString())});
		}

		FAssetCookRequest TextureRequest = Dependency->second;
		TextureRequest.TextureColorSpace = Binding.ColorSpace;
		const auto Cooked = CookAssetInWorker(TextureRequest, {.WorkerPath = Worker, .ShouldCancel = ShouldCancel});
		if (!Cooked)
		{
			return std::unexpected(Cooked.error());
		}

		auto TextureAsset = LoadCookedAsset(TextureRequest.DerivedDataRoot, Cooked->Key);
		if (!TextureAsset || !std::holds_alternative<FCookedTexture>(*TextureAsset))
		{
			return std::unexpected(TextureAsset ? FAssetError{"Material dependency is not a cooked texture"} : TextureAsset.error());
		}

		Textures[Slot] = std::move(std::get<FCookedTexture>(*TextureAsset));
		if (Textures[Slot]->PixelFormat == ETexturePixelFormat::Rgba32Float && Binding.ColorSpace != ETextureColorSpace::Linear)
		{
			return std::unexpected(FAssetError{"HDR textures require Linear color space"});
		}
	}

	return Textures;
}
}

struct FPreviewAssets::FMeshLoad
{
	std::expected<FCookedModel, FAssetError> Model = std::unexpected(FAssetError{"The cook task did not run"});
	std::optional<FMaterialAsset> Material;
	std::array<std::optional<FCookedTexture>, MaterialTextureSlotCount> MaterialTextures;
	std::shared_ptr<const FCookedTexture> Texture;
	bool bSucceeded = false;
	std::optional<FPreviewAssetMetadata> Metadata;
	std::vector<std::string> Warnings;
	FHash128 Key;
	bool bCacheHit = false;
};

FPreviewAssetMetadata GetPreviewAssetMetadata(const FCookedAsset& Asset)
{
	if (const auto* Texture = std::get_if<FCookedTexture>(&Asset))
	{
		return FPreviewTextureMetadata{.Width = Texture->Mips.empty() ? 0 : Texture->Mips.front().Width, .Height = Texture->Mips.empty() ? 0 : Texture->Mips.front().Height, .Mips = Texture->Mips.size(), .ColorSpace = Texture->ColorSpace, .PixelFormat = Texture->PixelFormat};
	}

	if (const auto* Material = std::get_if<FMaterialAsset>(&Asset))
	{
		return FPreviewMaterialMetadata{.Name = Material->Name, .TextureMaps = static_cast<std::size_t>(std::ranges::count_if(Material->Textures, [](const FMaterialTextureBinding& Binding)
		{
			return Binding.Texture.IsValid();
		})),
		    .BlendMode = Material->Parameters.BlendMode};
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

	std::vector<std::filesystem::path> DependencyRoots;
	for (const auto& Mount : Assets->Mounts)
	{
		DependencyRoots.push_back(Mount.Root);
	}

	if (!Assets->Paths.EngineContentRoot.empty())
	{
		DependencyRoots.push_back(Assets->Paths.EngineContentRoot.parent_path() / "Shaders");
	}

	if (const auto Registered = RegisterAssetCommands(Assets->AssetCommands, {.DefaultContentRoot = Assets->Paths.ContentRoot, .DerivedDataRoot = Assets->Paths.DerivedDataRoot, .WorkerPath = Assets->Paths.WorkerPath, .TargetPlatform = Assets->Paths.TargetPlatform, .DependencyContentRoots = std::move(DependencyRoots), .EngineContentRoot = Assets->Paths.EngineContentRoot, .GameContentRoot = Assets->Paths.ContentRoot}); !Registered)
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
		bScanAgain = true;
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
	if (bPreviewPending && !bPreviewTask && bScanned && !bReimportAfterScan)
	{
		StartMaterialPreview();
	}

	if (bPreviewThumbnailDirty && !bPreviewTask && PreviewMaterial)
	{
		UpdateMaterialPreviewThumbnail();
	}

	if (!bScanning && std::exchange(bScanAgain, false))
	{
		RequestScan();
	}

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
	if (!std::exchange(bScanned, true) && !bReimportAfterScan)
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

void FPreviewAssets::RequestMaterial(const FAssetId& Asset)
{
	if (!Asset.IsValid())
	{
		return;
	}

	MaterialAssets.insert(Asset);
	GetOrLoadMesh(Asset);
}

void FPreviewAssets::SetMaterialAssets(const std::span<const FAssetId> Assets)
{
	std::set<FAssetId> Requested;
	for (const FAssetId& Asset : Assets)
	{
		if (Asset.IsValid())
		{
			Requested.insert(Asset);
		}
	}

	MaterialAssets = std::move(Requested);
	PruneCache();

	for (const FAssetId& Asset : MaterialAssets)
	{
		GetOrLoadMesh(Asset);
	}
}

std::shared_ptr<const FRenderMaterial> FPreviewAssets::GetMaterial(const FAssetId& Asset) const noexcept
{
	const auto* Cached = GetCachedAsset(Asset);
	return Cached ? Cached->Material : nullptr;
}

const FMaterialAsset* FPreviewAssets::GetMaterialSource(const FAssetId& Asset) const noexcept
{
	const auto* Cached = GetCachedAsset(Asset);
	return Cached && Cached->MaterialSource ? &*Cached->MaterialSource : nullptr;
}

std::expected<FAssetId, FAssetError> FPreviewAssets::CreateMaterial(const std::string_view MountedParent, const std::string_view Name)
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

	const auto Names = ReadSiblingNames(*Parent);
	if (!Names)
	{
		return std::unexpected(Names.error());
	}

	std::string UniqueName(Name);
	for (std::size_t Suffix = 1; Names->contains(FoldFolderName(UniqueName + ".hmat")) || Names->contains(FoldFolderName(UniqueName + ".hmat.hmeta")); ++Suffix)
	{
		UniqueName = std::format("{} {}", Name, Suffix);
	}

	if (UniqueName.size() > 244)
	{
		return std::unexpected(FAssetError{"Material name is too long"});
	}

	const auto Path = *Parent / Utf8Path(UniqueName + ".hmat");
	if (const auto Written = WriteMaterialAsset(Path, FMaterialAsset{.Name = UniqueName}, false); !Written)
	{
		return std::unexpected(Written.error());
	}

	const auto Imported = ImportSource(Paths.ContentRoot, Path);
	if (!Imported)
	{
		return std::unexpected(FAssetError{std::format("Created {}; asset registration failed: {}", PathToUtf8(Path), Imported.error().Message)});
	}

	const auto Mount = std::ranges::find(Mounts, "Game", &FMount::Name);
	if (Mount == Mounts.end())
	{
		return std::unexpected(FAssetError{"Game content mount is unavailable"});
	}

	Locations[Imported->Metadata.Id] = {.Mount = static_cast<std::size_t>(Mount - Mounts.begin()), .SourcePath = Imported->SourcePath};
	ContentChanged();
	RequestMaterial(Imported->Metadata.Id);
	return Imported->Metadata.Id;
}

std::expected<void, FAssetError> FPreviewAssets::SaveMaterial(const FAssetId& Asset, const FMaterialAsset& Material)
{
	const auto Location = Locations.find(Asset);
	if (Location == Locations.end() || Mounts[Location->second.Mount].Name != "Game")
	{
		return std::unexpected(FAssetError{"Engine content is read-only; save a material in Game content"});
	}

	const auto Path = GetMaterialPath(Asset);
	if (!Path)
	{
		return std::unexpected(Path.error());
	}

	if (const auto Written = WriteMaterialAsset(*Path, Material); !Written)
	{
		return Written;
	}

	ContentChanged();
	return {};
}

std::expected<std::filesystem::path, FAssetError> FPreviewAssets::GetMaterialPath(const FAssetId& Asset) const
{
	const auto Location = Locations.find(Asset);
	if (Location == Locations.end() || FindImporterForSource(Utf8Path(Location->second.SourcePath)) != "Material")
	{
		return std::unexpected(FAssetError{"Material is not registered in content"});
	}

	const FMount& Mount = Mounts[Location->second.Mount];
	const auto Relative = Utf8Path(Location->second.SourcePath);
	const auto Parent = ResolveContentFolder(Mount.Root, Mount.Name, Relative.parent_path().empty() ? Mount.Name : std::format("{}/{}", Mount.Name, PathToUtf8(Relative.parent_path())));
	if (!Parent)
	{
		return std::unexpected(Parent.error());
	}

	const auto Path = *Parent / Relative.filename();
	std::error_code Error;
	if (std::filesystem::is_symlink(Path, Error) || Error)
	{
		return std::unexpected(FAssetError{"Material source must not be a symbolic link"});
	}

	return Path;
}

void FPreviewAssets::RequestTexture(const FAssetId& Asset)
{
	if (!Asset.IsValid())
	{
		return;
	}

	const bool bNewRequest = TextureAssets.insert(Asset).second;
	const auto& Cached = GetOrLoadMesh(Asset);
	if (bNewRequest && Cached.Mesh && !Cached.Texture && !Cached.bLoading)
	{
		StartLoad(Asset);
	}
}

void FPreviewAssets::SetTextureAssets(const std::span<const FAssetId> Assets)
{
	std::set<FAssetId> Requested;
	for (const FAssetId& Asset : Assets)
	{
		if (Asset.IsValid())
		{
			Requested.insert(Asset);
		}
	}

	std::erase_if(TextureAssets, [&Requested](const FAssetId& Asset)
	{
		return !Requested.contains(Asset);
	});

	for (const FAssetId& Asset : Requested)
	{
		RequestTexture(Asset);
	}

	PruneCache();
}

std::shared_ptr<const FCookedTexture> FPreviewAssets::GetCookedTexture(const FAssetId& Asset) const noexcept
{
	const auto* Cached = GetCachedAsset(Asset);
	return Cached ? Cached->CookedTexture : nullptr;
}

FTextureHandle FPreviewAssets::GetTexture(const FAssetId& Asset) const noexcept
{
	const auto* Cached = GetCachedAsset(Asset);
	return Cached ? Cached->Texture : nullptr;
}

void FPreviewAssets::SetMaterialThumbnailRenderer(FEditorMaterialThumbnailRenderer Renderer)
{
	RenderMaterialThumbnail = std::move(Renderer);
	for (auto& [Asset, Cached] : MeshCache)
	{
		if (Cached.Slot.Material)
		{
			Cached.ThumbnailKey.reset();
		}
	}

	bPreviewThumbnailDirty = PreviewMaterial != nullptr;
}

void FPreviewAssets::SetMaterialShaderGeneration(const std::uint64_t Generation)
{
	if (MaterialShaderGeneration == Generation)
	{
		return;
	}

	MaterialShaderGeneration = Generation;
	for (auto& [Asset, Cached] : MeshCache)
	{
		if (Cached.Slot.Material)
		{
			Cached.ThumbnailKey.reset();
		}
	}

	bPreviewThumbnailDirty = PreviewMaterial != nullptr;
}

void FPreviewAssets::UpdateMaterialPreviewThumbnail()
{
	if (!RenderMaterialThumbnail || !PreviewMaterial)
	{
		bPreviewThumbnailDirty = false;
		return;
	}

	const auto Mesh = GetMaterialPreviewMesh();
	if (!Mesh)
	{
		PreviewError = Mesh.error().Message;
		bPreviewThumbnailDirty = false;
		return;
	}

	if (!*Mesh)
	{
		return;
	}

	const auto Thumbnail = RenderMaterialThumbnail(**Mesh, *PreviewMaterial);
	bPreviewThumbnailDirty = false;
	if (!Thumbnail)
	{
		PreviewError = Thumbnail.error().Message;
		return;
	}

	PreviewThumbnail = *Thumbnail;
}

bool FPreviewAssets::AreMaterialThumbnailsReady() const noexcept
{
	if (!RenderMaterialThumbnail)
	{
		return true;
	}

	bool bNeeded = PreviewDraft.has_value();
	for (const FAssetId Asset : ThumbnailAssets)
	{
		const auto Cached = MeshCache.find(Asset);
		const auto Option = std::ranges::find(Options, Asset, &FPreviewAssetOption::Id);
		if (Option != Options.end() && Option->Importer == "Material")
		{
			bNeeded = true;
			if (Cached == MeshCache.end() || Cached->second.Slot.bLoading || !Cached->second.Slot.Thumbnail || Cached->second.ThumbnailKey != Cached->second.Slot.Key)
			{
				return false;
			}
		}
	}

	const auto* Sphere = GetCachedAsset(MaterialPreviewMeshAsset);
	return !bNeeded || (Sphere && Sphere->Mesh && !Sphere->bLoading && !IsMaterialPreviewLoading());
}

std::expected<std::shared_ptr<const FRenderMesh>, FAssetError> FPreviewAssets::GetMaterialPreviewMesh()
{
	if (!bScanned)
	{
		return nullptr;
	}

	if (!MaterialPreviewMeshAsset.IsValid())
	{
		const auto Sphere = std::ranges::find(Options, "Engine/Shapes/Sphere.gltf", &FPreviewAssetOption::Label);
		if (Sphere == Options.end())
		{
			return std::unexpected(FAssetError{"Material preview requires the registered Engine/Shapes/Sphere.gltf asset"});
		}

		MaterialPreviewMeshAsset = Sphere->Id;
	}

	const auto& Cached = GetOrLoadMesh(MaterialPreviewMeshAsset);
	if (!Cached.Mesh && !Cached.bLoading && !Cached.Error.empty())
	{
		return std::unexpected(FAssetError{Cached.Error});
	}

	return Cached.Mesh;
}

void FPreviewAssets::RequestMaterialPreview(const FAssetId& Asset, const FMaterialAsset& Draft)
{
	if (!Asset.IsValid())
	{
		PreviewGeneration->fetch_add(1, std::memory_order_acq_rel);
		PreviewDraft.reset();
		PreviewDraftAsset = {};
		PublishedPreviewDraft.reset();
		PublishedPreviewAsset = {};
		PreviewMaterial.reset();
		PreviewThumbnail.reset();
		bPreviewThumbnailDirty = false;
		PreviewError.clear();
		bPreviewPending = false;
		return;
	}

	if (PreviewDraftAsset == Asset && PreviewDraft && *PreviewDraft == Draft)
	{
		return;
	}

	if (PreviewDraftAsset != Asset)
	{
		PublishedPreviewDraft.reset();
		PublishedPreviewAsset = {};
		PreviewMaterial.reset();
		PreviewThumbnail.reset();
		bPreviewThumbnailDirty = false;
	}

	PreviewDraftAsset = Asset;
	PreviewDraft = Draft;
	PreviewGeneration->fetch_add(1, std::memory_order_acq_rel);
	PreviewError.clear();
	const auto Valid = ValidateMaterial(Draft);
	if (!Valid)
	{
		PreviewError = Valid.error().Message;
		bPreviewPending = false;
		return;
	}

	bPreviewPending = true;
	if (!bPreviewTask && bScanned && !bReimportAfterScan)
	{
		StartMaterialPreview();
	}
}

std::shared_ptr<const FRenderMaterial> FPreviewAssets::GetMaterialPreview() const noexcept
{
	return PreviewMaterial;
}

FEditorAssetThumbnail FPreviewAssets::GetMaterialPreviewThumbnail() const noexcept
{
	return PreviewThumbnail;
}

FAssetId FPreviewAssets::GetMaterialPreviewAsset() const noexcept
{
	return PublishedPreviewAsset;
}

bool FPreviewAssets::IsMaterialPreviewLoading() const noexcept
{
	return PreviewDraft && (bPreviewTask || bPreviewPending || bPreviewThumbnailDirty);
}

std::string_view FPreviewAssets::GetMaterialPreviewError() const noexcept
{
	return PreviewError;
}

std::map<FAssetId, FAssetCookRequest> FPreviewAssets::GetTextureRequests() const
{
	std::map<FAssetId, FAssetCookRequest> Requests;
	for (const auto& [Asset, Location] : Locations)
	{
		if (FindImporterForSource(Utf8Path(Location.SourcePath)) != "Texture")
		{
			continue;
		}

		FAssetCookRequest Request{.ContentRoot = Mounts[Location.Mount].Root, .DerivedDataRoot = Paths.DerivedDataRoot, .SourcePath = Location.SourcePath, .TargetPlatform = Paths.TargetPlatform};
		Request.EngineContentRoot = Paths.EngineContentRoot;
		Request.GameContentRoot = Paths.ContentRoot;
		for (const FMount& Mount : Mounts)
		{
			Request.DependencyContentRoots.push_back(Mount.Root);
		}

		if (!Paths.EngineContentRoot.empty())
		{
			Request.DependencyContentRoots.push_back(Paths.EngineContentRoot.parent_path() / "Shaders");
		}

		Requests.emplace(Asset, std::move(Request));
	}

	return Requests;
}

void FPreviewAssets::StartMaterialPreview()
{
	if (!PreviewDraft || !bPreviewPending || bPreviewTask)
	{
		return;
	}

	std::shared_ptr<const FRenderMesh> PreviewMesh;
	if (RenderMaterialThumbnail)
	{
		const auto Mesh = GetMaterialPreviewMesh();
		if (!Mesh)
		{
			PreviewError = Mesh.error().Message;
			bPreviewPending = false;
			return;
		}

		if (!*Mesh)
		{
			return;
		}

		PreviewMesh = *Mesh;
	}

	const FMaterialAsset Draft = *PreviewDraft;
	const auto Valid = ValidateMaterial(Draft);
	if (!Valid)
	{
		PreviewError = Valid.error().Message;
		bPreviewPending = false;
		return;
	}

	const FAssetId Asset = PreviewDraftAsset;
	const auto Location = Locations.find(Asset);
	const std::string Label = Location != Locations.end() ? std::format("{}/{} draft", Mounts[Location->second.Mount].Name, Location->second.SourcePath) : "Game/Material draft";
	const std::uint64_t Generation = PreviewGeneration->load(std::memory_order_acquire);
	const std::uint64_t Content = ContentGeneration;
	std::shared_ptr<const FRenderMaterial> Reuse;
	if (PublishedPreviewAsset == Asset && PublishedPreviewContentGeneration == Content && PublishedPreviewDraft && PublishedPreviewDraft->Textures == Draft.Textures && PublishedPreviewDraft->ShaderPath == Draft.ShaderPath)
	{
		Reuse = PreviewMaterial;
	}
	else if (const auto* Saved = GetCachedAsset(Asset); Saved && Saved->ContentGeneration == Content && Saved->MaterialSource && Saved->MaterialSource->Textures == Draft.Textures && Saved->MaterialSource->ShaderPath == Draft.ShaderPath)
	{
		Reuse = Saved->Material;
	}

	auto Textures = std::make_shared<std::expected<FMaterialTextureData, FAssetError>>(std::unexpected(FAssetError{"Material preview task did not run"}));
	const auto Cook = Tasks.Submit(*Scope, {.Name = "Cook material draft", .Lane = ETaskLane::BlockingIo}, [Textures, Draft, Dependencies = GetTextureRequests(), Worker = Paths.WorkerPath, Token = PreviewGeneration, Generation, bReuse = static_cast<bool>(Reuse)](FTaskContext& Context)
	{
		const auto Cancelled = [&]
		{
			return Context.IsCancellationRequested() || Token->load(std::memory_order_acquire) != Generation;
		};

		if (Cancelled())
		{
			return;
		}

		if (bReuse)
		{
			*Textures = FMaterialTextureData{};
		}
		else
		{
			*Textures = CookMaterialTextures(Draft, Dependencies, Worker, Cancelled);
		}
	});

	const auto Publish = Cook ? Tasks.ContinueOnMainThread(*Scope, *Cook, "Publish material draft", [this, Textures, Draft, Asset, Label, Generation, Content, Reuse = std::move(Reuse), PreviewMesh = std::move(PreviewMesh)](FTaskContext& Context)
	{
		if (Context.IsCancellationRequested())
		{
			return;
		}

		bPreviewTask = false;
		if (PreviewGeneration->load(std::memory_order_acquire) != Generation || ContentGeneration != Content)
		{
			if (PreviewDraft && ContentGeneration != Content)
			{
				bPreviewPending = true;
			}

			if (bPreviewPending && bScanned && !bReimportAfterScan)
			{
				StartMaterialPreview();
			}

			return;
		}

		if (!*Textures)
		{
			PreviewError = Textures->error().Message;
			return;
		}

		std::shared_ptr<const FRenderMaterial> Material;
		if (Reuse)
		{
			const auto Updated = Reuse->WithParameters(Draft.Parameters);
			if (!Updated)
			{
				PreviewError = Updated.error().Message;
				return;
			}

			Material = *Updated;
		}
		else
		{
			const auto Created = FRenderMaterial::Create(Device, Draft, **Textures, Label);
			if (!Created)
			{
				PreviewError = Created.error().Message;
				return;
			}

			Material = *Created;
		}

		FEditorAssetThumbnail Thumbnail;
		if (RenderMaterialThumbnail)
		{
			const auto Rendered = RenderMaterialThumbnail(*PreviewMesh, *Material);
			if (!Rendered)
			{
				PreviewError = Rendered.error().Message;
				return;
			}

			Thumbnail = *Rendered;
		}

		PreviewMaterial = std::move(Material);
		PreviewThumbnail = std::move(Thumbnail);
		bPreviewThumbnailDirty = false;
		PublishedPreviewDraft = Draft;
		PublishedPreviewAsset = Asset;
		PublishedPreviewContentGeneration = Content;
		PreviewError.clear();
	})
	                          : std::unexpected(Cook.error());

	if (!Publish)
	{
		PreviewError = Publish.error().Message;
		bPreviewPending = false;
		return;
	}

	bPreviewTask = true;
	bPreviewPending = false;
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
	if (PreviewDraft)
	{
		PreviewGeneration->fetch_add(1, std::memory_order_acq_rel);
		bPreviewPending = true;
	}

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
		Slot.Error = Slot.Mesh || Slot.Material ? std::format("{} is no longer registered; keeping the previous version", Slot.Label) : std::format("Asset {} is not registered in content", Slot.Asset.ToString());
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
	Request.EngineContentRoot = Paths.EngineContentRoot;
	Request.GameContentRoot = Paths.ContentRoot;
	for (const FMount& Mount : Mounts)
	{
		Request.DependencyContentRoots.push_back(Mount.Root);
	}

	if (!Paths.EngineContentRoot.empty())
	{
		Request.DependencyContentRoots.push_back(Paths.EngineContentRoot.parent_path() / "Shaders");
	}

	std::expected<FTaskHandle, FTaskError> Cook = Tasks.Submit(*Scope, {.Name = "Cook " + Label, .Lane = ETaskLane::BlockingIo}, [Load, Request, Dependencies = GetTextureRequests(), Worker = Paths.WorkerPath](FTaskContext& Context)
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
			Load->Texture = std::make_shared<const FCookedTexture>(std::move(*Texture));
			Load->Model = CreateTexturedCubeModel(*Load->Texture);
		}
		else if (auto* const Material = std::get_if<FMaterialAsset>(&*Asset))
		{
			Load->Material = std::move(*Material);
			auto Textures = CookMaterialTextures(*Load->Material, Dependencies, Worker, [&Context]
			{
				return Context.IsCancellationRequested();
			});

			if (!Textures)
			{
				Load->Model = std::unexpected(Textures.error());
				return;
			}

			Load->MaterialTextures = std::move(*Textures);
		}
		else
		{
			Load->Model = std::move(std::get<FCookedModel>(*Asset));
		}

		Load->bSucceeded = true;
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
	if (MaterialPreviewMeshAsset.IsValid())
	{
		Shown.insert(MaterialPreviewMeshAsset);
	}
	Shown.insert(MaterialAssets.begin(), MaterialAssets.end());
	Shown.insert(TextureAssets.begin(), TextureAssets.end());

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
		return Asset != MaterialPreviewMeshAsset && !ThumbnailAssets.contains(Asset) && !MaterialAssets.contains(Asset) && !TextureAssets.contains(Asset) && (Cached.bThumbnailOnly || !Cached.Slot.bLoading || Cached.Slot.Generation == 0 || Cached.RequestContentGeneration != ContentGeneration) && std::ranges::none_of(Slots, [&Asset](const FPreviewMeshSlot& Slot)
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

	if (Asset != MaterialPreviewMeshAsset && !ThumbnailAssets.contains(Asset) && !MaterialAssets.contains(Asset) && !TextureAssets.contains(Asset) && std::ranges::none_of(Slots, [&Asset](const FPreviewMeshSlot& Slot)
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

	if (!Load.bSucceeded)
	{
		Slot.Error = Slot.Mesh || Slot.Material ? std::format("Reimport failed; keeping the previous version: {}", Load.Model.error().Message) : Load.Model.error().Message;
		HERTA_LOG_ERROR(Log, AssetLog, "{}", Slot.Error);
		PublishSlots(Slot);
		return;
	}

	if ((Slot.Mesh || Slot.Material) && Slot.Key == Load.Key && (!TextureAssets.contains(Asset) || Slot.Texture))
	{
		Slot.Metadata = Load.Metadata;
		Slot.ContentGeneration = ContentGeneration;
		UpdateThumbnail(Entry->second);
		PublishSlots(Slot);
		return;
	}

	if (Load.Material)
	{
		const auto Material = FRenderMaterial::Create(Device, *Load.Material, Load.MaterialTextures, Slot.Label);
		if (!Material)
		{
			Slot.Error = Slot.Material ? std::format("Reimport failed; keeping the previous version: {}", Material.error().Message) : Material.error().Message;
			HERTA_LOG_ERROR(Log, AssetLog, "Could not upload {}: {}", Slot.Label, Slot.Error);
			return;
		}

		Slot.Material = *Material;
		Slot.MaterialSource = std::move(Load.Material);
		Slot.Mesh.reset();
	}
	else
	{
		const auto Mesh = FRenderMesh::Create(Device, *Load.Model, Slot.Label);
		if (!Mesh)
		{
			Slot.Error = Slot.Mesh ? std::format("Reimport failed; keeping the previous version: {}", Mesh.error().Message) : Mesh.error().Message;
			HERTA_LOG_ERROR(Log, AssetLog, "Could not upload {}: {}", Slot.Label, Slot.Error);
			PublishSlots(Slot);
			return;
		}

		FTextureHandle Texture;
		if (Load.Texture && TextureAssets.contains(Asset))
		{
			const FCookedTexture& Cooked = *Load.Texture;
			const ETextureFormat Format = Cooked.PixelFormat == ETexturePixelFormat::Rgba32Float ? ETextureFormat::Rgba32Float : Cooked.ColorSpace == ETextureColorSpace::Srgb ? ETextureFormat::Rgba8Srgb
			                                                                                                                                                                   : ETextureFormat::Rgba8;
			const auto Created = Device.CreateTexture({.Name = Slot.Label, .Extent = {.Width = Cooked.Mips.front().Width, .Height = Cooked.Mips.front().Height}, .Format = Format, .MipLevels = static_cast<std::uint32_t>(Cooked.Mips.size())});
			if (!Created)
			{
				Slot.Error = Created.error().Message;
				return;
			}

			auto Uploaded = Device.BeginCommands();
			if (!Uploaded)
			{
				Slot.Error = Uploaded.error().Message;
				return;
			}

			for (std::uint32_t Mip = 0; Mip < Cooked.Mips.size() && Uploaded; ++Mip)
			{
				Uploaded = Device.WriteTexture(*Created, Mip, Cooked.Mips[Mip].Pixels);
			}

			if (!Uploaded)
			{
				Device.CancelCommands();
				Slot.Error = Uploaded.error().Message;
				return;
			}

			const auto Submitted = Device.SubmitCommands();
			if (!Submitted)
			{
				Device.CancelCommands();
				Slot.Error = Submitted.error().Message;
				return;
			}

			Texture = *Created;
		}

		Slot.Mesh = *Mesh;
		if (Asset == MaterialPreviewMeshAsset)
		{
			for (auto& [Id, Cached] : MeshCache)
			{
				if (Cached.Slot.Material)
				{
					Cached.ThumbnailKey.reset();
				}
			}
		}
		Slot.CookedTexture = std::move(Load.Texture);
		Slot.Texture = std::move(Texture);
		Slot.Material.reset();
		Slot.MaterialSource.reset();
	}

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
	if (!ThumbnailAssets.contains(Slot.Asset) || Cached.ThumbnailKey == Slot.Key || (Slot.Material ? !RenderMaterialThumbnail : !RenderThumbnail || !Slot.Mesh))
	{
		return;
	}

	std::expected<FEditorAssetThumbnail, FPresentationError> Thumbnail;
	if (Slot.Material)
	{
		const auto Mesh = GetMaterialPreviewMesh();
		if (!Mesh)
		{
			Cached.ThumbnailKey = Slot.Key;
			HERTA_LOG_WARNING(Log, AssetLog, "{}", Mesh.error().Message);
			return;
		}

		if (!*Mesh)
		{
			return;
		}

		Cached.ThumbnailKey = Slot.Key;
		Thumbnail = RenderMaterialThumbnail(**Mesh, *Slot.Material);
	}
	else
	{
		Cached.ThumbnailKey = Slot.Key;
		Thumbnail = RenderThumbnail(*Slot.Mesh);
	}
	if (!Thumbnail)
	{
		HERTA_LOG_WARNING(Log, AssetLog, "Could not render thumbnail for {}: {}", Slot.Label, Thumbnail.error().Message);
		return;
	}

	Slot.Thumbnail = std::move(*Thumbnail);
}
}
