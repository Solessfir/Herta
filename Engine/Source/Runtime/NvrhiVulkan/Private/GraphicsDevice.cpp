#include "GraphicsDevice.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <exception>
#include <format>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace Herta
{
namespace
{
constexpr std::size_t FrameCount = 3;
constexpr std::size_t MaximumUploadBytes = MaximumUploadBytesPerRecording;
constexpr std::size_t MaximumCommands = 16'384;
constexpr std::uint32_t MaximumTextureDimension = 8192;
constexpr std::uint32_t MeshPushConstantSize = sizeof(FIndexedDraw::WorldToClip) + sizeof(FIndexedDraw::ObjectToView);
constexpr std::size_t MaximumUniformBytesPerFrame = 4 * 1024 * 1024;
constexpr std::size_t MaximumGpuPassTimings = 32;

[[nodiscard]] std::unexpected<FPresentationError> Invalid(const std::string& Message)
{
	return std::unexpected(FPresentationError{.Code = EPresentationErrorCode::InvalidDescriptor, .Message = Message});
}

[[nodiscard]] std::unexpected<FPresentationError> Failed(const std::string& Message)
{
	return std::unexpected(FPresentationError{.Code = EPresentationErrorCode::CommandSubmissionFailed, .Message = Message});
}

[[nodiscard]] nvrhi::Format ToFormat(const ETextureFormat Format)
{
	switch (Format)
	{
		case ETextureFormat::Rgba8:
			return nvrhi::Format::RGBA8_UNORM;
		case ETextureFormat::Rgba8Srgb:
			return nvrhi::Format::SRGBA8_UNORM;
		case ETextureFormat::Depth32:
			return nvrhi::Format::D32;
		case ETextureFormat::Rgba16Float:
			return nvrhi::Format::RGBA16_FLOAT;
		case ETextureFormat::Rgba32Float:
			return nvrhi::Format::RGBA32_FLOAT;
		case ETextureFormat::R32Float:
			return nvrhi::Format::R32_FLOAT;
		case ETextureFormat::R32Uint:
			return nvrhi::Format::R32_UINT;
		case ETextureFormat::Rg16Float:
			return nvrhi::Format::RG16_FLOAT;
		case ETextureFormat::Depth16:
			return nvrhi::Format::D16;
		case ETextureFormat::R8:
			return nvrhi::Format::R8_UNORM;
	}

	return nvrhi::Format::UNKNOWN;
}

struct FBuffer final : IRhiBuffer
{
	FBufferDescriptor Descriptor;
	std::shared_ptr<const std::uint8_t> Owner;
	nvrhi::BufferHandle Handle;
	bool bInitialized = false;
	std::uint32_t MaximumIndex = 0;
	// The frame whose copy filled a readback buffer; zero before any copy completes.
	std::uint64_t ReadySerial = 0;

	[[nodiscard]] const FBufferDescriptor& GetDescriptor() const noexcept override
	{
		return Descriptor;
	}
};

struct FTexture final : IRhiTexture
{
	FTextureDescriptor Descriptor;
	std::shared_ptr<const std::uint8_t> Owner;
	nvrhi::IDevice* DeviceIdentity = nullptr;
	nvrhi::TextureHandle Handle;
	bool bInitialized = false;
	// One bit per mip level. Sampling waits until every level has been written.
	std::uint32_t WrittenMips = 0;
	// Aliased textures share this with textures of other phases, and discard their contents on their first use in each recording.
	nvrhi::HeapHandle Heap;

	[[nodiscard]] const FTextureDescriptor& GetDescriptor() const noexcept override
	{
		return Descriptor;
	}
};

[[nodiscard]] std::uint32_t AllMips(const FTexture& Texture) noexcept
{
	return Texture.Descriptor.MipLevels >= 32 ? ~0u : (1u << Texture.Descriptor.MipLevels) - 1;
}

// Aliased textures start each recording undefined, after whatever other phase last wrote their memory. NVRHI cannot track an undefined prior state,
// so this transitions them from undefined to the read-only layout NVRHI believes they hold, which discards stale contents and compression metadata.
// Barriers are illegal inside a render pass, so any open one ends first; the next draw or dispatch rebinds its state.
void DiscardAliases(nvrhi::ICommandList* const Commands, const std::vector<std::shared_ptr<FTexture>>& Textures)
{
	if (Textures.empty())
	{
		return;
	}

	Commands->clearState();
	Commands->commitBarriers();
	std::vector<VkImageMemoryBarrier2> Barriers;
	for (const auto& Texture : Textures)
	{
		VkImageMemoryBarrier2 Barrier{};
		Barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
		Barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
		Barrier.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
		Barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
		Barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
		Barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		Barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		Barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		Barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		Barrier.image = static_cast<VkImage>(Texture->Handle->getNativeObject(nvrhi::ObjectTypes::VK_Image));
		Barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
		Barriers.push_back(Barrier);
	}

	VkDependencyInfo Dependency{};
	Dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
	Dependency.imageMemoryBarrierCount = static_cast<std::uint32_t>(Barriers.size());
	Dependency.pImageMemoryBarriers = Barriers.data();
	vkCmdPipelineBarrier2(static_cast<VkCommandBuffer>(Commands->getNativeObject(nvrhi::ObjectTypes::VK_CommandBuffer)), &Dependency);
}

struct FPipeline final : IRhiGraphicsPipeline
{
	std::string Name;
	std::shared_ptr<const std::uint8_t> Owner;
	nvrhi::GraphicsPipelineHandle Handle;
	nvrhi::BindingLayoutHandle Layout;
	nvrhi::SamplerHandle Sampler;
	ETextureFormat ColorFormat;
	std::optional<ETextureFormat> SecondColorFormat;
	EGraphicsVertexFormat VertexFormat;
	bool bInstanced = false;
	bool bInstanceMotion = false;
	bool bDepthOnly = false;
	bool bDepthAttachment = true;
	bool bPushConstants = false;
	std::uint32_t TextureCount = 0;
	std::uint32_t BufferCount = 0;
	std::uint32_t UniformBufferSize = 0;
};

struct FComputePipeline final : IRhiComputePipeline
{
	std::string Name;
	std::shared_ptr<const std::uint8_t> Owner;
	nvrhi::ComputePipelineHandle Handle;
	nvrhi::BindingLayoutHandle Layout;
	nvrhi::SamplerHandle Sampler;
	std::uint32_t TextureCount = 0;
	std::uint32_t StorageTextureCount = 0;
	std::uint32_t StorageBufferCount = 0;
	std::uint32_t BufferCount = 0;
	std::uint32_t UniformBufferSize = 0;
	bool bRayQuery = false;
	bool bResourceTable = false;
};

struct FAccelerationStructure final : IRhiAccelerationStructure
{
	std::shared_ptr<const std::uint8_t> Owner;
	nvrhi::rt::AccelStructHandle Handle;
	std::size_t ByteSize = 0;
	bool bTopLevel = false;
	std::uint32_t MaximumInstances = 0;
	// Bottom levels keep their inputs alive and rebuild from the same geometry when they refit.
	std::shared_ptr<FBuffer> Vertices;
	std::shared_ptr<FBuffer> Indices;
	std::vector<nvrhi::rt::GeometryDesc> Geometries;
	bool bDeforming = false;
	bool bBuilt = false;

	[[nodiscard]] std::size_t GetByteSize() const noexcept override
	{
		return ByteSize;
	}
};

struct FResourceTable final : IRhiResourceTable
{
	std::shared_ptr<const std::uint8_t> Owner;
	nvrhi::DescriptorTableHandle Handle;
	// Dispatches copy these so a rewritten slot's old resource outlives the recordings that read it.
	std::vector<std::shared_ptr<FTexture>> Textures;
	std::vector<std::shared_ptr<FBuffer>> Buffers;
};

struct FPassTiming
{
	std::string Name;
	nvrhi::TimerQueryHandle Query;
	bool bEnded = false;
};

struct FFrame
{
	nvrhi::CommandListHandle Commands;
	std::uint64_t Serial = 0;
	std::size_t UploadBytes = 0;
	std::size_t UniformBytes = 0;
	// Draws in a recording mostly share targets and material textures, so their framebuffers and descriptor sets are created once per recording.
	std::map<std::array<nvrhi::ITexture*, 3>, nvrhi::FramebufferHandle> Framebuffers;
	// Keys hold the layout, each bound resource, and the mip level of each storage binding.
	std::map<std::vector<std::uintptr_t>, nvrhi::BindingSetHandle> BindingSets;
	std::array<FPassTiming, MaximumGpuPassTimings> Timings;
	std::size_t TimingCount = 0;
	std::optional<std::size_t> ActiveTiming;
	std::size_t SkippedTimingDepth = 0;
	// NVRHI cannot abandon an open command list. Delay emission until the graph has validated.
	std::vector<std::function<void(nvrhi::ICommandList*)>> Operations;
	std::unordered_map<FBuffer*, std::uint32_t> UploadedBuffers;
	std::vector<FBuffer*> CopiedBuffers;
	std::unordered_map<FTexture*, std::uint32_t> WrittenTextures;
	std::vector<FAccelerationStructure*> BuiltStructures;
	// NVRHI only revisits resource states when a dispatch's bindings differ from the previous one, so a repeat needs its UAV barriers placed explicitly.
	nvrhi::IBindingSet* PreviousComputeBindings = nullptr;
	// Aliased textures this recording has used, so only the first use discards.
	std::unordered_set<FTexture*> UsedAliases;

	void Reset()
	{
		Operations.clear();
		UsedAliases.clear();
		BuiltStructures.clear();
		PreviousComputeBindings = nullptr;
		UploadedBuffers.clear();
		CopiedBuffers.clear();
		WrittenTextures.clear();
		Framebuffers.clear();
		BindingSets.clear();
		UploadBytes = 0;
		UniformBytes = 0;
		TimingCount = 0;
		ActiveTiming.reset();
		SkippedTimingDepth = 0;
		Serial = 0;
	}
};

class FGraphicsDevice final : public IGraphicsDevice
{
public:
	FGraphicsDevice(nvrhi::IDevice* const InDevice, nvrhi::vulkan::IDevice* const InVulkanDevice, const VkDevice InNativeDevice)
	    : Device(InDevice)
	    , VulkanDevice(InVulkanDevice)
	    , NativeDevice(InNativeDevice)
	{
	}

	~FGraphicsDevice() override
	{
		CancelCommands();
		try
		{
			if (const auto Idle = WaitForIdle(); !Idle)
			{
				Device->getMessageCallback()->message(nvrhi::MessageSeverity::Error, Idle.error().Message.c_str());
			}
		}
		catch (...) // NOLINT(bugprone-empty-catch)
		{
			// The owner must still destroy the native device after a backend failure.
		}
	}

	FGraphicsDevice(const FGraphicsDevice&) = delete;
	FGraphicsDevice& operator=(const FGraphicsDevice&) = delete;
	FGraphicsDevice(FGraphicsDevice&&) = delete;
	FGraphicsDevice& operator=(FGraphicsDevice&&) = delete;

	[[nodiscard]] std::expected<void, FPresentationError> Initialize()
	{
		const VkPhysicalDevice PhysicalDevice = VulkanDevice->getNativeObject(nvrhi::ObjectTypes::VK_PhysicalDevice);
		VkPhysicalDeviceProperties Properties{};
		vkGetPhysicalDeviceProperties(PhysicalDevice, &Properties);
		TextureDimensionLimit = std::min(MaximumTextureDimension, Properties.limits.maxImageDimension2D);
		if (Device->queryFeatureSupport(nvrhi::Feature::RayQuery) && Device->queryFeatureSupport(nvrhi::Feature::RayTracingAccelStruct))
		{
			nvrhi::BindlessLayoutDesc Table;
			Table.setVisibility(nvrhi::ShaderType::Compute).setMaxCapacity(MaximumResourceTableSlots).addRegisterSpace(nvrhi::BindingLayoutItem::Texture_SRV(0)).addRegisterSpace(nvrhi::BindingLayoutItem::RawBuffer_SRV(1));
			ResourceTableLayout = Device->createBindlessLayout(Table);
			if (!ResourceTableLayout)
			{
				return Failed("Could not create the resource table layout");
			}
		}

		for (FFrame& Frame : Frames)
		{
			Frame.Commands = Device->createCommandList(nvrhi::CommandListParameters().setUploadChunkSize(std::size_t{1024} * 1024));
			if (!Frame.Commands)
			{
				return Failed("Could not create a graphics frame command list");
			}
		}

		return {};
	}

	[[nodiscard]] std::expected<FBufferHandle, FPresentationError> CreateBuffer(const FBufferDescriptor& Descriptor) override
	{
		if (Descriptor.Usage == EBufferUsage::Readback)
		{
			if (Descriptor.Size == 0 || Descriptor.Size > MaximumUploadBytes || Descriptor.Size % sizeof(std::uint32_t) != 0)
			{
				return Invalid("Readback buffer size must hold whole uints and fit 64 MiB");
			}

			auto Buffer = std::make_shared<FBuffer>();
			Buffer->Descriptor = Descriptor;
			Buffer->Owner = Owner;
			Buffer->Handle = Device->createBuffer(nvrhi::BufferDesc().setByteSize(Descriptor.Size).setDebugName(Descriptor.Name).setCpuAccess(nvrhi::CpuAccessMode::Read).enableAutomaticStateTracking(nvrhi::ResourceStates::CopyDest));
			if (!Buffer->Handle)
			{
				return Failed("Could not create readback buffer '" + Descriptor.Name + "'");
			}

			return Buffer;
		}

		if (Descriptor.Usage == EBufferUsage::Storage)
		{
			const bool bInstances = Descriptor.VertexFormat == EGraphicsVertexFormat::MeshInstance || Descriptor.VertexFormat == EGraphicsVertexFormat::MeshInstanceMotion;
			const std::size_t Element = Descriptor.VertexFormat == EGraphicsVertexFormat::MeshInstance ? sizeof(FMeshInstance) : bInstances ? sizeof(FMeshInstanceMotion)
			                                                                                                                                : sizeof(std::uint32_t);
			if (Descriptor.Size == 0 || Descriptor.Size > MaximumStorageBufferBytes || Descriptor.Size % Element != 0)
			{
				return Invalid("Storage buffer size must hold whole uints, or whole mesh instances for instance buffers, and fit 256 MiB");
			}

			auto Buffer = std::make_shared<FBuffer>();
			Buffer->Descriptor = Descriptor;
			Buffer->Owner = Owner;
			Buffer->Handle = Device->createBuffer(nvrhi::BufferDesc().setByteSize(Descriptor.Size).setDebugName(Descriptor.Name).setStructStride(sizeof(std::uint32_t)).setCanHaveUAVs(true).setIsDrawIndirectArgs(true).setIsVertexBuffer(bInstances).enableAutomaticStateTracking(nvrhi::ResourceStates::UnorderedAccess));
			if (!Buffer->Handle)
			{
				return Failed("Could not create storage buffer '" + Descriptor.Name + "'");
			}

			return Buffer;
		}

		const bool bVertex = Descriptor.Usage == EBufferUsage::Vertex;
		if (Descriptor.bRayTracing && (!ResourceTableLayout || (bVertex && Descriptor.VertexFormat != EGraphicsVertexFormat::Mesh)))
		{
			return Invalid("Ray tracing buffers need ray query support and hold mesh vertices or indices");
		}

		const std::size_t VertexStride = Descriptor.VertexFormat == EGraphicsVertexFormat::Mesh ? sizeof(FMeshVertex) : Descriptor.VertexFormat == EGraphicsVertexFormat::MeshInstance ? sizeof(FMeshInstance)
		                                                                                                                                                                               : sizeof(FColoredClipVertex);
		const std::size_t Stride = bVertex ? VertexStride : sizeof(std::uint32_t);
		if ((!bVertex && Descriptor.Usage != EBufferUsage::Index) || (Descriptor.VertexFormat != EGraphicsVertexFormat::Mesh && Descriptor.VertexFormat != EGraphicsVertexFormat::ColoredClipPosition && Descriptor.VertexFormat != EGraphicsVertexFormat::MeshInstance) || Descriptor.Size == 0 || Descriptor.Size > MaximumUploadBytes || Descriptor.Size % Stride != 0)
		{
			return Invalid("Mesh buffer size must contain complete vertices or uint32 indices and fit the 64 MiB upload budget");
		}

		auto Buffer = std::make_shared<FBuffer>();
		Buffer->Descriptor = Descriptor;
		Buffer->Owner = Owner;
		Buffer->Handle = Device->createBuffer(nvrhi::BufferDesc().setByteSize(Descriptor.Size).setDebugName(Descriptor.Name).setIsVertexBuffer(bVertex).setIsIndexBuffer(!bVertex).setIsAccelStructBuildInput(Descriptor.bRayTracing).setCanHaveRawViews(Descriptor.bRayTracing).enableAutomaticStateTracking(bVertex ? nvrhi::ResourceStates::VertexBuffer : nvrhi::ResourceStates::IndexBuffer));
		if (!Buffer->Handle)
		{
			return Failed("Could not create mesh buffer '" + Descriptor.Name + "'");
		}

		return Buffer;
	}

	[[nodiscard]] std::expected<FTextureHandle, FPresentationError> CreateTexture(const FTextureDescriptor& Descriptor) override
	{
		auto Texture = MakeTexture(Descriptor, false);
		if (!Texture)
		{
			return std::unexpected(Texture.error());
		}

		return *Texture;
	}

	[[nodiscard]] std::expected<std::vector<FTextureHandle>, FPresentationError> CreateAliasedTextures(const std::span<const std::span<const FTextureDescriptor>> Phases) override
	{
		std::vector<std::pair<std::shared_ptr<FTexture>, std::uint64_t>> Placed;
		std::uint64_t Capacity = 0;
		for (const std::span<const FTextureDescriptor> Phase : Phases)
		{
			std::uint64_t Offset = 0;
			for (const FTextureDescriptor& Descriptor : Phase)
			{
				if (IsDepthFormat(Descriptor.Format))
				{
					return Invalid("Aliased textures are color textures");
				}

				auto Texture = MakeTexture(Descriptor, true);
				if (!Texture)
				{
					return std::unexpected(Texture.error());
				}

				const nvrhi::MemoryRequirements Requirements = Device->getTextureMemoryRequirements((*Texture)->Handle);
				const std::uint64_t Alignment = std::max<std::uint64_t>(Requirements.alignment, 1);
				Offset = (Offset + Alignment - 1) / Alignment * Alignment;
				Placed.emplace_back(*Texture, Offset);
				Offset += Requirements.size;
			}

			Capacity = std::max(Capacity, Offset);
		}

		if (Placed.empty())
		{
			return Invalid("Aliased textures need at least one texture");
		}

		const nvrhi::HeapHandle Heap = Device->createHeap(nvrhi::HeapDesc().setCapacity(Capacity).setType(nvrhi::HeapType::DeviceLocal).setDebugName("Aliased textures"));
		if (!Heap)
		{
			return Failed("Could not allocate aliased texture memory");
		}

		std::vector<FTextureHandle> Textures;
		for (auto& [Texture, Offset] : Placed)
		{
			if (!Device->bindTextureMemory(Texture->Handle, Heap, Offset))
			{
				return Failed("Could not place aliased texture '" + Texture->Descriptor.Name + "'");
			}

			Texture->Heap = Heap;
			Textures.push_back(std::move(Texture));
		}

		return Textures;
	}

	[[nodiscard]] std::expected<std::shared_ptr<FTexture>, FPresentationError> MakeTexture(const FTextureDescriptor& Descriptor, const bool bVirtual)
	{
		const nvrhi::Format Format = ToFormat(Descriptor.Format);
		const bool bDepth = IsDepthFormat(Descriptor.Format);
		if (Descriptor.Extent.IsEmpty() || Descriptor.Extent.Width > TextureDimensionLimit || Descriptor.Extent.Height > TextureDimensionLimit || Format == nvrhi::Format::UNKNOWN || (bDepth && (!Descriptor.bRenderTarget || Descriptor.bStorage)))
		{
			return Invalid(std::format("Texture requires a supported format and dimensions in [1, {}]; depth textures must be render targets without storage", TextureDimensionLimit));
		}

		if (Descriptor.MipLevels == 0 || Descriptor.MipLevels > GetMipLevelCount(Descriptor.Extent) || (Descriptor.bRenderTarget && Descriptor.MipLevels != 1))
		{
			return Invalid("Texture mip levels must fit the extent, and render targets have exactly one level");
		}

		// Integer textures are read with Load, which needs no filtering support.
		const nvrhi::FormatSupport Required = nvrhi::FormatSupport::Texture | (Descriptor.Format == ETextureFormat::R32Uint ? nvrhi::FormatSupport::ShaderLoad : nvrhi::FormatSupport::ShaderSample) | (bDepth ? nvrhi::FormatSupport::DepthStencil : Descriptor.bRenderTarget ? nvrhi::FormatSupport::RenderTarget
		                                                                                                                                                                                                                                                                       : nvrhi::FormatSupport::None)
		                                      | (Descriptor.bStorage ? nvrhi::FormatSupport::ShaderUavStore : nvrhi::FormatSupport::None);
		if ((Device->queryFormatSupport(Format) & Required) != Required)
		{
			return Invalid("Texture format does not support the requested usage on this device");
		}

		auto Texture = std::make_shared<FTexture>();
		Texture->Descriptor = Descriptor;
		Texture->Owner = Owner;
		Texture->DeviceIdentity = Device;
		nvrhi::TextureDesc Native;
		Native.setWidth(Descriptor.Extent.Width).setHeight(Descriptor.Extent.Height).setMipLevels(Descriptor.MipLevels).setFormat(Format).setDebugName(Descriptor.Name).setIsRenderTarget(Descriptor.bRenderTarget).setIsTypeless(Descriptor.Format == ETextureFormat::Rgba8Srgb && Descriptor.bRenderTarget).enableAutomaticStateTracking(bDepth ? nvrhi::ResourceStates::DepthWrite : nvrhi::ResourceStates::ShaderResource);
		Native.isShaderResource = true;
		Native.isUAV = Descriptor.bStorage;
		Native.isVirtual = bVirtual;
		Texture->Handle = Device->createTexture(Native);
		if (!Texture->Handle)
		{
			return Failed("Could not create texture '" + Descriptor.Name + "'");
		}

		return Texture;
	}

	[[nodiscard]] std::expected<FGraphicsPipelineHandle, FPresentationError> CreateGraphicsPipeline(const FGraphicsPipelineDescriptor& Descriptor) override
	{
		const bool bColored = Descriptor.VertexFormat == EGraphicsVertexFormat::ColoredClipPosition;
		const bool bShadow = Descriptor.VertexFormat == EGraphicsVertexFormat::ShadowMesh;
		const bool bFragment = !Descriptor.FragmentShader.Bytecode.empty();
		if ((!bColored && !bShadow && Descriptor.VertexFormat != EGraphicsVertexFormat::Mesh) || (bShadow && !Descriptor.bDepthOnly) || (bColored && Descriptor.bInstanced) || (Descriptor.bDepthOnly && !Descriptor.bDepthTest && !Descriptor.bDepthWrite) || Descriptor.VertexShader.Stage != EShaderStage::Vertex || !SerializeCookedShader(Descriptor.VertexShader) || (!Descriptor.bDepthOnly && !bFragment) || (bFragment && (Descriptor.FragmentShader.Stage != EShaderStage::Fragment || !SerializeCookedShader(Descriptor.FragmentShader))) || (!Descriptor.bDepthOnly && (ToFormat(Descriptor.ColorFormat) == nvrhi::Format::UNKNOWN || IsDepthFormat(Descriptor.ColorFormat))) || (Descriptor.SecondColorFormat && (Descriptor.bDepthOnly || ToFormat(*Descriptor.SecondColorFormat) == nvrhi::Format::UNKNOWN || IsDepthFormat(*Descriptor.SecondColorFormat))) || !IsDepthFormat(Descriptor.DepthFormat) || Descriptor.TextureCount > MaximumGraphicsTextures || Descriptor.BufferCount > MaximumReadBuffers || Descriptor.UniformBufferSize > MaximumGraphicsUniformBytes || Descriptor.UniformBufferSize % 16 != 0 || (Descriptor.CullMode != EGraphicsCullMode::None && Descriptor.CullMode != EGraphicsCullMode::Back && Descriptor.CullMode != EGraphicsCullMode::Front))
		{
			return Invalid("Graphics pipeline requires supported shaders, color format, binding counts, and aligned uniforms");
		}

		const auto HasSupportedBindings = [&Descriptor](const FShaderAsset& Shader)
		{
			return (Shader.PushConstantSize == 0 || Shader.PushConstantSize == MeshPushConstantSize) && std::ranges::all_of(Shader.Bindings, [&Descriptor](const FShaderBinding& Binding)
			{
				return Binding.Space == 0 && ((Binding.Type == EShaderBindingType::Texture && Binding.Binding < Descriptor.TextureCount) || (Binding.Type == EShaderBindingType::Buffer && Binding.Binding >= ReadBufferBinding && Binding.Binding < ReadBufferBinding + Descriptor.BufferCount) || (Binding.Type == EShaderBindingType::Sampler && Binding.Binding == 128 && Descriptor.TextureCount != 0) || (Binding.Type == EShaderBindingType::ConstantBuffer && Binding.Binding == 64 && Binding.ByteSize <= Descriptor.UniformBufferSize));
			});
		};

		// Slang reflects the module-global transform block even when the instanced entry point does not read it.
		if (!HasSupportedBindings(Descriptor.VertexShader) || (bFragment && !HasSupportedBindings(Descriptor.FragmentShader)) || (bColored && (Descriptor.VertexShader.PushConstantSize != 0 || Descriptor.FragmentShader.PushConstantSize != 0)))
		{
			return Invalid("Graphics shaders do not match the selected vertex format and binding ABI");
		}

		const auto Vertex = Device->createShader(nvrhi::ShaderDesc().setShaderType(nvrhi::ShaderType::Vertex).setEntryName(Descriptor.VertexShader.EntryPoint).setDebugName(Descriptor.Name + ".Vertex"), Descriptor.VertexShader.Bytecode.data(), Descriptor.VertexShader.Bytecode.size() * sizeof(std::uint32_t));
		const nvrhi::ShaderHandle Fragment = bFragment ? Device->createShader(nvrhi::ShaderDesc().setShaderType(nvrhi::ShaderType::Pixel).setEntryName(Descriptor.FragmentShader.EntryPoint).setDebugName(Descriptor.Name + ".Fragment"), Descriptor.FragmentShader.Bytecode.data(), Descriptor.FragmentShader.Bytecode.size() * sizeof(std::uint32_t)) : nullptr;
		if (!Vertex || (bFragment && !Fragment))
		{
			return Failed("Could not create graphics shaders");
		}

		const std::array Attributes{
		    nvrhi::VertexAttributeDesc().setName("POSITION").setFormat(nvrhi::Format::RGB32_FLOAT).setOffset(offsetof(FMeshVertex, Position)).setElementStride(sizeof(FMeshVertex)),
		    nvrhi::VertexAttributeDesc().setName("TEXCOORD").setFormat(nvrhi::Format::RG32_FLOAT).setOffset(offsetof(FMeshVertex, UV)).setElementStride(sizeof(FMeshVertex)),
		    nvrhi::VertexAttributeDesc().setName("NORMAL").setFormat(nvrhi::Format::RGB32_FLOAT).setOffset(offsetof(FMeshVertex, Normal)).setElementStride(sizeof(FMeshVertex)),
		    nvrhi::VertexAttributeDesc().setName("TANGENT").setFormat(nvrhi::Format::RGBA32_FLOAT).setOffset(offsetof(FMeshVertex, Tangent)).setElementStride(sizeof(FMeshVertex)),
		};

		const std::array ColoredAttributes{
		    nvrhi::VertexAttributeDesc().setName("POSITION").setFormat(nvrhi::Format::RGBA32_FLOAT).setOffset(offsetof(FColoredClipVertex, Position)).setElementStride(sizeof(FColoredClipVertex)),
		    nvrhi::VertexAttributeDesc().setName("COLOR").setFormat(nvrhi::Format::RGBA32_FLOAT).setOffset(offsetof(FColoredClipVertex, Color)).setElementStride(sizeof(FColoredClipVertex)),
		};

		// Four vertex attributes and two instance matrices, plus the previous-frame matrix from a second stream for motion, use all 16 locations Vulkan guarantees.
		std::array<nvrhi::VertexAttributeDesc, 16> InstancedAttributes;
		std::ranges::copy(Attributes, InstancedAttributes.begin());
		for (std::uint32_t Column = 0; Column < 8; ++Column)
		{
			InstancedAttributes[Column + 4] = nvrhi::VertexAttributeDesc().setName("TEXCOORD").setFormat(nvrhi::Format::RGBA32_FLOAT).setBufferIndex(1).setOffset(Column * 16u).setElementStride(sizeof(FMeshInstance)).setIsInstanced(true);
		}

		for (std::uint32_t Column = 0; Column < 4; ++Column)
		{
			InstancedAttributes[Column + 12] = nvrhi::VertexAttributeDesc().setName("TEXCOORD").setFormat(nvrhi::Format::RGBA32_FLOAT).setBufferIndex(2).setOffset(Column * 16u).setElementStride(sizeof(FMeshInstanceMotion)).setIsInstanced(true);
		}

		std::array<nvrhi::VertexAttributeDesc, 6> ShadowAttributes;
		std::ranges::copy(std::span(Attributes).first(2), ShadowAttributes.begin());
		for (std::uint32_t Column = 0; Column < 4; ++Column)
		{
			ShadowAttributes[Column + 2] = nvrhi::VertexAttributeDesc().setName("TEXCOORD").setFormat(nvrhi::Format::RGBA32_FLOAT).setBufferIndex(1).setOffset(static_cast<std::uint32_t>(offsetof(FMeshInstance, ObjectToView)) + Column * 16u).setElementStride(sizeof(FMeshInstance)).setIsInstanced(true);
		}

		const std::span<const nvrhi::VertexAttributeDesc> SelectedAttributes = bColored ? std::span<const nvrhi::VertexAttributeDesc>(ColoredAttributes) : bShadow             ? std::span<const nvrhi::VertexAttributeDesc>(ShadowAttributes).first(Descriptor.bInstanced ? 6 : 2)
		                                                                                                                                               : Descriptor.bInstanced ? std::span<const nvrhi::VertexAttributeDesc>(InstancedAttributes).first(Descriptor.bInstanceMotion ? 16 : 12)
		                                                                                                                                                                       : std::span<const nvrhi::VertexAttributeDesc>(Attributes);
		const auto InputLayout = Device->createInputLayout(SelectedAttributes.data(), static_cast<std::uint32_t>(SelectedAttributes.size()), Vertex);
		auto Pipeline = std::make_shared<FPipeline>();
		Pipeline->Name = Descriptor.Name;
		Pipeline->Owner = Owner;
		Pipeline->ColorFormat = Descriptor.ColorFormat;
		Pipeline->SecondColorFormat = Descriptor.SecondColorFormat;
		Pipeline->VertexFormat = Descriptor.VertexFormat;
		Pipeline->bInstanced = Descriptor.bInstanced;
		Pipeline->bInstanceMotion = Descriptor.bInstanced && Descriptor.bInstanceMotion;
		Pipeline->bDepthOnly = Descriptor.bDepthOnly;
		Pipeline->bDepthAttachment = Descriptor.bDepthTest || Descriptor.bDepthWrite;
		Pipeline->bPushConstants = Descriptor.VertexShader.PushConstantSize != 0 || Descriptor.FragmentShader.PushConstantSize != 0;
		const bool bUnboundColored = bColored && Descriptor.VertexShader.Bindings.empty() && Descriptor.FragmentShader.Bindings.empty();
		Pipeline->TextureCount = bUnboundColored ? 0 : Descriptor.TextureCount;
		Pipeline->BufferCount = Descriptor.BufferCount;
		Pipeline->UniformBufferSize = Descriptor.UniformBufferSize;
		nvrhi::BindingLayoutDesc Layout;
		Layout.setVisibility(nvrhi::ShaderType::AllGraphics).setBindingOffsets(nvrhi::VulkanBindingOffsets().setConstantBufferOffset(64));

		for (std::uint32_t Slot = 0; Slot < Pipeline->TextureCount; ++Slot)
		{
			Layout.addItem(nvrhi::BindingLayoutItem::Texture_SRV(Slot));
		}

		for (std::uint32_t Slot = 0; Slot < Pipeline->BufferCount; ++Slot)
		{
			Layout.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(ReadBufferBinding + Slot));
		}

		if (Pipeline->TextureCount != 0)
		{
			Layout.addItem(nvrhi::BindingLayoutItem::Sampler(0));
			Pipeline->Sampler = Device->createSampler(nvrhi::SamplerDesc().setAllAddressModes(Descriptor.bClampSampler ? nvrhi::SamplerAddressMode::Clamp : nvrhi::SamplerAddressMode::Wrap));
		}

		if (Pipeline->UniformBufferSize != 0)
		{
			Layout.addItem(nvrhi::BindingLayoutItem::VolatileConstantBuffer(0));
		}

		if (Pipeline->bPushConstants)
		{
			// NVRHI shares the constant-buffer register namespace with push constants.
			Layout.addItem(nvrhi::BindingLayoutItem::PushConstants(1, MeshPushConstantSize));
		}

		if (!Layout.bindings.empty())
		{
			Pipeline->Layout = Device->createBindingLayout(Layout);
		}

		if (!InputLayout || (!Layout.bindings.empty() && !Pipeline->Layout) || (Pipeline->TextureCount != 0 && !Pipeline->Sampler))
		{
			return Failed("Could not create graphics pipeline layouts or sampler");
		}

		nvrhi::RenderState RenderState;
		RenderState.depthStencilState.setDepthTestEnable(Descriptor.bDepthTest).setDepthWriteEnable(Descriptor.bDepthWrite && !bColored).setDepthFunc(Descriptor.bDepthOverwrite ? nvrhi::ComparisonFunc::Always : nvrhi::ComparisonFunc::GreaterOrEqual);
		const nvrhi::RasterCullMode CullMode = bColored || Descriptor.CullMode == EGraphicsCullMode::None ? nvrhi::RasterCullMode::None : Descriptor.CullMode == EGraphicsCullMode::Front ? nvrhi::RasterCullMode::Front
		                                                                                                                                                                                  : nvrhi::RasterCullMode::Back;
		RenderState.rasterState.setFrontCounterClockwise(true).setCullMode(CullMode).setDepthClipEnable(true).setScissorEnable(true);
		if (Descriptor.bAlphaBlend)
		{
			RenderState.blendState.targets[0].setBlendEnable(true).setSrcBlend(nvrhi::BlendFactor::SrcAlpha).setDestBlend(nvrhi::BlendFactor::InvSrcAlpha).setSrcBlendAlpha(nvrhi::BlendFactor::One).setDestBlendAlpha(nvrhi::BlendFactor::InvSrcAlpha);
		}

		nvrhi::GraphicsPipelineDesc NativeDescriptor;
		NativeDescriptor.setVertexShader(Vertex).setFragmentShader(Fragment).setInputLayout(InputLayout).setRenderState(RenderState);
		if (Pipeline->Layout)
		{
			NativeDescriptor.addBindingLayout(Pipeline->Layout);
		}

		nvrhi::FramebufferInfo FramebufferInfo;
		FramebufferInfo.setDepthFormat(Descriptor.bDepthTest || Descriptor.bDepthWrite ? ToFormat(Descriptor.DepthFormat) : nvrhi::Format::UNKNOWN);

		if (!Descriptor.bDepthOnly)
		{
			FramebufferInfo.addColorFormat(ToFormat(Descriptor.ColorFormat));
		}

		if (Descriptor.SecondColorFormat)
		{
			FramebufferInfo.addColorFormat(ToFormat(*Descriptor.SecondColorFormat));
		}

		Pipeline->Handle = Device->createGraphicsPipeline(NativeDescriptor, FramebufferInfo);
		if (!Pipeline->Handle)
		{
			return Failed("Could not create graphics pipeline '" + Descriptor.Name + "'");
		}

		return Pipeline;
	}

	[[nodiscard]] std::expected<FComputePipelineHandle, FPresentationError> CreateComputePipeline(const FComputePipelineDescriptor& Descriptor) override
	{
		const FShaderAsset& Shader = Descriptor.Shader;
		const bool bValidBindings = std::ranges::all_of(Shader.Bindings, [&Descriptor](const FShaderBinding& Binding)
		{
			switch (Binding.Type)
			{
				case EShaderBindingType::Texture:
					return Binding.Binding < Descriptor.TextureCount;
				case EShaderBindingType::Sampler:
					return Binding.Binding == 128;
				case EShaderBindingType::ConstantBuffer:
					return Binding.Binding == 64 && Binding.ByteSize <= Descriptor.UniformBufferSize;
				case EShaderBindingType::StorageTexture:
					return Binding.Binding >= StorageBinding && Binding.Binding < StorageBinding + Descriptor.StorageTextureCount;
				case EShaderBindingType::StorageBuffer:
					return Binding.Binding >= StorageBinding + Descriptor.StorageTextureCount && Binding.Binding < StorageBinding + Descriptor.StorageTextureCount + Descriptor.StorageBufferCount;
				case EShaderBindingType::Buffer:
					return Binding.Binding >= ReadBufferBinding && Binding.Binding < ReadBufferBinding + Descriptor.BufferCount;
				case EShaderBindingType::AccelerationStructure:
					return Descriptor.bRayQuery && Binding.Binding == AccelerationStructureBinding;
				case EShaderBindingType::TableTextures:
				case EShaderBindingType::TableBuffers:
					return Descriptor.bResourceTable;
			}

			return false;
		});

		if ((Descriptor.bRayQuery || Descriptor.bResourceTable) && !ResourceTableLayout)
		{
			return Invalid("Compute pipeline '" + Descriptor.Name + "' needs ray query support");
		}

		if (Shader.Stage != EShaderStage::Compute || !SerializeCookedShader(Shader) || Shader.PushConstantSize != 0 || !bValidBindings || Descriptor.TextureCount > MaximumGraphicsTextures || Descriptor.BufferCount > MaximumReadBuffers || Descriptor.StorageTextureCount + Descriptor.StorageBufferCount > MaximumStorageBindings || Descriptor.UniformBufferSize > MaximumGraphicsUniformBytes || Descriptor.UniformBufferSize % 16 != 0)
		{
			return Invalid("Compute pipeline '" + Descriptor.Name + "' requires a compute shader whose bindings fit its texture, storage, and aligned uniform counts");
		}

		auto Pipeline = std::make_shared<FComputePipeline>();
		Pipeline->Name = Descriptor.Name;
		Pipeline->Owner = Owner;
		Pipeline->TextureCount = Descriptor.TextureCount;
		Pipeline->StorageTextureCount = Descriptor.StorageTextureCount;
		Pipeline->StorageBufferCount = Descriptor.StorageBufferCount;
		Pipeline->BufferCount = Descriptor.BufferCount;
		Pipeline->UniformBufferSize = Descriptor.UniformBufferSize;
		Pipeline->bRayQuery = Descriptor.bRayQuery;
		Pipeline->bResourceTable = Descriptor.bResourceTable;
		const auto Module = Device->createShader(nvrhi::ShaderDesc().setShaderType(nvrhi::ShaderType::Compute).setEntryName(Shader.EntryPoint).setDebugName(Descriptor.Name), Shader.Bytecode.data(), Shader.Bytecode.size() * sizeof(std::uint32_t));
		nvrhi::BindingLayoutDesc Layout;
		Layout.setVisibility(nvrhi::ShaderType::Compute).setBindingOffsets(nvrhi::VulkanBindingOffsets().setConstantBufferOffset(64).setUnorderedAccessViewOffset(StorageBinding));
		for (std::uint32_t Slot = 0; Slot < Descriptor.TextureCount; ++Slot)
		{
			Layout.addItem(nvrhi::BindingLayoutItem::Texture_SRV(Slot));
		}

		for (std::uint32_t Slot = 0; Slot < Descriptor.BufferCount; ++Slot)
		{
			Layout.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(ReadBufferBinding + Slot));
		}

		// Shared shader headers can declare the sampler in shaders that sample nothing.
		const bool bSampler = Descriptor.TextureCount != 0 || std::ranges::any_of(Shader.Bindings, [](const FShaderBinding& Binding)
		{
			return Binding.Type == EShaderBindingType::Sampler;
		});

		if (bSampler)
		{
			Layout.addItem(nvrhi::BindingLayoutItem::Sampler(0));
			Pipeline->Sampler = Device->createSampler(nvrhi::SamplerDesc().setAllFilters(Descriptor.bLinearSampler).setAllAddressModes(Descriptor.bBorderSampler ? nvrhi::SamplerAddressMode::Border : Descriptor.bRepeatSampler ? nvrhi::SamplerAddressMode::Wrap : nvrhi::SamplerAddressMode::Clamp).setBorderColor(nvrhi::Color(0.f)));
		}

		if (Descriptor.UniformBufferSize != 0)
		{
			Layout.addItem(nvrhi::BindingLayoutItem::VolatileConstantBuffer(0));
		}

		for (std::uint32_t Slot = 0; Slot < Descriptor.StorageTextureCount; ++Slot)
		{
			Layout.addItem(nvrhi::BindingLayoutItem::Texture_UAV(Slot));
		}

		for (std::uint32_t Slot = 0; Slot < Descriptor.StorageBufferCount; ++Slot)
		{
			Layout.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(Descriptor.StorageTextureCount + Slot));
		}

		if (Descriptor.bRayQuery)
		{
			Layout.addItem(nvrhi::BindingLayoutItem::RayTracingAccelStruct(AccelerationStructureBinding));
		}

		// NVRHI numbers descriptor sets by layout order, so the table only lands in set 1 behind a non-empty set 0.
		if (Descriptor.bResourceTable && Layout.bindings.empty())
		{
			return Invalid("Compute pipeline '" + Descriptor.Name + "' reads a resource table, so it needs at least one other binding");
		}

		if (!Layout.bindings.empty())
		{
			Pipeline->Layout = Device->createBindingLayout(Layout);
		}

		if (!Module || (!Layout.bindings.empty() && !Pipeline->Layout) || (bSampler && !Pipeline->Sampler))
		{
			return Failed("Could not create compute shader, layout, or sampler for '" + Descriptor.Name + "'");
		}

		nvrhi::ComputePipelineDesc NativeDescriptor;
		NativeDescriptor.setComputeShader(Module);
		if (Pipeline->Layout)
		{
			NativeDescriptor.addBindingLayout(Pipeline->Layout);
		}

		if (Descriptor.bResourceTable)
		{
			NativeDescriptor.addBindingLayout(ResourceTableLayout);
		}

		Pipeline->Handle = Device->createComputePipeline(NativeDescriptor);
		if (!Pipeline->Handle)
		{
			return Failed("Could not create compute pipeline '" + Descriptor.Name + "'");
		}

		return Pipeline;
	}

	[[nodiscard]] std::expected<void, FPresentationError> BeginCommands() override
	{
		if (bRecording)
		{
			return Invalid("A graphics frame is already being recorded");
		}

		if (const auto Wait = WaitForSerial(Frames[FrameIndex].Serial); !Wait)
		{
			return Wait;
		}

		if (const auto Retired = RetireCompleted(); !Retired)
		{
			return Retired;
		}

		Frames[FrameIndex].Reset();
		bRecording = true;
		return {};
	}

	[[nodiscard]] std::expected<void, FPresentationError> WriteBuffer(const FBufferHandle& Buffer, const std::span<const std::byte> Data) override
	{
		const auto Native = std::dynamic_pointer_cast<FBuffer>(Buffer);
		// Instance buffers keep spare capacity, and their draws name the instance range they read.
		// Storage buffers may reset just a header, such as a counter, ahead of the elements the GPU fills.
		const bool bInstances = Owns(Native) && Native->Descriptor.Usage != EBufferUsage::Index && (Native->Descriptor.VertexFormat == EGraphicsVertexFormat::MeshInstance || Native->Descriptor.VertexFormat == EGraphicsVertexFormat::MeshInstanceMotion);
		const bool bPartial = Owns(Native) && (bInstances || Native->Descriptor.Usage == EBufferUsage::Storage);
		if (!Owns(Native) || Native->Descriptor.Usage == EBufferUsage::Readback || (bPartial ? Data.empty() || Data.size() > Native->Descriptor.Size || Data.size() % sizeof(std::uint32_t) != 0 : Data.size() != Native->Descriptor.Size))
		{
			return Invalid("Buffer upload must fully initialize a buffer belonging to this device");
		}

		if (const auto Ready = CanRecord(Data.size()); !Ready)
		{
			return Ready;
		}

		if (bInstances)
		{
			// A float is NaN or infinite exactly when its exponent bits are all set. Testing the bits without branching lets the loop vectorize over
			// the megabytes of instances a large scene uploads each frame.
			std::uint32_t NonFinite = 0;
			for (std::size_t Offset = 0; Offset < Data.size(); Offset += sizeof(std::uint32_t))
			{
				std::uint32_t Bits;
				std::memcpy(&Bits, Data.data() + Offset, sizeof(Bits));
				NonFinite |= static_cast<std::uint32_t>((Bits & 0x7F800000u) == 0x7F800000u);
			}

			if (NonFinite != 0)
			{
				return Invalid("Mesh instance transforms must be finite");
			}
		}

		std::uint32_t MaximumIndex = 0;
		if (Native->Descriptor.Usage == EBufferUsage::Index)
		{
			for (std::size_t Offset = 0; Offset < Data.size(); Offset += sizeof(std::uint32_t))
			{
				std::uint32_t Index;
				std::memcpy(&Index, Data.data() + Offset, sizeof(Index));
				MaximumIndex = std::max(MaximumIndex, Index);
			}
		}

		FFrame& Frame = Frames[FrameIndex];
		Frame.Operations.emplace_back([Native, Bytes = std::vector<std::byte>(Data.begin(), Data.end())](nvrhi::ICommandList* const Commands)
		{
			Commands->beginMarker("Upload mesh buffer");
			Commands->writeBuffer(Native->Handle, Bytes.data(), Bytes.size());
			Commands->endMarker();
		});

		Frame.UploadBytes += Data.size();
		Frame.UploadedBuffers[Native.get()] = MaximumIndex;
		return {};
	}

	[[nodiscard]] std::expected<void, FPresentationError> WriteTexture(const FTextureHandle& Texture, const std::uint32_t MipLevel, const std::span<const std::byte> RgbaPixels) override
	{
		const auto Native = std::dynamic_pointer_cast<FTexture>(Texture);
		const FExtent2D Extent = Native ? GetMipExtent(Native->Descriptor.Extent, MipLevel) : FExtent2D{};
		if (!Owns(Native) || IsDepthFormat(Native->Descriptor.Format) || MipLevel >= Native->Descriptor.MipLevels || RgbaPixels.size() != static_cast<std::size_t>(Extent.Width) * Extent.Height * GetTextureTexelBytes(Native->Descriptor.Format))
		{
			return Invalid("Texture upload must provide every texel in the texture format of one color mip level belonging to this device");
		}

		if (const auto Ready = CanRecord(RgbaPixels.size()); !Ready)
		{
			return Ready;
		}

		FFrame& Frame = Frames[FrameIndex];
		Frame.Operations.emplace_back([Native, MipLevel, RowPitch = static_cast<std::size_t>(Extent.Width) * GetTextureTexelBytes(Native->Descriptor.Format), Bytes = std::vector<std::byte>(RgbaPixels.begin(), RgbaPixels.end())](nvrhi::ICommandList* const Commands)
		{
			Commands->beginMarker("Upload mesh texture");
			Commands->writeTexture(Native->Handle, 0, MipLevel, Bytes.data(), RowPitch);
			Commands->endMarker();
		});

		Frame.UploadBytes += RgbaPixels.size();
		Frame.WrittenTextures[Native.get()] |= 1u << MipLevel;
		return {};
	}

	[[nodiscard]] std::expected<void, FPresentationError> ClearTargets(const FTextureHandle& Color, const FTextureHandle& Depth, const std::array<float, 4>& LinearColor) override
	{
		const auto NativeColor = std::dynamic_pointer_cast<FTexture>(Color);
		const auto NativeDepth = std::dynamic_pointer_cast<FTexture>(Depth);
		if ((Color && !NativeColor) || (Depth && !NativeDepth) || !ValidTargets(NativeColor, NativeDepth) || !std::ranges::all_of(LinearColor, [](const float Value)
		{
			return std::isfinite(Value);
		}))
		{
			return Invalid("Clear requires matching color/depth targets and a finite color");
		}

		if (const auto Ready = CanRecord(); !Ready)
		{
			return Ready;
		}

		FFrame& Frame = Frames[FrameIndex];
		const std::vector Cleared{NativeColor};
		Frame.Operations.emplace_back([NativeColor, NativeDepth, LinearColor, Discarded = FirstAliasUses(Frame, {&Cleared})](nvrhi::ICommandList* const Commands)
		{
			DiscardAliases(Commands, Discarded);
			Commands->beginMarker("Clear scene targets");
			if (NativeColor)
			{
				Commands->clearTextureFloat(NativeColor->Handle, nvrhi::AllSubresources, nvrhi::Color(LinearColor[0], LinearColor[1], LinearColor[2], LinearColor[3]));
			}

			if (NativeDepth)
			{
				Commands->clearDepthStencilTexture(NativeDepth->Handle, nvrhi::AllSubresources, true, 0.f, false, 0);
			}

			Commands->endMarker();
		});

		if (NativeColor)
		{
			Frame.WrittenTextures[NativeColor.get()] |= 1u;
		}

		if (NativeDepth)
		{
			Frame.WrittenTextures[NativeDepth.get()] |= 1u;
		}

		return {};
	}

	[[nodiscard]] std::expected<void, FPresentationError> DrawIndexed(const FIndexedDraw& Draw) override
	{
		if (const auto Ready = CanRecord(); !Ready)
		{
			return Ready;
		}

		const auto Pipeline = std::dynamic_pointer_cast<FPipeline>(Draw.Pipeline);
		const auto Vertices = std::dynamic_pointer_cast<FBuffer>(Draw.Vertices);
		const auto Indices = std::dynamic_pointer_cast<FBuffer>(Draw.Indices);
		const auto Instances = std::dynamic_pointer_cast<FBuffer>(Draw.Instances);
		const auto PreviousInstances = std::dynamic_pointer_cast<FBuffer>(Draw.PreviousInstances);
		const auto Color = std::dynamic_pointer_cast<FTexture>(Draw.ColorTarget);
		const auto SecondColor = std::dynamic_pointer_cast<FTexture>(Draw.SecondColorTarget);
		const auto Depth = std::dynamic_pointer_cast<FTexture>(Draw.DepthTarget);
		const bool bColored = Pipeline && Pipeline->VertexFormat == EGraphicsVertexFormat::ColoredClipPosition;

		const auto IsFiniteValue = [](const float Value)
		{
			return std::isfinite(Value);
		};

		if (!Owns(Pipeline) || !Owns(Vertices) || !Owns(Indices) || (Draw.ColorTarget && !Color) || (Draw.DepthTarget && !Depth) || !ValidTargets(Color, Depth) || (Pipeline->bDepthOnly ? Color || !Depth : !Color || Pipeline->ColorFormat != Color->Descriptor.Format) || Pipeline->bDepthAttachment != static_cast<bool>(Depth) || Vertices->Descriptor.Usage != EBufferUsage::Vertex || Vertices->Descriptor.VertexFormat != (Pipeline->VertexFormat == EGraphicsVertexFormat::ShadowMesh ? EGraphicsVertexFormat::Mesh : Pipeline->VertexFormat) || Indices->Descriptor.Usage != EBufferUsage::Index || Draw.IndexCount == 0 || Draw.IndexCount % 3 != 0 || Draw.FirstIndex > Indices->Descriptor.Size / sizeof(std::uint32_t) || Draw.IndexCount > Indices->Descriptor.Size / sizeof(std::uint32_t) - Draw.FirstIndex || !std::ranges::all_of(Draw.WorldToClip, IsFiniteValue) || !std::ranges::all_of(Draw.ObjectToView, IsFiniteValue) || Draw.Uniforms.size() != Pipeline->UniformBufferSize)
		{
			return Invalid(std::format("Indexed draw '{}' has incompatible resources, indices, targets, or transform (uniforms {}/{}, depth {}/{}, color {})", Pipeline ? Pipeline->Name : "invalid pipeline", Draw.Uniforms.size(), Pipeline ? Pipeline->UniformBufferSize : 0, static_cast<bool>(Depth), Pipeline ? Pipeline->bDepthAttachment : false, static_cast<bool>(Color)));
		}

		if (Pipeline->bInstanced ? !Owns(Instances) || Instances->Descriptor.Usage == EBufferUsage::Index || Instances->Descriptor.VertexFormat != EGraphicsVertexFormat::MeshInstance || Draw.InstanceCount == 0 || Draw.FirstInstance > Instances->Descriptor.Size / sizeof(FMeshInstance) || Draw.InstanceCount > Instances->Descriptor.Size / sizeof(FMeshInstance) - Draw.FirstInstance : Draw.Instances || Draw.InstanceCount != 1 || Draw.FirstInstance != 0)
		{
			return Invalid("Indexed draw instances must match the pipeline and fit a mesh instance buffer belonging to this device");
		}

		if (Pipeline->bInstanceMotion ? !Owns(PreviousInstances) || PreviousInstances->Descriptor.VertexFormat != EGraphicsVertexFormat::MeshInstanceMotion || Draw.FirstInstance + std::size_t{Draw.InstanceCount} > PreviousInstances->Descriptor.Size / sizeof(FMeshInstanceMotion) || (!PreviousInstances->bInitialized && !Frames[FrameIndex].UploadedBuffers.contains(PreviousInstances.get())) : static_cast<bool>(Draw.PreviousInstances))
		{
			return Invalid("Previous-frame instances belong only to motion pipelines and must cover the drawn instances");
		}

		if (Pipeline->SecondColorFormat ? !Owns(SecondColor) || !SecondColor->Descriptor.bRenderTarget || SecondColor->Descriptor.Format != *Pipeline->SecondColorFormat || SecondColor->Descriptor.Extent != Color->Descriptor.Extent || SecondColor == Color || !IsInitialized(SecondColor) : static_cast<bool>(Draw.SecondColorTarget))
		{
			return Invalid(std::format("Indexed draw '{}' needs an initialized second color target exactly when its pipeline has one, matching its format and extent", Pipeline->Name));
		}

		const auto Indirect = std::dynamic_pointer_cast<FBuffer>(Draw.IndirectArguments);
		if (Draw.IndirectArguments && (!Pipeline->bInstanced || !Owns(Indirect) || Indirect->Descriptor.Usage != EBufferUsage::Storage || Draw.IndirectOffset % sizeof(std::uint32_t) != 0 || Draw.IndirectOffset > Indirect->Descriptor.Size || Indirect->Descriptor.Size - Draw.IndirectOffset < 5 * sizeof(std::uint32_t)))
		{
			return Invalid("Indirect draw arguments must be five aligned uints inside a storage buffer belonging to this device, for an instanced pipeline");
		}

		FFrame& Frame = Frames[FrameIndex];
		const FExtent2D Extent = Color ? Color->Descriptor.Extent : Depth->Descriptor.Extent;
		FRenderViewport Viewport = Draw.Viewport;

		if (Viewport.Width == 0 && Viewport.Height == 0 && Viewport.X == 0 && Viewport.Y == 0)
		{
			Viewport = {.Width = Extent.Width, .Height = Extent.Height};
		}

		if (Viewport.Width == 0 || Viewport.Height == 0 || Viewport.X > Extent.Width || Viewport.Y > Extent.Height || Viewport.Width > Extent.Width - Viewport.X || Viewport.Height > Extent.Height - Viewport.Y)
		{
			return Invalid("Draw viewport must fit the render target");
		}

		std::vector<std::shared_ptr<FTexture>> Textures;
		if (Draw.Textures.size() > MaximumGraphicsTextures)
		{
			return Invalid("Draw exceeds the graphics texture binding limit");
		}

		if (!Draw.Textures.empty())
		{
			for (const FTextureHandle& Texture : Draw.Textures)
			{
				Textures.push_back(std::dynamic_pointer_cast<FTexture>(Texture));
			}
		}
		else if (Pipeline->TextureCount == 1)
		{
			Textures.push_back(std::dynamic_pointer_cast<FTexture>(Draw.Texture));
		}

		if (Textures.size() != Pipeline->TextureCount || !std::ranges::all_of(Textures, [&](const auto& Texture)
		{
			return Owns(Texture) && Texture != Color && Texture != SecondColor && Texture != Depth && IsInitialized(Texture);
		}))
		{
			return Invalid("Draw textures must match the pipeline and contain initialized non-target resources owned by this device");
		}

		std::vector<std::shared_ptr<FBuffer>> Buffers;
		for (const FBufferHandle& Buffer : Draw.Buffers)
		{
			Buffers.push_back(std::dynamic_pointer_cast<FBuffer>(Buffer));
		}

		if (Buffers.size() != Pipeline->BufferCount || !std::ranges::all_of(Buffers, [this](const auto& Buffer)
		{
			return Owns(Buffer) && Buffer->Descriptor.Usage == EBufferUsage::Storage;
		}))
		{
			return Invalid("Draw buffers must match the pipeline and be storage buffers owned by this device");
		}

		const auto IndexUpload = Frame.UploadedBuffers.find(Indices.get());
		const std::uint32_t MaximumIndex = IndexUpload == Frame.UploadedBuffers.end() ? Indices->MaximumIndex : IndexUpload->second;
		const std::size_t VertexStride = bColored ? sizeof(FColoredClipVertex) : sizeof(FMeshVertex);
		if ((!Vertices->bInitialized && !Frame.UploadedBuffers.contains(Vertices.get())) || (!Indices->bInitialized && IndexUpload == Frame.UploadedBuffers.end()) || (Instances && !Instances->bInitialized && !Frame.UploadedBuffers.contains(Instances.get())) || MaximumIndex >= Vertices->Descriptor.Size / VertexStride || (Color && !Draw.bCoversColorTarget && !IsInitialized(Color)) || (Depth && !IsInitialized(Depth)))
		{
			return Invalid(std::format("Indexed draw '{}' reads uninitialized resources or references vertices outside the vertex buffer (vertices {}, indices {}, instances {}, color {}, depth {}, maximum index {}, vertex count {})", Pipeline->Name, Vertices->bInitialized || Frame.UploadedBuffers.contains(Vertices.get()), Indices->bInitialized || IndexUpload != Frame.UploadedBuffers.end(), !Instances || Instances->bInitialized || Frame.UploadedBuffers.contains(Instances.get()), !Color || Draw.bCoversColorTarget || IsInitialized(Color), !Depth || IsInitialized(Depth), MaximumIndex, Vertices->Descriptor.Size / VertexStride));
		}

		const std::size_t UniformAllocation = (Draw.Uniforms.size() + 255) & ~std::size_t{255};
		const auto UniformBuffer = AllocateUniforms(UniformAllocation, Draw.Uniforms.size());
		if (!UniformBuffer)
		{
			return std::unexpected(UniformBuffer.error());
		}

		nvrhi::FramebufferDesc FramebufferDescriptor;
		if (Color)
		{
			FramebufferDescriptor.addColorAttachment(Color->Handle);
		}

		if (SecondColor)
		{
			FramebufferDescriptor.addColorAttachment(SecondColor->Handle);
		}

		if (Depth)
		{
			FramebufferDescriptor.setDepthAttachment(Depth->Handle);
		}

		nvrhi::FramebufferHandle& Framebuffer = Frame.Framebuffers[{Color ? Color->Handle.Get() : nullptr, SecondColor ? SecondColor->Handle.Get() : nullptr, Depth ? Depth->Handle.Get() : nullptr}];
		if (!Framebuffer)
		{
			Framebuffer = Device->createFramebuffer(FramebufferDescriptor);
		}

		nvrhi::BindingSetDesc BindingDescriptor;
		for (std::uint32_t Slot = 0; Slot < Textures.size(); ++Slot)
		{
			BindingDescriptor.addItem(nvrhi::BindingSetItem::Texture_SRV(Slot, Textures[Slot]->Handle));
		}

		for (std::uint32_t Slot = 0; Slot < Buffers.size(); ++Slot)
		{
			BindingDescriptor.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(ReadBufferBinding + Slot, Buffers[Slot]->Handle));
		}

		if (Pipeline->Sampler)
		{
			BindingDescriptor.addItem(nvrhi::BindingSetItem::Sampler(0, Pipeline->Sampler));
		}

		if (*UniformBuffer)
		{
			BindingDescriptor.addItem(nvrhi::BindingSetItem::ConstantBuffer(0, *UniformBuffer));
		}

		if (Pipeline->bPushConstants)
		{
			BindingDescriptor.addItem(nvrhi::BindingSetItem::PushConstants(1, MeshPushConstantSize));
		}

		nvrhi::BindingSetHandle Bindings;
		if (Pipeline->Layout)
		{
			Bindings = CachedBindingSet(Frame, BindingDescriptor, Pipeline->Layout);
		}

		if (!Framebuffer || (Pipeline->Layout && !Bindings))
		{
			return Failed("Could not create draw framebuffer or bindings");
		}

		const std::vector Targets{Color, SecondColor};
		auto Discarded = FirstAliasUses(Frame, {&Targets, &Textures});
		Frame.Operations.emplace_back([Draw, Pipeline, Vertices, Indices, Instances, PreviousInstances, Indirect, Framebuffer, Bindings, Viewport, Discarded = std::move(Discarded), Textures = std::move(Textures), Buffers = std::move(Buffers), UniformBuffer = *UniformBuffer, Uniforms = std::vector<std::byte>(Draw.Uniforms.begin(), Draw.Uniforms.end())](nvrhi::ICommandList* const Commands)
		{
			Commands->beginMarker(Pipeline->Name.c_str());
			DiscardAliases(Commands, Discarded);
			if (UniformBuffer)
			{
				Commands->writeBuffer(UniformBuffer, Uniforms.data(), Uniforms.size());
			}

			nvrhi::GraphicsState State;
			State.setPipeline(Pipeline->Handle).setFramebuffer(Framebuffer).addVertexBuffer(nvrhi::VertexBufferBinding().setBuffer(Vertices->Handle).setSlot(0).setOffset(0)).setIndexBuffer(nvrhi::IndexBufferBinding().setBuffer(Indices->Handle).setFormat(nvrhi::Format::R32_UINT).setOffset(0)).setViewport(nvrhi::ViewportState().addViewportAndScissorRect(nvrhi::Viewport(static_cast<float>(Viewport.X), static_cast<float>(Viewport.X + Viewport.Width), static_cast<float>(Viewport.Y), static_cast<float>(Viewport.Y + Viewport.Height), 0.f, 1.f)));
			if (Instances)
			{
				State.addVertexBuffer(nvrhi::VertexBufferBinding().setBuffer(Instances->Handle).setSlot(1).setOffset(0));
			}

			if (PreviousInstances)
			{
				State.addVertexBuffer(nvrhi::VertexBufferBinding().setBuffer(PreviousInstances->Handle).setSlot(2).setOffset(0));
			}

			if (Bindings)
			{
				State.addBindingSet(Bindings);
			}

			if (Indirect)
			{
				State.setIndirectParams(Indirect->Handle);
			}

			Commands->setGraphicsState(State);
			if (Pipeline->bPushConstants)
			{
				std::array<float, 32> Constants;
				std::ranges::copy(Draw.WorldToClip, Constants.begin());
				std::ranges::copy(Draw.ObjectToView, Constants.begin() + 16);
				Commands->setPushConstants(Constants.data(), sizeof(Constants));
			}

			if (Indirect)
			{
				Commands->drawIndexedIndirect(Draw.IndirectOffset);
			}
			else
			{
				Commands->drawIndexed(nvrhi::DrawArguments().setVertexCount(Draw.IndexCount).setStartIndexLocation(Draw.FirstIndex).setInstanceCount(Draw.InstanceCount).setStartInstanceLocation(Draw.FirstInstance));
			}
			Commands->endMarker();
		});

		if (Color && Draw.bCoversColorTarget)
		{
			Frame.WrittenTextures[Color.get()] |= 1u;
		}

		Frame.UploadBytes += Draw.Uniforms.size();
		Frame.UniformBytes += UniformAllocation;
		return {};
	}

	[[nodiscard]] std::expected<void, FPresentationError> Dispatch(const FComputeDispatch& Dispatch) override
	{
		if (const auto Ready = CanRecord(Dispatch.Uniforms.size()); !Ready)
		{
			return Ready;
		}

		const auto Pipeline = std::dynamic_pointer_cast<FComputePipeline>(Dispatch.Pipeline);
		const auto Indirect = std::dynamic_pointer_cast<FBuffer>(Dispatch.IndirectArguments);
		if (!Owns(Pipeline) || Dispatch.Textures.size() != Pipeline->TextureCount || Dispatch.StorageTextures.size() != Pipeline->StorageTextureCount || Dispatch.StorageBuffers.size() != Pipeline->StorageBufferCount || (Dispatch.IndirectArguments && (!Owns(Indirect) || Indirect->Descriptor.Usage != EBufferUsage::Storage || Dispatch.IndirectOffset % sizeof(std::uint32_t) != 0 || Indirect->Descriptor.Size < Dispatch.IndirectOffset + 3 * sizeof(std::uint32_t))) || Dispatch.Uniforms.size() != Pipeline->UniformBufferSize || std::ranges::any_of(Dispatch.GroupCount, [](const std::uint32_t Count)
		{
			return Count == 0 || Count > 65535;
		}))
		{
			return Invalid(std::format("Dispatch '{}' does not match its pipeline's bindings or has an empty or oversized group count", Pipeline ? Pipeline->Name : "invalid pipeline"));
		}

		std::vector<std::shared_ptr<FTexture>> Textures;
		for (const FTextureHandle& Texture : Dispatch.Textures)
		{
			Textures.push_back(std::dynamic_pointer_cast<FTexture>(Texture));
		}

		std::vector<std::shared_ptr<FTexture>> Storage;
		for (const FStorageTextureBinding& Binding : Dispatch.StorageTextures)
		{
			Storage.push_back(std::dynamic_pointer_cast<FTexture>(Binding.Texture));
		}

		std::vector<std::shared_ptr<FBuffer>> Buffers;
		for (const FBufferHandle& Buffer : Dispatch.StorageBuffers)
		{
			Buffers.push_back(std::dynamic_pointer_cast<FBuffer>(Buffer));
		}

		std::vector<std::shared_ptr<FBuffer>> ReadBuffers;
		for (const FBufferHandle& Buffer : Dispatch.Buffers)
		{
			ReadBuffers.push_back(std::dynamic_pointer_cast<FBuffer>(Buffer));
		}

		const auto IsStorage = [this](const auto& Buffer)
		{
			return Owns(Buffer) && Buffer->Descriptor.Usage == EBufferUsage::Storage;
		};

		if (ReadBuffers.size() != Pipeline->BufferCount || !std::ranges::all_of(Buffers, IsStorage) || !std::ranges::all_of(ReadBuffers, IsStorage))
		{
			return Invalid(std::format("Dispatch '{}' needs storage buffers owned by this device that match its pipeline", Pipeline->Name));
		}

		const bool bValidTextures = std::ranges::all_of(Textures, [&](const auto& Texture)
		{
			return Owns(Texture) && IsInitialized(Texture) && !std::ranges::contains(Storage, Texture);
		});

		bool bValidStorage = true;
		for (std::size_t Index = 0; Index < Storage.size(); ++Index)
		{
			bValidStorage = bValidStorage && Owns(Storage[Index]) && Storage[Index]->Descriptor.bStorage && Dispatch.StorageTextures[Index].MipLevel < Storage[Index]->Descriptor.MipLevels;
		}

		if (!bValidTextures || !bValidStorage)
		{
			return Invalid(std::format("Dispatch '{}' needs initialized sampled textures and storage textures with valid mip levels, owned by this device and not both read and written", Pipeline->Name));
		}

		const auto Scene = std::dynamic_pointer_cast<FAccelerationStructure>(Dispatch.Scene);
		const auto Table = std::dynamic_pointer_cast<FResourceTable>(Dispatch.ResourceTable);
		if (Pipeline->bRayQuery != static_cast<bool>(Dispatch.Scene) || (Scene && (!Owns(Scene) || !Scene->bTopLevel || !IsBuilt(*Scene))) || Pipeline->bResourceTable != static_cast<bool>(Dispatch.ResourceTable) || (Table && !Owns(Table)))
		{
			return Invalid(std::format("Dispatch '{}' needs a built top-level structure and a resource table exactly when its pipeline reads them", Pipeline->Name));
		}

		const std::size_t UniformAllocation = (Dispatch.Uniforms.size() + 255) & ~std::size_t{255};
		const auto UniformBuffer = AllocateUniforms(UniformAllocation, Dispatch.Uniforms.size());
		if (!UniformBuffer)
		{
			return std::unexpected(UniformBuffer.error());
		}

		FFrame& Frame = Frames[FrameIndex];
		nvrhi::BindingSetHandle Bindings;
		if (Pipeline->Layout)
		{
			nvrhi::BindingSetDesc BindingDescriptor;
			for (std::uint32_t Slot = 0; Slot < Textures.size(); ++Slot)
			{
				BindingDescriptor.addItem(nvrhi::BindingSetItem::Texture_SRV(Slot, Textures[Slot]->Handle));
			}

			if (Pipeline->Sampler)
			{
				BindingDescriptor.addItem(nvrhi::BindingSetItem::Sampler(0, Pipeline->Sampler));
			}

			if (*UniformBuffer)
			{
				BindingDescriptor.addItem(nvrhi::BindingSetItem::ConstantBuffer(0, *UniformBuffer));
			}

			for (std::uint32_t Slot = 0; Slot < Storage.size(); ++Slot)
			{
				BindingDescriptor.addItem(nvrhi::BindingSetItem::Texture_UAV(Slot, Storage[Slot]->Handle, nvrhi::Format::UNKNOWN, nvrhi::TextureSubresourceSet(Dispatch.StorageTextures[Slot].MipLevel, 1, 0, 1)));
			}

			for (std::uint32_t Slot = 0; Slot < Buffers.size(); ++Slot)
			{
				BindingDescriptor.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(static_cast<std::uint32_t>(Storage.size()) + Slot, Buffers[Slot]->Handle));
			}

			for (std::uint32_t Slot = 0; Slot < ReadBuffers.size(); ++Slot)
			{
				BindingDescriptor.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(ReadBufferBinding + Slot, ReadBuffers[Slot]->Handle));
			}

			if (Scene)
			{
				BindingDescriptor.addItem(nvrhi::BindingSetItem::RayTracingAccelStruct(AccelerationStructureBinding, Scene->Handle));
			}

			Bindings = CachedBindingSet(Frame, BindingDescriptor, Pipeline->Layout);
			if (!Bindings)
			{
				return Failed("Could not create dispatch bindings");
			}
		}

		auto Discarded = FirstAliasUses(Frame, {&Storage, &Textures});
		const bool bRepeatedBindings = Bindings && Bindings.Get() == Frame.PreviousComputeBindings && !Dispatch.bOverlapsPrevious;
		Frame.PreviousComputeBindings = Bindings.Get();
		Frame.Operations.emplace_back([Pipeline, Bindings, bRepeatedBindings, Discarded = std::move(Discarded), Textures = std::move(Textures), Storage, Buffers = std::move(Buffers), ReadBuffers = std::move(ReadBuffers), Indirect, UniformBuffer = *UniformBuffer, Uniforms = std::vector<std::byte>(Dispatch.Uniforms.begin(), Dispatch.Uniforms.end()), GroupCount = Dispatch.GroupCount, IndirectOffset = Dispatch.IndirectOffset, bOverlap = Dispatch.bOverlapsPrevious, Scene, Table, TableTextures = Table ? Table->Textures : decltype(FResourceTable::Textures){}, TableBuffers = Table ? Table->Buffers : decltype(FResourceTable::Buffers){}](nvrhi::ICommandList* const Commands)
		{
			Commands->beginMarker(Pipeline->Name.c_str());
			DiscardAliases(Commands, Discarded);
			if (bRepeatedBindings)
			{
				for (const auto& Texture : Storage)
				{
					Commands->setTextureState(Texture->Handle, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
				}

				for (const auto& Buffer : Buffers)
				{
					Commands->setBufferState(Buffer->Handle, nvrhi::ResourceStates::UnorderedAccess);
				}
			}

			// Descriptor tables bypass NVRHI's binding state tracking, so their resources are made readable here; setComputeState commits the barriers.
			for (const auto& Texture : TableTextures)
			{
				if (Texture)
				{
					Commands->setTextureState(Texture->Handle, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
				}
			}

			for (const auto& Buffer : TableBuffers)
			{
				if (Buffer)
				{
					Commands->setBufferState(Buffer->Handle, nvrhi::ResourceStates::ShaderResource);
				}
			}

			// NVRHI otherwise separates consecutive writes to one storage texture with a barrier. It applies when the bindings are set.
			for (const auto& Texture : bOverlap ? Storage : decltype(Storage){})
			{
				Commands->setEnableUavBarriersForTexture(Texture->Handle, false);
			}

			if (UniformBuffer)
			{
				Commands->writeBuffer(UniformBuffer, Uniforms.data(), Uniforms.size());
			}

			nvrhi::ComputeState State;
			State.setPipeline(Pipeline->Handle);
			if (Bindings)
			{
				State.addBindingSet(Bindings);
			}

			if (Table)
			{
				State.addBindingSet(Table->Handle);
			}

			if (Indirect)
			{
				State.setIndirectParams(Indirect->Handle);
			}

			Commands->setComputeState(State);
			if (Indirect)
			{
				Commands->dispatchIndirect(IndirectOffset);
			}
			else
			{
				Commands->dispatch(GroupCount[0], GroupCount[1], GroupCount[2]);
			}

			for (const auto& Texture : bOverlap ? Storage : decltype(Storage){})
			{
				Commands->setEnableUavBarriersForTexture(Texture->Handle, true);
			}

			Commands->endMarker();
		});

		for (std::size_t Index = 0; Index < Storage.size(); ++Index)
		{
			Frame.WrittenTextures[Storage[Index].get()] |= 1u << Dispatch.StorageTextures[Index].MipLevel;
		}

		// Instance buffers that only compute shaders fill become drawable once a dispatch writes them.
		for (const FBufferHandle& Buffer : Dispatch.StorageBuffers)
		{
			Frame.UploadedBuffers.try_emplace(static_cast<FBuffer*>(Buffer.get()), 0u);
		}

		Frame.UploadBytes += Dispatch.Uniforms.size();
		Frame.UniformBytes += UniformAllocation;
		return {};
	}

	[[nodiscard]] std::expected<std::uint64_t, FPresentationError> SubmitCommands() override
	{
		if (!bRecording)
		{
			return Invalid("No graphics frame is being recorded");
		}

		FFrame& Frame = Frames[FrameIndex];
		Frame.SkippedTimingDepth = 0;
		EndGpuTiming();
		Frame.Commands->open();
		Frame.Commands->beginMarker("Herta scene frame");
		for (const auto& Operation : Frame.Operations)
		{
			Operation(Frame.Commands);
		}

		Frame.Commands->endMarker();
		Frame.Commands->close();
		Frame.Serial = Device->executeCommandList(Frame.Commands);
		bRecording = false;
		if (Frame.Serial == 0)
		{
			return Failed("Graphics command submission failed");
		}

		for (const auto& [Buffer, MaximumIndex] : Frame.UploadedBuffers)
		{
			Buffer->bInitialized = true;
			Buffer->MaximumIndex = MaximumIndex;
		}

		for (const auto& [Texture, Mips] : Frame.WrittenTextures)
		{
			Texture->WrittenMips |= Mips;
			Texture->bInitialized = Texture->WrittenMips == AllMips(*Texture);
		}

		for (FBuffer* const Buffer : Frame.CopiedBuffers)
		{
			Buffer->ReadySerial = Frame.Serial;
		}

		for (FAccelerationStructure* const Structure : Frame.BuiltStructures)
		{
			Structure->bBuilt = true;
		}

		LastSubmittedSerial = Frame.Serial;
		FrameIndex = (FrameIndex + 1) % Frames.size();
		return LastSubmittedSerial;
	}

	void CancelCommands() noexcept override
	{
		if (bRecording)
		{
			Frames[FrameIndex].Reset();
			bRecording = false;
		}
	}

	[[nodiscard]] std::expected<void, FPresentationError> WaitForIdle() override
	{
		if (const auto Wait = WaitForSerial(LastSubmittedSerial); !Wait)
		{
			return Wait;
		}

		return RetireCompleted();
	}

	[[nodiscard]] std::expected<std::vector<std::byte>, FPresentationError> ReadbackTexture(const FTextureHandle& Texture) override
	{
		const auto Native = std::dynamic_pointer_cast<FTexture>(Texture);
		if (bRecording || !Owns(Native) || !Native->bInitialized || IsDepthFormat(Native->Descriptor.Format))
		{
			return Invalid("Readback requires an initialized color texture and no open graphics frame");
		}

		const auto Staging = Device->createStagingTexture(Native->Handle->getDesc(), nvrhi::CpuAccessMode::Read);
		if (!Staging)
		{
			return Failed("Could not create diagnostic texture readback staging");
		}

		if (const auto Begin = BeginCommands(); !Begin)
		{
			return std::unexpected(Begin.error());
		}

		Frames[FrameIndex].Operations.emplace_back([Native, Staging](nvrhi::ICommandList* const Commands)
		{
			Commands->beginMarker("Readback scene texture");
			Commands->copyTexture(Staging, nvrhi::TextureSlice(), Native->Handle, nvrhi::TextureSlice());
			Commands->endMarker();
		});

		const auto Submitted = SubmitCommands();
		if (!Submitted)
		{
			return std::unexpected(Submitted.error());
		}

		if (const auto Wait = WaitForIdle(); !Wait)
		{
			return std::unexpected(Wait.error());
		}

		const std::size_t PackedRow = static_cast<std::size_t>(Native->Descriptor.Extent.Width) * GetTextureTexelBytes(Native->Descriptor.Format);
		std::vector<std::byte> Pixels(PackedRow * Native->Descriptor.Extent.Height);
		std::size_t RowPitch = 0;
		const auto* const Mapped = static_cast<const std::byte*>(Device->mapStagingTexture(Staging, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, &RowPitch));
		if (!Mapped)
		{
			return Failed("Could not map diagnostic texture readback");
		}

		for (std::uint32_t Row = 0; Row < Native->Descriptor.Extent.Height; ++Row)
		{
			std::memcpy(Pixels.data() + Row * PackedRow, Mapped + Row * RowPitch, PackedRow);
		}

		Device->unmapStagingTexture(Staging);
		return Pixels;
	}

	[[nodiscard]] std::expected<void, FPresentationError> CopyBuffer(const FBufferHandle& Source, const FBufferHandle& Destination) override
	{
		const auto NativeSource = std::dynamic_pointer_cast<FBuffer>(Source);
		const auto NativeDestination = std::dynamic_pointer_cast<FBuffer>(Destination);
		FFrame& Frame = Frames[FrameIndex];
		if (!Owns(NativeSource) || !Owns(NativeDestination) || NativeSource->Descriptor.Usage != EBufferUsage::Storage || NativeDestination->Descriptor.Usage != EBufferUsage::Readback || NativeDestination->Descriptor.Size > NativeSource->Descriptor.Size || (!NativeSource->bInitialized && !Frame.UploadedBuffers.contains(NativeSource.get())))
		{
			return Invalid("Buffer copies read an initialized storage buffer into a smaller or equal readback buffer of this device");
		}

		if (const auto Ready = CanRecord(); !Ready)
		{
			return Ready;
		}

		Frame.Operations.emplace_back([NativeSource, NativeDestination](nvrhi::ICommandList* const Commands)
		{
			Commands->copyBuffer(NativeDestination->Handle, 0, NativeSource->Handle, 0, NativeDestination->Descriptor.Size);
		});

		Frame.CopiedBuffers.push_back(NativeDestination.get());
		return {};
	}

	[[nodiscard]] std::optional<std::vector<std::byte>> TryReadBuffer(const FBufferHandle& Buffer) override
	{
		const auto Native = std::dynamic_pointer_cast<FBuffer>(Buffer);
		if (!Owns(Native) || Native->Descriptor.Usage != EBufferUsage::Readback || Native->ReadySerial == 0 || GetStatistics().CompletedSerial < Native->ReadySerial)
		{
			return std::nullopt;
		}

		const auto* const Mapped = static_cast<const std::byte*>(Device->mapBuffer(Native->Handle, nvrhi::CpuAccessMode::Read));
		if (!Mapped)
		{
			return std::nullopt;
		}

		std::vector<std::byte> Bytes(Mapped, Mapped + Native->Descriptor.Size);
		Device->unmapBuffer(Native->Handle);
		return Bytes;
	}

	[[nodiscard]] FGraphicsStatistics GetStatistics() const noexcept override
	{
		std::uint64_t Completed = LastCompletedSerial;
		// Statistics must remain safe during device loss; command methods report the failure.
		if (vkGetSemaphoreCounterValue(NativeDevice, VulkanDevice->getQueueSemaphore(nvrhi::CommandQueue::Graphics), &Completed) == VK_SUCCESS)
		{
			LastCompletedSerial = Completed;
		}

		Completed = LastCompletedSerial;
		return {.SubmittedSerial = LastSubmittedSerial, .CompletedSerial = std::min(LastSubmittedSerial, Completed), .InFlightFrames = static_cast<std::size_t>(std::ranges::count_if(Frames, [Completed](const FFrame& Frame)
		{
			return Frame.Serial > Completed;
		}))};
	}

	void BeginGpuTiming(const std::string_view Name) override
	{
		if (!bRecording)
		{
			return;
		}

		FFrame& Frame = Frames[FrameIndex];
		if (Frame.ActiveTiming || Frame.SkippedTimingDepth != 0 || Frame.TimingCount == MaximumGpuPassTimings || Name.empty() || Name.size() > 128 || Frame.Operations.size() + 2 >= MaximumCommands)
		{
			++Frame.SkippedTimingDepth;
			return;
		}

		FPassTiming& Timing = Frame.Timings[Frame.TimingCount];
		if (!Timing.Query)
		{
			Timing.Query = Device->createTimerQuery();
		}

		if (!Timing.Query)
		{
			++Frame.SkippedTimingDepth;
			return;
		}

		Timing.Name = Name;
		Timing.bEnded = false;
		Device->resetTimerQuery(Timing.Query);
		Frame.ActiveTiming = Frame.TimingCount++;
		// The marker names the pass in RenderDoc and Nsight captures.
		Frame.Operations.emplace_back([Query = Timing.Query, Marker = std::string(Name)](nvrhi::ICommandList* const Commands)
		{
			Commands->beginMarker(Marker.c_str());
			Commands->beginTimerQuery(Query);
		});
	}

	void EndGpuTiming() override
	{
		if (!bRecording)
		{
			return;
		}

		FFrame& Frame = Frames[FrameIndex];
		if (Frame.SkippedTimingDepth != 0)
		{
			--Frame.SkippedTimingDepth;
			return;
		}

		if (!Frame.ActiveTiming)
		{
			return;
		}

		FPassTiming& Timing = Frame.Timings[*Frame.ActiveTiming];
		Frame.Operations.emplace_back([Query = Timing.Query](nvrhi::ICommandList* const Commands)
		{
			Commands->endTimerQuery(Query);
			Commands->endMarker();
		});

		Timing.bEnded = true;
		Frame.ActiveTiming.reset();
	}

	std::vector<FGpuPassTiming> GetGpuTimings() const override
	{
		return GpuTimings;
	}

	[[nodiscard]] bool SupportsRayQueries() const noexcept override
	{
		return static_cast<bool>(ResourceTableLayout);
	}

	[[nodiscard]] std::expected<FAccelerationStructureHandle, FPresentationError> CreateBottomLevel(const FBottomLevelDescriptor& Descriptor) override
	{
		const auto Vertices = std::dynamic_pointer_cast<FBuffer>(Descriptor.Vertices);
		const auto Indices = std::dynamic_pointer_cast<FBuffer>(Descriptor.Indices);
		if (!ResourceTableLayout || !Owns(Vertices) || !Owns(Indices) || !Vertices->Descriptor.bRayTracing || !Indices->Descriptor.bRayTracing || Vertices->Descriptor.Usage != EBufferUsage::Vertex || Indices->Descriptor.Usage != EBufferUsage::Index || Descriptor.Sections.empty())
		{
			return Invalid("Bottom level '" + Descriptor.Name + "' needs ray query support and ray tracing vertex and index buffers with at least one section");
		}

		const std::size_t IndexCount = Indices->Descriptor.Size / sizeof(std::uint32_t);
		auto Structure = std::make_shared<FAccelerationStructure>();
		Structure->Owner = Owner;
		Structure->Vertices = Vertices;
		Structure->Indices = Indices;
		Structure->bDeforming = Descriptor.bDeforming;
		for (const FBottomLevelSection& Section : Descriptor.Sections)
		{
			if (Section.IndexCount == 0 || Section.IndexCount % 3 != 0 || Section.FirstIndex > IndexCount || Section.IndexCount > IndexCount - Section.FirstIndex)
			{
				return Invalid("Bottom level '" + Descriptor.Name + "' sections must cover whole triangles inside the index buffer");
			}

			nvrhi::rt::GeometryTriangles Triangles;
			Triangles.setVertexBuffer(Vertices->Handle).setVertexFormat(nvrhi::Format::RGB32_FLOAT).setVertexStride(sizeof(FMeshVertex)).setVertexCount(static_cast<std::uint32_t>(Vertices->Descriptor.Size / sizeof(FMeshVertex))).setIndexBuffer(Indices->Handle).setIndexFormat(nvrhi::Format::R32_UINT).setIndexOffset(std::uint64_t{Section.FirstIndex} * sizeof(std::uint32_t)).setIndexCount(Section.IndexCount);
			Structure->Geometries.push_back(nvrhi::rt::GeometryDesc().setTriangles(Triangles).setFlags(nvrhi::rt::GeometryFlags::Opaque));
		}

		nvrhi::rt::AccelStructDesc Native;
		Native.setDebugName(Descriptor.Name).setBuildFlags(BottomLevelFlags(*Structure));
		Native.bottomLevelGeometries = Structure->Geometries;
		Structure->Handle = Device->createAccelStruct(Native);
		if (!Structure->Handle)
		{
			return Failed("Could not create bottom level '" + Descriptor.Name + "'");
		}

		Structure->ByteSize = AccelerationStructureBytes(*Structure);
		return Structure;
	}

	[[nodiscard]] std::expected<FAccelerationStructureHandle, FPresentationError> CreateTopLevel(const std::string_view Name, const std::uint32_t MaximumInstances) override
	{
		if (!ResourceTableLayout || MaximumInstances == 0 || MaximumInstances > MaximumTopLevelInstances)
		{
			return Invalid(std::format("Top level '{}' needs ray query support and room for 1 to {} instances", Name, MaximumTopLevelInstances));
		}

		auto Structure = std::make_shared<FAccelerationStructure>();
		Structure->Owner = Owner;
		Structure->bTopLevel = true;
		Structure->MaximumInstances = MaximumInstances;
		Structure->Handle = Device->createAccelStruct(nvrhi::rt::AccelStructDesc().setDebugName(std::string(Name)).setIsTopLevel(true).setTopLevelMaxInstances(MaximumInstances).setBuildFlags(nvrhi::rt::AccelStructBuildFlags::PreferFastTrace));
		if (!Structure->Handle)
		{
			return Failed(std::format("Could not create top level '{}'", Name));
		}

		Structure->ByteSize = AccelerationStructureBytes(*Structure);
		return Structure;
	}

	[[nodiscard]] std::expected<void, FPresentationError> BuildBottomLevel(const FAccelerationStructureHandle& BottomLevel) override
	{
		const auto Structure = std::dynamic_pointer_cast<FAccelerationStructure>(BottomLevel);
		if (!Owns(Structure) || Structure->bTopLevel)
		{
			return Invalid("Bottom level builds need a bottom level belonging to this device");
		}

		if (const auto Ready = CanRecord(); !Ready)
		{
			return Ready;
		}

		const bool bBuilt = IsBuilt(*Structure);
		if (bBuilt && !Structure->bDeforming)
		{
			return {};
		}

		FFrame& Frame = Frames[FrameIndex];
		const nvrhi::rt::AccelStructBuildFlags Flags = BottomLevelFlags(*Structure) | (bBuilt ? nvrhi::rt::AccelStructBuildFlags::PerformUpdate : nvrhi::rt::AccelStructBuildFlags::None);
		Frame.Operations.emplace_back([Structure, Flags](nvrhi::ICommandList* const Commands)
		{
			Commands->buildBottomLevelAccelStruct(Structure->Handle, Structure->Geometries.data(), Structure->Geometries.size(), Flags);
		});

		Frame.BuiltStructures.push_back(Structure.get());
		return {};
	}

	[[nodiscard]] std::expected<void, FPresentationError> BuildTopLevel(const FAccelerationStructureHandle& TopLevel, const std::span<const FRayTracingInstance> Instances) override
	{
		const auto Structure = std::dynamic_pointer_cast<FAccelerationStructure>(TopLevel);
		if (!Owns(Structure) || !Structure->bTopLevel || Instances.size() > Structure->MaximumInstances)
		{
			return Invalid("Top level builds need a top level belonging to this device with room for every instance");
		}

		if (const auto Ready = CanRecord(Instances.size_bytes()); !Ready)
		{
			return Ready;
		}

		std::vector<nvrhi::rt::InstanceDesc> Native;
		std::vector<std::shared_ptr<FAccelerationStructure>> BottomLevels;
		Native.reserve(Instances.size());
		BottomLevels.reserve(Instances.size());
		for (const FRayTracingInstance& Instance : Instances)
		{
			auto BottomLevel = std::dynamic_pointer_cast<FAccelerationStructure>(Instance.BottomLevel);
			if (!Owns(BottomLevel) || BottomLevel->bTopLevel || !IsBuilt(*BottomLevel) || Instance.CustomIndex >= (1u << 24) || !std::ranges::all_of(Instance.ObjectToWorld, [](const float Value)
			{
				return std::isfinite(Value);
			}))
			{
				return Invalid("Top level instances need built bottom levels of this device, finite transforms, and 24-bit custom indices");
			}

			nvrhi::rt::AffineTransform Transform;
			std::ranges::copy(Instance.ObjectToWorld, Transform);
			Native.push_back(nvrhi::rt::InstanceDesc().setTransform(Transform).setInstanceID(Instance.CustomIndex).setInstanceMask(Instance.Mask).setFlags(Instance.bAlphaTested ? nvrhi::rt::InstanceFlags::ForceNonOpaque : nvrhi::rt::InstanceFlags::None).setBLAS(BottomLevel->Handle));
			BottomLevels.push_back(std::move(BottomLevel));
		}

		FFrame& Frame = Frames[FrameIndex];
		Frame.Operations.emplace_back([Structure, Native = std::move(Native), BottomLevels = std::move(BottomLevels)](nvrhi::ICommandList* const Commands)
		{
			Commands->beginMarker("Build scene top level");
			Commands->buildTopLevelAccelStruct(Structure->Handle, Native.data(), Native.size(), nvrhi::rt::AccelStructBuildFlags::PreferFastTrace);
			Commands->endMarker();
		});

		Frame.BuiltStructures.push_back(Structure.get());
		Frame.UploadBytes += Instances.size() * sizeof(nvrhi::rt::InstanceDesc);
		return {};
	}

	[[nodiscard]] std::expected<FResourceTableHandle, FPresentationError> CreateResourceTable(const std::uint32_t Slots) override
	{
		if (!ResourceTableLayout || Slots == 0 || Slots > MaximumResourceTableSlots)
		{
			return Invalid(std::format("Resource tables need ray query support and 1 to {} slots", MaximumResourceTableSlots));
		}

		auto Table = std::make_shared<FResourceTable>();
		Table->Owner = Owner;
		Table->Handle = Device->createDescriptorTable(ResourceTableLayout);
		if (!Table->Handle)
		{
			return Failed("Could not create a resource table");
		}

		Device->resizeDescriptorTable(Table->Handle, Slots, false);
		Table->Textures.resize(Slots);
		Table->Buffers.resize(Slots);
		return Table;
	}

	[[nodiscard]] std::expected<void, FPresentationError> WriteResourceTable(const FResourceTableHandle& Table, const std::uint32_t Slot, const FTextureHandle& Texture) override
	{
		const auto Native = std::dynamic_pointer_cast<FResourceTable>(Table);
		const auto NativeTexture = std::dynamic_pointer_cast<FTexture>(Texture);
		if (!Owns(Native) || Slot >= Native->Textures.size() || !Owns(NativeTexture) || !NativeTexture->bInitialized || IsDepthFormat(NativeTexture->Descriptor.Format))
		{
			return Invalid("Resource table textures need a slot inside a table of this device and a fully written color texture");
		}

		if (!Device->writeDescriptorTable(Native->Handle, nvrhi::BindingSetItem::Texture_SRV(Slot, NativeTexture->Handle)))
		{
			return Failed("Could not write a resource table texture");
		}

		Native->Textures[Slot] = NativeTexture;
		return {};
	}

	[[nodiscard]] std::expected<void, FPresentationError> WriteResourceTable(const FResourceTableHandle& Table, const std::uint32_t Slot, const FBufferHandle& Buffer) override
	{
		const auto Native = std::dynamic_pointer_cast<FResourceTable>(Table);
		const auto NativeBuffer = std::dynamic_pointer_cast<FBuffer>(Buffer);
		if (!Owns(Native) || Slot >= Native->Buffers.size() || !Owns(NativeBuffer) || !NativeBuffer->Descriptor.bRayTracing)
		{
			return Invalid("Resource table buffers need a slot inside a table of this device and a ray tracing buffer");
		}

		if (!Device->writeDescriptorTable(Native->Handle, nvrhi::BindingSetItem::RawBuffer_SRV(Slot, NativeBuffer->Handle)))
		{
			return Failed("Could not write a resource table buffer");
		}

		Native->Buffers[Slot] = NativeBuffer;
		return {};
	}

private:
	[[nodiscard]] static nvrhi::rt::AccelStructBuildFlags BottomLevelFlags(const FAccelerationStructure& Structure) noexcept
	{
		return Structure.bDeforming ? nvrhi::rt::AccelStructBuildFlags::AllowUpdate | nvrhi::rt::AccelStructBuildFlags::PreferFastBuild : nvrhi::rt::AccelStructBuildFlags::PreferFastTrace;
	}

	[[nodiscard]] std::size_t AccelerationStructureBytes(const FAccelerationStructure& Structure) const
	{
		const auto Buffer = static_cast<VkBuffer>(Structure.Handle->getNativeObject(nvrhi::ObjectTypes::VK_Buffer));
		if (Buffer == VK_NULL_HANDLE)
		{
			return 0;
		}

		VkMemoryRequirements Requirements{};
		vkGetBufferMemoryRequirements(NativeDevice, Buffer, &Requirements);
		return static_cast<std::size_t>(Requirements.size);
	}

	// The aliased textures among Used that this recording has not used yet, which their operation discards first.
	[[nodiscard]] static std::vector<std::shared_ptr<FTexture>> FirstAliasUses(FFrame& Frame, const std::initializer_list<const std::vector<std::shared_ptr<FTexture>>*> Used)
	{
		std::vector<std::shared_ptr<FTexture>> First;
		for (const auto* const List : Used)
		{
			for (const auto& Texture : *List)
			{
				if (Texture && Texture->Heap && Frame.UsedAliases.insert(Texture.get()).second)
				{
					First.push_back(Texture);
				}
			}
		}

		return First;
	}

	[[nodiscard]] bool IsBuilt(const FAccelerationStructure& Structure) const
	{
		return Structure.bBuilt || std::ranges::contains(Frames[FrameIndex].BuiltStructures, &Structure);
	}

	// Volatile buffers suballocate each write from NVRHI's upload ring. Ordinary buffers would end the render pass and add a barrier before every draw.
	[[nodiscard]] std::expected<nvrhi::BufferHandle, FPresentationError> AllocateUniforms(const std::size_t Allocation, const std::size_t Bytes)
	{
		if (Allocation > MaximumUniformBytesPerFrame - Frames[FrameIndex].UniformBytes)
		{
			return Invalid("Uniforms exceed the 4 MiB per-frame budget");
		}

		if (const auto Ready = CanRecord(Bytes); !Ready)
		{
			return std::unexpected(Ready.error());
		}

		if (Allocation == 0)
		{
			return nvrhi::BufferHandle{};
		}

		auto& Volatile = VolatileUniforms[Allocation];
		if (!Volatile)
		{
			const auto Versions = static_cast<std::uint32_t>(MaximumUniformBytesPerFrame / Allocation * (FrameCount + 1));
			Volatile = Device->createBuffer(nvrhi::BufferDesc().setByteSize(Allocation).setIsConstantBuffer(true).setIsVolatile(true).setMaxVersions(Versions).setDebugName("Uniforms"));
			if (!Volatile)
			{
				VolatileUniforms.erase(Allocation);
				return Failed("Could not allocate a uniform buffer");
			}
		}

		return Volatile;
	}

	[[nodiscard]] nvrhi::BindingSetHandle CachedBindingSet(FFrame& Frame, const nvrhi::BindingSetDesc& Descriptor, nvrhi::IBindingLayout* const Layout) const
	{
		std::vector<std::uintptr_t> Key{std::bit_cast<std::uintptr_t>(Layout)};
		for (const nvrhi::BindingSetItem& Item : Descriptor.bindings)
		{
			Key.push_back(std::bit_cast<std::uintptr_t>(Item.resourceHandle));
			if (Item.type == nvrhi::ResourceType::Texture_UAV)
			{
				Key.push_back(Item.subresources.baseMipLevel);
			}
		}

		nvrhi::BindingSetHandle& Cached = Frame.BindingSets[std::move(Key)];
		if (!Cached)
		{
			Cached = Device->createBindingSet(Descriptor, Layout);
		}

		return Cached;
	}

	template <typename T> [[nodiscard]] bool Owns(const std::shared_ptr<T>& Resource) const noexcept
	{
		return Resource && Resource->Owner == Owner;
	}

	[[nodiscard]] bool ValidTargets(const std::shared_ptr<FTexture>& Color, const std::shared_ptr<FTexture>& Depth) const noexcept
	{
		return (Color || Depth) && (!Color || (Owns(Color) && Color->Descriptor.bRenderTarget && !IsDepthFormat(Color->Descriptor.Format))) && (!Depth || (Owns(Depth) && Depth->Descriptor.bRenderTarget && IsDepthFormat(Depth->Descriptor.Format))) && (!Color || !Depth || Color->Descriptor.Extent == Depth->Descriptor.Extent);
	}

	[[nodiscard]] bool IsInitialized(const std::shared_ptr<FTexture>& Texture) const
	{
		const auto Written = Frames[FrameIndex].WrittenTextures.find(Texture.get());
		return (Texture->WrittenMips | (Written != Frames[FrameIndex].WrittenTextures.end() ? Written->second : 0u)) == AllMips(*Texture);
	}

	[[nodiscard]] std::expected<void, FPresentationError> CanRecord(const std::size_t UploadBytes = 0) const
	{
		if (!bRecording)
		{
			return Invalid("BeginCommands must precede graphics commands");
		}

		const FFrame& Frame = Frames[FrameIndex];
		if (UploadBytes > MaximumUploadBytes - Frame.UploadBytes || Frame.Operations.size() >= MaximumCommands)
		{
			return Invalid("Graphics frame exceeds the 64 MiB upload or 16384 command budget");
		}

		return {};
	}

	[[nodiscard]] std::expected<void, FPresentationError> WaitForSerial(const std::uint64_t Serial) const
	{
		if (Serial == 0)
		{
			return {};
		}

		const VkSemaphore Semaphore = VulkanDevice->getQueueSemaphore(nvrhi::CommandQueue::Graphics);
		VkSemaphoreWaitInfo Wait{};
		Wait.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
		Wait.semaphoreCount = 1;
		Wait.pSemaphores = &Semaphore;
		Wait.pValues = &Serial;
		const VkResult Result = vkWaitSemaphores(NativeDevice, &Wait, std::numeric_limits<std::uint64_t>::max());
		if (Result != VK_SUCCESS)
		{
			return std::unexpected(FPresentationError{.Code = Result == VK_ERROR_DEVICE_LOST ? EPresentationErrorCode::DeviceLost : EPresentationErrorCode::CommandSubmissionFailed, .Message = std::format("Waiting for graphics submission {} failed with VkResult {}", Serial, static_cast<int>(Result))});
		}

		return {};
	}

	[[nodiscard]] std::expected<void, FPresentationError> RetireCompleted()
	{
		std::uint64_t Completed = 0;
		const VkResult Result = vkGetSemaphoreCounterValue(NativeDevice, VulkanDevice->getQueueSemaphore(nvrhi::CommandQueue::Graphics), &Completed);
		if (Result != VK_SUCCESS)
		{
			return std::unexpected(FPresentationError{.Code = Result == VK_ERROR_DEVICE_LOST ? EPresentationErrorCode::DeviceLost : EPresentationErrorCode::CommandSubmissionFailed, .Message = std::format("Reading the graphics completion timeline failed with VkResult {}", static_cast<int>(Result))});
		}

		LastCompletedSerial = Completed;
		for (FFrame& Frame : Frames)
		{
			if (Frame.Serial != 0 && Frame.Serial <= Completed)
			{
				if (Frame.Serial > LastGpuTimingSerial && Frame.TimingCount != 0)
				{
					std::vector<FGpuPassTiming> Results;
					for (std::size_t Index = 0; Index < Frame.TimingCount; ++Index)
					{
						const FPassTiming& Timing = Frame.Timings[Index];
						if (!Timing.bEnded || !Device->pollTimerQuery(Timing.Query))
						{
							break;
						}

						const double Milliseconds = static_cast<double>(Device->getTimerQueryTime(Timing.Query)) * 1000.0;
						if (!std::isfinite(Milliseconds) || Milliseconds < 0)
						{
							break;
						}

						Results.push_back({.Name = Timing.Name, .Milliseconds = Milliseconds});
					}

					if (Results.size() == Frame.TimingCount)
					{
						GpuTimings = std::move(Results);
						LastGpuTimingSerial = Frame.Serial;
					}
				}

				Frame.Reset();
			}
		}

		try
		{
			Device->runGarbageCollection();
		}
		catch (const std::exception& Error)
		{
			return Failed(std::format("Retiring graphics submissions failed: {}", Error.what()));
		}

		return {};
	}

	nvrhi::DeviceHandle Device;
	nvrhi::vulkan::DeviceHandle VulkanDevice;
	VkDevice NativeDevice;
	std::shared_ptr<const std::uint8_t> Owner = std::make_shared<const std::uint8_t>(0);
	std::array<FFrame, FrameCount> Frames;
	// Keyed by the 256-byte-aligned uniform size. Each holds enough versions for every recording in flight at the per-frame budget.
	std::unordered_map<std::size_t, nvrhi::BufferHandle> VolatileUniforms;
	std::size_t FrameIndex = 0;
	std::uint64_t LastSubmittedSerial = 0;
	mutable std::uint64_t LastCompletedSerial = 0;
	std::uint32_t TextureDimensionLimit = 0;
	// Set only when the device has ray queries, so it doubles as the capability flag.
	nvrhi::BindingLayoutHandle ResourceTableLayout;
	bool bRecording = false;
	std::vector<FGpuPassTiming> GpuTimings;
	std::uint64_t LastGpuTimingSerial = 0;
};
}

std::expected<std::unique_ptr<IGraphicsDevice>, FPresentationError> CreateGraphicsDevice(nvrhi::IDevice* const Device, nvrhi::vulkan::IDevice* const VulkanDevice, const VkDevice NativeDevice)
{
	if (!Device || !VulkanDevice || NativeDevice == VK_NULL_HANDLE)
	{
		return Invalid("Graphics device requires live NVRHI and Vulkan devices");
	}

	auto Graphics = std::make_unique<FGraphicsDevice>(Device, VulkanDevice, NativeDevice);
	if (const auto Initialized = Graphics->Initialize(); !Initialized)
	{
		return std::unexpected(Initialized.error());
	}

	return Graphics;
}

nvrhi::ITexture* GetNvrhiTexture(const FTextureHandle& Texture, nvrhi::IDevice* const ExpectedDevice) noexcept
{
	const auto* const Native = dynamic_cast<const FTexture*>(Texture.get());
	return Native && (!ExpectedDevice || Native->DeviceIdentity == ExpectedDevice) ? Native->Handle.Get() : nullptr;
}
}
