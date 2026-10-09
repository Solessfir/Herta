#include "VisualEnvironment.h"

#include "Herta/Core/BinaryStream.h"
#include "Herta/Core/Log.h"
#include "Herta/Renderer/EnvironmentLighting.h"
#include "Herta/Renderer/MeshRenderer.h"
#include "Herta/Tasks/TaskSystem.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <expected>
#include <utility>

namespace Herta
{
namespace
{
constexpr FLogCategory EnvironmentLog{.Name = "Renderer"};

FHash128 SourceKey(const FCookedTexture* Environment)
{
	if (!Environment)
	{
		return {};
	}

	FBinaryWriter Writer;
	Writer.Write(static_cast<std::uint8_t>(Environment->ColorSpace));
	Writer.Write(static_cast<std::uint8_t>(Environment->PixelFormat));
	if (!Environment->Mips.empty())
	{
		const auto& Mip = Environment->Mips.front();
		Writer.Write(Mip.Width);
		Writer.Write(Mip.Height);
		Writer.WriteBytes(Mip.Pixels);
	}

	return HashBytes(Writer.GetBytes());
}

FHash128 LightingKey(const bool bHasEnvironment, const FHash128& EnvironmentKey, const FVisualUniforms& Snapshot)
{
	FBinaryWriter Writer;
	Writer.Write(static_cast<std::uint8_t>(bHasEnvironment));
	if (bHasEnvironment)
	{
		Writer.Write(EnvironmentKey.High);
		Writer.Write(EnvironmentKey.Low);
		return HashBytes(Writer.GetBytes());
	}

	for (const float Value : Snapshot.Atmosphere)
	{
		Writer.WriteFloat(Value == 0.f ? 0.f : Value);
	}

	for (std::size_t Index = 0; Index < 2; ++Index)
	{
		Writer.WriteFloat(Snapshot.AtmosphereGeometry[Index]);
	}

	// The disabled atmosphere is a constant fallback; neither sun edits nor local lights change it.
	if (Snapshot.Atmosphere[3] < 0.5f)
	{
		return HashBytes(Writer.GetBytes());
	}

	const float Count = Snapshot.Controls[0];
	if (!std::isfinite(Count) || Count < 0.f || Count > static_cast<float>(MaximumRenderLights) || Count != std::floor(Count))
	{
		Writer.WriteFloat(Count);
		return HashBytes(Writer.GetBytes());
	}

	for (std::size_t Index = 0; Index < static_cast<std::size_t>(Count); ++Index)
	{
		const auto& Light = Snapshot.Lights[Index];
		if (Light.PositionType[3] != static_cast<float>(ELightType::Directional))
		{
			continue;
		}

		Writer.Write(std::uint8_t{1});

		for (std::size_t Axis = 0; Axis < 3; ++Axis)
		{
			Writer.WriteFloat(Light.DirectionRange[Axis] == 0.f ? 0.f : Light.DirectionRange[Axis]);
		}

		for (const float Value : Light.ColorIntensity)
		{
			Writer.WriteFloat(Value == 0.f ? 0.f : Value);
		}

		return HashBytes(Writer.GetBytes());
	}

	Writer.Write(std::uint8_t{0});
	return HashBytes(Writer.GetBytes());
}
}

FHash128 GetVisualEnvironmentKey(const FCookedTexture* Environment, const FVisualUniforms& Snapshot)
{
	return LightingKey(Environment != nullptr, SourceKey(Environment), Snapshot);
}

FVisualEnvironment::FVisualEnvironment(FTaskSystem& InTasks, FMeshRenderer& InRenderer, FLogService& InLog, std::unique_ptr<FTaskScope> InScope)
    : Tasks(InTasks)
    , Renderer(InRenderer)
    , Log(InLog)
    , Scope(std::move(InScope))
{
}

std::unique_ptr<FVisualEnvironment> FVisualEnvironment::Create(FTaskSystem& Tasks, FMeshRenderer& Renderer, FLogService& Log)
{
	auto Scope = Tasks.CreateScope("Environment lighting");
	if (!Scope)
	{
		HERTA_LOG_ERROR(Log, EnvironmentLog, "Environment lighting is unavailable: {}", Scope.error().Message);
		return nullptr;
	}

	return std::unique_ptr<FVisualEnvironment>(new FVisualEnvironment(Tasks, Renderer, Log, std::move(*Scope)));
}

FVisualEnvironment::~FVisualEnvironment()
{
	ActiveStop.request_stop();
	Scope->RequestCancellation();
	Scope->Wait();
}

const FVisualEnvironmentStatus& FVisualEnvironment::GetStatus() const noexcept
{
	return Status;
}

void FVisualEnvironment::Tick(std::shared_ptr<const FCookedTexture> Environment, const FVisualUniforms& Snapshot)
{
	if (CachedSource != Environment)
	{
		CachedSource = Environment;
		CachedSourceKey = SourceKey(Environment.get());
	}

	const FHash128 Key = LightingKey(Environment != nullptr, CachedSourceKey, Snapshot);
	if (!Desired || Desired->Key != Key)
	{
		++Status.Generation;
		Desired = FInputs{.Environment = std::move(Environment), .Snapshot = Snapshot, .Key = Key, .Generation = Status.Generation};
		if (bInFlight)
		{
			ActiveStop.request_stop();
		}
	}

	if (bInFlight)
	{
		return;
	}

	if (PublishedKey == Key)
	{
		Status.bUpdating = false;
		Status.Message = "Environment lighting ready";
		return;
	}

	if (FailedKey == Key)
	{
		Status.bUpdating = false;
		Status.Message = FailedMessage;
		return;
	}

	Start(*Desired);
}

void FVisualEnvironment::Start(const FInputs& Inputs)
{
	using FResult = std::expected<FEnvironmentLighting, FAssetError>;
	auto Result = std::make_shared<FResult>(std::unexpected(FAssetError{"Environment filtering cancelled"}));
	auto FilterMilliseconds = std::make_shared<double>(0.);
	ActiveStop = std::stop_source{};
	const auto StopToken = ActiveStop.get_token();
	bInFlight = true;
	Status.bUpdating = true;
	Status.Message = Status.bReady ? "Updating environment lighting" : "Preparing environment lighting";
	const auto Work = Tasks.Submit(*Scope, {.Name = "Filter environment lighting", .Lane = ETaskLane::Cpu, .Priority = ETaskPriority::Low}, [Result, FilterMilliseconds, Environment = Inputs.Environment, Snapshot = Inputs.Snapshot, StopToken](FTaskContext& Context)
	{
		if (!Context.IsCancellationRequested() && !StopToken.stop_requested())
		{
			const auto Started = std::chrono::steady_clock::now();
			*Result = BuildEnvironmentLighting(Environment.get(), Snapshot, StopToken);
			*FilterMilliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - Started).count();
		}
	});

	const auto Published = Work ? Tasks.ContinueOnMainThread(*Scope, *Work, "Publish environment lighting", [this, Result, FilterMilliseconds, Key = Inputs.Key, Generation = Inputs.Generation](FTaskContext& Context)
	{
		if (Context.IsCancellationRequested())
		{
			return;
		}

		bInFlight = false;
		if (!Desired || Desired->Key != Key || Desired->Generation != Generation)
		{
			return;
		}

		Status.bUpdating = false;
		if (!*Result)
		{
			FailedKey = Key;
			Status.Message = Result->error().Message;
			FailedMessage = Status.Message;
			HERTA_LOG_WARNING(Log, EnvironmentLog, "Environment lighting retained its previous valid state: {}", Status.Message);
			return;
		}

		const auto UploadStarted = std::chrono::steady_clock::now();
		const auto Uploaded = Renderer.SetEnvironmentLighting(**Result);
		if (!Uploaded)
		{
			FailedKey = Key;
			Status.Message = Uploaded.error().Message;
			FailedMessage = Status.Message;
			HERTA_LOG_WARNING(Log, EnvironmentLog, "Environment lighting retained its previous valid state: {}", Status.Message);
			return;
		}

		PublishedKey = Key;
		FailedKey.reset();
		Status.bReady = true;
		Status.Message = "Environment lighting ready";
		Status.FilterMilliseconds = *FilterMilliseconds;
		Status.UploadMilliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - UploadStarted).count();
		Status.TextureBytes = 0;
		for (const auto* Texture : {&(**Result).Diffuse, &(**Result).Specular})
		{
			for (const auto& Mip : Texture->Mips)
			{
				Status.TextureBytes += Mip.Pixels.size();
			}
		}

		HERTA_LOG_INFO(Log, EnvironmentLog, "Environment ready: filter_cpu_ms={:.3f} upload_cpu_ms={:.3f} texture_bytes={}", Status.FilterMilliseconds, Status.UploadMilliseconds, Status.TextureBytes);
	})
	                            : std::expected<FTaskHandle, FTaskError>(std::unexpected(Work.error()));

	if (!Published)
	{
		ActiveStop.request_stop();
		bInFlight = false;
		Status.bUpdating = false;
		FailedKey = Inputs.Key;
		Status.Message = Published.error().Message;
		FailedMessage = Status.Message;
		HERTA_LOG_WARNING(Log, EnvironmentLog, "Environment lighting could not be scheduled: {}", Status.Message);
	}
}
}
