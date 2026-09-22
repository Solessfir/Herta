#include "Herta/Math/Matrix.h"
#include "Herta/Renderer/MeshRenderer.h"

#include <cstring>
#include <doctest/doctest.h>
#include <limits>

namespace
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
};

class FTestGraphicsDevice final : public Herta::IGraphicsDevice
{
public:
	std::vector<Herta::FMeshVertex> Vertices;
	std::vector<std::uint32_t> Indices;
	std::vector<std::string> Events;
	Herta::FIndexedDraw LastDraw;
	std::uint64_t Submissions = 0;
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

	std::expected<Herta::FGraphicsPipelineHandle, Herta::FPresentationError> CreateGraphicsPipeline(const Herta::FGraphicsPipelineDescriptor&) override
	{
		return std::make_shared<FTestPipeline>();
	}

	std::expected<void, Herta::FPresentationError> BeginCommands() override
	{
		Events.emplace_back("Begin");
		return {};
	}

	std::expected<void, Herta::FPresentationError> WriteBuffer(const Herta::FBufferHandle& Buffer, const std::span<const std::byte> Data) override
	{
		if (Buffer->GetDescriptor().Usage == Herta::EBufferUsage::Vertex)
		{
			Vertices.resize(Data.size() / sizeof(Herta::FMeshVertex));
			std::memcpy(Vertices.data(), Data.data(), Data.size());
		}
		else
		{
			Indices.resize(Data.size() / sizeof(std::uint32_t));
			std::memcpy(Indices.data(), Data.data(), Data.size());
		}

		return {};
	}

	std::expected<void, Herta::FPresentationError> WriteTexture(const Herta::FTextureHandle&, std::span<const std::byte>) override
	{
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

Herta::FVector3 Position(const Herta::FMeshVertex& Vertex)
{
	return {Vertex.Position[0], Vertex.Position[1], Vertex.Position[2]};
}
}

TEST_CASE("Mesh renderer skips zero extents and rejects nonfinite rotation before recording")
{
	FTestGraphicsDevice Device;
	const auto Renderer = Herta::FMeshRenderer::Create(Device, {}, {});
	REQUIRE(Renderer);
	const std::uint64_t UploadSubmission = Device.Submissions;
	Device.Events.clear();
	CHECK((*Renderer)->Render({0, 100}));
	CHECK((*Renderer)->Render({100, 0}));
	CHECK_FALSE((*Renderer)->Render({100, 100}, std::numeric_limits<float>::infinity()));
	CHECK(Device.Events.empty());
	CHECK(Device.Submissions == UploadSubmission);
	CHECK_FALSE((*Renderer)->GetColorTarget());
}

TEST_CASE("Mesh renderer preserves previous targets when resize allocation fails")
{
	FTestGraphicsDevice Device;
	const auto Renderer = Herta::FMeshRenderer::Create(Device, {}, {});
	REQUIRE(Renderer);
	REQUIRE((*Renderer)->Render({320, 240}));
	const Herta::FTextureHandle Previous = (*Renderer)->GetColorTarget();
	const std::uint64_t Submission = Device.Submissions;
	Device.bFailDepth = true;
	Device.Events.clear();
	CHECK_FALSE((*Renderer)->Render({640, 480}));
	CHECK((*Renderer)->GetColorTarget() == Previous);
	CHECK(Previous->GetDescriptor().Extent == Herta::FExtent2D{320, 240});
	CHECK(Device.Submissions == Submission);
	CHECK(Device.Events.empty());
	Device.bFailDepth = false;
	REQUIRE((*Renderer)->Render({640, 480}));
	CHECK((*Renderer)->GetColorTarget() != Previous);
}

TEST_CASE("Mesh renderer records clear before indexed reversed-Z geometry")
{
	FTestGraphicsDevice Device;
	const auto Renderer = Herta::FMeshRenderer::Create(Device, {}, {});
	REQUIRE(Renderer);
	Device.Events.clear();
	REQUIRE((*Renderer)->Render({640, 480}, 0));
	CHECK(Device.Events == std::vector<std::string>{"Begin", "Clear", "Draw", "Submit"});
	CHECK(Device.LastDraw.IndexCount == 36);
	REQUIRE(Device.LastDraw.DepthTarget);
	CHECK(Device.LastDraw.DepthTarget->GetDescriptor().Format == Herta::ETextureFormat::Depth32);
	const Herta::FMatrix4 Transform(Device.LastDraw.WorldToClip);
	const Herta::FVector4 Near = Transform * Herta::FVector4{0, 0, -1, 1};
	const Herta::FVector4 Far = Transform * Herta::FVector4{0, 0, 1, 1};
	CHECK(Near.Z == doctest::Approx(0.1f));
	CHECK(Far.Z == doctest::Approx(0.1f));
	CHECK(Near.W > 0);
	CHECK(Far.W > Near.W);
	CHECK(Near.Z / Near.W > Far.Z / Far.W);
	CHECK(Near.Z / Near.W < 1.0f);
}

TEST_CASE("Mesh renderer uploads counter-clockwise cube triangles with valid UVs")
{
	FTestGraphicsDevice Device;
	const auto Renderer = Herta::FMeshRenderer::Create(Device, {}, {});
	REQUIRE(Renderer);
	REQUIRE(Device.Vertices.size() == 24);
	REQUIRE(Device.Indices.size() == 36);
	for (std::size_t Index = 0; Index < Device.Indices.size(); Index += 3)
	{
		REQUIRE(Device.Indices[Index] < Device.Vertices.size());
		REQUIRE(Device.Indices[Index + 1] < Device.Vertices.size());
		REQUIRE(Device.Indices[Index + 2] < Device.Vertices.size());
		const Herta::FVector3 A = Position(Device.Vertices[Device.Indices[Index]]);
		const Herta::FVector3 B = Position(Device.Vertices[Device.Indices[Index + 1]]);
		const Herta::FVector3 C = Position(Device.Vertices[Device.Indices[Index + 2]]);
		const Herta::FVector3 Normal = (B - A).Cross(C - A);
		CHECK(Normal.Dot(A + B + C) > 0);
	}

	for (const Herta::FMeshVertex& Vertex : Device.Vertices)
	{
		CHECK(Vertex.UV[0] >= 0.0f);
		CHECK(Vertex.UV[0] <= 1.0f);
		CHECK(Vertex.UV[1] >= 0.0f);
		CHECK(Vertex.UV[1] <= 1.0f);
	}
}

TEST_CASE("Mesh renderer cancels recording after a failed graph pass")
{
	FTestGraphicsDevice Device;
	const auto Renderer = Herta::FMeshRenderer::Create(Device, {}, {});
	REQUIRE(Renderer);
	Device.Events.clear();
	Device.bFailDraw = true;
	const std::uint64_t Submission = Device.Submissions;
	const auto Result = (*Renderer)->Render({320, 240});
	REQUIRE_FALSE(Result);
	CHECK(Result.error().Message == "Draw failed");
	CHECK(Device.Events == std::vector<std::string>{"Begin", "Clear", "Draw", "Cancel"});
	CHECK(Device.Submissions == Submission);
}
