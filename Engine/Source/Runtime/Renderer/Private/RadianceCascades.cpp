#include "RadianceCascades.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace Herta
{
namespace
{
// Keep in sync with RadianceCascades.slangh.
constexpr std::uint32_t CascadeCount = 3;
constexpr std::array<std::uint32_t, CascadeCount> SlotCapacity{131072, 65536, 16384};
constexpr std::uint32_t TotalRecords = 10485760;
constexpr std::uint32_t MergedRecords = 4194304;
constexpr std::uint32_t TotalSlots = 212992;
constexpr std::uint32_t TotalProbes = 106496;
// After the deposits, two alternating frame counts per cascade 0 probe, then eight floats per hit cache cell.
constexpr std::uint32_t CacheCapacity = 131072;
constexpr std::uint32_t HistoryWords = 2 * 65536 + CacheCapacity * 8;
constexpr FExtent2D AtlasExtent{2048, 2048};

enum class EStage : std::uint32_t
{
	Clear,
	InsertPixels,
	Compact,
	InsertParents,
	Links,
	Merge,
	ResetRecords,
	ResolveCache,
};

[[nodiscard]] constexpr std::uint32_t Groups(const std::uint32_t Threads, const std::uint32_t Size) noexcept
{
	return (Threads + Size - 1) / Size;
}
}

std::expected<std::unique_ptr<FRadianceCascades>, FPresentationError> FRadianceCascades::Create(IGraphicsDevice& Device, FRadianceCascadeShaders Shaders)
{
	if (!Device.SupportsRayQueries() || Shaders.Probes.Bytecode.empty() || Shaders.Trace.Bytecode.empty() || Shaders.Irradiance.Bytecode.empty() || Shaders.Shade.Bytecode.empty() || Shaders.Arguments.Bytecode.empty())
	{
		return std::unexpected(FPresentationError{.Code = EPresentationErrorCode::InvalidDescriptor, .Message = "Radiance cascades need ray queries and all six of their shaders"});
	}

	auto Probes = Device.CreateComputePipeline({.Name = "Radiance cascade probes", .Shader = std::move(Shaders.Probes), .TextureCount = 1, .StorageBufferCount = 7, .UniformBufferSize = VisualUniformBytes, .bLinearSampler = true});
	auto Trace = Device.CreateComputePipeline({.Name = "Radiance cascade rays", .Shader = std::move(Shaders.Trace), .TextureCount = 5, .StorageBufferCount = 7, .UniformBufferSize = VisualUniformBytes, .bLinearSampler = true, .bRepeatSampler = true, .BufferCount = 4, .bRayQuery = true, .bResourceTable = true});
	auto Irradiance = Device.CreateComputePipeline({.Name = "Radiance cascade irradiance", .Shader = std::move(Shaders.Irradiance), .TextureCount = 1, .StorageTextureCount = 1, .StorageBufferCount = 7, .UniformBufferSize = VisualUniformBytes, .BufferCount = 1});
	auto Shade = Device.CreateComputePipeline({.Name = "Radiance cascade gather", .Shader = std::move(Shaders.Shade), .TextureCount = 2, .StorageTextureCount = 1, .StorageBufferCount = 7, .UniformBufferSize = VisualUniformBytes, .bLinearSampler = true});
	auto Arguments = Device.CreateComputePipeline({.Name = "Radiance cascade arguments", .Shader = std::move(Shaders.Arguments), .StorageBufferCount = 1, .BufferCount = 1});
	for (const auto* const Pipeline : {&Probes, &Trace, &Irradiance, &Shade, &Arguments})
	{
		if (!*Pipeline)
		{
			return std::unexpected(Pipeline->error());
		}
	}

	return std::unique_ptr<FRadianceCascades>(new FRadianceCascades(Device, {std::move(*Probes), std::move(*Trace), std::move(*Irradiance), std::move(*Shade), std::move(*Arguments)}));
}

FRadianceCascades::FRadianceCascades(IGraphicsDevice& InDevice, std::array<FComputePipelineHandle, 5> InPipelines)
    : Device(InDevice)
    , Pipelines(std::move(InPipelines))
{
}

const FTextureHandle& FRadianceCascades::GetIrradiance() const noexcept
{
	return Gathered;
}

FGlobalIlluminationStatistics FRadianceCascades::GetStatistics() const noexcept
{
	return Statistics;
}

std::size_t FRadianceCascades::GetMemoryBytes() const noexcept
{
	std::size_t Bytes = 0;
	for (const FBufferHandle* const Buffer : {&Slots[0], &Slots[1], &Probes, &Counters, &Deposits, &RecordCounts, &Merged, &CacheKeys, &Arguments})
	{
		Bytes += *Buffer ? (*Buffer)->GetDescriptor().Size : 0;
	}

	for (const FTextureHandle* const Texture : {&Atlas[0], &Atlas[1], &Gathered})
	{
		Bytes += *Texture ? std::size_t{(*Texture)->GetDescriptor().Extent.Width} * (*Texture)->GetDescriptor().Extent.Height * GetTextureTexelBytes((*Texture)->GetDescriptor().Format) : 0;
	}

	return Bytes;
}

void FRadianceCascades::Release() noexcept
{
	// Recordings in flight hold their own references, so nothing here waits on the GPU.
	Slots = {};
	Probes.reset();
	Counters.reset();
	Deposits.reset();
	RecordCounts.reset();
	Merged.reset();
	CacheKeys.reset();
	Arguments.reset();
	Atlas = {};
	CounterReadback = {};
	Statistics = {};
	Gathered.reset();
	bRecordsReset = false;
	bPrevious = false;
}

std::expected<void, FPresentationError> FRadianceCascades::Allocate(const FExtent2D Extent)
{
	const auto Buffer = [this](FBufferHandle& Destination, const char* const Name, const std::size_t Words) -> std::expected<void, FPresentationError>
	{
		if (Destination)
		{
			return {};
		}

		auto Created = Device.CreateBuffer({.Name = Name, .Size = Words * sizeof(std::uint32_t), .Usage = EBufferUsage::Storage});
		if (!Created)
		{
			return std::unexpected(Created.error());
		}

		Destination = std::move(*Created);
		return {};
	};

	auto Result = Buffer(Slots[0], "Radiance cascade slots", std::size_t{TotalSlots} * 2);
	if (Result)
	{
		Result = Buffer(Slots[1], "Radiance cascade previous slots", std::size_t{TotalSlots} * 2);
	}

	if (Result)
	{
		Result = Buffer(Probes, "Radiance cascade probes", std::size_t{TotalProbes} * 3);
	}

	if (Result)
	{
		Result = Buffer(Counters, "Radiance cascade counters", 8);
	}

	for (FBufferHandle& Readback : CounterReadback)
	{
		if (Result && !Readback)
		{
			auto Created = Device.CreateBuffer({.Name = "Radiance cascade counter readback", .Size = 8 * sizeof(std::uint32_t), .Usage = EBufferUsage::Readback});
			if (!Created)
			{
				return std::unexpected(Created.error());
			}

			Readback = std::move(*Created);
		}
	}

	if (Result)
	{
		Result = Buffer(Deposits, "Radiance cascade deposits", std::size_t{TotalRecords} * 3 + HistoryWords);
	}

	if (Result)
	{
		Result = Buffer(RecordCounts, "Radiance cascade ray counts", TotalRecords);
	}

	if (Result)
	{
		Result = Buffer(Merged, "Radiance cascade merged radiance", MergedRecords);
	}

	if (Result)
	{
		Result = Buffer(CacheKeys, "Radiance cascade hit cache keys", std::size_t{CacheCapacity} * 2);
	}

	if (Result)
	{
		Result = Buffer(Arguments, "Radiance cascade arguments", 36);
	}

	for (FTextureHandle& Texture : Atlas)
	{
		if (Result && !Texture)
		{
			auto Created = Device.CreateTexture({.Name = "Radiance cascade irradiance probes", .Extent = AtlasExtent, .Format = ETextureFormat::Rgba16Float, .bStorage = true});
			if (!Created)
			{
				return std::unexpected(Created.error());
			}

			Texture = std::move(*Created);
		}
	}

	const FExtent2D Half{(Extent.Width + 1) / 2, (Extent.Height + 1) / 2};
	if (Result && (!Gathered || Gathered->GetDescriptor().Extent != Half))
	{
		auto Created = Device.CreateTexture({.Name = "Global illumination gathers", .Extent = Half, .Format = ETextureFormat::Rgba16Float, .bStorage = true});
		if (!Created)
		{
			return std::unexpected(Created.error());
		}

		Gathered = std::move(*Created);
	}

	return Result;
}

std::expected<void, FPresentationError> FRadianceCascades::Render(const FGlobalIlluminationInputs& Inputs)
{
	if (auto Allocated = Allocate(Inputs.Extent); !Allocated)
	{
		return Allocated;
	}

	FVisualUniforms Uniforms = *Inputs.Uniforms;
	++Frame;
	// The ray pixel stays fixed, so a still view keeps the same probes. Directions rotate along R2 every frame, and each probe's irradiance averages them.
	constexpr double Plastic = 1.32471795724474602596;
	Uniforms.Material.Surface = {static_cast<float>(std::fmod(Frame / Plastic, 1.)), static_cast<float>(std::fmod(Frame / (Plastic * Plastic), 1.)), 0.f, 0.f};
	const std::size_t Current = Frame & 1u;
	const std::size_t Previous = Current ^ 1u;
	const std::array ProbeSources{Inputs.Depth};
	const std::array Buffers{Slots[Current], Probes, Counters, Deposits, RecordCounts, Merged, CacheKeys};
	const auto Stage = [&](const EStage Kind, const std::uint32_t Cascade, const std::array<std::uint32_t, 3> GroupCount) -> std::expected<void, FPresentationError>
	{
		Uniforms.Material.BaseColor = {static_cast<float>(Kind), static_cast<float>(Cascade), static_cast<float>(Frame), 0.f};
		return Device.Dispatch({.Pipeline = Pipelines[0], .Textures = ProbeSources, .StorageBuffers = Buffers, .Uniforms = GetVisualUniformBytes(Uniforms), .GroupCount = GroupCount});
	};

	// Stages over probes read their group counts from the slots RadianceCascadesArguments.slang writes after each compaction.
	const auto IndirectStage = [&](const EStage Kind, const std::uint32_t Cascade, const std::uint32_t Slot) -> std::expected<void, FPresentationError>
	{
		Uniforms.Material.BaseColor = {static_cast<float>(Kind), static_cast<float>(Cascade), static_cast<float>(Frame), 0.f};
		return Device.Dispatch({.Pipeline = Pipelines[0], .Textures = ProbeSources, .StorageBuffers = Buffers, .Uniforms = GetVisualUniformBytes(Uniforms), .IndirectArguments = Arguments, .IndirectOffset = Slot * 3 * static_cast<std::uint32_t>(sizeof(std::uint32_t))});
	};

	const std::array CounterSources{Counters};
	const std::array ArgumentTargets{Arguments};
	const auto WriteArguments = [&]
	{
		return Device.Dispatch({.Pipeline = Pipelines[4], .StorageBuffers = ArgumentTargets, .Buffers = CounterSources});
	};

	// Stages are timed in groups; each group ends before the next begins.
	const auto Time = [this](const std::string_view Name)
	{
		Device.EndGpuTiming();
		Device.BeginGpuTiming(Name);
	};

	std::expected<void, FPresentationError> Result;
	Device.BeginGpuTiming("GI probes");
	if (!bRecordsReset)
	{
		Result = Stage(EStage::ResetRecords, 0, {Groups(TotalRecords / 4, 64), 1, 1});
		bRecordsReset = static_cast<bool>(Result);
	}

	if (Result)
	{
		Result = Stage(EStage::Clear, 0, {Groups(TotalSlots * 2, 64), 1, 1});
	}

	if (Result)
	{
		Result = Stage(EStage::InsertPixels, 0, {Groups((Inputs.Extent.Width + 1) / 2, 64), (Inputs.Extent.Height + 1) / 2, 1});
	}

	for (std::uint32_t Cascade = 0; Cascade < CascadeCount && Result; ++Cascade)
	{
		if (Cascade > 0)
		{
			Result = IndirectStage(EStage::InsertParents, Cascade, Cascade - 1);
		}

		if (Result)
		{
			Result = Stage(EStage::Compact, Cascade, {Groups(SlotCapacity[Cascade], 64), 1, 1});
		}

		if (Result)
		{
			Result = WriteArguments();
		}
	}

	for (std::uint32_t Cascade = 0; Cascade + 1 < CascadeCount && Result; ++Cascade)
	{
		Result = IndirectStage(EStage::Links, Cascade, 3 + Cascade);
	}

	Time("GI rays");
	if (Result)
	{
		Uniforms.Material.BaseColor = {0.f, 0.f, static_cast<float>(Frame), 0.f};
		Uniforms.Material.Emissive = {PreviousEye[0], PreviousEye[1], PreviousEye[2], bPrevious ? 1.f : 0.f};
		// Without a previous frame the shader reads neither; the environment stands in for the unwritten tiles.
		const std::array Sources{Inputs.Environment, Inputs.SkyView, Inputs.SpecularAlbedo, Inputs.Depth, bPrevious ? Atlas[Previous] : Inputs.Environment};
		const std::array SceneBuffers{Inputs.Lights, Inputs.Scene->GetSections(), Inputs.Scene->GetMaterials(), Slots[Previous]};
		Result = Device.Dispatch({.Pipeline = Pipelines[1], .Textures = Sources, .StorageBuffers = Buffers, .Uniforms = GetVisualUniformBytes(Uniforms), .GroupCount = {Groups((Inputs.Extent.Width + 1) / 2, 8), Groups((Inputs.Extent.Height + 1) / 2, 8), 1}, .Buffers = SceneBuffers, .Scene = Inputs.Scene->GetTopLevel(), .ResourceTable = Inputs.Scene->GetTable()});
	}

	if (Result)
	{
		Result = Stage(EStage::ResolveCache, 0, {Groups(CacheCapacity, 64), 1, 1});
	}

	Time("GI merge");
	for (std::uint32_t Cascade = CascadeCount; Cascade-- > 0 && Result;)
	{
		Result = IndirectStage(EStage::Merge, Cascade, 6 + Cascade);
	}

	Time("GI irradiance");
	if (Result)
	{
		const std::array Targets{FStorageTextureBinding{.Texture = Atlas[Current]}};
		// Without a previous frame the shader reads neither.
		const std::array Sources{bPrevious ? Atlas[Previous] : Inputs.Depth};
		const std::array PreviousBuffers{Slots[Previous]};
		Result = Device.Dispatch({.Pipeline = Pipelines[2], .Textures = Sources, .StorageTextures = Targets, .StorageBuffers = Buffers, .Uniforms = GetVisualUniformBytes(Uniforms), .IndirectArguments = Arguments, .IndirectOffset = 10 * 3 * sizeof(std::uint32_t), .Buffers = PreviousBuffers});
	}

	Time("GI gather");
	if (Result)
	{
		const std::array Sources{Inputs.Depth, Atlas[Current]};
		const std::array Targets{FStorageTextureBinding{.Texture = Gathered}};
		Result = Device.Dispatch({.Pipeline = Pipelines[3], .Textures = Sources, .StorageTextures = Targets, .StorageBuffers = Buffers, .Uniforms = GetVisualUniformBytes(Uniforms), .GroupCount = {Groups(Gathered->GetDescriptor().Extent.Width, 8), Groups(Gathered->GetDescriptor().Extent.Height, 8), 1}});
	}

	Device.EndGpuTiming();
	if (const auto Bytes = Device.TryReadBuffer(CounterReadback[(Frame + 1) % CounterReadback.size()]))
	{
		std::array<std::uint32_t, 8> Words{};
		std::memcpy(Words.data(), Bytes->data(), sizeof(Words));
		Statistics = {.Probes = {Words[0], Words[1], Words[2], Words[3]}, .Overflow = Words[5]};
	}

	if (Result)
	{
		Result = Device.CopyBuffer(Counters, CounterReadback[Frame % CounterReadback.size()]);
	}

	const FMatrix4 ViewToWorld(Inputs.Uniforms->ViewToWorld);
	PreviousEye = {ViewToWorld.Data()[12], ViewToWorld.Data()[13], ViewToWorld.Data()[14]};
	bPrevious = static_cast<bool>(Result);
	return Result;
}
}
