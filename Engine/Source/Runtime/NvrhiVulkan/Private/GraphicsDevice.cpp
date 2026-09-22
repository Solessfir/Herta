#include "GraphicsDevice.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <exception>
#include <format>
#include <functional>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace Herta
{
namespace
{
constexpr std::size_t FrameCount = 3;
constexpr std::size_t MaximumUploadBytes = std::size_t{64} * 1024 * 1024;
constexpr std::size_t MaximumCommands = 4096;
constexpr std::uint32_t MaximumTextureDimension = 8192;

[[nodiscard]] std::unexpected<FPresentationError> Invalid(const std::string& Message)
{
	return std::unexpected(FPresentationError{EPresentationErrorCode::InvalidDescriptor, Message});
}

[[nodiscard]] std::unexpected<FPresentationError> Failed(const std::string& Message)
{
	return std::unexpected(FPresentationError{EPresentationErrorCode::CommandSubmissionFailed, Message});
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
	[[nodiscard]] const FTextureDescriptor& GetDescriptor() const noexcept override
	{
		return Descriptor;
	}
};

struct FPipeline final : IRhiGraphicsPipeline
{
	std::shared_ptr<const std::uint8_t> Owner;
	nvrhi::GraphicsPipelineHandle Handle;
	nvrhi::BindingLayoutHandle Layout;
	nvrhi::SamplerHandle Sampler;
	ETextureFormat ColorFormat;
};

struct FFrame
{
	nvrhi::CommandListHandle Commands;
	std::uint64_t Serial = 0;
	std::size_t UploadBytes = 0;
	// NVRHI cannot abandon an open command list. Delay emission until the graph has validated.
	std::vector<std::function<void(nvrhi::ICommandList*)>> Operations;
	std::unordered_map<FBuffer*, std::uint32_t> UploadedBuffers;
	std::unordered_set<FTexture*> WrittenTextures;

	void Reset()
	{
		Operations.clear();
		UploadedBuffers.clear();
		WrittenTextures.clear();
		UploadBytes = 0;
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

	[[nodiscard]] std::expected<void, FPresentationError> Initialize()
	{
		const VkPhysicalDevice PhysicalDevice = VulkanDevice->getNativeObject(nvrhi::ObjectTypes::VK_PhysicalDevice);
		VkPhysicalDeviceProperties Properties{};
		vkGetPhysicalDeviceProperties(PhysicalDevice, &Properties);
		TextureDimensionLimit = std::min(MaximumTextureDimension, Properties.limits.maxImageDimension2D);
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
		const bool bVertex = Descriptor.Usage == EBufferUsage::Vertex;
		const std::size_t Stride = bVertex ? sizeof(FMeshVertex) : sizeof(std::uint32_t);
		if ((!bVertex && Descriptor.Usage != EBufferUsage::Index) || Descriptor.Size == 0 || Descriptor.Size > MaximumUploadBytes || Descriptor.Size % Stride != 0)
		{
			return Invalid("Mesh buffer size must contain complete vertices or uint32 indices and fit the 64 MiB upload budget");
		}
		auto Buffer = std::make_shared<FBuffer>();
		Buffer->Descriptor = Descriptor;
		Buffer->Owner = Owner;
		Buffer->Handle = Device->createBuffer(nvrhi::BufferDesc().setByteSize(Descriptor.Size).setDebugName(Descriptor.Name).setIsVertexBuffer(bVertex).setIsIndexBuffer(!bVertex).enableAutomaticStateTracking(bVertex ? nvrhi::ResourceStates::VertexBuffer : nvrhi::ResourceStates::IndexBuffer));
		if (!Buffer->Handle)
		{
			return Failed("Could not create mesh buffer '" + Descriptor.Name + "'");
		}
		return Buffer;
	}

	[[nodiscard]] std::expected<FTextureHandle, FPresentationError> CreateTexture(const FTextureDescriptor& Descriptor) override
	{
		const nvrhi::Format Format = ToFormat(Descriptor.Format);
		const bool bDepth = Descriptor.Format == ETextureFormat::Depth32;
		if (Descriptor.Extent.IsEmpty() || Descriptor.Extent.Width > TextureDimensionLimit || Descriptor.Extent.Height > TextureDimensionLimit || Format == nvrhi::Format::UNKNOWN || (bDepth && !Descriptor.bRenderTarget))
		{
			return Invalid(std::format("Texture requires a supported format and dimensions in [1, {}]; depth textures must be render targets", TextureDimensionLimit));
		}
		const nvrhi::FormatSupport Required = bDepth ? nvrhi::FormatSupport::DepthStencil : nvrhi::FormatSupport::Texture | nvrhi::FormatSupport::ShaderSample | (Descriptor.bRenderTarget ? nvrhi::FormatSupport::RenderTarget : nvrhi::FormatSupport::None);
		if ((Device->queryFormatSupport(Format) & Required) != Required)
		{
			return Invalid("Texture format does not support the requested usage on this device");
		}
		auto Texture = std::make_shared<FTexture>();
		Texture->Descriptor = Descriptor;
		Texture->Owner = Owner;
		Texture->DeviceIdentity = Device;
		nvrhi::TextureDesc Native;
		Native.setWidth(Descriptor.Extent.Width).setHeight(Descriptor.Extent.Height).setFormat(Format).setDebugName(Descriptor.Name).setIsRenderTarget(Descriptor.bRenderTarget).setIsTypeless(Descriptor.Format == ETextureFormat::Rgba8Srgb && Descriptor.bRenderTarget).enableAutomaticStateTracking(bDepth ? nvrhi::ResourceStates::DepthWrite : nvrhi::ResourceStates::ShaderResource);
		Native.isShaderResource = !bDepth;
		Texture->Handle = Device->createTexture(Native);
		if (!Texture->Handle)
		{
			return Failed("Could not create texture '" + Descriptor.Name + "'");
		}
		return Texture;
	}

	[[nodiscard]] std::expected<FGraphicsPipelineHandle, FPresentationError> CreateGraphicsPipeline(const FGraphicsPipelineDescriptor& Descriptor) override
	{
		if (Descriptor.VertexShader.Stage != EShaderStage::Vertex || Descriptor.FragmentShader.Stage != EShaderStage::Fragment || !SerializeCookedShader(Descriptor.VertexShader) || !SerializeCookedShader(Descriptor.FragmentShader) || (Descriptor.ColorFormat != ETextureFormat::Rgba8 && Descriptor.ColorFormat != ETextureFormat::Rgba8Srgb))
		{
			return Invalid("Graphics pipeline requires vertex and fragment SPIR-V shaders and an RGBA8 color format");
		}
		const auto HasSupportedBindings = [](const FShaderAsset& Shader)
		{
			return (Shader.PushConstantSize == 0 || Shader.PushConstantSize == sizeof(FIndexedDraw::WorldToClip)) && std::ranges::all_of(Shader.Bindings, [](const FShaderBinding& Binding)
			                                                                                                                             {
				                                                                                                                             return Binding.Space == 0 && ((Binding.Type == EShaderBindingType::Texture && Binding.Binding == 0) || (Binding.Type == EShaderBindingType::Sampler && Binding.Binding == 128));
			                                                                                                                             });
		};
		if (Descriptor.VertexShader.PushConstantSize != sizeof(FIndexedDraw::WorldToClip) || !HasSupportedBindings(Descriptor.VertexShader) || !HasSupportedBindings(Descriptor.FragmentShader))
		{
			return Invalid("Mesh shader ABI requires a 64-byte matrix push constant, texture binding 0, and sampler binding 128 in set 0");
		}
		const auto Vertex = Device->createShader(nvrhi::ShaderDesc().setShaderType(nvrhi::ShaderType::Vertex).setEntryName(Descriptor.VertexShader.EntryPoint).setDebugName(Descriptor.Name + ".Vertex"), Descriptor.VertexShader.Bytecode.data(), Descriptor.VertexShader.Bytecode.size() * sizeof(std::uint32_t));
		const auto Fragment = Device->createShader(nvrhi::ShaderDesc().setShaderType(nvrhi::ShaderType::Pixel).setEntryName(Descriptor.FragmentShader.EntryPoint).setDebugName(Descriptor.Name + ".Fragment"), Descriptor.FragmentShader.Bytecode.data(), Descriptor.FragmentShader.Bytecode.size() * sizeof(std::uint32_t));
		if (!Vertex || !Fragment)
		{
			return Failed("Could not create graphics shaders");
		}
		const std::array Attributes{
		    nvrhi::VertexAttributeDesc().setName("POSITION").setFormat(nvrhi::Format::RGB32_FLOAT).setOffset(offsetof(FMeshVertex, Position)).setElementStride(sizeof(FMeshVertex)),
		    nvrhi::VertexAttributeDesc().setName("TEXCOORD").setFormat(nvrhi::Format::RG32_FLOAT).setOffset(offsetof(FMeshVertex, UV)).setElementStride(sizeof(FMeshVertex))};
		const auto InputLayout = Device->createInputLayout(Attributes.data(), static_cast<std::uint32_t>(Attributes.size()), Vertex);
		auto Pipeline = std::make_shared<FPipeline>();
		Pipeline->Owner = Owner;
		Pipeline->ColorFormat = Descriptor.ColorFormat;
		Pipeline->Layout = Device->createBindingLayout(nvrhi::BindingLayoutDesc().setVisibility(nvrhi::ShaderType::AllGraphics).addItem(nvrhi::BindingLayoutItem::Texture_SRV(0)).addItem(nvrhi::BindingLayoutItem::Sampler(0)).addItem(nvrhi::BindingLayoutItem::PushConstants(0, sizeof(FIndexedDraw::WorldToClip))));
		Pipeline->Sampler = Device->createSampler(nvrhi::SamplerDesc().setAllAddressModes(nvrhi::SamplerAddressMode::Wrap));
		if (!InputLayout || !Pipeline->Layout || !Pipeline->Sampler)
		{
			return Failed("Could not create graphics pipeline layouts or sampler");
		}
		nvrhi::RenderState RenderState;
		RenderState.depthStencilState.setDepthTestEnable(true).setDepthWriteEnable(true).setDepthFunc(nvrhi::ComparisonFunc::GreaterOrEqual);
		RenderState.rasterState.setFrontCounterClockwise(true).setCullMode(nvrhi::RasterCullMode::Back).setDepthClipEnable(true).setScissorEnable(true);
		Pipeline->Handle = Device->createGraphicsPipeline(nvrhi::GraphicsPipelineDesc().setVertexShader(Vertex).setFragmentShader(Fragment).setInputLayout(InputLayout).addBindingLayout(Pipeline->Layout).setRenderState(RenderState), nvrhi::FramebufferInfo().addColorFormat(ToFormat(Descriptor.ColorFormat)).setDepthFormat(nvrhi::Format::D32));
		if (!Pipeline->Handle)
		{
			return Failed("Could not create graphics pipeline '" + Descriptor.Name + "'");
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
		if (!Owns(Native) || Data.size() != Native->Descriptor.Size)
		{
			return Invalid("Buffer upload must fully initialize a buffer belonging to this device");
		}
		if (const auto Ready = CanRecord(Data.size()); !Ready)
		{
			return Ready;
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

	[[nodiscard]] std::expected<void, FPresentationError> WriteTexture(const FTextureHandle& Texture, const std::span<const std::byte> RgbaPixels) override
	{
		const auto Native = std::dynamic_pointer_cast<FTexture>(Texture);
		if (!Owns(Native) || Native->Descriptor.Format == ETextureFormat::Depth32 || RgbaPixels.size() != static_cast<std::size_t>(Native->Descriptor.Extent.Width) * Native->Descriptor.Extent.Height * 4)
		{
			return Invalid("Texture upload must provide every RGBA8 texel of a color texture belonging to this device");
		}
		if (const auto Ready = CanRecord(RgbaPixels.size()); !Ready)
		{
			return Ready;
		}
		FFrame& Frame = Frames[FrameIndex];
		Frame.Operations.emplace_back([Native, Bytes = std::vector<std::byte>(RgbaPixels.begin(), RgbaPixels.end())](nvrhi::ICommandList* const Commands)
		                              {
			                              Commands->beginMarker("Upload mesh texture");
			                              Commands->writeTexture(Native->Handle, 0, 0, Bytes.data(), static_cast<std::size_t>(Native->Descriptor.Extent.Width) * 4);
			                              Commands->endMarker();
		                              });
		Frame.UploadBytes += RgbaPixels.size();
		Frame.WrittenTextures.insert(Native.get());
		return {};
	}

	[[nodiscard]] std::expected<void, FPresentationError> ClearTargets(const FTextureHandle& Color, const FTextureHandle& Depth, const std::array<float, 4>& LinearColor) override
	{
		const auto NativeColor = std::dynamic_pointer_cast<FTexture>(Color);
		const auto NativeDepth = std::dynamic_pointer_cast<FTexture>(Depth);
		if (!ValidTargets(NativeColor, NativeDepth) || !std::ranges::all_of(LinearColor, [](const float Value)
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
		Frame.Operations.emplace_back([NativeColor, NativeDepth, LinearColor](nvrhi::ICommandList* const Commands)
		                              {
			                              Commands->beginMarker("Clear scene targets");
			                              Commands->clearTextureFloat(NativeColor->Handle, nvrhi::AllSubresources, nvrhi::Color(LinearColor[0], LinearColor[1], LinearColor[2], LinearColor[3]));
			                              Commands->clearDepthStencilTexture(NativeDepth->Handle, nvrhi::AllSubresources, true, 0.0f, false, 0);
			                              Commands->endMarker();
		                              });
		Frame.WrittenTextures.insert(NativeColor.get());
		Frame.WrittenTextures.insert(NativeDepth.get());
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
		const auto Texture = std::dynamic_pointer_cast<FTexture>(Draw.Texture);
		const auto Color = std::dynamic_pointer_cast<FTexture>(Draw.ColorTarget);
		const auto Depth = std::dynamic_pointer_cast<FTexture>(Draw.DepthTarget);
		if (!Owns(Pipeline) || !Owns(Vertices) || !Owns(Indices) || !Owns(Texture) || !ValidTargets(Color, Depth) || Pipeline->ColorFormat != Color->Descriptor.Format || Vertices->Descriptor.Usage != EBufferUsage::Vertex || Indices->Descriptor.Usage != EBufferUsage::Index || Texture->Descriptor.Format == ETextureFormat::Depth32 || Texture == Color || Draw.IndexCount == 0 || Draw.IndexCount % 3 != 0 || Draw.IndexCount > Indices->Descriptor.Size / sizeof(std::uint32_t) || !std::ranges::all_of(Draw.WorldToClip, [](const float Value)
		                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                        {
			                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                        return std::isfinite(Value);
		                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                        }))
		{
			return Invalid("Indexed draw has incompatible resources, indices, targets, or transform");
		}
		FFrame& Frame = Frames[FrameIndex];
		const auto IndexUpload = Frame.UploadedBuffers.find(Indices.get());
		const std::uint32_t MaximumIndex = IndexUpload == Frame.UploadedBuffers.end() ? Indices->MaximumIndex : IndexUpload->second;
		if ((!Vertices->bInitialized && !Frame.UploadedBuffers.contains(Vertices.get())) || (!Indices->bInitialized && IndexUpload == Frame.UploadedBuffers.end()) || MaximumIndex >= Vertices->Descriptor.Size / sizeof(FMeshVertex) || !IsInitialized(Texture) || !IsInitialized(Color) || !IsInitialized(Depth))
		{
			return Invalid("Indexed draw reads uninitialized resources or references vertices outside the vertex buffer");
		}
		const auto Framebuffer = Device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(Color->Handle).setDepthAttachment(Depth->Handle));
		const auto Bindings = Device->createBindingSet(nvrhi::BindingSetDesc().addItem(nvrhi::BindingSetItem::Texture_SRV(0, Texture->Handle)).addItem(nvrhi::BindingSetItem::Sampler(0, Pipeline->Sampler)).addItem(nvrhi::BindingSetItem::PushConstants(0, sizeof(Draw.WorldToClip))), Pipeline->Layout);
		if (!Framebuffer || !Bindings)
		{
			return Failed("Could not create draw framebuffer or bindings");
		}
		Frame.Operations.emplace_back([Draw, Pipeline, Vertices, Indices, Framebuffer, Bindings, Extent = Color->Descriptor.Extent](nvrhi::ICommandList* const Commands)
		                              {
			                              Commands->beginMarker("Draw textured mesh");
			                              nvrhi::GraphicsState State;
			                              State.setPipeline(Pipeline->Handle).setFramebuffer(Framebuffer).addBindingSet(Bindings).addVertexBuffer(nvrhi::VertexBufferBinding().setBuffer(Vertices->Handle).setSlot(0).setOffset(0)).setIndexBuffer(nvrhi::IndexBufferBinding().setBuffer(Indices->Handle).setFormat(nvrhi::Format::R32_UINT).setOffset(0)).setViewport(nvrhi::ViewportState().addViewportAndScissorRect(nvrhi::Viewport(static_cast<float>(Extent.Width), static_cast<float>(Extent.Height))));
			                              Commands->setGraphicsState(State);
			                              Commands->setPushConstants(Draw.WorldToClip.data(), sizeof(Draw.WorldToClip));
			                              Commands->drawIndexed(nvrhi::DrawArguments().setVertexCount(Draw.IndexCount));
			                              Commands->endMarker();
		                              });
		return {};
	}

	[[nodiscard]] std::expected<std::uint64_t, FPresentationError> SubmitCommands() override
	{
		if (!bRecording)
		{
			return Invalid("No graphics frame is being recorded");
		}
		FFrame& Frame = Frames[FrameIndex];
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
		for (FTexture* const Texture : Frame.WrittenTextures)
		{
			Texture->bInitialized = true;
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
		if (bRecording || !Owns(Native) || !Native->bInitialized || Native->Descriptor.Format == ETextureFormat::Depth32)
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
		const std::size_t PackedRow = static_cast<std::size_t>(Native->Descriptor.Extent.Width) * 4;
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

	[[nodiscard]] FGraphicsStatistics GetStatistics() const noexcept override
	{
		std::uint64_t Completed = LastCompletedSerial;
		// Statistics must remain safe during device loss; command methods report the failure.
		if (vkGetSemaphoreCounterValue(NativeDevice, VulkanDevice->getQueueSemaphore(nvrhi::CommandQueue::Graphics), &Completed) == VK_SUCCESS)
		{
			LastCompletedSerial = Completed;
		}
		Completed = LastCompletedSerial;
		return {LastSubmittedSerial, std::min(LastSubmittedSerial, Completed), static_cast<std::size_t>(std::ranges::count_if(Frames, [Completed](const FFrame& Frame)
		                                                                                                                      {
			                                                                                                                      return Frame.Serial > Completed;
		                                                                                                                      }))};
	}

private:
	template <typename T> [[nodiscard]] bool Owns(const std::shared_ptr<T>& Resource) const noexcept
	{
		return Resource && Resource->Owner == Owner;
	}

	[[nodiscard]] bool ValidTargets(const std::shared_ptr<FTexture>& Color, const std::shared_ptr<FTexture>& Depth) const noexcept
	{
		return Owns(Color) && Owns(Depth) && Color->Descriptor.bRenderTarget && Depth->Descriptor.bRenderTarget && Color->Descriptor.Format != ETextureFormat::Depth32 && Depth->Descriptor.Format == ETextureFormat::Depth32 && Color->Descriptor.Extent == Depth->Descriptor.Extent;
	}

	[[nodiscard]] bool IsInitialized(const std::shared_ptr<FTexture>& Texture) const
	{
		return Texture->bInitialized || Frames[FrameIndex].WrittenTextures.contains(Texture.get());
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
			return Invalid("Graphics frame exceeds the 64 MiB upload or 4096 command budget");
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
			return std::unexpected(FPresentationError{Result == VK_ERROR_DEVICE_LOST ? EPresentationErrorCode::DeviceLost : EPresentationErrorCode::CommandSubmissionFailed, std::format("Waiting for graphics submission {} failed with VkResult {}", Serial, static_cast<int>(Result))});
		}
		return {};
	}

	[[nodiscard]] std::expected<void, FPresentationError> RetireCompleted()
	{
		std::uint64_t Completed = 0;
		const VkResult Result = vkGetSemaphoreCounterValue(NativeDevice, VulkanDevice->getQueueSemaphore(nvrhi::CommandQueue::Graphics), &Completed);
		if (Result != VK_SUCCESS)
		{
			return std::unexpected(FPresentationError{Result == VK_ERROR_DEVICE_LOST ? EPresentationErrorCode::DeviceLost : EPresentationErrorCode::CommandSubmissionFailed, std::format("Reading the graphics completion timeline failed with VkResult {}", static_cast<int>(Result))});
		}
		LastCompletedSerial = Completed;
		for (FFrame& Frame : Frames)
		{
			if (Frame.Serial != 0 && Frame.Serial <= Completed)
			{
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
	std::size_t FrameIndex = 0;
	std::uint64_t LastSubmittedSerial = 0;
	mutable std::uint64_t LastCompletedSerial = 0;
	std::uint32_t TextureDimensionLimit = 0;
	bool bRecording = false;
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
