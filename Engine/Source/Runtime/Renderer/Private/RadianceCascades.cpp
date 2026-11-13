#include "RadianceCascades.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string_view>
#include <tuple>
#include <utility>

namespace Herta
{
namespace
{
// Keep in sync with IrradianceField.slangh and RadianceCascades.slangh.
constexpr std::uint32_t FieldLevels = 4;
constexpr std::array<std::int32_t, 3> FieldGrid{64, 64, 64};
constexpr std::uint32_t FieldCellsPerLevel = 64 * 64 * 64;
constexpr std::uint32_t FieldProbeCount = 65536;
constexpr std::uint32_t FieldMaxRequests = 8192;
constexpr float FieldBaseSpacing = 1.f;
constexpr std::uint32_t FieldTilesPerRow = 256;
constexpr FExtent2D IrradianceExtent{FieldTilesPerRow * 8, FieldProbeCount / FieldTilesPerRow * 8};
constexpr FExtent2D DistanceExtent{FieldTilesPerRow * 16, FieldProbeCount / FieldTilesPerRow * 16};
constexpr std::array<std::uint32_t, FieldLevels> LevelCount{49152, 12288, 3072, 1024};
constexpr std::array<std::uint32_t, FieldLevels> ListCount{8192, 2048, 512, 128};
constexpr std::uint32_t ListWords = 8192 + 2048 + 512 + 128;
constexpr std::uint32_t NearRays = 64;
// Probes record the frame they were last read, and a fresh buffer holds zero, so frames start far enough in that zero reads as long unused.
constexpr std::uint32_t FirstFrame = 128;

// Keep in sync with RadianceCascadesUpdate.slang.
enum class EStage : std::uint8_t
{
	Reset,
	Clear,
	Select,
	Allocate,
	Rays,
	Finish,
};

// The counters follow the lists' dispatch arguments in one buffer. Keep in sync with RadianceCascadesUpdate.slang.
constexpr std::uint32_t CounterListed = 16;
constexpr std::uint32_t CounterRead = 20;
constexpr std::uint32_t CounterInside = 21;
constexpr std::uint32_t CounterUnallocated = 22;
constexpr std::uint32_t CounterFree = 24;
constexpr std::uint32_t CounterWords = 36;

[[nodiscard]] constexpr std::uint32_t Groups(const std::uint32_t Threads, const std::uint32_t Size) noexcept
{
	return (Threads + Size - 1) / Size;
}

// A level's radiance atlas: tiles of 8 << Level texels a side inside a one-texel border, 256 >> Level to a row.
[[nodiscard]] constexpr FExtent2D RadianceExtent(const std::uint32_t Level) noexcept
{
	const std::uint32_t Tile = (8u << Level) + 2;
	const std::uint32_t PerRow = FieldTilesPerRow >> Level;
	return {PerRow * Tile, LevelCount[Level] / PerRow * Tile};
}
}

std::expected<std::unique_ptr<FRadianceCascades>, FPresentationError> FRadianceCascades::Create(IGraphicsDevice& Device, FRadianceCascadesShaders Shaders)
{
	if (!Device.SupportsRayQueries() || Shaders.Update.Bytecode.empty() || Shaders.Trace.Bytecode.empty() || Shaders.Near.Bytecode.empty() || Shaders.Blend.Bytecode.empty() || Shaders.Gather.Bytecode.empty() || Shaders.Reflect.Bytecode.empty())
	{
		return std::unexpected(FPresentationError{.Code = EPresentationErrorCode::InvalidDescriptor, .Message = "Radiance cascades need ray queries and all six of their shaders"});
	}

	auto Update = Device.CreateComputePipeline({.Name = "Radiance cascades update", .Shader = std::move(Shaders.Update), .StorageBufferCount = 8, .UniformBufferSize = VisualUniformBytes});
	auto Trace = Device.CreateComputePipeline({.Name = "Radiance cascades shells", .Shader = std::move(Shaders.Trace), .TextureCount = 6, .StorageTextureCount = 1, .StorageBufferCount = 3, .UniformBufferSize = VisualUniformBytes, .bLinearSampler = true, .bRepeatSampler = true, .BufferCount = 6, .bRayQuery = true, .bResourceTable = true});
	auto Near = Device.CreateComputePipeline({.Name = "Radiance cascades near rays", .Shader = std::move(Shaders.Near), .TextureCount = 8, .StorageBufferCount = 4, .UniformBufferSize = VisualUniformBytes, .bLinearSampler = true, .bRepeatSampler = true, .BufferCount = 6, .bRayQuery = true, .bResourceTable = true});
	auto Blend = Device.CreateComputePipeline({.Name = "Radiance cascades blend", .Shader = std::move(Shaders.Blend), .StorageTextureCount = 2, .StorageBufferCount = 2, .UniformBufferSize = VisualUniformBytes, .BufferCount = 2});
	auto Gather = Device.CreateComputePipeline({.Name = "Radiance cascades gather", .Shader = std::move(Shaders.Gather), .TextureCount = 4, .StorageTextureCount = 1, .StorageBufferCount = 3, .UniformBufferSize = VisualUniformBytes, .bLinearSampler = true, .BufferCount = 1});
	auto Reflect = Device.CreateComputePipeline({.Name = "Radiance cascades reflections", .Shader = std::move(Shaders.Reflect), .TextureCount = 7, .StorageTextureCount = 1, .StorageBufferCount = 3, .UniformBufferSize = VisualUniformBytes, .bLinearSampler = true, .bRepeatSampler = true, .BufferCount = 4, .bRayQuery = true, .bResourceTable = true});
	for (const auto* const Pipeline : {&Update, &Trace, &Near, &Blend, &Gather, &Reflect})
	{
		if (!*Pipeline)
		{
			return std::unexpected(Pipeline->error());
		}
	}

	return std::unique_ptr<FRadianceCascades>(new FRadianceCascades(Device, {std::move(*Update), std::move(*Trace), std::move(*Near), std::move(*Blend), std::move(*Gather), std::move(*Reflect)}));
}

FRadianceCascades::FRadianceCascades(IGraphicsDevice& InDevice, std::array<FComputePipelineHandle, 6> InPipelines)
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
	for (const FBufferHandle* const Buffer : {&Cells, &ProbeCells, &Probes, &Used, &Requests, &FreeList, &Lists, &Counters, &RayData})
	{
		Bytes += *Buffer ? (*Buffer)->GetDescriptor().Size : 0;
	}

	for (const FTextureHandle* const Texture : {&IrradianceAtlas, &DistanceAtlas, &Gathered, &RadianceAtlases[0], &RadianceAtlases[1], &RadianceAtlases[2]})
	{
		Bytes += *Texture ? std::size_t{(*Texture)->GetDescriptor().Extent.Width} * (*Texture)->GetDescriptor().Extent.Height * GetTextureTexelBytes((*Texture)->GetDescriptor().Format) : 0;
	}

	return Bytes;
}

void FRadianceCascades::Release() noexcept
{
	// Recordings in flight hold their own references, so nothing here waits on the GPU.
	IrradianceAtlas.reset();
	DistanceAtlas.reset();
	RadianceAtlases = {};
	Cells.reset();
	ProbeCells.reset();
	Probes.reset();
	Used.reset();
	Requests.reset();
	FreeList.reset();
	Lists.reset();
	Counters.reset();
	RayData.reset();
	Gathered.reset();
	CounterReadback = {};
	Statistics = {};
	bReset = false;
}

std::expected<void, FPresentationError> FRadianceCascades::Allocate(const FExtent2D Extent)
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
	for (const auto& [Destination, Name, Words] : {std::tuple{&Cells, "Radiance cascades cells", std::size_t{FieldLevels} * FieldCellsPerLevel}, std::tuple{&ProbeCells, "Radiance cascades probe cells", std::size_t{FieldProbeCount} * 4}, std::tuple{&Probes, "Radiance cascades probes", std::size_t{FieldProbeCount} * 4}, std::tuple{&Used, "Radiance cascades use", std::size_t{FieldProbeCount} * 4}, std::tuple{&Requests, "Radiance cascades requests", std::size_t{FieldMaxRequests + 1} * 4}, std::tuple{&FreeList, "Radiance cascades free list", std::size_t{FieldProbeCount}}, std::tuple{&Lists, "Radiance cascades lists", std::size_t{ListWords}}, std::tuple{&Counters, "Radiance cascades counters", std::size_t{CounterWords}}, std::tuple{&RayData, "Radiance cascades near rays", std::size_t{ListCount[0]} * NearRays * 4}})
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
			Result = Buffer(Readback, "Radiance cascades counter readback", CounterWords, EBufferUsage::Readback);
		}
	}

	if (Result && (!IrradianceAtlas || !DistanceAtlas || !RadianceAtlases[2]))
	{
		bReset = false;
		Result = Texture(IrradianceAtlas, "Radiance cascades irradiance", IrradianceExtent, ETextureFormat::Rgba16Float);
		if (Result)
		{
			Result = Texture(DistanceAtlas, "Radiance cascades distances", DistanceExtent, ETextureFormat::Rg16Float);
		}

		constexpr std::array Names{"Radiance cascades level 1", "Radiance cascades level 2", "Radiance cascades level 3"};
		for (std::uint32_t Level = 1; Level < FieldLevels && Result; ++Level)
		{
			Result = Texture(RadianceAtlases[Level - 1], Names[Level - 1], RadianceExtent(Level), ETextureFormat::R11G11B10Float);
		}
	}

	if (Result)
	{
		Result = Texture(Gathered, "Global illumination gathers", {(Extent.Width + 1) / 2 * 2, (Extent.Height + 1) / 2}, ETextureFormat::Rgba16Float);
	}

	return Result;
}

std::expected<void, FPresentationError> FRadianceCascades::Render(const FGlobalIlluminationInputs& Inputs)
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
			const std::int32_t HalfGrid = FieldGrid[Axis] / 2;
			(*Origins[Level])[Axis] = std::floor(Eye[Axis] / Spacing) - static_cast<float>(HalfGrid);
		}

		(*Origins[Level])[3] = 0.f;
	}

	// BaseColor carries the update's stage, the shells' level, the frame, and whether a dispatch only clears new atlases.
	const auto SetPass = [&](const float Stage, const float Level, const bool bClear)
	{
		Uniforms.Material.BaseColor = {Stage, Level, static_cast<float>(Frame), bClear ? 1.f : 0.f};
		return GetVisualUniformBytes(Uniforms);
	};

	const auto Stage = [&](const EStage Kind, const std::uint32_t GroupCount)
	{
		const std::array Buffers{Cells, ProbeCells, Probes, Used, Requests, FreeList, Lists, Counters};
		return Device.Dispatch({.Pipeline = Pipelines[0], .StorageBuffers = Buffers, .Uniforms = SetPass(static_cast<float>(Kind), 0.f, false), .GroupCount = {Groups(GroupCount, 64), 1, 1}});
	};

	// Stages are timed in groups; each group ends before the next begins.
	const auto Time = [this](const std::string_view Name)
	{
		Device.EndGpuTiming();
		Device.BeginGpuTiming(Name);
	};

	// A level's shells read the level above's atlas and write their own. The top level reads none, and a clear reads nothing, so they bind an
	// atlas they do not write that has already been written.
	const std::array SceneBuffers{Inputs.Lights, Inputs.Scene->GetSections(), Inputs.Scene->GetMaterials(), Probes, Lists, ProbeCells};
	const std::array FieldOutputs{Cells, Used, Requests};
	const auto Shells = [&](const std::uint32_t Level, const bool bClear)
	{
		const FTextureHandle& Target = RadianceAtlases[Level - 1];
		const FExtent2D Size = Target->GetDescriptor().Extent;
		const std::array Sources{Inputs.Environment, Inputs.SkyView, Inputs.SpecularAlbedo, IrradianceAtlas, DistanceAtlas, bClear ? IrradianceAtlas : RadianceAtlases[Level == FieldLevels - 1 ? 0 : Level]};
		const std::array Targets{FStorageTextureBinding{.Texture = Target}};
		FComputeDispatch Dispatch{.Pipeline = Pipelines[1], .Textures = Sources, .StorageTextures = Targets, .StorageBuffers = FieldOutputs, .Uniforms = SetPass(0.f, static_cast<float>(Level), bClear), .GroupCount = {Groups(Size.Width, 64), Size.Height, 1}, .Buffers = SceneBuffers, .Scene = Inputs.Scene->GetTopLevel(), .ResourceTable = Inputs.Scene->GetTable()};
		if (!bClear)
		{
			Dispatch.IndirectArguments = Counters;
			Dispatch.IndirectOffset = std::size_t{Level} * 4 * sizeof(std::uint32_t);
		}

		return Device.Dispatch(Dispatch);
	};

	const std::array AtlasTargets{FStorageTextureBinding{.Texture = IrradianceAtlas}, FStorageTextureBinding{.Texture = DistanceAtlas}};
	const std::array BlendOutputs{Probes, ProbeCells};
	const std::array BlendBuffers{RayData, Lists};
	std::expected<void, FPresentationError> Result;
	Device.BeginGpuTiming("GI probes");
	if (!bReset)
	{
		Result = Stage(EStage::Reset, FieldLevels * FieldCellsPerLevel);
		if (Result)
		{
			// The blend clears the new irradiance and distance atlases once, over the distance atlas's texels.
			Result = Device.Dispatch({.Pipeline = Pipelines[3], .StorageTextures = AtlasTargets, .StorageBuffers = BlendOutputs, .Uniforms = SetPass(0.f, 0.f, true), .GroupCount = {DistanceExtent.Width / 16, DistanceExtent.Height / 16, 1}, .Buffers = BlendBuffers});
		}

		for (std::uint32_t Level = 1; Level < FieldLevels && Result; ++Level)
		{
			Result = Shells(Level, true);
		}

		bReset = static_cast<bool>(Result);
	}

	// Allocation follows selection so it can reuse the probes selection freed, and probes only rays read follow both so they only take what room the others leave.
	for (const auto& [Kind, Threads] : {std::pair{EStage::Clear, FieldLevels}, std::pair{EStage::Select, FieldProbeCount}, std::pair{EStage::Allocate, FieldMaxRequests}, std::pair{EStage::Rays, FieldProbeCount}, std::pair{EStage::Finish, FieldLevels}})
	{
		if (Result)
		{
			Result = Stage(Kind, Threads);
		}
	}

	// From the top down, so light crosses every level within the frame.
	Time("GI shells");
	for (std::uint32_t Level = FieldLevels - 1; Level >= 1 && Result; --Level)
	{
		Result = Shells(Level, false);
	}

	const auto UniformBytes = SetPass(0.f, 0.f, false);
	Time("GI rays");
	if (Result)
	{
		const std::array Sources{Inputs.Environment, Inputs.SkyView, Inputs.SpecularAlbedo, IrradianceAtlas, DistanceAtlas, RadianceAtlases[0], RadianceAtlases[1], RadianceAtlases[2]};
		const std::array Targets{RayData, Cells, Used, Requests};
		Result = Device.Dispatch({.Pipeline = Pipelines[2], .Textures = Sources, .StorageBuffers = Targets, .Uniforms = UniformBytes, .IndirectArguments = Counters, .Buffers = SceneBuffers, .Scene = Inputs.Scene->GetTopLevel(), .ResourceTable = Inputs.Scene->GetTable()});
	}

	Time("GI irradiance");
	if (Result)
	{
		Result = Device.Dispatch({.Pipeline = Pipelines[3], .StorageTextures = AtlasTargets, .StorageBuffers = BlendOutputs, .Uniforms = UniformBytes, .IndirectArguments = Counters, .Buffers = BlendBuffers});
	}

	const FExtent2D Half = Gathered->GetDescriptor().Extent;
	const std::array<std::uint32_t, 3> BlockGroups{Groups(Half.Width / 2, 8), Groups(Half.Height, 8), 1};
	Time("GI gather");
	if (Result)
	{
		const std::array Sources{Inputs.Depth, Inputs.Surface, IrradianceAtlas, DistanceAtlas};
		const std::array Buffers{Probes};
		const std::array Targets{FStorageTextureBinding{.Texture = Gathered}};
		Result = Device.Dispatch({.Pipeline = Pipelines[4], .Textures = Sources, .StorageTextures = Targets, .StorageBuffers = FieldOutputs, .Uniforms = UniformBytes, .GroupCount = BlockGroups, .Buffers = Buffers});
	}

	Time("GI reflections");
	if (Result)
	{
		// The gather writes only the atlas's left half and reflections only its right, so the two need no barrier between them.
		const std::array Sources{Inputs.Environment, Inputs.SkyView, Inputs.SpecularAlbedo, Inputs.Depth, IrradianceAtlas, DistanceAtlas, Inputs.Surface};
		const std::array Buffers{Inputs.Lights, Inputs.Scene->GetSections(), Inputs.Scene->GetMaterials(), Probes};
		const std::array Targets{FStorageTextureBinding{.Texture = Gathered}};
		Result = Device.Dispatch({.Pipeline = Pipelines[5], .Textures = Sources, .StorageTextures = Targets, .StorageBuffers = FieldOutputs, .Uniforms = UniformBytes, .GroupCount = BlockGroups, .Buffers = Buffers, .bOverlapsPrevious = true, .Scene = Inputs.Scene->GetTopLevel(), .ResourceTable = Inputs.Scene->GetTable()});
	}

	Device.EndGpuTiming();
	if (const auto Bytes = Device.TryReadBuffer(CounterReadback[(Frame + 1) % CounterReadback.size()]))
	{
		std::array<std::uint32_t, CounterWords> Words{};
		std::memcpy(Words.data(), Bytes->data(), sizeof(Words));
		std::uint32_t Overflow = 0;
		std::uint32_t Free = 0;
		for (std::uint32_t Level = 0; Level < FieldLevels; ++Level)
		{
			Overflow += Words[CounterListed + Level] - std::min(Words[CounterListed + Level], ListCount[Level]);
			Free += std::min(Words[CounterFree + Level], LevelCount[Level]);
		}

		Statistics = {.Traced = std::min(Words[CounterListed], ListCount[0]), .Read = Words[CounterRead], .Inside = Words[CounterInside], .Overflow = Overflow, .Allocated = FieldProbeCount - Free, .Unallocated = Words[CounterUnallocated]};
	}

	if (Result)
	{
		Result = Device.CopyBuffer(Counters, CounterReadback[Frame % CounterReadback.size()]);
	}

	return Result;
}
}
