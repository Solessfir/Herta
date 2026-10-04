#pragma once

#include "Herta/RHI/Graphics.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

// A recording graphics device for renderer tests that run without a GPU.
namespace Herta::Tests
{
class FTestBuffer final : public Herta::IRhiBuffer
{
public:
	explicit FTestBuffer(const Herta::FBufferDescriptor& InDescriptor)
	    : Descriptor(InDescriptor)
	{
	}

	const Herta::FBufferDescriptor& GetDescriptor() const noexcept override
	{
		return Descriptor;
	}

private:
	Herta::FBufferDescriptor Descriptor;
};

class FTestTexture final : public Herta::IRhiTexture
{
public:
	explicit FTestTexture(const Herta::FTextureDescriptor& InDescriptor)
	    : Descriptor(InDescriptor)
	{
	}

	const Herta::FTextureDescriptor& GetDescriptor() const noexcept override
	{
		return Descriptor;
	}

private:
	Herta::FTextureDescriptor Descriptor;
};

class FTestPipeline final : public Herta::IRhiGraphicsPipeline
{
public:
	Herta::FGraphicsPipelineDescriptor Descriptor;
};

class FTestGraphicsDevice final : public Herta::IGraphicsDevice
{
public:
	std::vector<Herta::FMeshVertex> Vertices;
	std::vector<std::vector<Herta::FColoredClipVertex>> DebugUploads;
	std::vector<Herta::FMeshInstance> InstanceUpload;
	std::vector<std::uint32_t> Indices;
	std::vector<std::vector<std::uint32_t>> IndexUploads;
	std::vector<std::string> Events;
	std::vector<Herta::FIndexedDraw> Draws;
	Herta::FIndexedDraw LastDraw;
	std::uint64_t Submissions = 0;
	std::vector<std::uint32_t> TextureWrites;
	std::size_t RecordingBytes = 0;
	std::size_t MaximumRecordingBytes = 0;
	bool bFailDepth = false;
	bool bFailDraw = false;

	std::expected<Herta::FBufferHandle, Herta::FPresentationError> CreateBuffer(const Herta::FBufferDescriptor& Descriptor) override
	{
		return std::make_shared<FTestBuffer>(Descriptor);
	}

	std::expected<Herta::FTextureHandle, Herta::FPresentationError> CreateTexture(const Herta::FTextureDescriptor& Descriptor) override
	{
		if (bFailDepth && Descriptor.Format == Herta::ETextureFormat::Depth32)
		{
			return std::unexpected(Herta::FPresentationError{Herta::EPresentationErrorCode::InvalidDescriptor, "Depth allocation failed"});
		}

		return std::make_shared<FTestTexture>(Descriptor);
	}

	std::expected<Herta::FGraphicsPipelineHandle, Herta::FPresentationError> CreateGraphicsPipeline(const Herta::FGraphicsPipelineDescriptor& Descriptor) override
	{
		auto Pipeline = std::make_shared<FTestPipeline>();
		Pipeline->Descriptor = Descriptor;
		return Pipeline;
	}

	std::expected<void, Herta::FPresentationError> BeginCommands() override
	{
		Events.emplace_back("Begin");
		RecordingBytes = 0;
		return {};
	}

	void RecordUpload(const std::size_t Bytes)
	{
		RecordingBytes += Bytes;
		MaximumRecordingBytes = std::max(MaximumRecordingBytes, RecordingBytes);
	}

	std::expected<void, Herta::FPresentationError> WriteBuffer(const Herta::FBufferHandle& Buffer, const std::span<const std::byte> Data) override
	{
		RecordUpload(Data.size());
		if (Buffer->GetDescriptor().Usage == Herta::EBufferUsage::Vertex && Buffer->GetDescriptor().VertexFormat == Herta::EGraphicsVertexFormat::ColoredClipPosition)
		{
			auto& Upload = DebugUploads.emplace_back(Data.size() / sizeof(Herta::FColoredClipVertex));
			std::memcpy(Upload.data(), Data.data(), Data.size());
		}
		else if (Buffer->GetDescriptor().Usage == Herta::EBufferUsage::Vertex && Buffer->GetDescriptor().VertexFormat == Herta::EGraphicsVertexFormat::MeshInstance)
		{
			InstanceUpload.resize(Data.size() / sizeof(Herta::FMeshInstance));
			std::memcpy(InstanceUpload.data(), Data.data(), Data.size());
		}
		else if (Buffer->GetDescriptor().Usage == Herta::EBufferUsage::Vertex)
		{
			Vertices.resize(Data.size() / sizeof(Herta::FMeshVertex));
			std::memcpy(Vertices.data(), Data.data(), Data.size());
		}
		else
		{
			Indices.resize(Data.size() / sizeof(std::uint32_t));
			std::memcpy(Indices.data(), Data.data(), Data.size());
			IndexUploads.push_back(Indices);
		}

		return {};
	}

	std::expected<void, Herta::FPresentationError> WriteTexture(const Herta::FTextureHandle&, const std::uint32_t MipLevel, const std::span<const std::byte> Pixels) override
	{
		RecordUpload(Pixels.size());
		TextureWrites.push_back(MipLevel);
		return {};
	}

	std::expected<void, Herta::FPresentationError> ClearTargets(const Herta::FTextureHandle&, const Herta::FTextureHandle&, const std::array<float, 4>&) override
	{
		Events.emplace_back("Clear");
		return {};
	}

	std::expected<void, Herta::FPresentationError> DrawIndexed(const Herta::FIndexedDraw& Draw) override
	{
		Events.emplace_back("Draw");
		LastDraw = Draw;
		Draws.push_back(Draw);
		if (bFailDraw)
		{
			return std::unexpected(Herta::FPresentationError{Herta::EPresentationErrorCode::CommandSubmissionFailed, "Draw failed"});
		}

		return {};
	}

	std::expected<std::uint64_t, Herta::FPresentationError> SubmitCommands() override
	{
		Events.emplace_back("Submit");
		return ++Submissions;
	}

	void CancelCommands() noexcept override
	{
		Events.emplace_back("Cancel");
	}

	std::expected<void, Herta::FPresentationError> WaitForIdle() override
	{
		return {};
	}

	std::expected<std::vector<std::byte>, Herta::FPresentationError> ReadbackTexture(const Herta::FTextureHandle&) override
	{
		return std::vector<std::byte>{};
	}

	Herta::FGraphicsStatistics GetStatistics() const noexcept override
	{
		return {Submissions, Submissions, 0};
	}
};
}
