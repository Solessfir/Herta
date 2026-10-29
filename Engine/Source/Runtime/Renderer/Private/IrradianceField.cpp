#include "IrradianceField.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string_view>
#include <tuple>

namespace Herta
{
namespace
{
// Keep in sync with IrradianceField.slangh.
constexpr std::uint32_t FieldLevels = 4;
constexpr std::array<std::int32_t, 3> FieldGrid{32, 32, 32};
constexpr std::uint32_t FieldProbeCount = 32768 * FieldLevels;
constexpr float FieldBaseSpacing = 1.f;
constexpr std::uint32_t FieldRays = 128;
constexpr std::uint32_t FieldMaxTraced = 8192;
constexpr std::uint32_t FieldTilesPerRow = 256;
constexpr FExtent2D IrradianceExtent{FieldTilesPerRow * 8, FieldProbeCount / FieldTilesPerRow * 8};
constexpr FExtent2D DistanceExtent{FieldTilesPerRow * 16, FieldProbeCount / FieldTilesPerRow * 16};
// Probes record the frame they were last read, and a fresh buffer holds zero, so frames start far enough in that zero reads as long unused.
constexpr std::uint32_t FirstFrame = 64;

// Keep in sync with IrradianceFieldUpdate.slang.
enum class EStage : std::uint32_t
{
	Reset,
	Clear,
	Select,
	Arguments,
	Settle,
};

[[nodiscard]] constexpr std::uint32_t Groups(const std::uint32_t Threads, const std::uint32_t Size) noexcept
{
	return (Threads + Size - 1) / Size;
}
}

std::expected<std::unique_ptr<FIrradianceField>, FPresentationError> FIrradianceField::Create(IGraphicsDevice& Device, FIrradianceFieldShaders Shaders)
{
	if (!Device.SupportsRayQueries() || Shaders.Update.Bytecode.empty() || Shaders.Trace.Bytecode.empty() || Shaders.Blend.Bytecode.empty() || Shaders.Gather.Bytecode.empty() || Shaders.Reflect.Bytecode.empty())
	{
		return std::unexpected(FPresentationError{.Code = EPresentationErrorCode::InvalidDescriptor, .Message = "The irradiance field needs ray queries and all five of its shaders"});
	}

	auto Update = Device.CreateComputePipeline({.Name = "Irradiance field update", .Shader = std::move(Shaders.Update), .StorageTextureCount = 2, .StorageBufferCount = 6, .UniformBufferSize = VisualUniformBytes});
	auto Trace = Device.CreateComputePipeline({.Name = "Irradiance field rays", .Shader = std::move(Shaders.Trace), .TextureCount = 5, .StorageBufferCount = 2, .UniformBufferSize = VisualUniformBytes, .bLinearSampler = true, .bRepeatSampler = true, .BufferCount = 5, .bRayQuery = true, .bResourceTable = true});
	auto Blend = Device.CreateComputePipeline({.Name = "Irradiance field blend", .Shader = std::move(Shaders.Blend), .StorageTextureCount = 2, .StorageBufferCount = 2, .UniformBufferSize = VisualUniformBytes, .BufferCount = 2});
	auto Gather = Device.CreateComputePipeline({.Name = "Irradiance field gather", .Shader = std::move(Shaders.Gather), .TextureCount = 4, .StorageTextureCount = 1, .StorageBufferCount = 1, .UniformBufferSize = VisualUniformBytes, .bLinearSampler = true, .BufferCount = 1});
	auto Reflect = Device.CreateComputePipeline({.Name = "Irradiance field reflections", .Shader = std::move(Shaders.Reflect), .TextureCount = 7, .StorageTextureCount = 1, .StorageBufferCount = 1, .UniformBufferSize = VisualUniformBytes, .bLinearSampler = true, .bRepeatSampler = true, .BufferCount = 4, .bRayQuery = true, .bResourceTable = true});
	for (const auto* const Pipeline : {&Update, &Trace, &Blend, &Gather, &Reflect})
	{
		if (!*Pipeline)
		{
			return std::unexpected(Pipeline->error());
		}
	}

	return std::unique_ptr<FIrradianceField>(new FIrradianceField(Device, {std::move(*Update), std::move(*Trace), std::move(*Blend), std::move(*Gather), std::move(*Reflect)}));
}

FIrradianceField::FIrradianceField(IGraphicsDevice& InDevice, std::array<FComputePipelineHandle, 5> InPipelines)
    : Device(InDevice)
    , Pipelines(std::move(InPipelines))
{
}

const FTextureHandle& FIrradianceField::GetIrradiance() const noexcept
{
	return Gathered;
}

FGlobalIlluminationStatistics FIrradianceField::GetStatistics() const noexcept
{
	return Statistics;
}

std::size_t FIrradianceField::GetMemoryBytes() const noexcept
{
	std::size_t Bytes = 0;
	for (const FBufferHandle* const Buffer : {&Cells, &Probes, &Used, &TraceList, &Counters, &Arguments, &RayData})
	{
		Bytes += *Buffer ? (*Buffer)->GetDescriptor().Size : 0;
	}

	for (const FTextureHandle* const Texture : {&IrradianceAtlas, &DistanceAtlas, &Gathered})
	{
		Bytes += *Texture ? std::size_t{(*Texture)->GetDescriptor().Extent.Width} * (*Texture)->GetDescriptor().Extent.Height * GetTextureTexelBytes((*Texture)->GetDescriptor().Format) : 0;
	}

	return Bytes;
}

void FIrradianceField::Release() noexcept
{
	// Recordings in flight hold their own references, so nothing here waits on the GPU.
	IrradianceAtlas.reset();
	DistanceAtlas.reset();
	Cells.reset();
	Probes.reset();
	Used.reset();
	TraceList.reset();
	Counters.reset();
	Arguments.reset();
	RayData.reset();
	Gathered.reset();
	CounterReadback = {};
	Statistics = {};
	bReset = false;
}

std::expected<void, FPresentationError> FIrradianceField::Allocate(const FExtent2D Extent)
{
	const auto Buffer = [this](FBufferHandle& Destination, const char* const Name, const std::size_t Words, const EBufferUsage Usage = EBufferUsage::Storage) -> std::expected<void, FPresentationError>
	{
		if (Destination)
		{
			return {};
		}

		auto Created = Device.CreateBuffer({.Name = Name, .Size = Words * sizeof(std::uint32_t), .Usage = Usage});
		if (!Created)
		{
			return std::unexpected(Created.error());
		}

		Destination = std::move(*Created);
		return {};
	};

	const auto Texture = [this](FTextureHandle& Destination, const char* const Name, const FExtent2D Size, const ETextureFormat Format) -> std::expected<void, FPresentationError>
	{
		if (Destination && Destination->GetDescriptor().Extent == Size)
		{
			return {};
		}

		auto Created = Device.CreateTexture({.Name = Name, .Extent = Size, .Format = Format, .bStorage = true});
		if (!Created)
		{
			return std::unexpected(Created.error());
		}

		Destination = std::move(*Created);
		return {};
	};

	std::expected<void, FPresentationError> Result;
	for (const auto& [Destination, Name, Words] : {std::tuple{&Cells, "Irradiance field cells", std::size_t{FieldProbeCount} * 4}, std::tuple{&Probes, "Irradiance field probes", std::size_t{FieldProbeCount} * 4}, std::tuple{&Used, "Irradiance field use", std::size_t{FieldProbeCount} * 2}, std::tuple{&TraceList, "Irradiance field trace list", std::size_t{FieldMaxTraced}}, std::tuple{&Counters, "Irradiance field counters", std::size_t{4}}, std::tuple{&Arguments, "Irradiance field arguments", std::size_t{4}}, std::tuple{&RayData, "Irradiance field rays", std::size_t{FieldMaxTraced} * FieldRays * 4}})
	{
		if (Result)
		{
			Result = Buffer(*Destination, Name, Words);
		}
	}

	for (FBufferHandle& Readback : CounterReadback)
	{
		if (Result)
		{
			Result = Buffer(Readback, "Irradiance field counter readback", 4, EBufferUsage::Readback);
		}
	}

	if (Result && (!IrradianceAtlas || !DistanceAtlas))
	{
		bReset = false;
		Result = Texture(IrradianceAtlas, "Irradiance field irradiance", IrradianceExtent, ETextureFormat::Rgba16Float);
		if (Result)
		{
			Result = Texture(DistanceAtlas, "Irradiance field distances", DistanceExtent, ETextureFormat::Rg16Float);
		}
	}

	if (Result)
	{
		Result = Texture(Gathered, "Global illumination gathers", {(Extent.Width + 1) / 2 * 2, (Extent.Height + 1) / 2}, ETextureFormat::Rgba16Float);
	}

	return Result;
}

std::expected<void, FPresentationError> FIrradianceField::Render(const FGlobalIlluminationInputs& Inputs)
{
	if (auto Allocated = Allocate(Inputs.Extent); !Allocated)
	{
		return Allocated;
	}

	Frame = std::max(Frame + 1, FirstFrame);
	FVisualUniforms Uniforms = *Inputs.Uniforms;
	// Each level's grid centres on the camera's cell, so its first probe cell moves a whole cell at a time.
	const std::array<float, 3> Eye{Inputs.Uniforms->ViewToWorld[12], Inputs.Uniforms->ViewToWorld[13], Inputs.Uniforms->ViewToWorld[14]};
	const std::array<std::array<float, 4>*, FieldLevels> Origins{&Uniforms.Material.UV, &Uniforms.Material.Channels0, &Uniforms.Material.Channels1, &Uniforms.Material.TextureFlags};
	for (std::uint32_t Level = 0; Level < FieldLevels; ++Level)
	{
		const float Spacing = FieldBaseSpacing * static_cast<float>(1u << Level);
		for (std::size_t Axis = 0; Axis < 3; ++Axis)
		{
			(*Origins[Level])[Axis] = std::floor(Eye[Axis] / Spacing) - static_cast<float>(FieldGrid[Axis] / 2);
		}

		(*Origins[Level])[3] = 0.f;
	}

	const auto Stage = [&](const EStage Kind, const std::array<std::uint32_t, 3> GroupCount)
	{
		Uniforms.Material.BaseColor = {static_cast<float>(Kind), 0.f, static_cast<float>(Frame), 0.f};
		const std::array Targets{FStorageTextureBinding{.Texture = IrradianceAtlas}, FStorageTextureBinding{.Texture = DistanceAtlas}};
		const std::array Buffers{Cells, Probes, Used, TraceList, Counters, Arguments};
		return Device.Dispatch({.Pipeline = Pipelines[0], .StorageTextures = Targets, .StorageBuffers = Buffers, .Uniforms = GetVisualUniformBytes(Uniforms), .GroupCount = GroupCount});
	};

	// Stages are timed in groups; each group ends before the next begins.
	const auto Time = [this](const std::string_view Name)
	{
		Device.EndGpuTiming();
		Device.BeginGpuTiming(Name);
	};

	std::expected<void, FPresentationError> Result;
	Device.BeginGpuTiming("GI probes");
	if (!bReset)
	{
		Result = Stage(EStage::Reset, {Groups(DistanceExtent.Width, 64), DistanceExtent.Height, 1});
		bReset = static_cast<bool>(Result);
	}

	for (const EStage Kind : {EStage::Clear, EStage::Select, EStage::Arguments})
	{
		if (Result)
		{
			Result = Stage(Kind, {Kind == EStage::Select || Kind == EStage::Settle ? Groups(FieldProbeCount, 64) : 1, 1, 1});
		}
	}

	Uniforms.Material.BaseColor = {0.f, 0.f, static_cast<float>(Frame), 0.f};
	const auto UniformBytes = GetVisualUniformBytes(Uniforms);
	Time("GI rays");
	if (Result)
	{
		const std::array Sources{Inputs.Environment, Inputs.SkyView, Inputs.SpecularAlbedo, IrradianceAtlas, DistanceAtlas};
		const std::array Buffers{Inputs.Lights, Inputs.Scene->GetSections(), Inputs.Scene->GetMaterials(), Probes, TraceList};
		const std::array Targets{RayData, Used};
		Result = Device.Dispatch({.Pipeline = Pipelines[1], .Textures = Sources, .StorageBuffers = Targets, .Uniforms = UniformBytes, .IndirectArguments = Arguments, .Buffers = Buffers, .Scene = Inputs.Scene->GetTopLevel(), .ResourceTable = Inputs.Scene->GetTable()});
	}

	Time("GI irradiance");
	if (Result)
	{
		const std::array Targets{FStorageTextureBinding{.Texture = IrradianceAtlas}, FStorageTextureBinding{.Texture = DistanceAtlas}};
		const std::array Outputs{Probes, Cells};
		const std::array Buffers{RayData, TraceList};
		Result = Device.Dispatch({.Pipeline = Pipelines[2], .StorageTextures = Targets, .StorageBuffers = Outputs, .Uniforms = UniformBytes, .IndirectArguments = Arguments, .Buffers = Buffers});
	}

	const FExtent2D Half = Gathered->GetDescriptor().Extent;
	const std::array<std::uint32_t, 3> BlockGroups{Groups(Half.Width / 2, 8), Groups(Half.Height, 8), 1};
	Time("GI gather");
	if (Result)
	{
		const std::array Sources{Inputs.Depth, Inputs.Surface, IrradianceAtlas, DistanceAtlas};
		const std::array Buffers{Probes};
		const std::array Targets{FStorageTextureBinding{.Texture = Gathered}};
		const std::array Outputs{Used};
		Result = Device.Dispatch({.Pipeline = Pipelines[3], .Textures = Sources, .StorageTextures = Targets, .StorageBuffers = Outputs, .Uniforms = UniformBytes, .GroupCount = BlockGroups, .Buffers = Buffers});
	}

	Time("GI reflections");
	if (Result)
	{
		// The gather writes only the atlas's left half and reflections only its right, so the two need no barrier between them.
		const std::array Sources{Inputs.Environment, Inputs.SkyView, Inputs.SpecularAlbedo, Inputs.Depth, IrradianceAtlas, DistanceAtlas, Inputs.Surface};
		const std::array Buffers{Inputs.Lights, Inputs.Scene->GetSections(), Inputs.Scene->GetMaterials(), Probes};
		const std::array Targets{FStorageTextureBinding{.Texture = Gathered}};
		const std::array Outputs{Used};
		Result = Device.Dispatch({.Pipeline = Pipelines[4], .Textures = Sources, .StorageTextures = Targets, .StorageBuffers = Outputs, .Uniforms = UniformBytes, .GroupCount = BlockGroups, .Buffers = Buffers, .bOverlapsPrevious = true, .Scene = Inputs.Scene->GetTopLevel(), .ResourceTable = Inputs.Scene->GetTable()});
	}

	Device.EndGpuTiming();
	if (const auto Bytes = Device.TryReadBuffer(CounterReadback[(Frame + 1) % CounterReadback.size()]))
	{
		std::array<std::uint32_t, 4> Words{};
		std::memcpy(Words.data(), Bytes->data(), sizeof(Words));
		Statistics = {.Traced = std::min(Words[0], FieldMaxTraced), .Read = Words[1], .Inside = Words[2], .Overflow = Words[0] > FieldMaxTraced ? Words[0] - FieldMaxTraced : 0};
	}

	if (Result)
	{
		Result = Device.CopyBuffer(Counters, CounterReadback[Frame % CounterReadback.size()]);
	}

	return Result;
}
}
