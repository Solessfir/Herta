#include "EditorNavigation.h"

#include "Herta/Navigation/NavigationWorld.h"

#include <chrono>

namespace Herta
{
std::unique_ptr<FEditorNavigation> FEditorNavigation::Create(FTaskSystem& Tasks)
{
	auto Scope = Tasks.CreateScope("Editor navigation");
	if (!Scope)
	{
		return nullptr;
	}

	return std::unique_ptr<FEditorNavigation>(new FEditorNavigation(Tasks, std::move(*Scope)));
}

FEditorNavigation::FEditorNavigation(FTaskSystem& InTasks, std::unique_ptr<FTaskScope> InScope)
    : Tasks(InTasks)
    , Scope(std::move(InScope))
{
}

FEditorNavigation::~FEditorNavigation()
{
	Scope->RequestCancellation();
	Scope->Wait();
}

void FEditorNavigation::Request(const std::uint64_t RequestKey, FEditorNavigationInputs Inputs)
{
	if (Key == RequestKey || BuildingKey == RequestKey)
	{
		return;
	}

	if (Building)
	{
		Waiting.emplace(RequestKey, std::move(Inputs));
		return;
	}

	Start(RequestKey, std::move(Inputs));
}

void FEditorNavigation::Start(const std::uint64_t RequestKey, FEditorNavigationInputs Inputs)
{
	auto Build = std::make_shared<FBuild>(FBuild{.Key = RequestKey});
	auto Submitted = Tasks.Submit(*Scope, {.Name = "Build navigation", .Lane = ETaskLane::Cpu, .Priority = ETaskPriority::Low}, [Build, Inputs = std::move(Inputs)](FTaskContext&)
	{
		const auto Started = std::chrono::steady_clock::now();
		Build->Result = BuildLevelNavigation(Inputs.Level, Inputs.Regions, [&Inputs](const FAssetId& Mesh) -> std::optional<FNavigationBox>
		{
			const auto Found = Inputs.Bounds.find(Mesh);
			return Found != Inputs.Bounds.end() ? std::optional{Found->second} : std::nullopt;
		});

		// The viewport draws the whole build, every region's patch included.
		if (Build->Result && *Build->Result)
		{
			const FNavigationFile& File = **Build->Result;
			if (auto World = FNavigationWorld::Create(File.Base.Settings))
			{
				bool bLoaded = static_cast<bool>((*World)->AddData(File.Base));
				for (const FNavigationPatch& Patch : File.Patches)
				{
					bLoaded = bLoaded && static_cast<bool>((*World)->AddData(Patch.Data));
				}

				if (bLoaded)
				{
					(*World)->GetDebugTriangles(Build->Triangles);
				}
			}
		}

		Build->Milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - Started).count();
	});

	if (!Submitted)
	{
		Error = Submitted.error().Message;
		return;
	}

	Running = *Submitted;
	Building = std::move(Build);
	BuildingKey = RequestKey;
}

bool FEditorNavigation::Poll()
{
	if (!Building || !Running.IsComplete())
	{
		return false;
	}

	const std::shared_ptr<FBuild> Done = std::exchange(Building, nullptr);
	BuildingKey.reset();
	Key = Done->Key;
	Milliseconds = Done->Milliseconds;
	if (Done->Result)
	{
		File = *Done->Result ? std::make_shared<const FNavigationFile>(std::move(**Done->Result)) : nullptr;
		Triangles = std::move(Done->Triangles);
		Error.clear();
	}
	else
	{
		File.reset();
		Triangles.clear();
		Error = Done->Result.error().Message;
	}

	if (Waiting)
	{
		auto [NextKey, Inputs] = std::move(*Waiting);
		Waiting.reset();
		Start(NextKey, std::move(Inputs));
	}

	return true;
}

std::optional<std::uint64_t> FEditorNavigation::GetKey() const noexcept
{
	return Key;
}

bool FEditorNavigation::IsBuilding() const noexcept
{
	return Building != nullptr;
}

const std::shared_ptr<const FNavigationFile>& FEditorNavigation::GetFile() const noexcept
{
	return File;
}

std::span<const FVector3> FEditorNavigation::GetDebugTriangles() const noexcept
{
	return Triangles;
}

const std::string& FEditorNavigation::GetError() const noexcept
{
	return Error;
}

double FEditorNavigation::GetBuildMilliseconds() const noexcept
{
	return Milliseconds;
}
}
