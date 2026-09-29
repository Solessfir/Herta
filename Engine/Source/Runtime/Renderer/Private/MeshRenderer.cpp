#include "Herta/Renderer/MeshRenderer.h"

#include "Herta/Math/Matrix.h"
#include "Herta/RenderGraph/RenderGraph.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <numeric>
#include <ranges>
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

constexpr std::uint32_t CheckerTextureSize = 128;
constexpr auto CheckerPixels = []
{
	constexpr std::size_t TileSize = CheckerTextureSize / 4;
	std::array<std::uint8_t, std::size_t{CheckerTextureSize} * CheckerTextureSize * 4> Result{};
	for (std::size_t Y = 0; Y < CheckerTextureSize; ++Y)
	{
		for (std::size_t X = 0; X < CheckerTextureSize; ++X)
		{
			const bool bLight = ((X / TileSize) + (Y / TileSize)) % 2 == 0;
			const bool bSeam = X % TileSize == 0 || Y % TileSize == 0;
			const std::size_t Pixel = (Y * CheckerTextureSize + X) * 4;
			Result[Pixel] = bSeam ? 70 : (bLight ? 145 : 82);
			Result[Pixel + 1] = bSeam ? 83 : (bLight ? 160 : 99);
			Result[Pixel + 2] = bSeam ? 96 : (bLight ? 174 : 117);
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

[[nodiscard]] bool IsFinite(const FMatrix4& Matrix)
{
	return std::ranges::all_of(Matrix.Data(), [](const float Value)
	                           {
		                           return std::isfinite(Value);
	                           });
}

struct FProjectedDebugVertex
{
	FVector4 Position;
	float Size;
	std::array<float, 4> Color;
};

[[nodiscard]] double ClipDistance(const FVector4& Position, const std::size_t Plane)
{
	switch (Plane)
	{
		case 0:
			return static_cast<double>(Position.W) + Position.X;
		case 1:
			return static_cast<double>(Position.W) - Position.X;
		case 2:
			return static_cast<double>(Position.W) + Position.Y;
		case 3:
			return static_cast<double>(Position.W) - Position.Y;
		case 4:
			return Position.Z;
		default:
			return static_cast<double>(Position.W) - Position.Z;
	}
}

[[nodiscard]] FProjectedDebugVertex Interpolate(const FProjectedDebugVertex& A, const FProjectedDebugVertex& B, const float T)
{
	FProjectedDebugVertex Result{.Position = {}, .Size = std::lerp(A.Size, B.Size, T), .Color = {}};
	for (std::size_t Index = 0; Index < 4; ++Index)
	{
		Result.Position[Index] = std::lerp(A.Position[Index], B.Position[Index], T);
		Result.Color[Index] = std::lerp(A.Color[Index], B.Color[Index], T);
	}
	return Result;
}

[[nodiscard]] bool ClipLine(FProjectedDebugVertex& A, FProjectedDebugVertex& B)
{
	for (std::size_t Plane = 0; Plane < 6; ++Plane)
	{
		const double DistanceA = ClipDistance(A.Position, Plane);
		const double DistanceB = ClipDistance(B.Position, Plane);
		if (DistanceA < 0 && DistanceB < 0)
		{
			return false;
		}
		if ((DistanceA < 0) != (DistanceB < 0))
		{
			const FProjectedDebugVertex Intersection = Interpolate(A, B, static_cast<float>(DistanceA / (DistanceA - DistanceB)));
			if (DistanceA < 0)
			{
				A = Intersection;
			}
			else
			{
				B = Intersection;
			}
		}
	}
	return A.Position.W > 0.000001f && B.Position.W > 0.000001f;
}

[[nodiscard]] FColoredClipVertex RasterVertex(const FProjectedDebugVertex& Vertex, const FVector2& Offset = {})
{
	return {{{Vertex.Position.X + Offset.X * Vertex.Position.W, Vertex.Position.Y + Offset.Y * Vertex.Position.W, Vertex.Position.Z, Vertex.Position.W}}, Vertex.Color};
}

void AppendQuad(std::vector<FColoredClipVertex>& Vertices, const FProjectedDebugVertex& A, const FProjectedDebugVertex& B, const FVector2& OffsetA, const FVector2& OffsetB)
{
	const FColoredClipVertex ALeft = RasterVertex(A, OffsetA);
	const FColoredClipVertex ARight = RasterVertex(A, -OffsetA);
	const FColoredClipVertex BLeft = RasterVertex(B, OffsetB);
	const FColoredClipVertex BRight = RasterVertex(B, -OffsetB);
	Vertices.insert(Vertices.end(), {ALeft, ARight, BLeft, BLeft, ARight, BRight});
}

[[nodiscard]] std::expected<void, FPresentationError> ExpandDebugDraw(const FExtent2D Extent, const FMatrix4& WorldToClip, const std::span<const FDebugDrawList> Lists, std::array<std::vector<FColoredClipVertex>, 2>& Batches)
{
	constexpr std::size_t MaximumInputVertices = 262144;
	std::size_t InputVertices = 0;
	for (auto& Batch : Batches)
	{
		Batch.clear();
	}
	for (const FDebugDrawList& List : Lists)
	{
		const std::size_t PrimitiveSize = List.Primitive == EDebugPrimitive::Points ? 1 : List.Primitive == EDebugPrimitive::Lines ? 2
		                                                                                                                           : 3;
		if ((List.Primitive != EDebugPrimitive::Points && List.Primitive != EDebugPrimitive::Lines && List.Primitive != EDebugPrimitive::Triangles) || List.Vertices.size() % PrimitiveSize != 0 || List.Vertices.size() > MaximumInputVertices - InputVertices)
		{
			return std::unexpected(FPresentationError{EPresentationErrorCode::InvalidDescriptor, "Debug draw requires complete primitives and at most 262144 input vertices"});
		}
		InputVertices += List.Vertices.size();
		std::vector<FColoredClipVertex>& Batch = Batches[List.bDepthTest ? 0 : 1];
		for (std::size_t First = 0; First < List.Vertices.size(); First += PrimitiveSize)
		{
			std::array<FProjectedDebugVertex, 3> Projected{};
			for (std::size_t Index = 0; Index < PrimitiveSize; ++Index)
			{
				const FDebugDrawVertex& Vertex = List.Vertices[First + Index];
				Projected[Index] = {WorldToClip * FVector4{Vertex.Position, 1}, Vertex.Size, Vertex.Color};
				const FVector4& Position = Projected[Index].Position;
				if (!std::isfinite(Position.X) || !std::isfinite(Position.Y) || !std::isfinite(Position.Z) || !std::isfinite(Position.W) || (List.Primitive != EDebugPrimitive::Triangles && (!std::isfinite(Vertex.Size) || Vertex.Size <= 0 || Vertex.Size > 4096)) || !std::ranges::all_of(Vertex.Color, [](const float Value)
				                                                                                                                                                                                                                                                                              {
					                                                                                                                                                                                                                                                                              return std::isfinite(Value);
				                                                                                                                                                                                                                                                                              }))
				{
					return std::unexpected(FPresentationError{EPresentationErrorCode::InvalidDescriptor, "Debug vertices require finite positions/colors and pixel sizes in (0, 4096]"});
				}
			}
			FProjectedDebugVertex& A = Projected[0];
			FProjectedDebugVertex& B = Projected[1];
			if (List.Primitive == EDebugPrimitive::Triangles)
			{
				Batch.insert(Batch.end(), {RasterVertex(A), RasterVertex(B), RasterVertex(Projected[2])});
			}
			else if (List.Primitive == EDebugPrimitive::Lines)
			{
				// Clip before dividing by W so a line crossing the camera cannot expand across the screen.
				if (!ClipLine(A, B))
				{
					continue;
				}
				const FVector2 Direction{(B.Position.X / B.Position.W - A.Position.X / A.Position.W) * static_cast<float>(Extent.Width), (B.Position.Y / B.Position.W - A.Position.Y / A.Position.W) * static_cast<float>(Extent.Height)};
				const FVector2 Normal = FVector2{-Direction.Y, Direction.X}.Normalized();
				AppendQuad(Batch, A, B, {Normal.X * A.Size / static_cast<float>(Extent.Width), Normal.Y * A.Size / static_cast<float>(Extent.Height)}, {Normal.X * B.Size / static_cast<float>(Extent.Width), Normal.Y * B.Size / static_cast<float>(Extent.Height)});
			}
			else
			{
				if (A.Position.W <= 0.000001f || std::ranges::any_of(std::views::iota(std::size_t{0}, std::size_t{6}), [&](const std::size_t Plane)
				                                                     {
					                                                     return ClipDistance(A.Position, Plane) < 0;
				                                                     }))
				{
					continue;
				}
				const float HalfHeight = A.Size / static_cast<float>(Extent.Height);
				const FProjectedDebugVertex Bottom = {.Position = {A.Position.X, A.Position.Y - HalfHeight * A.Position.W, A.Position.Z, A.Position.W}, .Size = A.Size, .Color = A.Color};
				const FProjectedDebugVertex Top = {.Position = {A.Position.X, A.Position.Y + HalfHeight * A.Position.W, A.Position.Z, A.Position.W}, .Size = A.Size, .Color = A.Color};
				AppendQuad(Batch, Bottom, Top, {A.Size / static_cast<float>(Extent.Width), 0}, {A.Size / static_cast<float>(Extent.Width), 0});
			}
		}
	}
	for (const auto& Batch : Batches)
	{
		if (!std::ranges::all_of(Batch, [](const FColoredClipVertex& Vertex)
		                         {
			                         return std::ranges::all_of(Vertex.Position, [](const float Value)
			                                                    {
				                                                    return std::isfinite(Value);
			                                                    });
		                         }))
		{
			return std::unexpected(FPresentationError{EPresentationErrorCode::InvalidDescriptor, "Expanded debug positions must be finite"});
		}
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
	std::array<FGraphicsPipelineHandle, 2> DebugPipelines;
	std::array<FBufferHandle, 2> DebugVertices;
	std::array<FBufferHandle, 2> DebugIndices;
	std::array<std::vector<FColoredClipVertex>, 2> DebugBatches;
};

FMeshRenderer::FMeshRenderer(std::unique_ptr<FImplementation> InImplementation)
    : Implementation(std::move(InImplementation))
{
}

FMeshRenderer::~FMeshRenderer() = default;

std::expected<std::unique_ptr<FMeshRenderer>, FPresentationError> FMeshRenderer::Create(IGraphicsDevice& Device, FShaderAsset VertexShader, FShaderAsset FragmentShader, FShaderAsset DebugVertexShader, FShaderAsset DebugFragmentShader)
{
	if (DebugVertexShader.Bytecode.empty() != DebugFragmentShader.Bytecode.empty())
	{
		return std::unexpected(FPresentationError{EPresentationErrorCode::InvalidDescriptor, "Debug drawing requires both vertex and fragment shaders"});
	}
	auto State = std::make_unique<FImplementation>();
	State->Device = &Device;
	auto Vertices = Device.CreateBuffer({"Mesh vertices", sizeof(CubeVertices), EBufferUsage::Vertex});
	auto Indices = Device.CreateBuffer({"Mesh indices", sizeof(CubeIndices), EBufferUsage::Index});
	auto Checker = Device.CreateTexture({"Checkerboard", {CheckerTextureSize, CheckerTextureSize}, ETextureFormat::Rgba8Srgb, false});
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
	if (!DebugVertexShader.Bytecode.empty() || !DebugFragmentShader.Bytecode.empty())
	{
		for (std::size_t Index = 0; Index < State->DebugPipelines.size(); ++Index)
		{
			auto DebugPipeline = Device.CreateGraphicsPipeline({Index == 0 ? "Depth-tested debug primitives" : "Overlay debug primitives", DebugVertexShader, DebugFragmentShader, ETextureFormat::Rgba8Srgb, EGraphicsVertexFormat::ColoredClipPosition, Index == 0});
			if (!DebugPipeline)
			{
				return std::unexpected(DebugPipeline.error());
			}
			State->DebugPipelines[Index] = std::move(*DebugPipeline);
		}
	}
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
	const FMatrix4 Projection = FMatrix4::PerspectiveReversedInfinite(std::numbers::pi_v<float> / 3.0f, static_cast<float>(Extent.Width) / static_cast<float>(Extent.Height), 0.1f);
	const FQuaternion Rotation = FQuaternion::FromAxisAngle({0, 1, 0}, RotationRadians) * FQuaternion::FromAxisAngle({1, 0, 0}, -0.25f);
	return Render(Extent, {FMatrix4::Translation({0, 0, 5}), Projection, FMatrix4::Rotation(Rotation)});
}

std::expected<void, FPresentationError> FMeshRenderer::Render(const FExtent2D Extent, const FMeshRenderView& View, const std::span<const FDebugDrawList> DebugDraw)
{
	if (Extent.IsEmpty())
	{
		return {};
	}
	FImplementation& State = *Implementation;
	IGraphicsDevice& Device = *State.Device;
	const FMatrix4 WorldToClip = View.Projection * View.View;
	const FMatrix4 ModelToClip = WorldToClip * View.Model;
	if (!IsFinite(View.View) || !IsFinite(View.Projection) || !IsFinite(View.Model) || !IsFinite(WorldToClip) || !IsFinite(ModelToClip))
	{
		return std::unexpected(FPresentationError{EPresentationErrorCode::InvalidDescriptor, "Mesh view, projection, and model matrices must be finite"});
	}
	if (const auto Expanded = ExpandDebugDraw(Extent, WorldToClip, DebugDraw, State.DebugBatches); !Expanded)
	{
		return Expanded;
	}
	for (std::size_t Index = 0; Index < State.DebugBatches.size(); ++Index)
	{
		const auto& Batch = State.DebugBatches[Index];
		if (Batch.empty())
		{
			continue;
		}
		if (!State.DebugPipelines[Index])
		{
			return std::unexpected(FPresentationError{EPresentationErrorCode::InvalidState, "Debug drawing requires debug vertex and fragment shaders"});
		}
		const std::size_t VertexBytes = Batch.size() * sizeof(FColoredClipVertex);
		if (!State.DebugVertices[Index] || State.DebugVertices[Index]->GetDescriptor().Size != VertexBytes)
		{
			auto Vertices = Device.CreateBuffer({"Debug vertices", VertexBytes, EBufferUsage::Vertex, EGraphicsVertexFormat::ColoredClipPosition});
			auto Indices = Device.CreateBuffer({"Debug indices", Batch.size() * sizeof(std::uint32_t), EBufferUsage::Index});
			if (!Vertices || !Indices)
			{
				return std::unexpected(!Vertices ? Vertices.error() : Indices.error());
			}
			State.DebugVertices[Index] = std::move(*Vertices);
			State.DebugIndices[Index] = std::move(*Indices);
		}
	}
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
		                    return GraphResult(Device.ClearTargets(State.Color, FrameDepth, {0.035f, 0.035f, 0.035f, 1.0f}));
	                    });
	(void)Graph.AddPass("Textured mesh", {{Color, ERenderGraphAccess::ReadWrite}, {Depth, ERenderGraphAccess::ReadWrite}, {Geometry, ERenderGraphAccess::Read}, {Texture, ERenderGraphAccess::Read}}, [&]
	                    {
		                    return GraphResult(Device.DrawIndexed({State.Pipeline, State.Vertices, State.Indices, State.Checker, State.Color, FrameDepth, ModelToClip.Data(), static_cast<std::uint32_t>(CubeIndices.size())}));
	                    });
	(void)Graph.AddPass("Debug primitives", {{Color, ERenderGraphAccess::ReadWrite}, {Depth, ERenderGraphAccess::Read}}, [&]() -> std::expected<void, FRenderGraphError>
	                    {
		                    for (std::size_t Index = 0; Index < State.DebugBatches.size(); ++Index)
		                    {
			                    const auto& Batch = State.DebugBatches[Index];
			                    if (Batch.empty())
			                    {
				                    continue;
			                    }
			                    std::vector<std::uint32_t> Indices(Batch.size());
			                    std::iota(Indices.begin(), Indices.end(), 0u);
			                    auto Result = Device.WriteBuffer(State.DebugVertices[Index], std::as_bytes(std::span{Batch}));
			                    if (Result)
			                    {
				                    Result = Device.WriteBuffer(State.DebugIndices[Index], std::as_bytes(std::span{Indices}));
			                    }
			                    if (Result)
			                    {
				                    Result = Device.DrawIndexed({State.DebugPipelines[Index], State.DebugVertices[Index], State.DebugIndices[Index], {}, State.Color, FrameDepth, FMatrix4::Identity().Data(), static_cast<std::uint32_t>(Indices.size())});
			                    }
			                    if (!Result)
			                    {
				                    return GraphResult(Result);
			                    }
		                    }
		                    return {};
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
