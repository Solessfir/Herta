#include "Herta/Renderer/MeshRenderer.h"

#include "Herta/Math/Matrix.h"
#include "Herta/RenderGraph/RenderGraph.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <format>
#include <limits>
#include <numeric>
#include <ranges>
#include <utility>

namespace Herta
{
namespace
{
constexpr std::array<std::uint32_t, 6> GridIndices{0, 1, 2, 0, 2, 3};
constexpr float GridHalfExtent = 216.0f;

static_assert(sizeof(FCookedVertex) == sizeof(FMeshVertex) && offsetof(FCookedVertex, Position) == offsetof(FMeshVertex, Position) && offsetof(FCookedVertex, UV) == offsetof(FMeshVertex, UV), "Cooked vertices upload directly as RHI mesh vertices");

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

FCookedModel CreateTexturedCubeModel(FCookedTexture Texture)
{
	struct FFace
	{
		std::array<float, 3> Normal;
		std::array<float, 3> Up;
	};
	constexpr std::array<FFace, 6> Faces{{{{1, 0, 0}, {0, 1, 0}}, {{-1, 0, 0}, {0, 1, 0}}, {{0, 1, 0}, {0, 0, 1}}, {{0, -1, 0}, {0, 0, 1}}, {{0, 0, 1}, {0, 1, 0}}, {{0, 0, -1}, {0, 1, 0}}}};

	FCookedModel Model;
	for (const auto& [Normal, Up] : Faces)
	{
		// A viewer facing the face sees Up as up and cross(-Normal, Up) as right, so these corners run counter-clockwise from outside.
		const std::array<float, 3> Right{Normal[2] * Up[1] - Normal[1] * Up[2], Normal[0] * Up[2] - Normal[2] * Up[0], Normal[1] * Up[0] - Normal[0] * Up[1]};
		const auto Base = static_cast<std::uint32_t>(Model.Vertices.size());
		for (const auto& [X, Y] : {std::pair{-1.0f, 1.0f}, std::pair{-1.0f, -1.0f}, std::pair{1.0f, -1.0f}, std::pair{1.0f, 1.0f}})
		{
			FCookedVertex Vertex{{}, {(X + 1.0f) * 0.5f, (1.0f - Y) * 0.5f}};
			for (std::size_t Axis = 0; Axis < 3; ++Axis)
			{
				Vertex.Position[Axis] = (Normal[Axis] + X * Right[Axis] + Y * Up[Axis]) * 0.5f;
			}
			Model.Vertices.push_back(Vertex);
		}
		Model.Indices.insert(Model.Indices.end(), {Base, Base + 1, Base + 2, Base, Base + 2, Base + 3});
	}
	Model.Sections = {{0, static_cast<std::uint32_t>(Model.Indices.size()), 0}};
	Model.Materials = {{"Cube", 0}};
	Model.Textures.push_back(std::move(Texture));
	return Model;
}

std::expected<std::shared_ptr<const FRenderMesh>, FPresentationError> FRenderMesh::Create(IGraphicsDevice& Device, const FCookedModel& Model, const std::string_view Name)
{
	if (const std::expected<void, FAssetError> Valid = ValidateCookedModel(Model); !Valid)
	{
		return std::unexpected(FPresentationError{EPresentationErrorCode::InvalidDescriptor, Valid.error().Message});
	}

	auto Mesh = std::make_shared<FRenderMesh>();
	auto Vertices = Device.CreateBuffer({std::format("{} vertices", Name), Model.Vertices.size() * sizeof(FMeshVertex), EBufferUsage::Vertex});
	auto Indices = Device.CreateBuffer({std::format("{} indices", Name), Model.Indices.size() * sizeof(std::uint32_t), EBufferUsage::Index});
	if (!Vertices || !Indices)
	{
		return std::unexpected(!Vertices ? Vertices.error() : Indices.error());
	}
	Mesh->Vertices = std::move(*Vertices);
	Mesh->Indices = std::move(*Indices);
	for (std::size_t Index = 0; Index < Model.Textures.size(); ++Index)
	{
		const FCookedTexture& Source = Model.Textures[Index];
		const ETextureFormat Format = Source.ColorSpace == ETextureColorSpace::Srgb ? ETextureFormat::Rgba8Srgb : ETextureFormat::Rgba8;
		auto Texture = Device.CreateTexture({std::format("{} texture {}", Name, Index), {Source.Mips[0].Width, Source.Mips[0].Height}, Format, false, static_cast<std::uint32_t>(Source.Mips.size())});
		if (!Texture)
		{
			return std::unexpected(Texture.error());
		}
		Mesh->Textures.push_back(std::move(*Texture));
	}
	for (const FCookedMeshSection& Section : Model.Sections)
	{
		Mesh->Sections.push_back({Section.FirstIndex, Section.IndexCount, Model.Materials[Section.Material].BaseColorTexture});
	}

	constexpr float Infinity = std::numeric_limits<float>::infinity();
	Mesh->BoundsMinimum = {Infinity, Infinity, Infinity};
	Mesh->BoundsMaximum = {-Infinity, -Infinity, -Infinity};
	for (const FCookedVertex& Vertex : Model.Vertices)
	{
		for (std::size_t Axis = 0; Axis < 3; ++Axis)
		{
			Mesh->BoundsMinimum[Axis] = std::min(Mesh->BoundsMinimum[Axis], Vertex.Position[Axis]);
			Mesh->BoundsMaximum[Axis] = std::max(Mesh->BoundsMaximum[Axis], Vertex.Position[Axis]);
		}
	}

	// Large textures exceed one recording's upload budget, so submit whenever the next write would not fit.
	std::size_t RecordedBytes = 0;
	const auto Upload = [&](const std::size_t Size, const auto& Write) -> std::expected<void, FPresentationError>
	{
		if (RecordedBytes > 0 && Size > MaximumUploadBytesPerRecording - RecordedBytes)
		{
			if (auto Submitted = Device.SubmitCommands(); !Submitted)
			{
				return std::unexpected(Submitted.error());
			}
			if (auto Begun = Device.BeginCommands(); !Begun)
			{
				return Begun;
			}
			RecordedBytes = 0;
		}
		RecordedBytes += Size;
		return Write();
	};

	if (auto Begun = Device.BeginCommands(); !Begun)
	{
		return std::unexpected(Begun.error());
	}
	const auto VertexBytes = std::as_bytes(std::span(Model.Vertices));
	const auto IndexBytes = std::as_bytes(std::span(Model.Indices));
	std::expected<void, FPresentationError> Result = Upload(VertexBytes.size(), [&]
	                                                        {
		                                                        return Device.WriteBuffer(Mesh->Vertices, VertexBytes);
	                                                        });
	if (Result)
	{
		Result = Upload(IndexBytes.size(), [&]
		                {
			                return Device.WriteBuffer(Mesh->Indices, IndexBytes);
		                });
	}
	for (std::size_t Texture = 0; Texture < Model.Textures.size() && Result; ++Texture)
	{
		const std::vector<FCookedTextureMip>& Mips = Model.Textures[Texture].Mips;
		for (std::uint32_t Level = 0; Level < Mips.size() && Result; ++Level)
		{
			Result = Upload(Mips[Level].Pixels.size(), [&]
			                {
				                return Device.WriteTexture(Mesh->Textures[Texture], Level, Mips[Level].Pixels);
			                });
		}
	}
	if (!Result)
	{
		Device.CancelCommands();
		return std::unexpected(Result.error());
	}
	if (auto Submitted = Device.SubmitCommands(); !Submitted)
	{
		return std::unexpected(Submitted.error());
	}
	return Mesh;
}

struct FMeshRenderer::FImplementation
{
	IGraphicsDevice* Device = nullptr;
	FTextureHandle Color;
	FTextureHandle Depth;
	FGraphicsPipelineHandle Pipeline;
	FGraphicsPipelineHandle GridPipeline;
	FBufferHandle GridVertices;
	FBufferHandle GridIndices;
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

std::expected<std::unique_ptr<FMeshRenderer>, FPresentationError> FMeshRenderer::Create(IGraphicsDevice& Device, FShaderAsset VertexShader, FShaderAsset FragmentShader, FShaderAsset DebugVertexShader, FShaderAsset DebugFragmentShader, FShaderAsset GridVertexShader, FShaderAsset GridFragmentShader)
{
	if (DebugVertexShader.Bytecode.empty() != DebugFragmentShader.Bytecode.empty())
	{
		return std::unexpected(FPresentationError{EPresentationErrorCode::InvalidDescriptor, "Debug drawing requires both vertex and fragment shaders"});
	}
	if (GridVertexShader.Bytecode.empty() != GridFragmentShader.Bytecode.empty())
	{
		return std::unexpected(FPresentationError{EPresentationErrorCode::InvalidDescriptor, "World grid requires both vertex and fragment shaders"});
	}
	auto State = std::make_unique<FImplementation>();
	State->Device = &Device;
	auto Pipeline = Device.CreateGraphicsPipeline({"Textured mesh reversed-Z", std::move(VertexShader), std::move(FragmentShader), ETextureFormat::Rgba8Srgb});
	if (!Pipeline)
	{
		return std::unexpected(Pipeline.error());
	}
	State->Pipeline = std::move(*Pipeline);
	if (!GridVertexShader.Bytecode.empty())
	{
		auto GridVertices = Device.CreateBuffer({"World grid vertices", 4 * sizeof(FColoredClipVertex), EBufferUsage::Vertex, EGraphicsVertexFormat::ColoredClipPosition});
		auto GridIndexBuffer = Device.CreateBuffer({"World grid indices", sizeof(GridIndices), EBufferUsage::Index});
		auto GridPipeline = Device.CreateGraphicsPipeline({"World grid", std::move(GridVertexShader), std::move(GridFragmentShader), ETextureFormat::Rgba8Srgb, EGraphicsVertexFormat::ColoredClipPosition, true});
		if (!GridVertices || !GridIndexBuffer || !GridPipeline)
		{
			return std::unexpected(!GridVertices ? GridVertices.error() : !GridIndexBuffer ? GridIndexBuffer.error()
			                                                                               : GridPipeline.error());
		}
		State->GridVertices = std::move(*GridVertices);
		State->GridIndices = std::move(*GridIndexBuffer);
		State->GridPipeline = std::move(*GridPipeline);
	}
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
	if (State->GridIndices)
	{
		Result = Device.WriteBuffer(State->GridIndices, std::as_bytes(std::span{GridIndices}));
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

std::expected<void, FPresentationError> FMeshRenderer::Render(const FExtent2D Extent, const FMeshRenderView& View, const std::span<const FDebugDrawList> DebugDraw)
{
	if (Extent.IsEmpty())
	{
		return {};
	}
	FImplementation& State = *Implementation;
	IGraphicsDevice& Device = *State.Device;
	const FMatrix4 WorldToClip = View.Projection * View.View;
	if (View.Meshes.size() != View.Models.size())
	{
		return std::unexpected(FPresentationError{EPresentationErrorCode::InvalidDescriptor, "Mesh render view requires one mesh entry per model"});
	}
	if (!IsFinite(View.View) || !IsFinite(View.Projection) || !IsFinite(WorldToClip) || !std::ranges::all_of(View.Models, [&](const FMatrix4& Model)
	                                                                                                      {
		                                                                                                      return IsFinite(Model) && IsFinite(WorldToClip * Model);
	                                                                                                      }) || (View.bDrawGrid && (!std::isfinite(View.GridCenter.X) || !std::isfinite(View.GridCenter.Z))))
	{
		return std::unexpected(FPresentationError{EPresentationErrorCode::InvalidDescriptor, "Mesh view, projection, model, and grid center must be finite"});
	}
	if (View.bDrawGrid && !State.GridPipeline)
	{
		return std::unexpected(FPresentationError{EPresentationErrorCode::InvalidState, "World grid requires grid vertex and fragment shaders"});
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
	const auto Texture = Graph.ImportResource("Mesh textures");
	(void)Graph.AddPass("Clear", {{Color, ERenderGraphAccess::Write}, {Depth, ERenderGraphAccess::Write}}, [&]
	                    {
		                    return GraphResult(Device.ClearTargets(State.Color, FrameDepth, {0.035f, 0.035f, 0.035f, 1.0f}));
	                    });
	(void)Graph.AddPass("Textured meshes", {{Color, ERenderGraphAccess::ReadWrite}, {Depth, ERenderGraphAccess::ReadWrite}, {Geometry, ERenderGraphAccess::Read}, {Texture, ERenderGraphAccess::Read}}, [&]() -> std::expected<void, FRenderGraphError>
	                    {
		                    for (std::size_t Index = 0; Index < View.Models.size(); ++Index)
		                    {
			                    if (View.Meshes[Index] == nullptr)
			                    {
				                    continue;
			                    }
			                    const FRenderMesh& Mesh = *View.Meshes[Index];
			                    const auto Transform = (WorldToClip * View.Models[Index]).Data();
			                    for (const FRenderMesh::FSection& Section : Mesh.Sections)
			                    {
				                    const auto Result = Device.DrawIndexed({State.Pipeline, Mesh.Vertices, Mesh.Indices, Mesh.Textures[Section.Texture], State.Color, FrameDepth, Transform, Section.IndexCount, Section.FirstIndex});
				                    if (!Result)
				                    {
					                    return GraphResult(Result);
				                    }
			                    }
		                    }
		                    return {};
	                    });
	if (View.bDrawGrid)
	{
		const auto GridGeometry = Graph.ImportResource("World grid geometry");
		(void)Graph.AddPass("World grid", {{Color, ERenderGraphAccess::ReadWrite}, {Depth, ERenderGraphAccess::Read}, {GridGeometry, ERenderGraphAccess::Read}}, [&]
		                    {
			                    const float CenterX = View.GridCenter.X;
			                    const float CenterZ = View.GridCenter.Z;
			                    const std::array<FVector3, 4> Corners{{{CenterX - GridHalfExtent, 0.0f, CenterZ - GridHalfExtent},
			                                                           {CenterX + GridHalfExtent, 0.0f, CenterZ - GridHalfExtent},
			                                                           {CenterX + GridHalfExtent, 0.0f, CenterZ + GridHalfExtent},
			                                                           {CenterX - GridHalfExtent, 0.0f, CenterZ + GridHalfExtent}}};
			                    std::array<FColoredClipVertex, 4> Vertices{};
			                    for (std::size_t Index = 0; Index < Corners.size(); ++Index)
			                    {
				                    const FVector4 Clip = WorldToClip * FVector4{Corners[Index], 1.0f};
				                    Vertices[Index] = {{Clip.X, Clip.Y, Clip.Z, Clip.W}, {Corners[Index].X, Corners[Index].Z, CenterX, CenterZ}};
			                    }
			                    auto Result = Device.WriteBuffer(State.GridVertices, std::as_bytes(std::span{Vertices}));
			                    if (Result)
			                    {
				                    Result = Device.DrawIndexed({State.GridPipeline, State.GridVertices, State.GridIndices, {}, State.Color, FrameDepth, FMatrix4::Identity().Data(), static_cast<std::uint32_t>(GridIndices.size())});
			                    }
			                    return GraphResult(Result);
		                    });
	}
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
