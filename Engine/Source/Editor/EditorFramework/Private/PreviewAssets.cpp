#include "PreviewAssets.h"

#include "Herta/AssetPipeline/AssetCommands.h"
#include "Herta/AssetPipeline/AssetCooker.h"
#include "Herta/AssetPipeline/ContentRoot.h"
#include "Herta/Core/Log.h"
#include "Herta/Tasks/TaskSystem.h"

#include <algorithm>
#include <format>
#include <set>
#include <utility>

namespace Herta
{
namespace
{
inline constexpr FLogCategory AssetLog{.Name = "Assets"};
inline constexpr std::chrono::seconds PollInterval{1};
}

struct FPreviewAssets::FMeshLoad
{
	std::expected<FCookedModel, FAssetError> Model = std::unexpected(FAssetError{"The cook task did not run"});
	std::vector<std::string> Warnings;
	FHash128 Key;
	bool bCacheHit = false;
};

FPreviewAssets::FPreviewAssets(FTaskSystem& InTasks, IGraphicsDevice& InDevice, FLogService& InLog, FEditorAssetPaths InPaths, const std::size_t ObjectCount, std::unique_ptr<FTaskScope> InScope)
    : Tasks(InTasks)
    , Device(InDevice)
    , Log(InLog)
    , Paths(std::move(InPaths))
    , Slots(ObjectCount)
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

	ScanCaches.resize(Mounts.size());
}

std::unique_ptr<FPreviewAssets> FPreviewAssets::Create(FTaskSystem& Tasks, IGraphicsDevice& Device, FLogService& Log, FEditorAssetPaths Paths, const std::size_t ObjectCount)
{
	std::expected<std::unique_ptr<FTaskScope>, FTaskError> Scope = Tasks.CreateScope("Preview assets");
	if (!Scope)
	{
		HERTA_LOG_ERROR(Log, AssetLog, "Preview assets are unavailable: {}", Scope.error().Message);
		return nullptr;
	}

	std::unique_ptr<FPreviewAssets> Assets(new FPreviewAssets(Tasks, Device, Log, std::move(Paths), ObjectCount, std::move(*Scope)));

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
	std::expected<FTaskHandle, FTaskError> Scan = Tasks.Submit(*Scope, {.Name = "Scan content", .Lane = ETaskLane::BlockingIo}, [Results, Roots = std::move(Roots), Caches = &ScanCaches](FTaskContext&)
	{
		for (std::size_t Mount = 0; Mount < Roots.size(); ++Mount)
		{
			Results->push_back(ScanContentRoot(Roots[Mount], &(*Caches)[Mount]));
		}
	});

	std::expected<FTaskHandle, FTaskError> Publish = Scan ? Tasks.ContinueOnMainThread(*Scope, *Scan, "Publish content scan", [this, Results](FTaskContext& Context)
	{
		if (Context.IsCancellationRequested())
		{
			return;
		}

		PublishScan(*Results);
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
	std::expected<FTaskHandle, FTaskError> Poll = Tasks.Submit(*Scope, {.Name = "Poll content", .Lane = ETaskLane::BlockingIo}, [Taken, Roots = std::move(Roots)](FTaskContext&)
	{
		for (const std::filesystem::path& Root : Roots)
		{
			Taken->push_back(TakeContentSnapshot(Root));
		}
	});

	std::expected<FTaskHandle, FTaskError> Compare = Poll ? Tasks.ContinueOnMainThread(*Scope, *Poll, "Compare content", [this, Taken](FTaskContext& Context)
	{
		if (Context.IsCancellationRequested())
		{
			return;
		}

		bPolling = false;

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

void FPreviewAssets::ImportFiles(std::vector<std::filesystem::path> Files)
{
	if (Files.empty())
	{
		return;
	}

	struct FImportOutcome
	{
		bool bSucceeded = false;
		std::string Message;
	};

	auto Outcomes = std::make_shared<std::vector<FImportOutcome>>();
	// ponytail: asset.import is not cancellable, so closing the editor waits for the file being imported; thread ShouldCancel through the command if that hurts.
	std::expected<FTaskHandle, FTaskError> Import = Tasks.Submit(*Scope, {.Name = "Import dropped files", .Lane = ETaskLane::BlockingIo}, [this, Outcomes, Files = std::move(Files)](FTaskContext& Context)
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

			const auto Result = AssetCommands.Execute(std::format("asset.import \"{}\" --destination {}", Quoted, *Importer == "Texture" ? "Textures" : "Models"));
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
			if (Cached.Slot.bLoading)
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

FPreviewMeshSlot& FPreviewAssets::GetOrLoadMesh(const FAssetId& Asset)
{
	auto [Entry, bInserted] = MeshCache.try_emplace(Asset);
	FCachedMesh& Cached = Entry->second;
	if (bInserted)
	{
		Cached.Slot.Asset = Asset;
		Cached.Slot.Label = Asset.ToString();
		Cached.Slot.bLoading = true;
		Cached.RequestContentGeneration = ContentGeneration;
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
		}
		else if (auto* const Texture = std::get_if<FCookedTexture>(&*Asset))
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
	std::set<FAssetId> Shown;

	for (const FPreviewMeshSlot& Slot : Slots)
	{
		if (MeshCache.contains(Slot.Asset))
		{
			Shown.insert(Slot.Asset);
		}
	}

	for (const FAssetId& Asset : Shown)
	{
		StartLoad(Asset);
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
	std::erase_if(MeshCache, [this](const auto& Entry)
	{
		const auto& [Asset, Cached] = Entry;
		return (!Cached.Slot.bLoading || Cached.Slot.Generation == 0 || Cached.RequestContentGeneration != ContentGeneration) && std::ranges::none_of(Slots, [&Asset](const FPreviewMeshSlot& Slot)
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

	if (std::ranges::none_of(Slots, [&Asset](const FPreviewMeshSlot& Slot)
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
		Slot.ContentGeneration = ContentGeneration;
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
	Slot.Key = Load.Key;
	Slot.ContentGeneration = ContentGeneration;
	PublishSlots(Slot);
	HERTA_LOG_INFO(Log, AssetLog, "Loaded {} ({})", Slot.Label, Load.bCacheHit ? "cached" : "cooked");
}
}
