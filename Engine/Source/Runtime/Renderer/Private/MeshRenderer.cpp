#include "Herta/Renderer/MeshRenderer.h"

#include "Herta/Math/Matrix.h"
#include "Herta/RenderGraph/RenderGraph.h"
#include "Herta/Renderer/EnvironmentLighting.h"

#include <AreaTex.h>
#include <SearchTex.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <format>
#include <limits>
#include <numeric>
#include <ranges>
#include <unordered_map>
#include <utility>

namespace Herta
{
namespace
{
constexpr std::array<std::uint32_t, 6> GridIndices{0, 1, 2, 0, 2, 3};
constexpr float GridHalfExtent = 216.f;

static_assert(sizeof(FCookedVertex) == sizeof(FMeshVertex) && offsetof(FCookedVertex, Position) == offsetof(FMeshVertex, Position) && offsetof(FCookedVertex, UV) == offsetof(FMeshVertex, UV), "Cooked vertices upload directly as RHI mesh vertices");

[[nodiscard]] std::expected<void, FRenderGraphError> GraphResult(const std::expected<void, FPresentationError>& Result)
{
	if (!Result)
	{
		return std::unexpected(FRenderGraphError{.Code = ERenderGraphErrorCode::ExecutionFailed, .Message = Result.error().Message});
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
	float Size = 1.f;
	std::array<float, 4> Color{};
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
	return {.Position = {{Vertex.Position.X + Offset.X * Vertex.Position.W, Vertex.Position.Y + Offset.Y * Vertex.Position.W, Vertex.Position.Z, Vertex.Position.W}}, .Color = Vertex.Color};
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
			return std::unexpected(FPresentationError{.Code = EPresentationErrorCode::InvalidDescriptor, .Message = "Debug draw requires complete primitives and at most 262144 input vertices"});
		}

		InputVertices += List.Vertices.size();
		std::vector<FColoredClipVertex>& Batch = Batches[List.bDepthTest ? 0 : 1];
		for (std::size_t First = 0; First < List.Vertices.size(); First += PrimitiveSize)
		{
			std::array<FProjectedDebugVertex, 3> Projected{};
			for (std::size_t Index = 0; Index < PrimitiveSize; ++Index)
			{
				const FDebugDrawVertex& Vertex = List.Vertices[First + Index];
				Projected[Index] = {.Position = WorldToClip * FVector4{Vertex.Position, 1}, .Size = Vertex.Size, .Color = Vertex.Color};
				const FVector4& Position = Projected[Index].Position;
				if (!std::isfinite(Position.X) || !std::isfinite(Position.Y) || !std::isfinite(Position.Z) || !std::isfinite(Position.W) || (List.Primitive != EDebugPrimitive::Triangles && (!std::isfinite(Vertex.Size) || Vertex.Size <= 0 || Vertex.Size > 4096)) || !std::ranges::all_of(Vertex.Color, [](const float Value)
				{
					return std::isfinite(Value);
				}))
				{
					return std::unexpected(FPresentationError{.Code = EPresentationErrorCode::InvalidDescriptor, .Message = "Debug vertices require finite positions/colors and pixel sizes in (0, 4096]"});
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
			return std::unexpected(FPresentationError{.Code = EPresentationErrorCode::InvalidDescriptor, .Message = "Expanded debug positions must be finite"});
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

	constexpr std::array<FFace, 6> Faces{{{.Normal = {1, 0, 0}, .Up = {0, 1, 0}}, {.Normal = {-1, 0, 0}, .Up = {0, 1, 0}}, {.Normal = {0, 1, 0}, .Up = {0, 0, 1}}, {.Normal = {0, -1, 0}, .Up = {0, 0, 1}}, {.Normal = {0, 0, 1}, .Up = {0, 1, 0}}, {.Normal = {0, 0, -1}, .Up = {0, 1, 0}}}};

	FCookedModel Model;
	for (const auto& [Normal, Up] : Faces)
	{
		// A viewer facing the face sees Up as up and cross(-Normal, Up) as right, so these corners run counter-clockwise from outside.
		const std::array<float, 3> Right{Normal[2] * Up[1] - Normal[1] * Up[2], Normal[0] * Up[2] - Normal[2] * Up[0], Normal[1] * Up[0] - Normal[0] * Up[1]};
		const auto Base = static_cast<std::uint32_t>(Model.Vertices.size());
		for (const auto& [X, Y] : {std::pair{-1.f, 1.f}, std::pair{-1.f, -1.f}, std::pair{1.f, -1.f}, std::pair{1.f, 1.f}})
		{
			FCookedVertex Vertex{.Position = {}, .UV = {(X + 1.f) * 0.5f, (1.f - Y) * 0.5f}, .Normal = Normal, .Tangent = {Right[0], Right[1], Right[2], -1.f}};
			for (std::size_t Axis = 0; Axis < 3; ++Axis)
			{
				Vertex.Position[Axis] = (Normal[Axis] + X * Right[Axis] + Y * Up[Axis]) * 0.5f;
			}

			Model.Vertices.push_back(Vertex);
		}

		Model.Indices.insert(Model.Indices.end(), {Base, Base + 1, Base + 2, Base, Base + 2, Base + 3});
	}

	Model.Sections = {{.FirstIndex = 0, .IndexCount = static_cast<std::uint32_t>(Model.Indices.size()), .Material = 0}};
	Model.Materials = {{.Name = "Cube", .Textures = {0, NoCookedTexture, NoCookedTexture, NoCookedTexture, NoCookedTexture, NoCookedTexture}}};
	Model.Textures.push_back(std::move(Texture));
	return Model;
}

std::expected<std::shared_ptr<const FRenderMesh>, FPresentationError> FRenderMesh::Create(IGraphicsDevice& Device, const FCookedModel& Model, const std::string_view Name)
{
	if (const std::expected<void, FAssetError> Valid = ValidateCookedModel(Model); !Valid)
	{
		return std::unexpected(FPresentationError{.Code = EPresentationErrorCode::InvalidDescriptor, .Message = Valid.error().Message});
	}

	auto Mesh = std::make_shared<FRenderMesh>();
	auto Vertices = Device.CreateBuffer({.Name = std::format("{} vertices", Name), .Size = Model.Vertices.size() * sizeof(FMeshVertex), .Usage = EBufferUsage::Vertex});
	auto Indices = Device.CreateBuffer({.Name = std::format("{} indices", Name), .Size = Model.Indices.size() * sizeof(std::uint32_t), .Usage = EBufferUsage::Index});
	if (!Vertices || !Indices)
	{
		return std::unexpected(!Vertices ? Vertices.error() : Indices.error());
	}

	Mesh->Vertices = std::move(*Vertices);
	Mesh->Indices = std::move(*Indices);
	for (std::size_t Index = 0; Index < Model.Textures.size(); ++Index)
	{
		const FCookedTexture& Source = Model.Textures[Index];
		const ETextureFormat Format = Source.PixelFormat == ETexturePixelFormat::Rgba32Float ? ETextureFormat::Rgba32Float : Source.ColorSpace == ETextureColorSpace::Srgb ? ETextureFormat::Rgba8Srgb
		                                                                                                                                                                   : ETextureFormat::Rgba8;
		auto Texture = Device.CreateTexture({.Name = std::format("{} texture {}", Name, Index), .Extent = {.Width = Source.Mips[0].Width, .Height = Source.Mips[0].Height}, .Format = Format, .bRenderTarget = false, .MipLevels = static_cast<std::uint32_t>(Source.Mips.size())});
		if (!Texture)
		{
			return std::unexpected(Texture.error());
		}

		Mesh->Textures.push_back(std::move(*Texture));
	}

	for (const FCookedMeshSection& Section : Model.Sections)
	{
		Mesh->Sections.push_back({.FirstIndex = Section.FirstIndex, .IndexCount = Section.IndexCount, .Material = Section.Material});
	}

	Mesh->Materials = Model.Materials;

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
	struct FInstanceKey
	{
		bool operator==(const FInstanceKey&) const = default;

		const FRenderMesh* Mesh = nullptr;
		std::vector<const FRenderMaterial*> Materials{};
	};

	struct FInstanceKeyHash
	{
		std::size_t operator()(const FInstanceKey& Key) const
		{
			std::size_t Hash = std::hash<const FRenderMesh*>{}(Key.Mesh);
			for (const FRenderMaterial* Material : Key.Materials)
			{
				Hash ^= std::hash<const FRenderMaterial*>{}(Material) + 0x9e3779b9 + (Hash << 6) + (Hash >> 2);
			}

			return Hash;
		}
	};

	struct FInstanceBatch
	{
		const FRenderMesh* Mesh = nullptr;
		std::uint32_t FirstInstance = 0;
		std::uint32_t InstanceCount = 0;
		std::vector<const FRenderMaterial*> Materials;
	};

	IGraphicsDevice* Device = nullptr;
	FTextureHandle Color;
	FTextureHandle Depth;
	FTextureHandle HdrColor;
	FTextureHandle FogColor;
	FTextureHandle CompositeColor;
	FTextureHandle ToneColor;
	FTextureHandle Edges;
	FTextureHandle Weights;
	FTextureHandle SelectionDepth;
	FTextureHandle SkyView;
	FTextureHandle SkyMultipleScattering;
	// Rayleigh and Mie scales, planet radius, and atmosphere height the table was built from.
	std::array<float, 4> SkyMultipleScatteringKey{};
	FTextureHandle ShadowAtlas;
	FTextureHandle White;
	FTextureHandle EnvironmentDiffuse;
	FTextureHandle EnvironmentSpecular;
	bool bHdrEnvironment = false;
	FTextureHandle SmaaArea;
	FTextureHandle SmaaSearch;
	FBufferHandle FullscreenVertices;
	FBufferHandle FullscreenIndices;
	std::array<FGraphicsPipelineHandle, 8> VisualPipelines;
	FGraphicsPipelineHandle OutlinePipeline;
	FGraphicsPipelineHandle ShadowPipeline;
	FGraphicsPipelineHandle InstancedShadowPipeline;
	FVisualUniforms Uniforms;
	std::vector<FShadowView> Shadows;
	FGraphicsPipelineHandle Pipeline;
	FGraphicsPipelineHandle InstancedPipeline;
	std::unordered_map<std::string, std::array<FGraphicsPipelineHandle, 2>> MaterialPipelines;
	std::uint64_t MaterialShaderGeneration = 0;
	FBufferHandle Instances;
	std::vector<FMeshInstance> InstanceData;
	std::vector<FInstanceBatch> InstanceBatches;
	std::vector<std::size_t> ModelBatches;
	std::unordered_map<FInstanceKey, std::size_t, FInstanceKeyHash> InstanceBatchIndices;
	FGraphicsPipelineHandle GridPipeline;
	FBufferHandle GridVertices;
	FBufferHandle GridIndices;
	std::array<FGraphicsPipelineHandle, 2> DebugPipelines;
	std::array<FBufferHandle, 2> DebugVertices;
	std::array<FBufferHandle, 2> DebugIndices;
	std::array<std::vector<FColoredClipVertex>, 2> DebugBatches;
	std::size_t LastDrawCount = 0;
};

std::span<const FCookedMaterial> FRenderMesh::GetMaterials() const noexcept
{
	return Materials;
}

FMeshRenderer::FMeshRenderer(std::unique_ptr<FImplementation> InImplementation)
    : Implementation(std::move(InImplementation))
{
}

FMeshRenderer::~FMeshRenderer() = default;

std::expected<std::unique_ptr<FMeshRenderer>, FPresentationError> FMeshRenderer::Create(IGraphicsDevice& Device, FShaderAsset VertexShader, FShaderAsset FragmentShader, FShaderAsset DebugVertexShader, FShaderAsset DebugFragmentShader, FShaderAsset GridVertexShader, FShaderAsset GridFragmentShader, FShaderAsset InstancedVertexShader, FVisualShaderSet VisualShaders)
{
	if (DebugVertexShader.Bytecode.empty() != DebugFragmentShader.Bytecode.empty())
	{
		return std::unexpected(FPresentationError{.Code = EPresentationErrorCode::InvalidDescriptor, .Message = "Debug drawing requires both vertex and fragment shaders"});
	}

	if (GridVertexShader.Bytecode.empty() != GridFragmentShader.Bytecode.empty())
	{
		return std::unexpected(FPresentationError{.Code = EPresentationErrorCode::InvalidDescriptor, .Message = "World grid requires both vertex and fragment shaders"});
	}

	auto State = std::make_unique<FImplementation>();
	State->Device = &Device;
	const ETextureFormat MeshColorFormat = VisualShaders.FullscreenVertex.Bytecode.empty() ? ETextureFormat::Rgba8Srgb : ETextureFormat::Rgba16Float;
	if (!InstancedVertexShader.Bytecode.empty())
	{
		auto InstancedPipeline = Device.CreateGraphicsPipeline({.Name = "Instanced PBR reversed-Z", .VertexShader = std::move(InstancedVertexShader), .FragmentShader = FragmentShader, .ColorFormat = MeshColorFormat, .bInstanced = true, .TextureCount = 9, .UniformBufferSize = sizeof(FVisualUniforms)});
		if (!InstancedPipeline)
		{
			return std::unexpected(InstancedPipeline.error());
		}

		State->InstancedPipeline = std::move(*InstancedPipeline);
	}

	auto Pipeline = Device.CreateGraphicsPipeline({.Name = "PBR reversed-Z", .VertexShader = std::move(VertexShader), .FragmentShader = std::move(FragmentShader), .ColorFormat = MeshColorFormat, .TextureCount = 9, .UniformBufferSize = sizeof(FVisualUniforms)});
	if (!Pipeline)
	{
		return std::unexpected(Pipeline.error());
	}

	State->Pipeline = std::move(*Pipeline);
	if (!GridVertexShader.Bytecode.empty())
	{
		auto GridVertices = Device.CreateBuffer({.Name = "World grid vertices", .Size = 4 * sizeof(FColoredClipVertex), .Usage = EBufferUsage::Vertex, .VertexFormat = EGraphicsVertexFormat::ColoredClipPosition});
		auto GridIndexBuffer = Device.CreateBuffer({.Name = "World grid indices", .Size = sizeof(GridIndices), .Usage = EBufferUsage::Index});
		auto GridPipeline = Device.CreateGraphicsPipeline({.Name = "World grid", .VertexShader = std::move(GridVertexShader), .FragmentShader = std::move(GridFragmentShader), .ColorFormat = ETextureFormat::Rgba8Srgb, .VertexFormat = EGraphicsVertexFormat::ColoredClipPosition, .bDepthTest = true, .bDepthWrite = false, .bAlphaBlend = true});
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
			auto DebugPipeline = Device.CreateGraphicsPipeline({.Name = Index == 0 ? "Depth-tested debug primitives" : "Overlay debug primitives", .VertexShader = DebugVertexShader, .FragmentShader = DebugFragmentShader, .ColorFormat = ETextureFormat::Rgba8Srgb, .VertexFormat = EGraphicsVertexFormat::ColoredClipPosition, .bDepthTest = Index == 0, .bDepthWrite = false, .bAlphaBlend = true});
			if (!DebugPipeline)
			{
				return std::unexpected(DebugPipeline.error());
			}

			State->DebugPipelines[Index] = std::move(*DebugPipeline);
		}
	}

	auto White = Device.CreateTexture({.Name = "Default material map", .Extent = {1, 1}, .Format = ETextureFormat::Rgba8});
	auto ShadowAtlas = Device.CreateTexture({.Name = "Shadow atlas", .Extent = {ShadowAtlasWidth, ShadowAtlasHeight}, .Format = ETextureFormat::Depth32, .bRenderTarget = true});
	if (!White || !ShadowAtlas)
	{
		return std::unexpected(!White ? White.error() : ShadowAtlas.error());
	}

	State->White = std::move(*White);
	State->ShadowAtlas = std::move(*ShadowAtlas);
	State->EnvironmentDiffuse = State->White;
	State->EnvironmentSpecular = State->White;
	std::vector<std::byte> AreaPixels;
	std::vector<std::byte> SearchPixels;
	if (!VisualShaders.FullscreenVertex.Bytecode.empty())
	{
		std::array<FShaderAsset, 8> Fragments{std::move(VisualShaders.SkyFragment), std::move(VisualShaders.FogFragment), std::move(VisualShaders.CompositeFragment), std::move(VisualShaders.ToneMapFragment), std::move(VisualShaders.SmaaEdges), std::move(VisualShaders.SmaaWeights), std::move(VisualShaders.SmaaNeighborhood), std::move(VisualShaders.SkyViewFragment)};
		constexpr std::array<std::uint32_t, 8> TextureCounts{2, 3, 3, 1, 1, 3, 2, 1};
		constexpr std::array<std::string_view, 8> Names{"Sky", "Volumetric fog", "Depth-aware fog composite", "Exposure and tone mapping", "SMAA edges", "SMAA weights", "SMAA neighborhood", "Sky view"};
		constexpr std::array Formats{ETextureFormat::Rgba16Float, ETextureFormat::Rgba16Float, ETextureFormat::Rgba16Float, ETextureFormat::Rgba8Srgb, ETextureFormat::Rgba8, ETextureFormat::Rgba8, ETextureFormat::Rgba8Srgb, ETextureFormat::Rgba16Float};
		for (std::size_t Index = 0; Index < Fragments.size(); ++Index)
		{
			auto VisualPipeline = Device.CreateGraphicsPipeline({.Name = std::string(Names[Index]), .VertexShader = VisualShaders.FullscreenVertex, .FragmentShader = std::move(Fragments[Index]), .ColorFormat = Formats[Index],
			    .VertexFormat = EGraphicsVertexFormat::ColoredClipPosition,
			    .bDepthTest = false,
			    .TextureCount = TextureCounts[Index],
			    .UniformBufferSize = sizeof(FVisualUniforms),
			    .bDepthWrite = false,
			    .CullMode = EGraphicsCullMode::None,
			    .bClampSampler = true});
			if (!VisualPipeline)
			{
				return std::unexpected(VisualPipeline.error());
			}

			State->VisualPipelines[Index] = std::move(*VisualPipeline);
		}

		if (!VisualShaders.SelectionOutline.Bytecode.empty())
		{
			auto OutlinePipeline = Device.CreateGraphicsPipeline({.Name = "Selection outline", .VertexShader = VisualShaders.FullscreenVertex, .FragmentShader = std::move(VisualShaders.SelectionOutline), .VertexFormat = EGraphicsVertexFormat::ColoredClipPosition, .bDepthTest = false, .TextureCount = 2, .UniformBufferSize = sizeof(FVisualUniforms), .bDepthWrite = false, .CullMode = EGraphicsCullMode::None, .bAlphaBlend = true});
			if (!OutlinePipeline)
			{
				return std::unexpected(OutlinePipeline.error());
			}

			State->OutlinePipeline = std::move(*OutlinePipeline);
		}

		auto ShadowPipeline = Device.CreateGraphicsPipeline({.Name = "Masked shadow atlas", .VertexShader = std::move(VisualShaders.ShadowVertex), .FragmentShader = VisualShaders.ShadowFragment, .VertexFormat = EGraphicsVertexFormat::ShadowMesh, .TextureCount = 1, .UniformBufferSize = sizeof(FVisualUniforms), .bDepthOnly = true});
		auto InstancedShadowPipeline = Device.CreateGraphicsPipeline({.Name = "Instanced masked shadow atlas", .VertexShader = std::move(VisualShaders.ShadowInstancedVertex), .FragmentShader = std::move(VisualShaders.ShadowFragment), .VertexFormat = EGraphicsVertexFormat::ShadowMesh, .bInstanced = true, .TextureCount = 1, .UniformBufferSize = sizeof(FVisualUniforms), .bDepthOnly = true});
		auto FullscreenVertices = Device.CreateBuffer({.Name = "Fullscreen quad", .Size = 4 * sizeof(FColoredClipVertex), .Usage = EBufferUsage::Vertex, .VertexFormat = EGraphicsVertexFormat::ColoredClipPosition});
		auto FullscreenIndices = Device.CreateBuffer({.Name = "Fullscreen indices", .Size = sizeof(GridIndices), .Usage = EBufferUsage::Index});
		auto Area = Device.CreateTexture({.Name = "SMAA reference area", .Extent = {AREATEX_WIDTH, AREATEX_HEIGHT}, .Format = ETextureFormat::Rgba8});
		auto Search = Device.CreateTexture({.Name = "SMAA reference search", .Extent = {SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT}, .Format = ETextureFormat::Rgba8});
		if (!ShadowPipeline || !InstancedShadowPipeline || !FullscreenVertices || !FullscreenIndices || !Area || !Search)
		{
			return std::unexpected(!ShadowPipeline ? ShadowPipeline.error() : !InstancedShadowPipeline ? InstancedShadowPipeline.error()
			                                                              : !FullscreenVertices        ? FullscreenVertices.error()
			                                                              : !FullscreenIndices         ? FullscreenIndices.error()
			                                                              : !Area                      ? Area.error()
			                                                                                           : Search.error());
		}

		State->ShadowPipeline = std::move(*ShadowPipeline);
		State->InstancedShadowPipeline = std::move(*InstancedShadowPipeline);
		auto SkyView = Device.CreateTexture({.Name = "Sky view", .Extent = {256, 128}, .Format = ETextureFormat::Rgba16Float, .bRenderTarget = true});
		if (!SkyView)
		{
			return std::unexpected(SkyView.error());
		}

		State->SkyView = std::move(*SkyView);
		State->FullscreenVertices = std::move(*FullscreenVertices);
		State->FullscreenIndices = std::move(*FullscreenIndices);
		State->SmaaArea = std::move(*Area);
		State->SmaaSearch = std::move(*Search);
		AreaPixels.resize(AREATEX_WIDTH * AREATEX_HEIGHT * 4);
		for (std::size_t Pixel = 0; Pixel < AreaPixels.size() / 4; ++Pixel)
		{
			AreaPixels[Pixel * 4] = static_cast<std::byte>(areaTexBytes[Pixel * 2]);
			AreaPixels[Pixel * 4 + 1] = static_cast<std::byte>(areaTexBytes[Pixel * 2 + 1]);
			AreaPixels[Pixel * 4 + 3] = std::byte{255};
		}

		SearchPixels.resize(SEARCHTEX_WIDTH * SEARCHTEX_HEIGHT * 4);
		for (std::size_t Pixel = 0; Pixel < SearchPixels.size() / 4; ++Pixel)
		{
			SearchPixels[Pixel * 4] = static_cast<std::byte>(searchTexBytes[Pixel]);
			SearchPixels[Pixel * 4 + 3] = std::byte{255};
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

	const std::array<std::byte, 4> WhitePixel{std::byte{255}, std::byte{255}, std::byte{255}, std::byte{255}};
	if (Result)
	{
		Result = Device.WriteTexture(State->White, 0, WhitePixel);
	}

	if (Result)
	{
		Result = Device.ClearTargets({}, State->ShadowAtlas, {});
	}

	if (Result && State->FullscreenVertices)
	{
		constexpr std::array<FColoredClipVertex, 4> Quad{{{.Position = {-1, 1, 0, 1}, .Color = {0, 0, 0, 0}}, {.Position = {-1, -1, 0, 1}, .Color = {0, 1, 0, 0}}, {.Position = {1, -1, 0, 1}, .Color = {1, 1, 0, 0}}, {.Position = {1, 1, 0, 1}, .Color = {1, 0, 0, 0}}}};
		Result = Device.WriteBuffer(State->FullscreenVertices, std::as_bytes(std::span{Quad}));
		if (Result)
		{
			Result = Device.WriteBuffer(State->FullscreenIndices, std::as_bytes(std::span{GridIndices}));
		}

		if (Result)
		{
			Result = Device.WriteTexture(State->SmaaArea, 0, AreaPixels);
		}

		if (Result)
		{
			Result = Device.WriteTexture(State->SmaaSearch, 0, SearchPixels);
		}
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

std::size_t FMeshRenderer::GetLastDrawCount() const noexcept
{
	return Implementation->LastDrawCount;
}

std::vector<FGpuPassTiming> FMeshRenderer::GetGpuTimings() const
{
	return Implementation->Device->GetGpuTimings();
}

std::size_t FMeshRenderer::GetRenderTargetBytes() const noexcept
{
	const FImplementation& State = *Implementation;
	std::size_t Bytes = 0;
	for (const FTextureHandle* Target : {&State.Color, &State.Depth, &State.HdrColor, &State.FogColor, &State.CompositeColor, &State.ToneColor, &State.Edges, &State.Weights, &State.SelectionDepth, &State.SkyView, &State.ShadowAtlas})
	{
		if (*Target)
		{
			const auto& Descriptor = (*Target)->GetDescriptor();
			Bytes += std::size_t{Descriptor.Extent.Width} * Descriptor.Extent.Height * (Descriptor.Format == ETextureFormat::Rgba16Float ? 8 : 4);
		}
	}

	return Bytes;
}

std::expected<void, FPresentationError> FMeshRenderer::SetEnvironmentLighting(const FEnvironmentLighting& Lighting)
{
	FImplementation& State = *Implementation;
	std::array<FTextureHandle, 2> Uploaded;
	const std::array<const FCookedTexture*, 2> Sources{&Lighting.Diffuse, &Lighting.Specular};
	for (std::size_t Index = 0; Index < Sources.size(); ++Index)
	{
		const FCookedTexture& Source = *Sources[Index];
		if (const auto Valid = ValidateCookedTexture(Source); !Valid)
		{
			return std::unexpected(FPresentationError{.Code = EPresentationErrorCode::InvalidDescriptor, .Message = Valid.error().Message});
		}

		auto Texture = State.Device->CreateTexture({.Name = Index == 0 ? "Diffuse sky irradiance" : "GGX sky radiance", .Extent = {.Width = Source.Mips[0].Width, .Height = Source.Mips[0].Height}, .Format = ETextureFormat::Rgba32Float, .MipLevels = static_cast<std::uint32_t>(Source.Mips.size())});
		if (!Texture)
		{
			return std::unexpected(Texture.error());
		}

		if (auto Begun = State.Device->BeginCommands(); !Begun)
		{
			return Begun;
		}

		State.Device->BeginGpuTiming(Index == 0 ? "Environment irradiance upload" : "Environment radiance upload");

		for (std::uint32_t Mip = 0; Mip < Source.Mips.size(); ++Mip)
		{
			if (auto Written = State.Device->WriteTexture(*Texture, Mip, Source.Mips[Mip].Pixels); !Written)
			{
				State.Device->CancelCommands();
				return Written;
			}
		}

		State.Device->EndGpuTiming();
		if (auto Submitted = State.Device->SubmitCommands(); !Submitted)
		{
			return std::unexpected(Submitted.error());
		}

		Uploaded[Index] = std::move(*Texture);
	}

	State.EnvironmentDiffuse = std::move(Uploaded[0]);
	State.EnvironmentSpecular = std::move(Uploaded[1]);
	State.bHdrEnvironment = Lighting.bFromHdr;
	return {};
}

std::expected<void, FPresentationError> FMeshRenderer::PublishMaterialShader(std::string Key, FShaderAsset Vertex, FShaderAsset InstancedVertex, FShaderAsset Fragment)
{
	const auto Compatible = [](const FShaderAsset& Shader, const EShaderStage Stage, const std::string_view EntryPoint)
	{
		bool bVisualUniforms = false;
		if (Shader.Stage != Stage || Shader.EntryPoint != EntryPoint || (Shader.PushConstantSize != 0 && Shader.PushConstantSize != 128))
		{
			return false;
		}

		for (const auto& Binding : Shader.Bindings)
		{
			if (Binding.Space != 0)
			{
				return false;
			}

			switch (Binding.Type)
			{
				case EShaderBindingType::Texture:
					if (Binding.Binding >= 9)
					{
						return false;
					}

					break;
				case EShaderBindingType::Sampler:
					if (Binding.Binding != 128)
					{
						return false;
					}

					break;
				case EShaderBindingType::ConstantBuffer:
					if (Binding.Binding != 64 || Binding.ByteSize != sizeof(FVisualUniforms) || bVisualUniforms)
					{
						return false;
					}

					bVisualUniforms = true;
					break;
				default:
					return false;
			}
		}

		return bVisualUniforms;
	};

	if (!Compatible(Vertex, EShaderStage::Vertex, "vertexMain") || !Compatible(InstancedVertex, EShaderStage::Vertex, "instancedVertexMain") || !Compatible(Fragment, EShaderStage::Fragment, "fragmentMain"))
	{
		return std::unexpected(FPresentationError{.Code = EPresentationErrorCode::InvalidDescriptor, .Message = "Material shader must use the current VisualShared ABI, entry points, and exact visual uniform layout"});
	}

	FImplementation& State = *Implementation;
	const ETextureFormat Format = State.VisualPipelines[0] ? ETextureFormat::Rgba16Float : ETextureFormat::Rgba8Srgb;
	auto Pipeline = State.Device->CreateGraphicsPipeline({.Name = std::format("Material {}", Key), .VertexShader = std::move(Vertex), .FragmentShader = Fragment, .ColorFormat = Format, .TextureCount = 9, .UniformBufferSize = sizeof(FVisualUniforms)});
	auto Instanced = State.Device->CreateGraphicsPipeline({.Name = std::format("Instanced material {}", Key), .VertexShader = std::move(InstancedVertex), .FragmentShader = std::move(Fragment), .ColorFormat = Format, .bInstanced = true, .TextureCount = 9, .UniformBufferSize = sizeof(FVisualUniforms)});
	if (!Pipeline || !Instanced)
	{
		return std::unexpected(!Pipeline ? Pipeline.error() : Instanced.error());
	}

	if (Key.empty())
	{
		State.Pipeline = std::move(*Pipeline);
		State.InstancedPipeline = std::move(*Instanced);
	}
	else
	{
		State.MaterialPipelines.insert_or_assign(std::move(Key), std::array{std::move(*Pipeline), std::move(*Instanced)});
	}

	++State.MaterialShaderGeneration;
	return {};
}

std::expected<void, FPresentationError> FMeshRenderer::ShareMaterialShaders(const FMeshRenderer& Source)
{
	FImplementation& State = *Implementation;
	const FImplementation& Shared = *Source.Implementation;
	if (State.Device != Shared.Device || static_cast<bool>(State.VisualPipelines[0]) != static_cast<bool>(Shared.VisualPipelines[0]))
	{
		return std::unexpected(FPresentationError{.Code = EPresentationErrorCode::InvalidDescriptor, .Message = "Material shader sharing requires the same graphics device and mesh color format"});
	}

	auto Pipelines = Shared.MaterialPipelines;
	State.MaterialPipelines.swap(Pipelines);
	State.Pipeline = Shared.Pipeline;
	State.InstancedPipeline = Shared.InstancedPipeline;
	return {};
}

std::uint64_t FMeshRenderer::GetMaterialShaderGeneration() const noexcept
{
	return Implementation->MaterialShaderGeneration;
}

std::expected<void, FPresentationError> FMeshRenderer::Render(const FExtent2D Extent, const FMeshRenderView& View, const std::span<const FDebugDrawList> DebugDraw)
{
	Implementation->LastDrawCount = 0;
	if (Extent.IsEmpty())
	{
		return {};
	}

	FImplementation& State = *Implementation;
	IGraphicsDevice& Device = *State.Device;
	const FMatrix4 WorldToClip = View.Projection * View.View;
	if (View.Meshes.size() != View.Models.size())
	{
		return std::unexpected(FPresentationError{.Code = EPresentationErrorCode::InvalidDescriptor, .Message = "Mesh render view requires one mesh entry per model"});
	}

	if (!View.Materials.empty() && View.Materials.size() != View.Models.size())
	{
		return std::unexpected(FPresentationError{.Code = EPresentationErrorCode::InvalidDescriptor, .Message = "Material overrides require one slot list per model"});
	}

	const auto VisualUniforms = BuildVisualUniforms(View.View, View.Projection, Extent, View.Lights, View.Visuals);
	if (!VisualUniforms)
	{
		return std::unexpected(VisualUniforms.error());
	}

	State.Uniforms = *VisualUniforms;
	State.Uniforms.Sky[2] = static_cast<float>(State.EnvironmentSpecular->GetDescriptor().MipLevels - 1);
	State.Uniforms.Sky[3] = State.VisualPipelines[0] ? 1.f : 0.f;
	State.Uniforms.AtmosphereGeometry[2] = State.bHdrEnvironment ? 1.f : 0.f;

	if (View.Visuals.bStudioPreview && View.Lights.empty() && !View.Visuals.Atmosphere && State.VisualPipelines[0])
	{
		// Unlit new levels retain a studio preview until authored lighting exists.
		State.Uniforms.Sky[3] = 2.f;
		State.Uniforms.Controls[2] = 1.f;
	}

	State.Shadows = State.ShadowPipeline && View.Visuals.ShadowQuality != EShadowQuality::Off ? BuildShadowViews(State.Uniforms, View.Lights, View.Projection) : std::vector<FShadowView>{};

	// The table depends only on the atmosphere, so moving the sun or dragging the time of day reuses it.
	const std::array AtmosphereKey{State.Uniforms.Atmosphere[0], State.Uniforms.Atmosphere[1], State.Uniforms.AtmosphereGeometry[0], State.Uniforms.AtmosphereGeometry[1]};
	if (State.VisualPipelines[0] && State.Uniforms.Atmosphere[3] > 0.5f && (!State.SkyMultipleScattering || AtmosphereKey != State.SkyMultipleScatteringKey))
	{
		const FCookedTextureMip Table = BuildSkyMultipleScattering(State.Uniforms);
		auto Texture = Device.CreateTexture({.Name = "Sky multiple scattering", .Extent = {SkyMultipleScatteringSize, SkyMultipleScatteringSize}, .Format = ETextureFormat::Rgba32Float});
		if (!Texture)
		{
			return std::unexpected(Texture.error());
		}

		if (auto Begun = Device.BeginCommands(); !Begun)
		{
			return Begun;
		}

		if (auto Written = Device.WriteTexture(*Texture, 0, Table.Pixels); !Written)
		{
			Device.CancelCommands();
			return Written;
		}

		if (auto Submitted = Device.SubmitCommands(); !Submitted)
		{
			return std::unexpected(Submitted.error());
		}

		State.SkyMultipleScattering = std::move(*Texture);
		State.SkyMultipleScatteringKey = AtmosphereKey;
	}

	if (!IsFinite(View.View) || !IsFinite(View.Projection) || !IsFinite(WorldToClip) || !std::ranges::all_of(View.Models, [&](const FMatrix4& Model)
	{
		return IsFinite(Model) && IsFinite(WorldToClip * Model) && IsFinite(View.View * Model);
	}) || (View.bDrawGrid && (!std::isfinite(View.GridCenter.X) || !std::isfinite(View.GridCenter.Z))))
	{
		return std::unexpected(FPresentationError{.Code = EPresentationErrorCode::InvalidDescriptor, .Message = "Mesh view, projection, model, and grid center must be finite"});
	}

	if (View.bDrawGrid && !State.GridPipeline)
	{
		return std::unexpected(FPresentationError{.Code = EPresentationErrorCode::InvalidState, .Message = "World grid requires grid vertex and fragment shaders"});
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
			return std::unexpected(FPresentationError{.Code = EPresentationErrorCode::InvalidState, .Message = "Debug drawing requires debug vertex and fragment shaders"});
		}

		const std::size_t VertexBytes = Batch.size() * sizeof(FColoredClipVertex);
		if (!State.DebugVertices[Index] || State.DebugVertices[Index]->GetDescriptor().Size != VertexBytes)
		{
			auto Vertices = Device.CreateBuffer({.Name = "Debug vertices", .Size = VertexBytes, .Usage = EBufferUsage::Vertex, .VertexFormat = EGraphicsVertexFormat::ColoredClipPosition});
			auto Indices = Device.CreateBuffer({.Name = "Debug indices", .Size = Batch.size() * sizeof(std::uint32_t), .Usage = EBufferUsage::Index});
			if (!Vertices || !Indices)
			{
				return std::unexpected(!Vertices ? Vertices.error() : Indices.error());
			}

			State.DebugVertices[Index] = std::move(*Vertices);
			State.DebugIndices[Index] = std::move(*Indices);
		}
	}

	if (State.InstancedPipeline)
	{
		State.InstanceBatches.clear();
		State.InstanceBatchIndices.clear();
		State.ModelBatches.resize(View.Models.size());
		std::size_t InstanceCount = 0;
		for (std::size_t Model = 0; Model < View.Meshes.size(); ++Model)
		{
			const FRenderMesh* Mesh = View.Meshes[Model];
			if (!Mesh)
			{
				continue;
			}

			FImplementation::FInstanceKey Key{.Mesh = Mesh};
			if (!View.Materials.empty())
			{
				Key.Materials.assign(View.Materials[Model].begin(), View.Materials[Model].end());
			}

			const auto [Entry, bInserted] = State.InstanceBatchIndices.try_emplace(Key, State.InstanceBatches.size());
			if (bInserted)
			{
				State.InstanceBatches.push_back({.Mesh = Mesh, .Materials = std::move(Key.Materials)});
			}

			State.ModelBatches[Model] = Entry->second;
			++State.InstanceBatches[Entry->second].InstanceCount;
			++InstanceCount;
		}

		if (InstanceCount > MaximumUploadBytesPerRecording / sizeof(FMeshInstance))
		{
			return std::unexpected(FPresentationError{.Code = EPresentationErrorCode::InvalidDescriptor, .Message = "Mesh instances must fit the 64 MiB recording upload budget"});
		}

		std::uint32_t FirstInstance = 0;
		for (FImplementation::FInstanceBatch& Batch : State.InstanceBatches)
		{
			Batch.FirstInstance = FirstInstance;
			FirstInstance += Batch.InstanceCount;
			Batch.InstanceCount = 0;
		}

		State.InstanceData.resize(InstanceCount);
		for (std::size_t Index = 0; Index < View.Models.size(); ++Index)
		{
			if (!View.Meshes[Index])
			{
				continue;
			}

			FImplementation::FInstanceBatch& Batch = State.InstanceBatches[State.ModelBatches[Index]];
			State.InstanceData[Batch.FirstInstance + Batch.InstanceCount++] = {.WorldToClip = (WorldToClip * View.Models[Index]).Data(), .ObjectToView = (View.View * View.Models[Index]).Data()};
		}

		const std::size_t InstanceBytes = InstanceCount * sizeof(FMeshInstance);
		if (InstanceBytes > 0 && (!State.Instances || State.Instances->GetDescriptor().Size != InstanceBytes))
		{
			auto Instances = Device.CreateBuffer({.Name = "Mesh instances", .Size = InstanceBytes, .Usage = EBufferUsage::Vertex, .VertexFormat = EGraphicsVertexFormat::MeshInstance});
			if (!Instances)
			{
				return std::unexpected(Instances.error());
			}

			State.Instances = std::move(*Instances);
		}
	}

	if (!State.Color || State.Color->GetDescriptor().Extent != Extent)
	{
		auto Color = Device.CreateTexture({.Name = "Scene color", .Extent = Extent, .Format = ETextureFormat::Rgba8Srgb, .bRenderTarget = true});
		auto Depth = Device.CreateTexture({.Name = "Scene reversed-Z depth", .Extent = Extent, .Format = ETextureFormat::Depth32, .bRenderTarget = true});
		if (!Color || !Depth)
		{
			return std::unexpected(!Color ? Color.error() : Depth.error());
		}

		State.Color = std::move(*Color);
		State.Depth = std::move(*Depth);
		if (State.VisualPipelines[0])
		{
			const auto Target = [&](const char* Name, const FExtent2D Size, const ETextureFormat Format, FTextureHandle& Destination) -> std::expected<void, FPresentationError>
			{
				auto Texture = Device.CreateTexture({.Name = Name, .Extent = Size, .Format = Format, .bRenderTarget = true});
				if (!Texture)
				{
					return std::unexpected(Texture.error());
				}

				Destination = std::move(*Texture);
				return {};
			};

			std::expected<void, FPresentationError> Created = Target("HDR scene", Extent, ETextureFormat::Rgba16Float, State.HdrColor);
			if (Created)
			{
				Created = Target("HDR fog composite", Extent, ETextureFormat::Rgba16Float, State.CompositeColor);
			}

			if (Created)
			{
				Created = Target("Tone-mapped scene", Extent, ETextureFormat::Rgba8Srgb, State.ToneColor);
			}

			if (Created)
			{
				Created = Target("SMAA edges", Extent, ETextureFormat::Rgba8, State.Edges);
			}

			if (Created)
			{
				Created = Target("SMAA weights", Extent, ETextureFormat::Rgba8, State.Weights);
			}

			if (Created)
			{
				Created = Target("Selection depth", Extent, ETextureFormat::Depth32, State.SelectionDepth);
			}

			if (!Created)
			{
				return Created;
			}
		}
	}

	const float FogScale = std::min({0.25f, 512.f / static_cast<float>(Extent.Width), 512.f / static_cast<float>(Extent.Height)});
	const FExtent2D FogExtent{std::max(1u, static_cast<std::uint32_t>(std::ceil(static_cast<float>(Extent.Width) * FogScale))), std::max(1u, static_cast<std::uint32_t>(std::ceil(static_cast<float>(Extent.Height) * FogScale)))};
	if (State.VisualPipelines[0] && (!State.FogColor || State.FogColor->GetDescriptor().Extent != FogExtent))
	{
		auto Fog = Device.CreateTexture({.Name = "Quarter-resolution fog volume", .Extent = FogExtent, .Format = ETextureFormat::Rgba16Float, .bRenderTarget = true});
		if (!Fog)
		{
			return std::unexpected(Fog.error());
		}

		State.FogColor = std::move(*Fog);
	}

	const FTextureHandle MeshColor = State.HdrColor ? State.HdrColor : State.Color;
	const auto MaterialTextures = [&](const FRenderMesh& Mesh, const FRenderMesh::FSection& Section, const std::span<const FRenderMaterial* const> Overrides)
	{
		std::array<FTextureHandle, 9> Textures;
		std::ranges::fill(Textures, State.White);
		const FRenderMaterial* Override = Section.Material < Overrides.size() ? Overrides[Section.Material] : nullptr;
		const FCookedMaterial& Imported = Mesh.Materials[Section.Material];
		const FMaterialParameters& Material = Override ? Override->Parameters : Imported.Parameters;
		State.Uniforms.Material = {.BaseColor = Material.BaseColor, .Surface = {Material.Metallic, Material.Roughness, Material.NormalStrength, Material.OcclusionStrength}, .Emissive = {Material.Emissive[0], Material.Emissive[1], Material.Emissive[2], Material.EmissiveIntensity}, .UV = {Material.UVScale[0], Material.UVScale[1], Material.UVOffset[0], Material.UVOffset[1]}, .Channels1 = {0, 0, static_cast<float>(Material.BlendMode), Material.AlphaCutoff}};
		for (std::size_t Slot = 0; Slot < MaterialTextureSlotCount; ++Slot)
		{
			const EMaterialChannel Channel = Override ? Override->Channels[Slot] : Imported.Channels[Slot];
			if (Slot < 4)
			{
				State.Uniforms.Material.Channels0[Slot] = static_cast<float>(Channel);
			}
			else
			{
				State.Uniforms.Material.Channels1[Slot - 4] = static_cast<float>(Channel);
			}

			const FTextureHandle Texture = Override ? Override->Textures[Slot] : Imported.Textures[Slot] != NoCookedTexture ? Mesh.Textures[Imported.Textures[Slot]]
			                                                                                                                : FTextureHandle{};
			if (Texture)
			{
				Textures[Slot] = Texture;
				State.Uniforms.Material.TextureFlags[0] += static_cast<float>(1u << Slot);
			}
		}

		Textures[6] = State.EnvironmentDiffuse;
		Textures[7] = State.EnvironmentSpecular;
		Textures[8] = State.ShadowAtlas;
		return Textures;
	};

	const auto TimedPass = [&](const std::string_view Name)
	{
		Device.BeginGpuTiming(Name);
		const auto Finish = [](IGraphicsDevice* Owner)
		{
			Owner->EndGpuTiming();
		};
		return std::unique_ptr<IGraphicsDevice, decltype(Finish)>(&Device, Finish);
	};

	const auto Fullscreen = [&](const std::size_t Pipeline, const FTextureHandle& Destination, const std::span<const FTextureHandle> Sources)
	{
		constexpr std::array Names{"Sky", "Volumetric fog", "Fog composite", "Tone mapping", "SMAA edges", "SMAA weights", "SMAA neighborhood", "Sky view"};
		const auto Timer = TimedPass(Names[Pipeline]);
		if (const auto Cleared = Device.ClearTargets(Destination, {}, {}); !Cleared)
		{
			return Cleared;
		}

		return Device.DrawIndexed({.Pipeline = State.VisualPipelines[Pipeline], .Vertices = State.FullscreenVertices, .Indices = State.FullscreenIndices, .ColorTarget = Destination, .IndexCount = 6, .Textures = Sources, .Uniforms = std::as_bytes(std::span{&State.Uniforms, 1})});
	};

	const auto MaterialPipeline = [&](const FRenderMesh::FSection& Section, const std::span<const FRenderMaterial* const> Overrides, const bool bInstanced)
	{
		const FRenderMaterial* Material = Section.Material < Overrides.size() ? Overrides[Section.Material] : nullptr;
		if (Material && !Material->ShaderKey.empty())
		{
			const auto Custom = State.MaterialPipelines.find(Material->ShaderKey);
			if (Custom != State.MaterialPipelines.end())
			{
				return Custom->second[bInstanced ? 1 : 0];
			}
		}

		return bInstanced ? State.InstancedPipeline : State.Pipeline;
	};

	FRenderGraph Graph;
	const auto Color = Graph.ImportResource("Scene color");
	FTextureHandle FrameDepth;
	const auto Depth = Graph.CreateResource("Scene depth", [&]() -> std::expected<void, FRenderGraphError>
	{
		FrameDepth = State.Depth;
		return {};
	}, [&]
	{
		FrameDepth.reset();
	});

	const auto Geometry = Graph.ImportResource("Uploaded mesh");
	const auto Texture = Graph.ImportResource("Mesh textures");
	const auto Shadow = Graph.ImportResource("Shadow atlas");

	Graph.AddPass("Shadow atlas", {{.Resource = Shadow, .Access = ERenderGraphAccess::Write}, {.Resource = Geometry, .Access = ERenderGraphAccess::Read}, {.Resource = Texture, .Access = ERenderGraphAccess::Read}}, [&]() -> std::expected<void, FRenderGraphError>
	{
		if (State.Shadows.empty())
		{
			return {};
		}

		const auto Timer = TimedPass("Shadows");

		if (const auto Cleared = Device.ClearTargets({}, State.ShadowAtlas, {}); !Cleared)
		{
			return GraphResult(Cleared);
		}

		if (State.InstancedShadowPipeline && State.InstancedPipeline && !State.InstanceData.empty())
		{
			if (const auto Written = Device.WriteBuffer(State.Instances, std::as_bytes(std::span{State.InstanceData})); !Written)
			{
				return GraphResult(Written);
			}
		}

		for (const FShadowView& ShadowView : State.Shadows)
		{
			if (State.InstancedShadowPipeline && State.InstancedPipeline)
			{
				const auto Transform = (ShadowView.WorldToClip * FMatrix4(State.Uniforms.ViewToWorld)).Data();
				for (const FImplementation::FInstanceBatch& Batch : State.InstanceBatches)
				{
					const FRenderMesh& Mesh = *Batch.Mesh;
					for (const FRenderMesh::FSection& Section : Mesh.Sections)
					{
						const auto Textures = MaterialTextures(Mesh, Section, Batch.Materials);
						const auto Draw = Device.DrawIndexed({.Pipeline = State.InstancedShadowPipeline, .Vertices = Mesh.Vertices, .Indices = Mesh.Indices, .Texture = Textures[0], .DepthTarget = State.ShadowAtlas, .WorldToClip = Transform, .IndexCount = Section.IndexCount, .FirstIndex = Section.FirstIndex, .Instances = State.Instances, .InstanceCount = Batch.InstanceCount, .FirstInstance = Batch.FirstInstance, .Uniforms = std::as_bytes(std::span{&State.Uniforms, 1}), .Viewport = ShadowView.Viewport});
						if (!Draw)
						{
							return GraphResult(Draw);
						}

						++State.LastDrawCount;
					}
				}
			}
			else
			{
				for (std::size_t Model = 0; Model < View.Models.size(); ++Model)
				{
					if (!View.Meshes[Model])
					{
						continue;
					}

					const FRenderMesh& Mesh = *View.Meshes[Model];
					const auto Overrides = View.Materials.empty() ? std::span<const FRenderMaterial* const>{} : View.Materials[Model];
					for (const FRenderMesh::FSection& Section : Mesh.Sections)
					{
						const auto Textures = MaterialTextures(Mesh, Section, Overrides);
						const auto Draw = Device.DrawIndexed({.Pipeline = State.ShadowPipeline, .Vertices = Mesh.Vertices, .Indices = Mesh.Indices, .Texture = Textures[0], .DepthTarget = State.ShadowAtlas, .WorldToClip = (ShadowView.WorldToClip * View.Models[Model]).Data(), .IndexCount = Section.IndexCount, .FirstIndex = Section.FirstIndex, .Uniforms = std::as_bytes(std::span{&State.Uniforms, 1}), .Viewport = ShadowView.Viewport});
						if (!Draw)
						{
							return GraphResult(Draw);
						}

						++State.LastDrawCount;
					}
				}
			}
		}

		return {};
	});

	Graph.AddPass("Clear", {{.Resource = Color, .Access = ERenderGraphAccess::Write}, {.Resource = Depth, .Access = ERenderGraphAccess::Write}}, [&]
	{
		auto Result = Device.ClearTargets(MeshColor, FrameDepth, {0.035f, 0.035f, 0.035f, 1.f});
		if (Result && State.VisualPipelines[0])
		{
			const std::array SkySources{State.SkyMultipleScattering ? State.SkyMultipleScattering : State.White};
			Result = Fullscreen(7, State.SkyView, SkySources);
		}

		if (Result && State.VisualPipelines[0])
		{
			const std::array Sources{State.EnvironmentSpecular, State.SkyView};
			Result = Fullscreen(0, MeshColor, Sources);
		}

		return GraphResult(Result);
	});

	Graph.AddPass("PBR meshes", {{.Resource = Color, .Access = ERenderGraphAccess::ReadWrite}, {.Resource = Depth, .Access = ERenderGraphAccess::ReadWrite}, {.Resource = Geometry, .Access = ERenderGraphAccess::Read}, {.Resource = Texture, .Access = ERenderGraphAccess::Read}, {.Resource = Shadow, .Access = ERenderGraphAccess::Read}}, [&]() -> std::expected<void, FRenderGraphError>
	{
		const auto Timer = TimedPass("PBR meshes");
		if (State.InstancedPipeline)
		{
			if (!State.InstanceData.empty())
			{
				if (const auto Written = Device.WriteBuffer(State.Instances, std::as_bytes(std::span{State.InstanceData})); !Written)
				{
					return GraphResult(Written);
				}
			}

			for (const FImplementation::FInstanceBatch& Batch : State.InstanceBatches)
			{
				const FRenderMesh& Mesh = *Batch.Mesh;
				for (const FRenderMesh::FSection& Section : Mesh.Sections)
				{
					const auto Textures = MaterialTextures(Mesh, Section, Batch.Materials);
					const auto Result = Device.DrawIndexed({.Pipeline = MaterialPipeline(Section, Batch.Materials, true), .Vertices = Mesh.Vertices, .Indices = Mesh.Indices, .ColorTarget = MeshColor, .DepthTarget = FrameDepth, .IndexCount = Section.IndexCount, .FirstIndex = Section.FirstIndex, .Instances = State.Instances, .InstanceCount = Batch.InstanceCount, .FirstInstance = Batch.FirstInstance, .Textures = Textures, .Uniforms = std::as_bytes(std::span{&State.Uniforms, 1})});
					if (!Result)
					{
						return GraphResult(Result);
					}

					++State.LastDrawCount;
				}
			}

			return {};
		}

		for (std::size_t Index = 0; Index < View.Models.size(); ++Index)
		{
			if (View.Meshes[Index] == nullptr)
			{
				continue;
			}

			const FRenderMesh& Mesh = *View.Meshes[Index];
			const auto Transform = (WorldToClip * View.Models[Index]).Data();
			const auto ObjectToView = (View.View * View.Models[Index]).Data();
			for (const FRenderMesh::FSection& Section : Mesh.Sections)
			{
				const auto Textures = MaterialTextures(Mesh, Section, View.Materials.empty() ? std::span<const FRenderMaterial* const>{} : View.Materials[Index]);
				const auto Result = Device.DrawIndexed({.Pipeline = MaterialPipeline(Section, View.Materials.empty() ? std::span<const FRenderMaterial* const>{} : View.Materials[Index], false), .Vertices = Mesh.Vertices, .Indices = Mesh.Indices, .ColorTarget = MeshColor, .DepthTarget = FrameDepth, .WorldToClip = Transform, .IndexCount = Section.IndexCount, .FirstIndex = Section.FirstIndex, .ObjectToView = ObjectToView, .Textures = Textures, .Uniforms = std::as_bytes(std::span{&State.Uniforms, 1})});
				if (!Result)
				{
					return GraphResult(Result);
				}

				++State.LastDrawCount;
			}
		}

		return {};
	});

	if (State.VisualPipelines[0])
	{
		Graph.AddPass("Fog, tone mapping and SMAA", {{.Resource = Color, .Access = ERenderGraphAccess::ReadWrite}, {.Resource = Depth, .Access = ERenderGraphAccess::Read}, {.Resource = Shadow, .Access = ERenderGraphAccess::Read}}, [&]() -> std::expected<void, FRenderGraphError>
		{
			FTextureHandle ToneSource = State.HdrColor;
			if (State.Uniforms.Fog[0] > 0.f)
			{
				const std::array FogSources{State.Depth, State.ShadowAtlas, State.EnvironmentDiffuse};
				if (const auto Result = Fullscreen(1, State.FogColor, FogSources); !Result)
				{
					return GraphResult(Result);
				}

				const std::array CompositeSources{State.HdrColor, State.FogColor, State.Depth};
				if (const auto Result = Fullscreen(2, State.CompositeColor, CompositeSources); !Result)
				{
					return GraphResult(Result);
				}

				ToneSource = State.CompositeColor;
			}

			const bool bSmaa = View.Visuals.AntiAliasing != EAntiAliasing::Off;
			const std::array ToneSources{ToneSource};
			if (const auto Result = Fullscreen(3, bSmaa ? State.ToneColor : State.Color, ToneSources); !Result)
			{
				return GraphResult(Result);
			}

			if (bSmaa)
			{
				constexpr std::array<float, 4> Thresholds{0.15f, 0.1f, 0.1f, 0.05f};
				constexpr std::array<float, 4> SearchSteps{4, 8, 16, 32};
				constexpr std::array<float, 4> DiagonalSteps{0, 0, 8, 16};
				const std::size_t Preset = static_cast<std::size_t>(View.Visuals.AntiAliasing) - 1;
				State.Uniforms.Material.BaseColor[0] = Thresholds[Preset];
				State.Uniforms.Material.TextureFlags = {0, SearchSteps[Preset], DiagonalSteps[Preset], Preset < 2 ? 0.f : 25.f};
				if (const auto Result = Device.ClearTargets(State.Edges, {}, {}); !Result)
				{
					return GraphResult(Result);
				}

				const std::array EdgeSources{State.ToneColor};
				if (const auto Result = Fullscreen(4, State.Edges, EdgeSources); !Result)
				{
					return GraphResult(Result);
				}

				const std::array WeightSources{State.Edges, State.SmaaArea, State.SmaaSearch};
				if (const auto Result = Fullscreen(5, State.Weights, WeightSources); !Result)
				{
					return GraphResult(Result);
				}

				const std::array NeighborSources{State.ToneColor, State.Weights};
				if (const auto Result = Fullscreen(6, State.Color, NeighborSources); !Result)
				{
					return GraphResult(Result);
				}
			}

			return {};
		});
	}

	if (View.bDrawGrid)
	{
		const auto GridGeometry = Graph.ImportResource("World grid geometry");
		Graph.AddPass("World grid", {{.Resource = Color, .Access = ERenderGraphAccess::ReadWrite}, {.Resource = Depth, .Access = ERenderGraphAccess::Read}, {.Resource = GridGeometry, .Access = ERenderGraphAccess::Read}}, [&]
		{
			const float CenterX = View.GridCenter.X;
			const float CenterZ = View.GridCenter.Z;
			const std::array<FVector3, 4> Corners{{{CenterX - GridHalfExtent, 0.f, CenterZ - GridHalfExtent}, {CenterX + GridHalfExtent, 0.f, CenterZ - GridHalfExtent}, {CenterX + GridHalfExtent, 0.f, CenterZ + GridHalfExtent}, {CenterX - GridHalfExtent, 0.f, CenterZ + GridHalfExtent}}};

			std::array<FColoredClipVertex, 4> Vertices{};
			for (std::size_t Index = 0; Index < Corners.size(); ++Index)
			{
				const FVector4 Clip = WorldToClip * FVector4{Corners[Index], 1.f};
				Vertices[Index] = {.Position = {Clip.X, Clip.Y, Clip.Z, Clip.W}, .Color = {Corners[Index].X, Corners[Index].Z, CenterX, CenterZ}};
			}

			auto Result = Device.WriteBuffer(State.GridVertices, std::as_bytes(std::span{Vertices}));
			if (Result)
			{
				Result = Device.DrawIndexed({.Pipeline = State.GridPipeline, .Vertices = State.GridVertices, .Indices = State.GridIndices, .Texture = {}, .ColorTarget = State.Color, .DepthTarget = FrameDepth, .WorldToClip = FMatrix4::Identity().Data(), .IndexCount = static_cast<std::uint32_t>(GridIndices.size())});
				if (Result)
				{
					++State.LastDrawCount;
				}
			}

			return GraphResult(Result);
		});
	}

	if (State.OutlinePipeline && State.ShadowPipeline && !View.Selected.empty())
	{
		const auto Selection = Graph.ImportResource("Selection depth");
		Graph.AddPass("Selection outline", {{.Resource = Selection, .Access = ERenderGraphAccess::ReadWrite}, {.Resource = Color, .Access = ERenderGraphAccess::ReadWrite}, {.Resource = Depth, .Access = ERenderGraphAccess::Read}, {.Resource = Geometry, .Access = ERenderGraphAccess::Read}, {.Resource = Texture, .Access = ERenderGraphAccess::Read}}, [&]() -> std::expected<void, FRenderGraphError>
		{
			const auto Timer = TimedPass("Selection outline");
			if (const auto Cleared = Device.ClearTargets({}, State.SelectionDepth, {}); !Cleared)
			{
				return GraphResult(Cleared);
			}

			// The depth-only shadow pipeline rasterizes the selected meshes, including alpha cutouts, without another mesh shader.
			for (const std::size_t Index : View.Selected)
			{
				if (Index >= View.Models.size() || Index >= View.Meshes.size() || View.Meshes[Index] == nullptr)
				{
					continue;
				}

				const FRenderMesh& Mesh = *View.Meshes[Index];
				const auto Transform = (WorldToClip * View.Models[Index]).Data();
				for (const FRenderMesh::FSection& Section : Mesh.Sections)
				{
					const auto Textures = MaterialTextures(Mesh, Section, View.Materials.empty() ? std::span<const FRenderMaterial* const>{} : View.Materials[Index]);
					const auto Draw = Device.DrawIndexed({.Pipeline = State.ShadowPipeline, .Vertices = Mesh.Vertices, .Indices = Mesh.Indices, .Texture = Textures[0], .DepthTarget = State.SelectionDepth, .WorldToClip = Transform, .IndexCount = Section.IndexCount, .FirstIndex = Section.FirstIndex, .Uniforms = std::as_bytes(std::span{&State.Uniforms, 1})});
					if (!Draw)
					{
						return GraphResult(Draw);
					}

					++State.LastDrawCount;
				}
			}

			const std::array Sources{State.SelectionDepth, State.Depth};
			return GraphResult(Device.DrawIndexed({.Pipeline = State.OutlinePipeline, .Vertices = State.FullscreenVertices, .Indices = State.FullscreenIndices, .ColorTarget = State.Color, .IndexCount = 6, .Textures = Sources, .Uniforms = std::as_bytes(std::span{&State.Uniforms, 1})}));
		});
	}

	Graph.AddPass("Debug primitives", {{.Resource = Color, .Access = ERenderGraphAccess::ReadWrite}, {.Resource = Depth, .Access = ERenderGraphAccess::Read}}, [&]() -> std::expected<void, FRenderGraphError>
	{
		for (std::size_t Index = 0; Index < State.DebugBatches.size(); ++Index)
		{
			const auto& Batch = State.DebugBatches[Index];
			if (Batch.empty())
			{
				continue;
			}

			std::vector<std::uint32_t> Indices(Batch.size());
			std::ranges::iota(Indices, 0u);
			auto Result = Device.WriteBuffer(State.DebugVertices[Index], std::as_bytes(std::span{Batch}));
			if (Result)
			{
				Result = Device.WriteBuffer(State.DebugIndices[Index], std::as_bytes(std::span{Indices}));
			}

			if (Result)
			{
				Result = Device.DrawIndexed({.Pipeline = State.DebugPipelines[Index], .Vertices = State.DebugVertices[Index], .Indices = State.DebugIndices[Index], .Texture = {}, .ColorTarget = State.Color, .DepthTarget = Index == 0 ? FrameDepth : FTextureHandle{}, .WorldToClip = FMatrix4::Identity().Data(), .IndexCount = static_cast<std::uint32_t>(Indices.size())});
			}

			if (!Result)
			{
				return GraphResult(Result);
			}

			++State.LastDrawCount;
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
		return std::unexpected(FPresentationError{.Code = EPresentationErrorCode::CommandSubmissionFailed, .Message = Execution.error().Message});
	}

	auto Submission = Device.SubmitCommands();
	if (!Submission)
	{
		return std::unexpected(Submission.error());
	}

	return {};
}
}
