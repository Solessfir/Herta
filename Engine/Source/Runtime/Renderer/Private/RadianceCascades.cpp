#include "RadianceCascades.h"

#include <algorithm>

namespace Herta
{
namespace
{
// Keep in sync with RadianceCascades.slangh.
constexpr std::uint32_t CascadeCount = 4;
constexpr std::array<std::uint32_t, CascadeCount> ProbeCapacity{65536, 16384, 4096, 1024};
constexpr std::array<std::uint32_t, CascadeCount> SlotCapacity{131072, 32768, 8192, 2048};
constexpr std::uint32_t RecordsPerCascade = 2097152;
constexpr std::uint32_t MergedRecords = 3670016;
constexpr std::uint32_t TotalSlots = 174080;
constexpr std::uint32_t TotalProbes = 87040;
constexpr FExtent2D AtlasExtent{2048, 2048};

enum class EStage : std::uint32_t
{
	Clear,
	InsertPixels,
	Compact,
	InsertParents,
	Offsets,
	Merge,
	ResetRecords,
};

[[nodiscard]] constexpr std::uint32_t DirectionCount(const std::uint32_t Cascade) noexcept
{
	return 32u << (2u * Cascade);
}

[[nodiscard]] constexpr std::uint32_t Groups(const std::uint32_t Threads, const std::uint32_t Size) noexcept
{
	return (Threads + Size - 1) / Size;
}
}

std::expected<std::unique_ptr<FRadianceCascades>, FPresentationError> FRadianceCascades::Create(IGraphicsDevice& Device, FRadianceCascadeShaders Shaders)
{
	if (!Device.SupportsRayQueries() || Shaders.Probes.Bytecode.empty() || Shaders.Trace.Bytecode.empty() || Shaders.Irradiance.Bytecode.empty() || Shaders.Shade.Bytecode.empty())
	{
		return std::unexpected(FPresentationError{.Code = EPresentationErrorCode::InvalidDescriptor, .Message = "Radiance cascades need ray queries and all four of their shaders"});
	}

	auto Probes = Device.CreateComputePipeline({.Name = "Radiance cascade probes", .Shader = std::move(Shaders.Probes), .TextureCount = 2, .StorageBufferCount = 6, .UniformBufferSize = VisualUniformBytes, .bLinearSampler = true});
	auto Trace = Device.CreateComputePipeline({.Name = "Radiance cascade rays", .Shader = std::move(Shaders.Trace), .TextureCount = 5, .StorageBufferCount = 6, .UniformBufferSize = VisualUniformBytes, .bLinearSampler = true, .bRepeatSampler = true, .BufferCount = 4, .bRayQuery = true, .bResourceTable = true});
	auto Irradiance = Device.CreateComputePipeline({.Name = "Radiance cascade irradiance", .Shader = std::move(Shaders.Irradiance), .StorageTextureCount = 1, .StorageBufferCount = 6, .UniformBufferSize = VisualUniformBytes});
	auto Shade = Device.CreateComputePipeline({.Name = "Radiance cascade gather", .Shader = std::move(Shaders.Shade), .TextureCount = 2, .StorageTextureCount = 1, .StorageBufferCount = 6, .UniformBufferSize = VisualUniformBytes, .bLinearSampler = true});
	for (const auto* const Pipeline : {&Probes, &Trace, &Irradiance, &Shade})
	{
		if (!*Pipeline)
		{
			return std::unexpected(Pipeline->error());
		}
	}

	return std::unique_ptr<FRadianceCascades>(new FRadianceCascades(Device, {std::move(*Probes), std::move(*Trace), std::move(*Irradiance), std::move(*Shade)}));
}

FRadianceCascades::FRadianceCascades(IGraphicsDevice& InDevice, std::array<FComputePipelineHandle, 4> InPipelines)
    : Device(InDevice)
    , Pipelines(std::move(InPipelines))
{
}

const FTextureHandle& FRadianceCascades::GetIrradiance() const noexcept
{
	return Irradiance;
}

std::size_t FRadianceCascades::GetMemoryBytes() const noexcept
{
	std::size_t Bytes = 0;
	for (const FBufferHandle* const Buffer : {&Slots[0], &Slots[1], &Probes, &Counters, &Deposits, &RecordCounts, &Merged})
	{
		Bytes += *Buffer ? (*Buffer)->GetDescriptor().Size : 0;
	}

	for (const FTextureHandle* const Texture : {&Atlas[0], &Atlas[1], &Irradiance})
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
	Atlas = {};
	Irradiance.reset();
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

	auto Result = Buffer(Slots[0], "Radiance cascade slots", std::size_t{TotalSlots} * 4);
	if (Result)
	{
		Result = Buffer(Slots[1], "Radiance cascade previous slots", std::size_t{TotalSlots} * 4);
	}

	if (Result)
	{
		Result = Buffer(Probes, "Radiance cascade probes", std::size_t{TotalProbes} * 3);
	}

	if (Result)
	{
		Result = Buffer(Counters, "Radiance cascade counters", 8);
	}

	if (Result)
	{
		Result = Buffer(Deposits, "Radiance cascade deposits", std::size_t{RecordsPerCascade} * CascadeCount * 3);
	}

	if (Result)
	{
		Result = Buffer(RecordCounts, "Radiance cascade ray counts", std::size_t{RecordsPerCascade} * CascadeCount);
	}

	if (Result)
	{
		Result = Buffer(Merged, "Radiance cascade merged radiance", std::size_t{MergedRecords} * 2);
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

	if (Result && (!Irradiance || Irradiance->GetDescriptor().Extent != Extent))
	{
		auto Created = Device.CreateTexture({.Name = "Global illumination", .Extent = Extent, .Format = ETextureFormat::Rgba16Float, .bStorage = true});
		if (!Created)
		{
			return std::unexpected(Created.error());
		}

		Irradiance = std::move(*Created);
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
	// The ray pixel and direction jitter stay fixed, so a still view gets the same probes and rays every frame instead of flickering without accumulation.
	Uniforms.Material.Surface = {0.f, 0.f, 0.f, 0.f};
	const std::size_t Current = Frame & 1u;
	const std::size_t Previous = Current ^ 1u;
	const std::array ProbeSources{Inputs.Depth, Inputs.Environment};
	const std::array Buffers{Slots[Current], Probes, Counters, Deposits, RecordCounts, Merged};
	const auto Stage = [&](const EStage Kind, const std::uint32_t Cascade, const std::array<std::uint32_t, 3> GroupCount) -> std::expected<void, FPresentationError>
	{
		Uniforms.Material.BaseColor = {static_cast<float>(Kind), static_cast<float>(Cascade), static_cast<float>(Frame), 0.f};
		return Device.Dispatch({.Pipeline = Pipelines[0], .Textures = ProbeSources, .StorageBuffers = Buffers, .Uniforms = GetVisualUniformBytes(Uniforms), .GroupCount = GroupCount});
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
		Result = Stage(EStage::ResetRecords, 0, {Groups(RecordsPerCascade * CascadeCount / 4, 64), 1, 1});
		bRecordsReset = static_cast<bool>(Result);
	}

	if (Result)
	{
		Result = Stage(EStage::Clear, 0, {Groups(TotalSlots * 4, 64), 1, 1});
	}

	if (Result)
	{
		Result = Stage(EStage::InsertPixels, 0, {Groups((Inputs.Extent.Width + 1) / 2, 64), (Inputs.Extent.Height + 1) / 2, 1});
	}

	if (Result)
	{
		Result = Stage(EStage::Compact, 0, {Groups(SlotCapacity[0], 64), 1, 1});
	}

	for (std::uint32_t Cascade = 1; Cascade < CascadeCount && Result; ++Cascade)
	{
		Result = Stage(EStage::InsertParents, Cascade, {Groups(ProbeCapacity[Cascade - 1], 64), 1, 1});
		if (Result)
		{
			Result = Stage(EStage::Compact, Cascade, {Groups(SlotCapacity[Cascade], 64), 1, 1});
		}
	}

	for (std::uint32_t Cascade = CascadeCount; Cascade-- > 0 && Result;)
	{
		Result = Stage(EStage::Offsets, Cascade, {Groups(ProbeCapacity[Cascade], 64), 1, 1});
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

	Time("GI merge");
	for (std::uint32_t Cascade = CascadeCount; Cascade-- > 0 && Result;)
	{
		Result = Stage(EStage::Merge, Cascade, {Groups(ProbeCapacity[Cascade] * DirectionCount(Cascade), 64), 1, 1});
	}

	Time("GI irradiance");
	if (Result)
	{
		const std::array Targets{FStorageTextureBinding{.Texture = Atlas[Current]}};
		Result = Device.Dispatch({.Pipeline = Pipelines[2], .StorageTextures = Targets, .StorageBuffers = Buffers, .Uniforms = GetVisualUniformBytes(Uniforms), .GroupCount = {1024, ProbeCapacity[0] / 1024, 1}});
	}

	Time("GI gather");
	if (Result)
	{
		const std::array Sources{Inputs.Depth, Atlas[Current]};
		const std::array Targets{FStorageTextureBinding{.Texture = Irradiance}};
		Result = Device.Dispatch({.Pipeline = Pipelines[3], .Textures = Sources, .StorageTextures = Targets, .StorageBuffers = Buffers, .Uniforms = GetVisualUniformBytes(Uniforms), .GroupCount = {Groups(Inputs.Extent.Width, 8), Groups(Inputs.Extent.Height, 8), 1}});
	}

	Device.EndGpuTiming();
	const FMatrix4 ViewToWorld(Inputs.Uniforms->ViewToWorld);
	PreviousEye = {ViewToWorld.Data()[12], ViewToWorld.Data()[13], ViewToWorld.Data()[14]};
	bPrevious = static_cast<bool>(Result);
	return Result;
}
}
