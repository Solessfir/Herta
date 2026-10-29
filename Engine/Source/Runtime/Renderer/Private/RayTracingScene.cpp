#include "RayTracingScene.h"

#include "MaterialUniform.h"

#include <algorithm>
#include <bit>
#include <format>
#include <unordered_set>

namespace Herta
{
namespace
{
// Keep in sync with RayTracing.slangh.
struct FSceneSection
{
	std::uint32_t VertexSlot = 0;
	std::uint32_t IndexSlot = 0;
	std::uint32_t FirstIndex = 0;
	std::uint32_t Material = 0;
};

struct alignas(16) FSceneMaterial
{
	FMaterialUniform Material{};
	std::array<std::uint32_t, 8> Textures{};
};

static_assert(sizeof(FSceneSection) == 16 && sizeof(FSceneMaterial) == 160);

template <typename T>
void AppendKey(std::vector<std::byte>& Key, const T& Value)
{
	const auto Bytes = std::as_bytes(std::span{&Value, 1});
	Key.insert(Key.end(), Bytes.begin(), Bytes.end());
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

FRayTracingScene::FRayTracingScene(IGraphicsDevice& InDevice)
    : Device(InDevice)
{
}

std::uint64_t FRayTracingScene::GetGeneration() const noexcept
{
	return Generation;
}

const FAccelerationStructureHandle& FRayTracingScene::GetTopLevel() const noexcept
{
	return TopLevel;
}

const FResourceTableHandle& FRayTracingScene::GetTable() const noexcept
{
	return Table;
}

const FBufferHandle& FRayTracingScene::GetSections() const noexcept
{
	return Sections;
}

const FBufferHandle& FRayTracingScene::GetMaterials() const noexcept
{
	return Materials;
}

void FRayTracingScene::Release() noexcept
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
	Key.clear();
}

std::expected<std::uint32_t, FPresentationError> FRayTracingScene::AllocateSlot()
{
	const std::uint64_t Completed = Device.GetStatistics().CompletedSerial;
	std::erase_if(RetiredSlots, [&](const FRetiredSlot& Retired)
	{
		if (Retired.Serial > Completed)
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
		return std::unexpected(FPresentationError{.Code = EPresentationErrorCode::InvalidDescriptor, .Message = std::format("The ray traced scene needs more than {} mesh buffers and textures", MaximumResourceTableSlots)});
	}

	return NextSlot++;
}

void FRayTracingScene::RetireUnused()
{
	// Every recording that read an unused slot was submitted before this one, so the slot frees once those complete.
	const std::uint64_t Submitted = Device.GetStatistics().SubmittedSerial;
	std::erase_if(Meshes, [&](const auto& Entry)
	{
		if (Entry.second.LastUsed == Frame)
		{
			return false;
		}

		RetiredSlots.push_back({.Slot = Entry.second.VertexSlot, .Serial = Submitted});
		RetiredSlots.push_back({.Slot = Entry.second.IndexSlot, .Serial = Submitted});
		return true;
	});

	std::erase_if(Textures, [&](const auto& Entry)
	{
		if (Entry.second.LastUsed == Frame)
		{
			return false;
		}

		RetiredSlots.push_back({.Slot = Entry.second.Slot, .Serial = Submitted});
		return true;
	});
}

std::expected<FRayTracingScene::FMeshEntry*, FPresentationError> FRayTracingScene::FindMesh(const FRenderMesh& Mesh, const bool bDeforming)
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

std::expected<std::uint32_t, FPresentationError> FRayTracingScene::FindTexture(const FTextureHandle& Texture)
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

std::expected<void, FPresentationError> FRayTracingScene::Update(const FMeshRenderView& View)
{
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

	NextKey.clear();
	if (!Deforming.empty())
	{
		// Deforming meshes change every frame they update, which no stored key would notice.
		AppendKey(NextKey, Frame);
	}

	std::vector<FSceneSection> SectionRecords;
	std::vector<FSceneMaterial> MaterialRecords;
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
				FSceneMaterial Record{.Material = MakeMaterialUniform(Override ? Override->Parameters : Imported.Parameters, Override ? Override->Channels : Imported.Channels)};
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

	if (NextKey == Key && TopLevel)
	{
		return {};
	}

	std::swap(Key, NextKey);
	RetireUnused();
	if (!TopLevel || TopLevelCapacity < Instances.size())
	{
		TopLevelCapacity = std::bit_ceil(std::max<std::uint32_t>(static_cast<std::uint32_t>(Instances.size()), 64));
		auto Created = Device.CreateTopLevel("Ray traced scene", TopLevelCapacity);
		if (!Created)
		{
			return std::unexpected(Created.error());
		}

		TopLevel = std::move(*Created);
	}

	// Empty scenes still bind one zeroed record each.
	SectionRecords.resize(std::max<std::size_t>(SectionRecords.size(), 1));
	MaterialRecords.resize(std::max<std::size_t>(MaterialRecords.size(), 1));
	auto Result = Reserve(Device, Sections, SectionRecords.size() * sizeof(FSceneSection), "Ray traced sections");
	if (Result)
	{
		Result = Reserve(Device, Materials, MaterialRecords.size() * sizeof(FSceneMaterial), "Ray traced materials");
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
		TopLevel.reset();
		return Result;
	}

	++Generation;
	return {};
}
}
