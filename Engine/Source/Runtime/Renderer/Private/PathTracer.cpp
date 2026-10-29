#include "PathTracer.h"

#include "MaterialUniform.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <format>
#include <unordered_set>

namespace Herta
{
namespace
{
constexpr std::uint32_t MaximumBounces = 8;

// Keep in sync with PathTrace.slang.
struct FPathSection
{
	std::uint32_t VertexSlot = 0;
	std::uint32_t IndexSlot = 0;
	std::uint32_t FirstIndex = 0;
	std::uint32_t Material = 0;
};

struct alignas(16) FPathMaterial
{
	FMaterialUniform Material{};
	std::array<std::uint32_t, 8> Textures{};
};

static_assert(sizeof(FPathSection) == 16 && sizeof(FPathMaterial) == 160);

template <typename T>
void AppendKey(std::vector<std::byte>& Key, const T& Value)
{
	const auto Bytes = std::as_bytes(std::span{&Value, 1});
	Key.insert(Key.end(), Bytes.begin(), Bytes.end());
}

[[nodiscard]] std::unexpected<FPresentationError> Invalid(const std::string& Message)
{
	return std::unexpected(FPresentationError{.Code = EPresentationErrorCode::InvalidDescriptor, .Message = Message});
}

// Grows Buffer to hold Bytes, in powers of two so a slowly growing scene reallocates rarely.
[[nodiscard]] std::expected<void, FPresentationError> Reserve(IGraphicsDevice& Device, FBufferHandle& Buffer, const std::size_t Bytes, const char* const Name)
{
	if (Buffer && Buffer->GetDescriptor().Size >= Bytes)
	{
		return {};
	}

	auto Created = Device.CreateBuffer({.Name = Name, .Size = std::bit_ceil(std::max<std::size_t>(Bytes, 256)), .Usage = EBufferUsage::Storage});
	if (!Created)
	{
		return std::unexpected(Created.error());
	}

	Buffer = std::move(*Created);
	return {};
}
}

std::expected<std::unique_ptr<FPathTracer>, FPresentationError> FPathTracer::Create(IGraphicsDevice& Device, FShaderAsset Shader)
{
	if (!Device.SupportsRayQueries() || Shader.Bytecode.empty())
	{
		return Invalid("The path tracer needs ray queries and its shader");
	}

	auto Pipeline = Device.CreateComputePipeline({.Name = "Path tracing", .Shader = std::move(Shader), .TextureCount = 3, .StorageTextureCount = 2, .UniformBufferSize = VisualUniformBytes, .bLinearSampler = true, .bRepeatSampler = true, .BufferCount = 3, .bRayQuery = true, .bResourceTable = true});
	if (!Pipeline)
	{
		return std::unexpected(Pipeline.error());
	}

	return std::unique_ptr<FPathTracer>(new FPathTracer(Device, std::move(*Pipeline)));
}

FPathTracer::FPathTracer(IGraphicsDevice& InDevice, FComputePipelineHandle InPipeline)
    : Device(InDevice)
    , Pipeline(std::move(InPipeline))
{
}

std::uint32_t FPathTracer::GetSampleCount() const noexcept
{
	return SampleCount;
}

void FPathTracer::Release() noexcept
{
	// Recordings in flight hold their own references, so nothing here waits on the GPU.
	Table.reset();
	NextSlot = 0;
	FreeSlots.clear();
	RetiredSlots.clear();
	Meshes.clear();
	Textures.clear();
	TopLevel.reset();
	TopLevelCapacity = 0;
	Sections.reset();
	Materials.reset();
	Accumulation.reset();
	Key.clear();
	SampleCount = 0;
}

std::expected<std::uint32_t, FPresentationError> FPathTracer::AllocateSlot()
{
	const std::uint64_t Completed = Device.GetStatistics().CompletedSerial;
	std::erase_if(RetiredSlots, [&](const FRetiredSlot& Retired)
	{
		if (Retired.Frame > Completed)
		{
			return false;
		}

		FreeSlots.push_back(Retired.Slot);
		return true;
	});

	if (!FreeSlots.empty())
	{
		const std::uint32_t Slot = FreeSlots.back();
		FreeSlots.pop_back();
		return Slot;
	}

	if (NextSlot >= MaximumResourceTableSlots)
	{
		return Invalid(std::format("The path traced scene needs more than {} mesh buffers and textures", MaximumResourceTableSlots));
	}

	return NextSlot++;
}

void FPathTracer::RetireUnused()
{
	// Every recording that read an unused slot was submitted before this one, so the slot frees once those complete.
	const std::uint64_t Submitted = Device.GetStatistics().SubmittedSerial;
	std::erase_if(Meshes, [&](const auto& Entry)
	{
		if (Entry.second.LastUsed == Frame)
		{
			return false;
		}

		RetiredSlots.push_back({.Slot = Entry.second.VertexSlot, .Frame = Submitted});
		RetiredSlots.push_back({.Slot = Entry.second.IndexSlot, .Frame = Submitted});
		return true;
	});

	std::erase_if(Textures, [&](const auto& Entry)
	{
		if (Entry.second.LastUsed == Frame)
		{
			return false;
		}

		RetiredSlots.push_back({.Slot = Entry.second.Slot, .Frame = Submitted});
		return true;
	});
}

std::expected<FPathTracer::FMeshEntry*, FPresentationError> FPathTracer::FindMesh(const FRenderMesh& Mesh, const bool bDeforming)
{
	FMeshEntry& Entry = Meshes[Mesh.Vertices.get()];
	if (!Entry.Vertices)
	{
		auto VertexSlot = AllocateSlot();
		auto IndexSlot = VertexSlot ? AllocateSlot() : VertexSlot;
		if (!VertexSlot || !IndexSlot)
		{
			Meshes.erase(Mesh.Vertices.get());
			return std::unexpected(!VertexSlot ? VertexSlot.error() : IndexSlot.error());
		}

		Entry.Vertices = Mesh.Vertices;
		Entry.Indices = Mesh.Indices;
		Entry.VertexSlot = *VertexSlot;
		Entry.IndexSlot = *IndexSlot;
		auto Written = Device.WriteResourceTable(Table, Entry.VertexSlot, Entry.Vertices);
		if (Written)
		{
			Written = Device.WriteResourceTable(Table, Entry.IndexSlot, Entry.Indices);
		}

		if (!Written)
		{
			return std::unexpected(Written.error());
		}
	}

	// A mesh that starts deforming gets a structure that refits; a static one keeps its faster trace.
	if (!Entry.BottomLevel || (bDeforming && !Entry.bDeforming))
	{
		std::vector<FBottomLevelSection> Ranges;
		for (const FRenderMesh::FSection& Section : Mesh.Sections)
		{
			Ranges.push_back({.FirstIndex = Section.FirstIndex, .IndexCount = Section.IndexCount});
		}

		auto BottomLevel = Device.CreateBottomLevel({.Name = "Mesh bottom level", .Vertices = Entry.Vertices, .Indices = Entry.Indices, .Sections = Ranges, .bDeforming = bDeforming});
		if (!BottomLevel)
		{
			return std::unexpected(BottomLevel.error());
		}

		Entry.BottomLevel = std::move(*BottomLevel);
		Entry.bDeforming = bDeforming;
		if (auto Built = Device.BuildBottomLevel(Entry.BottomLevel); !Built)
		{
			return std::unexpected(Built.error());
		}
	}
	else if (bDeforming)
	{
		if (auto Built = Device.BuildBottomLevel(Entry.BottomLevel); !Built)
		{
			return std::unexpected(Built.error());
		}
	}

	Entry.LastUsed = Frame;
	return &Entry;
}

std::expected<std::uint32_t, FPresentationError> FPathTracer::FindTexture(const FTextureHandle& Texture)
{
	FTextureEntry& Entry = Textures[Texture.get()];
	if (!Entry.Texture)
	{
		auto Slot = AllocateSlot();
		if (!Slot)
		{
			Textures.erase(Texture.get());
			return std::unexpected(Slot.error());
		}

		Entry.Texture = Texture;
		Entry.Slot = *Slot;
		if (auto Written = Device.WriteResourceTable(Table, Entry.Slot, Texture); !Written)
		{
			return std::unexpected(Written.error());
		}
	}

	Entry.LastUsed = Frame;
	return Entry.Slot;
}

std::expected<void, FPresentationError> FPathTracer::Render(const FPathTraceInputs& Inputs)
{
	const FMeshRenderView& View = *Inputs.View;
	const FVisualUniforms& Uniforms = *Inputs.Uniforms;
	++Frame;
	if (!Table)
	{
		auto Created = Device.CreateResourceTable(MaximumResourceTableSlots);
		if (!Created)
		{
			return std::unexpected(Created.error());
		}

		Table = std::move(*Created);
	}

	std::unordered_set<const FRenderMesh*> Deforming;
	for (const FRenderMeshVertexUpdate& Update : View.VertexUpdates)
	{
		Deforming.insert(Update.Mesh);
	}

	// The view, lights, and environment; exposure and display settings only change how the result is shown.
	NextKey.clear();
	AppendKey(NextKey, Inputs.Extent);
	AppendKey(NextKey, Uniforms.ViewToWorld);
	AppendKey(NextKey, Uniforms.ClipToView);
	AppendKey(NextKey, Uniforms.Sky);
	AppendKey(NextKey, Uniforms.SkyColor);
	AppendKey(NextKey, Uniforms.Atmosphere);
	AppendKey(NextKey, Uniforms.AtmosphereGeometry);
	AppendKey(NextKey, Uniforms.Controls);
	AppendKey(NextKey, Uniforms.SunIlluminance);
	AppendKey(NextKey, Uniforms.SunDirection);
	AppendKey(NextKey, Uniforms.SunRadiance);
	AppendKey(NextKey, Inputs.EnvironmentIdentity);
	const std::size_t LightCount = std::min<std::size_t>(static_cast<std::size_t>(Uniforms.Controls[0]), MaximumRenderLights);
	const auto LightBytes = std::as_bytes(std::span{Uniforms.Lights}.first(LightCount));
	NextKey.insert(NextKey.end(), LightBytes.begin(), LightBytes.end());
	if (!Deforming.empty())
	{
		// Deforming meshes change every frame they update, which no stored key would notice.
		AppendKey(NextKey, Frame);
	}

	std::vector<FPathSection> SectionRecords;
	std::vector<FPathMaterial> MaterialRecords;
	std::unordered_map<const void*, std::uint32_t> MaterialIndices;
	std::vector<FRayTracingInstance> Instances;
	for (std::size_t Model = 0; Model < View.Models.size(); ++Model)
	{
		const FRenderMesh* const Mesh = View.Meshes[Model];
		if (!Mesh || Mesh->Sections.empty())
		{
			continue;
		}

		auto Entry = FindMesh(*Mesh, Deforming.contains(Mesh));
		if (!Entry)
		{
			return std::unexpected(Entry.error());
		}

		const std::span<const FRenderMaterial* const> Overrides = View.Materials.empty() ? std::span<const FRenderMaterial* const>{} : View.Materials[Model];
		FRayTracingInstance Instance{.CustomIndex = static_cast<std::uint32_t>(SectionRecords.size()), .BottomLevel = (*Entry)->BottomLevel};
		for (const FRenderMesh::FSection& Section : Mesh->Sections)
		{
			const FRenderMaterial* const Override = Section.Material < Overrides.size() ? Overrides[Section.Material] : nullptr;
			const FCookedMaterial& Imported = Mesh->Materials[Section.Material];
			const void* const MaterialKey = Override ? static_cast<const void*>(Override) : static_cast<const void*>(&Imported);
			auto [Found, bInserted] = MaterialIndices.try_emplace(MaterialKey, static_cast<std::uint32_t>(MaterialRecords.size()));
			if (bInserted)
			{
				FPathMaterial Record{.Material = MakeMaterialUniform(Override ? Override->Parameters : Imported.Parameters, Override ? Override->Channels : Imported.Channels)};
				for (std::size_t Slot = 0; Slot < MaterialTextureSlotCount; ++Slot)
				{
					const FTextureHandle Texture = Override ? Override->Textures[Slot] : Imported.Textures[Slot] != NoCookedTexture ? Mesh->Textures[Imported.Textures[Slot]]
					                                                                                                                : FTextureHandle{};
					if (!Texture)
					{
						continue;
					}

					auto TextureSlot = FindTexture(Texture);
					if (!TextureSlot)
					{
						return std::unexpected(TextureSlot.error());
					}

					Record.Textures[Slot] = *TextureSlot;
					Record.Material.TextureFlags[0] += static_cast<float>(1u << Slot);
				}

				MaterialRecords.push_back(Record);
				AppendKey(NextKey, Record);
			}

			Instance.bAlphaTested = Instance.bAlphaTested || MaterialRecords[Found->second].Material.Channels1[2] > 0.5f;
			SectionRecords.push_back({.VertexSlot = (*Entry)->VertexSlot, .IndexSlot = (*Entry)->IndexSlot, .FirstIndex = Section.FirstIndex, .Material = Found->second});
			AppendKey(NextKey, Found->second);
		}

		const std::array<float, 16>& Matrix = View.Models[Model].Data();
		for (std::size_t Row = 0; Row < 3; ++Row)
		{
			for (std::size_t Column = 0; Column < 4; ++Column)
			{
				Instance.ObjectToWorld[Row * 4 + Column] = Matrix[Column * 4 + Row];
			}
		}

		AppendKey(NextKey, Mesh->Vertices.get());
		AppendKey(NextKey, Instance.ObjectToWorld);
		Instances.push_back(std::move(Instance));
	}

	const bool bChanged = NextKey != Key || !Accumulation || Accumulation->GetDescriptor().Extent != Inputs.Extent;
	std::swap(Key, NextKey);
	if (!bChanged && SampleCount >= PathTracingSampleLimit)
	{
		return {};
	}

	if (bChanged)
	{
		SampleCount = 0;
		RetireUnused();
		if (!Accumulation || Accumulation->GetDescriptor().Extent != Inputs.Extent)
		{
			auto Created = Device.CreateTexture({.Name = "Path tracing accumulation", .Extent = Inputs.Extent, .Format = ETextureFormat::Rgba32Float, .bStorage = true});
			if (!Created)
			{
				return std::unexpected(Created.error());
			}

			Accumulation = std::move(*Created);
		}

		if (!TopLevel || TopLevelCapacity < Instances.size())
		{
			TopLevelCapacity = std::bit_ceil(std::max<std::uint32_t>(static_cast<std::uint32_t>(Instances.size()), 64));
			auto Created = Device.CreateTopLevel("Path tracing scene", TopLevelCapacity);
			if (!Created)
			{
				return std::unexpected(Created.error());
			}

			TopLevel = std::move(*Created);
		}

		// Empty scenes still bind one zeroed record each.
		SectionRecords.resize(std::max<std::size_t>(SectionRecords.size(), 1));
		MaterialRecords.resize(std::max<std::size_t>(MaterialRecords.size(), 1));
		auto Result = Reserve(Device, Sections, SectionRecords.size() * sizeof(FPathSection), "Path tracing sections");
		if (Result)
		{
			Result = Reserve(Device, Materials, MaterialRecords.size() * sizeof(FPathMaterial), "Path tracing materials");
		}

		if (Result)
		{
			Result = Device.WriteBuffer(Sections, std::as_bytes(std::span{SectionRecords}));
		}

		if (Result)
		{
			Result = Device.WriteBuffer(Materials, std::as_bytes(std::span{MaterialRecords}));
		}

		if (Result)
		{
			Result = Device.BuildTopLevel(TopLevel, Instances);
		}

		if (!Result)
		{
			// The next frame rebuilds everything rather than tracing a half-updated scene.
			Key.clear();
			return Result;
		}
	}

	FVisualUniforms Dispatch = Uniforms;
	// The angle one pixel subtends at the image centre seeds each ray cone.
	const float FocalScale = std::abs(View.Projection.Data()[5]);
	Dispatch.Material.BaseColor = {static_cast<float>(SampleCount), static_cast<float>(MaximumBounces), std::atan(2.f / (FocalScale * static_cast<float>(Inputs.Extent.Height))), 0.f};
	const std::array Sources{Inputs.Environment, Inputs.SkyView, Inputs.SpecularAlbedo};
	const std::array Targets{FStorageTextureBinding{.Texture = Accumulation}, FStorageTextureBinding{.Texture = Inputs.Output}};
	const std::array Buffers{Inputs.Lights, Sections, Materials};
	auto Traced = Device.Dispatch({.Pipeline = Pipeline, .Textures = Sources, .StorageTextures = Targets, .Uniforms = GetVisualUniformBytes(Dispatch), .GroupCount = {(Inputs.Extent.Width + 7) / 8, (Inputs.Extent.Height + 7) / 8, 1}, .Buffers = Buffers, .Scene = TopLevel, .ResourceTable = Table});
	if (!Traced)
	{
		Key.clear();
		return Traced;
	}

	++SampleCount;
	return {};
}
}
