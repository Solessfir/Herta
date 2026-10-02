#include "PreviewAssets.h"

#include "Herta/AssetPipeline/AssetCooker.h"
#include "Herta/AssetPipeline/ContentRoot.h"
#include "Herta/Core/Log.h"
#include "Herta/Tasks/TaskSystem.h"

#include <utility>

namespace Herta
{
namespace
{
inline constexpr FLogCategory AssetLog{"Assets"};

struct FMeshLoad
{
	std::expected<FCookedModel, FAssetError> Model = std::unexpected(FAssetError{"The cook task did not run"});
	std::vector<std::string> Warnings;
	bool bCacheHit = false;
};
}

struct FPreviewAssets::FState
{
	FAssetRegistry Registry;
	bool bScanning = false;
	std::vector<FPreviewMeshSlot> Slots;
};

FPreviewAssets::FPreviewAssets(FTaskSystem& InTasks, IGraphicsDevice& InDevice, FLogService& InLog, FEditorAssetPaths InPaths, const std::size_t ObjectCount, std::unique_ptr<FTaskScope> InScope)
    : Tasks(InTasks)
    , Device(InDevice)
    , Log(InLog)
    , Paths(std::move(InPaths))
    , State(std::make_shared<FState>())
    , Scope(std::move(InScope))
{
	State->Slots.resize(ObjectCount);
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
	// Cancellation kills running workers, and cancelled continuations skip their GPU uploads.
	Scope->RequestCancellation();
	Scope->Wait();
}

void FPreviewAssets::RequestScan()
{
	if (State->bScanning)
	{
		return;
	}
	State->bScanning = true;

	auto Result = std::make_shared<std::expected<FContentScanResult, FAssetError>>(std::unexpected(FAssetError{"The scan task did not run"}));
	std::expected<FTaskHandle, FTaskError> Scan = Tasks.Submit(*Scope, {"Scan content", ETaskLane::BlockingIo}, [Result, Root = Paths.ContentRoot](FTaskContext&)
	                                                           {
		                                                           *Result = ScanContentRoot(Root);
	                                                           });
	std::expected<FTaskHandle, FTaskError> Publish = Scan ? Tasks.ContinueOnMainThread(*Scope, *Scan, "Publish content scan", [Result, State = State, &Log = Log](FTaskContext& Context)
	                                                                                   {
		                                                                                   State->bScanning = false;
		                                                                                   if (Context.IsCancellationRequested())
		                                                                                   {
			                                                                                   return;
		                                                                                   }
		                                                                                   if (!*Result)
		                                                                                   {
			                                                                                   HERTA_LOG_ERROR(Log, AssetLog, "Content scan failed: {}", Result->error().Message);
			                                                                                   return;
		                                                                                   }
		                                                                                   if (!(*Result)->Errors.empty())
		                                                                                   {
			                                                                                   HERTA_LOG_WARNING(Log, AssetLog, "{} content files have errors. Run asset.validate for details", (*Result)->Errors.size());
		                                                                                   }
		                                                                                   State->Registry = std::move((*Result)->Registry);
	                                                                                   })
	                                                      : std::unexpected(Scan.error());
	if (!Publish)
	{
		State->bScanning = false;
		HERTA_LOG_ERROR(Log, AssetLog, "Could not scan content: {}", Publish.error().Message);
	}
}

void FPreviewAssets::RequestMesh(const std::size_t Object, const FAssetRecord& Record)
{
	FPreviewMeshSlot& Slot = State->Slots.at(Object);
	const std::uint64_t Generation = ++Slot.Generation;
	Slot.Asset = Record.Id;
	Slot.Label = Record.SourcePath;
	Slot.bLoading = true;
	Slot.Error.clear();

	auto Load = std::make_shared<FMeshLoad>();
	FAssetCookRequest Request{Paths.ContentRoot, Paths.DerivedDataRoot, Record.SourcePath, Paths.TargetPlatform, false};
	std::expected<FTaskHandle, FTaskError> Cook = Tasks.Submit(*Scope, {"Cook " + Record.SourcePath, ETaskLane::BlockingIo}, [Load, Request, Worker = Paths.WorkerPath](FTaskContext& Context)
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
	std::expected<FTaskHandle, FTaskError> Publish = Cook ? Tasks.ContinueOnMainThread(*Scope, *Cook, "Publish preview mesh", [Load, State = State, Object, Generation, Label = Record.SourcePath, &Device = Device, &Log = Log](FTaskContext& Context)
	                                                                                   {
		                                                                                   FPreviewMeshSlot& Target = State->Slots[Object];
		                                                                                   if (Context.IsCancellationRequested() || Target.Generation != Generation)
		                                                                                   {
			                                                                                   return;
		                                                                                   }
		                                                                                   Target.bLoading = false;
		                                                                                   for (const std::string& Warning : Load->Warnings)
		                                                                                   {
			                                                                                   HERTA_LOG_WARNING(Log, AssetLog, "{}: {}", Label, Warning);
		                                                                                   }
		                                                                                   if (!Load->Model)
		                                                                                   {
			                                                                                   Target.Error = Load->Model.error().Message;
			                                                                                   HERTA_LOG_ERROR(Log, AssetLog, "{}", Target.Error);
			                                                                                   return;
		                                                                                   }
		                                                                                   std::expected<std::shared_ptr<const FRenderMesh>, FPresentationError> Mesh = FRenderMesh::Create(Device, *Load->Model, Label);
		                                                                                   if (!Mesh)
		                                                                                   {
			                                                                                   Target.Error = Mesh.error().Message;
			                                                                                   HERTA_LOG_ERROR(Log, AssetLog, "Could not upload {}: {}", Label, Target.Error);
			                                                                                   return;
		                                                                                   }
		                                                                                   Target.Mesh = std::move(*Mesh);
		                                                                                   HERTA_LOG_INFO(Log, AssetLog, "Loaded {} ({})", Label, Load->bCacheHit ? "cached" : "cooked");
	                                                                                   })
	                                                      : std::unexpected(Cook.error());
	if (!Publish)
	{
		Slot.bLoading = false;
		Slot.Error = Publish.error().Message;
		HERTA_LOG_ERROR(Log, AssetLog, "Could not load {}: {}", Record.SourcePath, Slot.Error);
	}
}

void FPreviewAssets::ResetMesh(const std::size_t Object)
{
	FPreviewMeshSlot& Slot = State->Slots.at(Object);
	const std::uint64_t Generation = Slot.Generation + 1;
	Slot = FPreviewMeshSlot{};
	Slot.Generation = Generation;
}

const FAssetRegistry& FPreviewAssets::GetRegistry() const noexcept
{
	return State->Registry;
}

bool FPreviewAssets::IsScanning() const noexcept
{
	return State->bScanning;
}

const FPreviewMeshSlot& FPreviewAssets::GetSlot(const std::size_t Object) const
{
	return State->Slots.at(Object);
}
}
