#include "Herta/Renderer/MeshRenderer.h"

#include "Herta/Math/Matrix.h"
#include "Herta/RenderGraph/RenderGraph.h"

#include <array>
#include <cmath>
#include <numbers>
#include <utility>

namespace Herta
{
namespace
{
constexpr std::array<FMeshVertex, 24> CubeVertices{{{{-1, -1, -1}, {0, 1}}, {{-1, 1, -1}, {0, 0}}, {{1, 1, -1}, {1, 0}}, {{1, -1, -1}, {1, 1}}, {{1, -1, 1}, {0, 1}}, {{1, 1, 1}, {0, 0}}, {{-1, 1, 1}, {1, 0}}, {{-1, -1, 1}, {1, 1}}, {{-1, -1, 1}, {0, 1}}, {{-1, 1, 1}, {0, 0}}, {{-1, 1, -1}, {1, 0}}, {{-1, -1, -1}, {1, 1}}, {{1, -1, -1}, {0, 1}}, {{1, 1, -1}, {0, 0}}, {{1, 1, 1}, {1, 0}}, {{1, -1, 1}, {1, 1}}, {{-1, 1, -1}, {0, 1}}, {{-1, 1, 1}, {0, 0}}, {{1, 1, 1}, {1, 0}}, {{1, 1, -1}, {1, 1}}, {{-1, -1, 1}, {0, 1}}, {{-1, -1, -1}, {0, 0}}, {{1, -1, -1}, {1, 0}}, {{1, -1, 1}, {1, 1}}}};

constexpr auto CubeIndices = []
{
	std::array<std::uint32_t, 36> Result{};
	for (std::uint32_t Face = 0; Face < 6; ++Face)
	{
		const std::array Pattern{0u, 1u, 2u, 0u, 2u, 3u};
		for (std::size_t Index = 0; Index < Pattern.size(); ++Index)
		{
			Result[static_cast<std::size_t>(Face) * 6 + Index] = Face * 4 + Pattern[Index];
		}
	}
	return Result;
}();

constexpr auto CheckerPixels = []
{
	std::array<std::uint8_t, std::size_t{64} * 64 * 4> Result{};
	for (std::size_t Y = 0; Y < 64; ++Y)
	{
		for (std::size_t X = 0; X < 64; ++X)
		{
			const bool bLight = ((X / 8) + (Y / 8)) % 2 == 0;
			const std::size_t Pixel = (Y * 64 + X) * 4;
			Result[Pixel] = bLight ? 225 : 36;
			Result[Pixel + 1] = bLight ? 235 : 100;
			Result[Pixel + 2] = bLight ? 245 : 170;
			Result[Pixel + 3] = 255;
		}
	}
	return Result;
}();

[[nodiscard]] std::expected<void, FRenderGraphError> GraphResult(const std::expected<void, FPresentationError>& Result)
{
	if (!Result)
	{
		return std::unexpected(FRenderGraphError{ERenderGraphErrorCode::ExecutionFailed, Result.error().Message});
	}
	return {};
}
}

struct FMeshRenderer::FImplementation
{
	IGraphicsDevice* Device = nullptr;
	FBufferHandle Vertices;
	FBufferHandle Indices;
	FTextureHandle Checker;
	FTextureHandle Color;
	FTextureHandle Depth;
	FGraphicsPipelineHandle Pipeline;
};

FMeshRenderer::FMeshRenderer(std::unique_ptr<FImplementation> InImplementation)
    : Implementation(std::move(InImplementation))
{
}

FMeshRenderer::~FMeshRenderer() = default;

std::expected<std::unique_ptr<FMeshRenderer>, FPresentationError> FMeshRenderer::Create(IGraphicsDevice& Device, FShaderAsset VertexShader, FShaderAsset FragmentShader)
{
	auto State = std::make_unique<FImplementation>();
	State->Device = &Device;
	auto Vertices = Device.CreateBuffer({"Mesh vertices", sizeof(CubeVertices), EBufferUsage::Vertex});
	auto Indices = Device.CreateBuffer({"Mesh indices", sizeof(CubeIndices), EBufferUsage::Index});
	auto Checker = Device.CreateTexture({"Checkerboard", {64, 64}, ETextureFormat::Rgba8Srgb, false});
	auto Pipeline = Device.CreateGraphicsPipeline({"Textured mesh reversed-Z", std::move(VertexShader), std::move(FragmentShader), ETextureFormat::Rgba8Srgb});
	if (!Vertices || !Indices || !Checker || !Pipeline)
	{
		return std::unexpected(!Vertices ? Vertices.error() : !Indices ? Indices.error()
		                                                  : !Checker   ? Checker.error()
		                                                               : Pipeline.error());
	}
	State->Vertices = std::move(*Vertices);
	State->Indices = std::move(*Indices);
	State->Checker = std::move(*Checker);
	State->Pipeline = std::move(*Pipeline);
	auto Result = Device.BeginCommands();
	if (!Result)
	{
		return std::unexpected(Result.error());
	}
	Result = Device.WriteBuffer(State->Vertices, std::as_bytes(std::span{CubeVertices}));
	if (Result)
	{
		Result = Device.WriteBuffer(State->Indices, std::as_bytes(std::span{CubeIndices}));
	}
	if (Result)
	{
		Result = Device.WriteTexture(State->Checker, std::as_bytes(std::span{CheckerPixels}));
	}
	if (!Result)
	{
		Device.CancelCommands();
		return std::unexpected(Result.error());
	}
	auto Submission = Device.SubmitCommands();
	if (!Submission)
	{
		return std::unexpected(Submission.error());
	}
	return std::unique_ptr<FMeshRenderer>(new FMeshRenderer(std::move(State)));
}

const FTextureHandle& FMeshRenderer::GetColorTarget() const noexcept
{
	return Implementation->Color;
}

std::expected<void, FPresentationError> FMeshRenderer::Render(const FExtent2D Extent, const float RotationRadians)
{
	if (Extent.IsEmpty())
	{
		return {};
	}
	if (!std::isfinite(RotationRadians))
	{
		return std::unexpected(FPresentationError{EPresentationErrorCode::InvalidDescriptor, "Mesh rotation must be finite"});
	}
	FImplementation& State = *Implementation;
	IGraphicsDevice& Device = *State.Device;
	if (!State.Color || State.Color->GetDescriptor().Extent != Extent)
	{
		auto Color = Device.CreateTexture({"Scene color", Extent, ETextureFormat::Rgba8Srgb, true});
		auto Depth = Device.CreateTexture({"Scene reversed-Z depth", Extent, ETextureFormat::Depth32, true});
		if (!Color || !Depth)
		{
			return std::unexpected(!Color ? Color.error() : Depth.error());
		}
		State.Color = std::move(*Color);
		State.Depth = std::move(*Depth);
	}
	const FMatrix4 Projection = FMatrix4::PerspectiveReversedInfinite(std::numbers::pi_v<float> / 3.0f, static_cast<float>(Extent.Width) / static_cast<float>(Extent.Height), 0.1f);
	const FQuaternion Rotation = FQuaternion::FromAxisAngle({0, 1, 0}, RotationRadians) * FQuaternion::FromAxisAngle({1, 0, 0}, -0.25f);
	const FMatrix4 WorldToClip = Projection * FMatrix4::Translation({0, 0, 5}) * FMatrix4::Rotation(Rotation);
	FRenderGraph Graph;
	const auto Color = Graph.ImportResource("Scene color");
	FTextureHandle FrameDepth;
	const auto Depth = Graph.CreateResource("Scene depth", [&]() -> std::expected<void, FRenderGraphError>
	                                        {
		                                        FrameDepth = State.Depth;
		                                        return {};
	                                        },
	                                        [&]
	                                        {
		                                        FrameDepth.reset();
	                                        });
	const auto Geometry = Graph.ImportResource("Uploaded mesh");
	const auto Texture = Graph.ImportResource("Checkerboard");
	(void)Graph.AddPass("Clear", {{Color, ERenderGraphAccess::Write}, {Depth, ERenderGraphAccess::Write}}, [&]
	                    {
		                    return GraphResult(Device.ClearTargets(State.Color, FrameDepth, {0.012f, 0.017f, 0.025f, 1.0f}));
	                    });
	(void)Graph.AddPass("Textured mesh", {{Color, ERenderGraphAccess::ReadWrite}, {Depth, ERenderGraphAccess::ReadWrite}, {Geometry, ERenderGraphAccess::Read}, {Texture, ERenderGraphAccess::Read}}, [&]
	                    {
		                    return GraphResult(Device.DrawIndexed({State.Pipeline, State.Vertices, State.Indices, State.Checker, State.Color, FrameDepth, WorldToClip.Data(), static_cast<std::uint32_t>(CubeIndices.size())}));
	                    });
	auto Begin = Device.BeginCommands();
	if (!Begin)
	{
		return Begin;
	}
	auto Execution = Graph.Execute();
	if (!Execution)
	{
		Device.CancelCommands();
		return std::unexpected(FPresentationError{EPresentationErrorCode::CommandSubmissionFailed, Execution.error().Message});
	}
	auto Submission = Device.SubmitCommands();
	if (!Submission)
	{
		return std::unexpected(Submission.error());
	}
	return {};
}
}
