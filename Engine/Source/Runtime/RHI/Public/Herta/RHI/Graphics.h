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
#include <vector>

namespace Herta
{
// Writes recorded between BeginCommands and SubmitCommands may upload at most this many bytes in total.
inline constexpr std::size_t MaximumUploadBytesPerRecording = std::size_t{64} * 1024 * 1024;

enum class EBufferUsage : std::uint8_t
{
	Vertex,
	Index
};

enum class ETextureFormat : std::uint8_t
{
	Rgba8,
	Rgba8Srgb,
	Depth32
};

enum class EGraphicsVertexFormat : std::uint8_t
{
	Mesh,
	ColoredClipPosition
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
	// Render targets have exactly one level. Sampled textures may have a full chain down to 1x1.
	std::uint32_t MipLevels = 1;
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

// Handles share ownership with recorded frames until their GPU submission completes.
using FBufferHandle = std::shared_ptr<IRhiBuffer>;
using FTextureHandle = std::shared_ptr<IRhiTexture>;
using FGraphicsPipelineHandle = std::shared_ptr<IRhiGraphicsPipeline>;

struct FGraphicsPipelineDescriptor
{
	std::string Name;
	FShaderAsset VertexShader;
	FShaderAsset FragmentShader;
	ETextureFormat ColorFormat = ETextureFormat::Rgba8Srgb;
	EGraphicsVertexFormat VertexFormat = EGraphicsVertexFormat::Mesh;
	bool bDepthTest = true;
};

struct FMeshVertex
{
	std::array<float, 3> Position;
	std::array<float, 2> UV;
};

struct FColoredClipVertex
{
	std::array<float, 4> Position;
	std::array<float, 4> Color;
};

struct FIndexedDraw
{
	FGraphicsPipelineHandle Pipeline;
	FBufferHandle Vertices;
	FBufferHandle Indices;
	FTextureHandle Texture;
	FTextureHandle ColorTarget;
	FTextureHandle DepthTarget;
	std::array<float, 16> WorldToClip{};
	std::uint32_t IndexCount = 0;
	std::uint32_t FirstIndex = 0;
	// Pushed after WorldToClip; mesh shaders light in view space.
	std::array<float, 16> ObjectToView{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
};

struct FGraphicsStatistics
{
	std::uint64_t SubmittedSerial = 0;
	std::uint64_t CompletedSerial = 0;
	std::size_t InFlightFrames = 0;
};

class IGraphicsDevice
{
public:
	// Calls are render-thread owned. Release all handles before destroying the owning presentation device.
	virtual ~IGraphicsDevice() = default;
	[[nodiscard]] virtual std::expected<FBufferHandle, FPresentationError> CreateBuffer(const FBufferDescriptor& Descriptor) = 0;
	[[nodiscard]] virtual std::expected<FTextureHandle, FPresentationError> CreateTexture(const FTextureDescriptor& Descriptor) = 0;
	[[nodiscard]] virtual std::expected<FGraphicsPipelineHandle, FPresentationError> CreateGraphicsPipeline(const FGraphicsPipelineDescriptor& Descriptor) = 0;
	[[nodiscard]] virtual std::expected<void, FPresentationError> BeginCommands() = 0;
	[[nodiscard]] virtual std::expected<void, FPresentationError> WriteBuffer(const FBufferHandle& Buffer, std::span<const std::byte> Data) = 0;

	// A texture becomes readable once every mip level has been written.
	[[nodiscard]] virtual std::expected<void, FPresentationError> WriteTexture(const FTextureHandle& Texture, std::uint32_t MipLevel, std::span<const std::byte> RgbaPixels) = 0;
	[[nodiscard]] virtual std::expected<void, FPresentationError> ClearTargets(const FTextureHandle& Color, const FTextureHandle& Depth, const std::array<float, 4>& LinearColor) = 0;
	[[nodiscard]] virtual std::expected<void, FPresentationError> DrawIndexed(const FIndexedDraw& Draw) = 0;
	[[nodiscard]] virtual std::expected<std::uint64_t, FPresentationError> SubmitCommands() = 0;
	virtual void CancelCommands() noexcept = 0;
	[[nodiscard]] virtual std::expected<void, FPresentationError> WaitForIdle() = 0;

	// Readback is an explicit blocking diagnostic path, never part of interactive rendering.
	[[nodiscard]] virtual std::expected<std::vector<std::byte>, FPresentationError> ReadbackTexture(const FTextureHandle& Texture) = 0;
	[[nodiscard]] virtual FGraphicsStatistics GetStatistics() const noexcept = 0;
};
}
