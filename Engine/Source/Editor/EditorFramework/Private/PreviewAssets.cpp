#include "PreviewAssets.h"

#include "Herta/AssetPipeline/AssetCooker.h"
#include "Herta/AssetPipeline/ContentRoot.h"
#include "Herta/Core/Log.h"
#include "Herta/Tasks/TaskSystem.h"

#include <format>
#include <utility>

namespace Herta
{
namespace
{
inline constexpr FLogCategory AssetLog{"Assets"};
}

struct FPreviewAssets::FMeshLoad
{
	std::expected<FCookedModel, FAssetError> Model = std::unexpected(FAssetError{"The cook task did not run"});
	std::vector<std::string> Warnings;
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
		Mounts.push_back({"Engine", Paths.EngineContentRoot});
	}
	if (!Paths.ContentRoot.empty())
	{
		Mounts.push_back({"Game", Paths.ContentRoot});
	}
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
	Assets->RequestScan();
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
	std::expected<FTaskHandle, FTaskError> Scan = Tasks.Submit(*Scope, {"Scan content", ETaskLane::BlockingIo}, [Results, Roots = std::move(Roots)](FTaskContext&)
	                                                           {
		                                                           for (const std::filesystem::path& Root : Roots)
		                                                           {
			                                                           Results->push_back(ScanContentRoot(Root));
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
			if (Locations.emplace(Record.Id, FLocation{Mount, Record.SourcePath}).second)
			{
				Options.push_back({Record.Id, std::format("{}/{}", Mounts[Mount].Name, Record.SourcePath), Record.Importer});
			}
		}
	}

	// The first scan releases requests made before it, such as the default preview meshes at startup.
	if (!std::exchange(bScanned, true))
	{
		for (std::size_t Object = 0; Object < Slots.size(); ++Object)
		{
			if (Slots[Object].bLoading)
			{
				StartLoad(Object);
			}
		}
	}
}

void FPreviewAssets::RequestMesh(const std::size_t Object, const FAssetId& Asset)
{
	FPreviewMeshSlot& Slot = Slots.at(Object);
	++Slot.Generation;
	Slot.Asset = Asset;
	Slot.Label = Asset.ToString();
	Slot.bLoading = true;
	Slot.Error.clear();
	if (bScanned)
	{
		StartLoad(Object);
	}
}

void FPreviewAssets::StartLoad(const std::size_t Object)
{
	FPreviewMeshSlot& Slot = Slots[Object];
	const auto Location = Locations.find(Slot.Asset);
	if (Location == Locations.end())
	{
		Slot.bLoading = false;
		Slot.Mesh.reset();
		Slot.Error = std::format("Asset {} is not registered in content", Slot.Asset.ToString());
		HERTA_LOG_ERROR(Log, AssetLog, "{}", Slot.Error);
		return;
	}
	const FMount& Mount = Mounts[Location->second.Mount];
	Slot.Label = std::format("{}/{}", Mount.Name, Location->second.SourcePath);
	if (std::shared_ptr<const FRenderMesh> Shared = FindLoadedMesh(Slot.Asset, Object))
	{
		Slot.Mesh = std::move(Shared);
		Slot.bLoading = false;
		return;
	}

	auto Load = std::make_shared<FMeshLoad>();
	FAssetCookRequest Request{Mount.Root, Paths.DerivedDataRoot, Location->second.SourcePath, Paths.TargetPlatform, false};
	std::expected<FTaskHandle, FTaskError> Cook = Tasks.Submit(*Scope, {"Cook " + Slot.Label, ETaskLane::BlockingIo}, [Load, Request, Worker = Paths.WorkerPath](FTaskContext& Context)
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
	std::expected<FTaskHandle, FTaskError> Publish = Cook ? Tasks.ContinueOnMainThread(*Scope, *Cook, "Publish preview mesh", [this, Load, Object, Generation = Slot.Generation](FTaskContext& Context)
	                                                                                   {
		                                                                                   if (Context.IsCancellationRequested())
		                                                                                   {
			                                                                                   return;
		                                                                                   }
		                                                                                   PublishMesh(Object, Generation, *Load);
	                                                                                   })
	                                                      : std::unexpected(Cook.error());
	if (!Publish)
	{
		Slot.bLoading = false;
		Slot.Mesh.reset();
		Slot.Error = Publish.error().Message;
		HERTA_LOG_ERROR(Log, AssetLog, "Could not load {}: {}", Slot.Label, Slot.Error);
	}
}

std::shared_ptr<const FRenderMesh> FPreviewAssets::FindLoadedMesh(const FAssetId& Asset, const std::size_t ExcludedObject) const
{
	// The requesting slot is excluded because it may still hold the mesh of the asset it is replacing.
	for (std::size_t Object = 0; Object < Slots.size(); ++Object)
	{
		const FPreviewMeshSlot& Slot = Slots[Object];
		if (Object != ExcludedObject && Slot.Asset == Asset && Slot.Mesh && !Slot.bLoading)
		{
			return Slot.Mesh;
		}
	}
	return nullptr;
}

void FPreviewAssets::PublishMesh(const std::size_t Object, const std::uint64_t Generation, FMeshLoad& Load)
{
	FPreviewMeshSlot& Slot = Slots[Object];
	if (Slot.Generation != Generation)
	{
		return;
	}
	Slot.bLoading = false;
	for (const std::string& Warning : Load.Warnings)
	{
		HERTA_LOG_WARNING(Log, AssetLog, "{}: {}", Slot.Label, Warning);
	}
	// The previous mesh stays visible while a replacement cooks, but a failure clears it so the viewport matches the selection.
	if (!Load.Model)
	{
		Slot.Mesh.reset();
		Slot.Error = Load.Model.error().Message;
		HERTA_LOG_ERROR(Log, AssetLog, "{}", Slot.Error);
		return;
	}
	// Objects that requested the same asset at once share one GPU copy.
	if (std::shared_ptr<const FRenderMesh> Shared = FindLoadedMesh(Slot.Asset, Object))
	{
		Slot.Mesh = std::move(Shared);
		return;
	}
	std::expected<std::shared_ptr<const FRenderMesh>, FPresentationError> Mesh = FRenderMesh::Create(Device, *Load.Model, Slot.Label);
	if (!Mesh)
	{
		Slot.Mesh.reset();
		Slot.Error = Mesh.error().Message;
		HERTA_LOG_ERROR(Log, AssetLog, "Could not upload {}: {}", Slot.Label, Slot.Error);
		return;
	}
	Slot.Mesh = std::move(*Mesh);
	HERTA_LOG_INFO(Log, AssetLog, "Loaded {} ({})", Slot.Label, Load.bCacheHit ? "cached" : "cooked");
}
}
