#pragma once

#include "Herta/RHI/CookedShader.h"
#include "Herta/RHI/Presentation.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Herta
{
// Writes recorded between BeginCommands and SubmitCommands may upload at most this many bytes in total.
inline constexpr std::size_t MaximumUploadBytesPerRecording = std::size_t{64} * 1024 * 1024;
inline constexpr std::uint32_t MaximumGraphicsTextures = 16;
inline constexpr std::uint32_t MaximumGraphicsUniformBytes = 16 * 1024;
// Compute storage images, then storage buffers, use Vulkan bindings from 384, NVRHI's default unordered-access offset.
inline constexpr std::uint32_t StorageBinding = 384;
inline constexpr std::uint32_t MaximumStorageBindings = 8;
// Read-only StructuredBuffer bindings follow the texture registers, at t16..t23 in graphics and compute shaders.
inline constexpr std::uint32_t ReadBufferBinding = 16;
inline constexpr std::uint32_t MaximumReadBuffers = 8;
inline constexpr std::size_t MaximumStorageBufferBytes = std::size_t{256} * 1024 * 1024;

enum class EBufferUsage : std::uint8_t
{
	Vertex,
	Index,
	// uint elements that compute shaders read and write, which can also hold indirect dispatch and draw arguments.
	// With the MeshInstance vertex format they also feed instanced draws, so compute shaders can cull instances.
	Storage
};

enum class ETextureFormat : std::uint8_t
{
	Rgba8,
	Rgba8Srgb,
	Depth32,
	Rgba16Float,
	Rgba32Float,
	R32Float,
	R32Uint,
	Rg16Float,
	// Orthographic shadow cascades keep millimetre precision in 16 bits at half the memory and bandwidth.
	Depth16
};

[[nodiscard]] constexpr bool IsDepthFormat(const ETextureFormat Format) noexcept
{
	return Format == ETextureFormat::Depth32 || Format == ETextureFormat::Depth16;
}

[[nodiscard]] constexpr std::size_t GetTextureTexelBytes(const ETextureFormat Format) noexcept
{
	switch (Format)
	{
		case ETextureFormat::Rgba8:
		case ETextureFormat::Rgba8Srgb:
		case ETextureFormat::Depth32:
		case ETextureFormat::R32Float:
		case ETextureFormat::R32Uint:
		case ETextureFormat::Rg16Float:
			return 4;
		case ETextureFormat::Rgba16Float:
			return 8;
		case ETextureFormat::Rgba32Float:
			return 16;
		case ETextureFormat::Depth16:
			return 2;
	}

	return 0;
}

enum class EGraphicsCullMode : std::uint8_t
{
	None,
	Back,
	Front
};

enum class EGraphicsVertexFormat : std::uint8_t
{
	Mesh,
	ColoredClipPosition,
	MeshInstance,
	// Position and UV from a Mesh buffer, with view-space instance columns for shadow pipelines.
	ShadowMesh
};

struct FBufferDescriptor
{
	std::string Name;
	std::size_t Size = 0;
	EBufferUsage Usage = EBufferUsage::Vertex;
	EGraphicsVertexFormat VertexFormat = EGraphicsVertexFormat::Mesh;
};

struct FTextureDescriptor
{
	std::string Name;
	FExtent2D Extent;
	ETextureFormat Format = ETextureFormat::Rgba8;
	bool bRenderTarget = false;
	// Render targets have exactly one level. Sampled and storage textures may have a full chain down to 1x1.
	std::uint32_t MipLevels = 1;
	// Compute shaders may write any mip level through a storage binding.
	bool bStorage = false;
};

[[nodiscard]] constexpr std::uint32_t GetMipLevelCount(const FExtent2D Extent) noexcept
{
	std::uint32_t Levels = 1;

	for (std::uint32_t Size = Extent.Width > Extent.Height ? Extent.Width : Extent.Height; Size > 1; Size /= 2)
	{
		++Levels;
	}

	return Levels;
}

[[nodiscard]] constexpr FExtent2D GetMipExtent(const FExtent2D Extent, const std::uint32_t MipLevel) noexcept
{
	const auto Shrink = [MipLevel](const std::uint32_t Size)
	{
		const std::uint32_t Shifted = MipLevel < 32 ? Size >> MipLevel : 0;
		return Shifted > 0 ? Shifted : 1u;
	};

	return {Shrink(Extent.Width), Shrink(Extent.Height)};
}

class IRhiBuffer
{
public:
	virtual ~IRhiBuffer() = default;
	[[nodiscard]] virtual const FBufferDescriptor& GetDescriptor() const noexcept = 0;
};

class IRhiTexture
{
public:
	virtual ~IRhiTexture() = default;
	[[nodiscard]] virtual const FTextureDescriptor& GetDescriptor() const noexcept = 0;
};

class IRhiGraphicsPipeline
{
public:
	virtual ~IRhiGraphicsPipeline() = default;
};

class IRhiComputePipeline
{
public:
	virtual ~IRhiComputePipeline() = default;
};

// Handles share ownership with recorded frames until their GPU submission completes.
using FBufferHandle = std::shared_ptr<IRhiBuffer>;
using FTextureHandle = std::shared_ptr<IRhiTexture>;
using FGraphicsPipelineHandle = std::shared_ptr<IRhiGraphicsPipeline>;
using FComputePipelineHandle = std::shared_ptr<IRhiComputePipeline>;

struct FGraphicsPipelineDescriptor
{
	std::string Name;
	FShaderAsset VertexShader{};
	FShaderAsset FragmentShader{};
	ETextureFormat ColorFormat = ETextureFormat::Rgba8Srgb;
	EGraphicsVertexFormat VertexFormat = EGraphicsVertexFormat::Mesh;
	bool bDepthTest = true;
	bool bInstanced = false;
	std::uint32_t TextureCount = 1;
	std::uint32_t UniformBufferSize = 0;
	bool bDepthOnly = false;
	bool bDepthWrite = true;
	ETextureFormat DepthFormat = ETextureFormat::Depth32;
	// Depth writes replace whatever the target holds, which clears part of a depth target.
	bool bDepthOverwrite = false;
	EGraphicsCullMode CullMode = EGraphicsCullMode::Back;
	bool bAlphaBlend = false;
	bool bClampSampler = false;
	// Storage buffers read as StructuredBuffer from ReadBufferBinding.
	std::uint32_t BufferCount = 0;
};

// Compute shaders sample t0..t15 through a clamped s0, write storage textures and then storage buffers from StorageBinding, and read b0.
struct FComputePipelineDescriptor
{
	std::string Name;
	FShaderAsset Shader{};
	std::uint32_t TextureCount = 0;
	std::uint32_t StorageTextureCount = 0;
	// Bound after the storage textures, from StorageBinding + StorageTextureCount.
	std::uint32_t StorageBufferCount = 0;
	std::uint32_t UniformBufferSize = 0;
	bool bLinearSampler = false;
	// Storage buffers read as StructuredBuffer from ReadBufferBinding.
	std::uint32_t BufferCount = 0;
};

struct FStorageTextureBinding
{
	FTextureHandle Texture{};
	std::uint32_t MipLevel = 0;
};

struct FComputeDispatch
{
	FComputePipelineHandle Pipeline{};
	// Recording copies these inputs. A dispatch may not sample a texture it also writes.
	std::span<const FTextureHandle> Textures{};
	std::span<const FStorageTextureBinding> StorageTextures{};
	std::span<const FBufferHandle> StorageBuffers{};
	std::span<const std::byte> Uniforms{};
	std::array<std::uint32_t, 3> GroupCount{1, 1, 1};
	// A storage buffer whose first three uints, written earlier on the GPU, replace GroupCount.
	FBufferHandle IndirectArguments{};
	std::span<const FBufferHandle> Buffers{};
};

struct FMeshVertex
{
	std::array<float, 3> Position{};
	std::array<float, 2> UV{};
	std::array<float, 3> Normal{0, 1, 0};
	std::array<float, 4> Tangent{1, 0, 0, 1};
};

static_assert(sizeof(FMeshVertex) == 48);

struct FColoredClipVertex
{
	std::array<float, 4> Position;
	std::array<float, 4> Color;
};

// Column-major transforms consumed by the mesh pipeline's per-instance vertex stream.
struct FMeshInstance
{
	std::array<float, 16> WorldToClip;
	std::array<float, 16> ObjectToView;
	// The previous frame's object-to-clip transform, for motion vectors.
	std::array<float, 16> PreviousWorldToClip;
};

struct FRenderViewport
{
	std::uint32_t X = 0;
	std::uint32_t Y = 0;
	std::uint32_t Width = 0;
	std::uint32_t Height = 0;
};

struct FIndexedDraw
{
	FGraphicsPipelineHandle Pipeline{};
	FBufferHandle Vertices{};
	FBufferHandle Indices{};
	FTextureHandle Texture{};
	FTextureHandle ColorTarget{};
	FTextureHandle DepthTarget{};
	std::array<float, 16> WorldToClip{};
	std::uint32_t IndexCount = 0;
	std::uint32_t FirstIndex = 0;
	// Pushed after WorldToClip; mesh shaders light in view space.
	std::array<float, 16> ObjectToView{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
	FBufferHandle Instances{};
	std::uint32_t InstanceCount = 1;
	std::uint32_t FirstInstance = 0;
	// Recording copies these inputs. Registers are t0..t15, b0 at Vulkan binding 64, and s0 at 128.
	std::span<const FTextureHandle> Textures{};
	std::span<const std::byte> Uniforms{};
	// A zero extent selects the complete target.
	FRenderViewport Viewport{};
	// Storage buffers read as StructuredBuffer from ReadBufferBinding.
	std::span<const FBufferHandle> Buffers{};
	// An instanced draw may take its arguments from a storage buffer written earlier on the GPU: five uints at IndirectOffset bytes for index count, instance count, first index, vertex offset, and first instance.
	// IndexCount, FirstIndex, InstanceCount, and FirstInstance still describe the largest ranges those arguments may reach.
	FBufferHandle IndirectArguments{};
	std::uint32_t IndirectOffset = 0;
	// The draw writes every pixel of ColorTarget, so the target needs no clear first and counts as initialized afterwards.
	bool bCoversColorTarget = false;
};

struct FGraphicsStatistics
{
	std::uint64_t SubmittedSerial = 0;
	std::uint64_t CompletedSerial = 0;
	std::size_t InFlightFrames = 0;
};

struct FGpuPassTiming
{
	std::string Name;
	double Milliseconds = 0;
};

class IGraphicsDevice
{
public:
	// Calls are render-thread owned. Release all handles before destroying the owning presentation device.
	virtual ~IGraphicsDevice() = default;
	[[nodiscard]] virtual std::expected<FBufferHandle, FPresentationError> CreateBuffer(const FBufferDescriptor& Descriptor) = 0;
	[[nodiscard]] virtual std::expected<FTextureHandle, FPresentationError> CreateTexture(const FTextureDescriptor& Descriptor) = 0;
	[[nodiscard]] virtual std::expected<FGraphicsPipelineHandle, FPresentationError> CreateGraphicsPipeline(const FGraphicsPipelineDescriptor& Descriptor) = 0;
	[[nodiscard]] virtual std::expected<FComputePipelineHandle, FPresentationError> CreateComputePipeline(const FComputePipelineDescriptor& Descriptor) = 0;
	[[nodiscard]] virtual std::expected<void, FPresentationError> BeginCommands() = 0;
	[[nodiscard]] virtual std::expected<void, FPresentationError> WriteBuffer(const FBufferHandle& Buffer, std::span<const std::byte> Data) = 0;

	// A texture becomes readable once every mip level has been written.
	[[nodiscard]] virtual std::expected<void, FPresentationError> WriteTexture(const FTextureHandle& Texture, std::uint32_t MipLevel, std::span<const std::byte> RgbaPixels) = 0;
	[[nodiscard]] virtual std::expected<void, FPresentationError> ClearTargets(const FTextureHandle& Color, const FTextureHandle& Depth, const std::array<float, 4>& LinearColor) = 0;
	[[nodiscard]] virtual std::expected<void, FPresentationError> DrawIndexed(const FIndexedDraw& Draw) = 0;
	// A storage write initializes its mip level once the frame is submitted, like a texture upload.
	[[nodiscard]] virtual std::expected<void, FPresentationError> Dispatch(const FComputeDispatch& Dispatch) = 0;
	[[nodiscard]] virtual std::expected<std::uint64_t, FPresentationError> SubmitCommands() = 0;
	virtual void CancelCommands() noexcept = 0;
	[[nodiscard]] virtual std::expected<void, FPresentationError> WaitForIdle() = 0;

	// Readback is an explicit blocking diagnostic path, never part of interactive rendering.
	[[nodiscard]] virtual std::expected<std::vector<std::byte>, FPresentationError> ReadbackTexture(const FTextureHandle& Texture) = 0;
	[[nodiscard]] virtual FGraphicsStatistics GetStatistics() const noexcept = 0;
	virtual void BeginGpuTiming(std::string_view Name);
	virtual void EndGpuTiming();
	virtual std::vector<FGpuPassTiming> GetGpuTimings() const;
};

inline void IGraphicsDevice::BeginGpuTiming(std::string_view)
{
}

inline void IGraphicsDevice::EndGpuTiming()
{
}

inline std::vector<FGpuPassTiming> IGraphicsDevice::GetGpuTimings() const
{
	return {};
}
}
