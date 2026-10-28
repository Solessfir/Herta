#include "Herta/Renderer/MeshRenderer.h"

#include "Herta/Math/Matrix.h"
#include "Herta/RenderGraph/RenderGraph.h"
#include "Herta/Renderer/EnvironmentLighting.h"

#include <AreaTex.h>
#include <LtcTables.h>
#include <SearchTex.h>

#include <algorithm>
#include <array>
#include <bit>
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

// Rounds a finite value in half's normal range or below to the nearest half, flushing values too small for a normal half to zero.
[[nodiscard]] std::uint16_t ToHalf(const float Value)
{
	const auto Bits = std::bit_cast<std::uint32_t>(Value);
	const std::uint32_t Sign = (Bits >> 16u) & 0x8000u;
	const int Exponent = static_cast<int>((Bits >> 23u) & 0xFFu) - 127 + 15;
	if (Exponent <= 0)
	{
		return static_cast<std::uint16_t>(Sign);
	}

	const std::uint32_t Rounded = ((static_cast<std::uint32_t>(Exponent) << 10u) | ((Bits >> 13u) & 0x3FFu)) + ((Bits >> 12u) & 1u);
	return static_cast<std::uint16_t>(Sign | std::min(Rounded, 0x7BFFu));
}

constexpr float GridHalfExtent = 216.f;
// Material maps, environment, shadow atlas, and the two Rect light LTC tables, matching TexturedMesh.slang.
constexpr std::uint32_t MeshTextureCount = 13;
// Lights, then their cluster masks, as in VisualLights.slangh.
constexpr std::uint32_t MeshBufferCount = 2;

static_assert(sizeof(FCookedVertex) == sizeof(FMeshVertex) && offsetof(FCookedVertex, Position) == offsetof(FMeshVertex, Position) && offsetof(FCookedVertex, UV) == offsetof(FMeshVertex, UV), "Cooked vertices upload directly as RHI mesh vertices");

[[nodiscard]] std::expected<void, FRenderGraphError> GraphResult(const std::expected<void, FPresentationError>& Result)
{
	if (!Result)
	{
		return std::unexpected(FRenderGraphError{.Code = ERenderGraphErrorCode::ExecutionFailed, .Message = Result.error().Message});
	}

	return {};
}

// Replaces Key with the concatenated Parts and reports whether they differed.
[[nodiscard]] bool UpdateKey(std::vector<std::byte>& Key, const std::initializer_list<std::span<const std::byte>> Parts)
{
	std::size_t Offset = 0;
	bool bSame = true;
	for (const std::span<const std::byte> Part : Parts)
	{
		bSame = bSame && Offset + Part.size() <= Key.size() && std::ranges::equal(Part, std::span(Key).subspan(Offset, Part.size()));
		Offset += Part.size();
	}

	if (bSame && Offset == Key.size())
	{
		return false;
	}

	Key.clear();
	for (const std::span<const std::byte> Part : Parts)
	{
		Key.insert(Key.end(), Part.begin(), Part.end());
	}

	return true;
}

template <typename T>
[[nodiscard]] std::span<const std::byte> KeyBytes(const T& Value)
{
	return std::as_bytes(std::span{&Value, 1});
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
	Mesh->UpdateBounds(Model.Vertices);

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

namespace
{
struct FDisplayShaders
{
	FShaderAsset FullscreenVertex;
	FShaderAsset ToneMap;
	FShaderAsset SmaaNeighborhood;
	FShaderAsset Outline;
	FShaderAsset GridVertex;
	FShaderAsset GridFragment;
	FShaderAsset DebugVertex;
	FShaderAsset DebugFragment;
};

// Passes that write the display target, for one display format.
// Matches FOcclusionCullUniforms in Compute/OcclusionCull.slang.
struct alignas(16) FOcclusionCullUniforms
{
	std::array<float, 16> WorldToClip{};
	// Candidates, draws, phase (0 early, 1 late), and depth hierarchy mips.
	std::array<std::uint32_t, 4> Counts{};
	std::array<float, 4> Hierarchy{};
};

// Matches FDepthHierarchyUniforms in Compute/DepthHierarchy.slang.
struct alignas(16) FDepthHierarchyUniforms
{
	std::array<std::uint32_t, 4> Settings{};
};

// One depth hierarchy dispatch writes this many consecutive levels.
constexpr std::uint32_t DepthHierarchyLevelsPerDispatch = 5;

// Matches the candidate layout in Compute/OcclusionCull.slang.
struct FOcclusionCandidate
{
	std::array<float, 3> Center{};
	std::uint32_t Model = 0;
	std::array<float, 3> Extent{};
	std::uint32_t FirstDraw = 0;
	std::uint32_t SectionCount = 0;
};

static_assert(sizeof(FOcclusionCandidate) == 9 * sizeof(std::uint32_t));
// OcclusionCull.slang copies instances as whole uints.
static_assert(sizeof(FMeshInstance) == 48 * sizeof(std::uint32_t));

// Indirect argument lists, in buffer order: drawn before the depth hierarchy, drawn after it, and everything visible for shading.
enum class EOcclusionList : std::uint32_t
{
	Early,
	Late,
	Final,
};

constexpr std::uint32_t OcclusionListCount = 3;
constexpr std::uint32_t IndirectDrawWords = 5;

struct FDisplayPipelines
{
	FGraphicsPipelineHandle ToneMap;
	FGraphicsPipelineHandle SmaaNeighborhood;
	FGraphicsPipelineHandle Outline;
	FGraphicsPipelineHandle Grid;
	std::array<FGraphicsPipelineHandle, 2> Debug;
};

[[nodiscard]] std::expected<FDisplayPipelines, FPresentationError> CreateDisplayPipelines(IGraphicsDevice& Device, const FDisplayShaders& Shaders, const ETextureFormat Format)
{
	const std::string_view Suffix = Format == ETextureFormat::Rgba8Srgb ? "" : " (HDR)";
	FDisplayPipelines Pipelines;
	const auto Create = [&](FGraphicsPipelineHandle& Destination, FGraphicsPipelineDescriptor Descriptor) -> std::expected<void, FPresentationError>
	{
		if (Descriptor.VertexShader.Bytecode.empty() && Descriptor.FragmentShader.Bytecode.empty())
		{
			return {};
		}

		Descriptor.Name += Suffix;
		auto Pipeline = Device.CreateGraphicsPipeline(Descriptor);
		if (!Pipeline)
		{
			return std::unexpected(Pipeline.error());
		}

		Destination = std::move(*Pipeline);
		return {};
	};

	const auto Fullscreen = [&](std::string Name, const FShaderAsset& Fragment, const bool bAlphaBlend, const std::uint32_t TextureCount = 2)
	{
		return FGraphicsPipelineDescriptor{.Name = std::move(Name), .VertexShader = Shaders.FullscreenVertex, .FragmentShader = Fragment, .ColorFormat = Format, .VertexFormat = EGraphicsVertexFormat::ColoredClipPosition, .bDepthTest = false, .TextureCount = TextureCount, .UniformBufferSize = VisualUniformBytes, .bDepthWrite = false, .CullMode = EGraphicsCullMode::None, .bAlphaBlend = bAlphaBlend, .bClampSampler = !bAlphaBlend};
	};

	std::expected<void, FPresentationError> Result = Create(Pipelines.ToneMap, Fullscreen("Fog, exposure and tone mapping", Shaders.ToneMap, false, 5));
	if (Result)
	{
		Result = Create(Pipelines.SmaaNeighborhood, Fullscreen("SMAA neighborhood", Shaders.SmaaNeighborhood, false));
	}

	if (Result && !Shaders.Outline.Bytecode.empty())
	{
		Result = Create(Pipelines.Outline, Fullscreen("Selection outline", Shaders.Outline, true));
	}

	if (Result)
	{
		Result = Create(Pipelines.Grid, {.Name = "World grid", .VertexShader = Shaders.GridVertex, .FragmentShader = Shaders.GridFragment, .ColorFormat = Format, .VertexFormat = EGraphicsVertexFormat::ColoredClipPosition, .bDepthTest = true, .bDepthWrite = false, .bAlphaBlend = true});
	}

	for (std::size_t Index = 0; Index < Pipelines.Debug.size() && Result; ++Index)
	{
		Result = Create(Pipelines.Debug[Index], {.Name = Index == 0 ? "Depth-tested debug primitives" : "Overlay debug primitives", .VertexShader = Shaders.DebugVertex, .FragmentShader = Shaders.DebugFragment, .ColorFormat = Format, .VertexFormat = EGraphicsVertexFormat::ColoredClipPosition, .bDepthTest = Index == 0, .bDepthWrite = false, .bAlphaBlend = true});
	}

	if (!Result)
	{
		return std::unexpected(Result.error());
	}

	return Pipelines;
}
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
		std::vector<const FRenderMaterial*> Materials;
	};

	struct FInstanceDraw
	{
		std::size_t Batch = 0;
		std::uint32_t FirstInstance = 0;
		std::uint32_t InstanceCount = 0;
	};

	struct FModelBounds
	{
		FVector3 Center;
		FVector3 Extent;
	};

	IGraphicsDevice* Device = nullptr;
	FTextureHandle Color;
	FTextureHandle Depth;
	FTextureHandle HdrColor;
	FTextureHandle FogColor;
	FTextureHandle ToneColor;
	FTextureHandle Edges;
	FTextureHandle Weights;
	FTextureHandle SelectionDepth;
	FTextureHandle SkyView;
	FTextureHandle SkyMultipleScattering;
	FTextureHandle ToneMapLut;
	// Depth prefilter, bitmask GTAO, and denoise. The view depth chain feeds both later passes.
	std::array<FComputePipelineHandle, 3> GtaoPipelines;
	FTextureHandle GtaoViewDepth;
	FTextureHandle GtaoNoisy;
	FTextureHandle GtaoOcclusion;
	// Filmic SMAA: the edge pass lists edge pixels and counts weight groups, then the weight pass runs indirectly over the list.
	std::array<FComputePipelineHandle, 2> SmaaPipelines;
	FBufferHandle SmaaEdgePixels;
	FBufferHandle SmaaArguments;
	// Two-phase occlusion culling of the camera's instances: the depth hierarchy, then the cull pass.
	std::array<FComputePipelineHandle, 2> OcclusionPipelines;
	FTextureHandle DepthHierarchy;
	FBufferHandle OcclusionCandidates;
	FBufferHandle CulledInstances;
	FBufferHandle OcclusionArguments;
	// Whether each model passed the late test last frame. Model indices may change meaning between frames; that only costs work, never correctness.
	FBufferHandle OcclusionHistory;
	bool bResetOcclusionHistory = false;
	bool bOcclusionCulling = false;
	std::vector<FOcclusionCandidate> CandidateData;
	std::vector<std::uint32_t> OcclusionArgumentData;
	// One indirect draw per section of each camera batch, in ViewDraws[0] order.
	std::uint32_t OcclusionDrawCount = 0;
	// Object motion from the instanced depth prepass, then camera motion for every pixel it left unknown.
	FGraphicsPipelineHandle InstancedVelocityPrepassPipeline;
	FComputePipelineHandle MotionVectorPipeline;
	FTextureHandle MotionVectors;
	bool bMotionVectors = false;
	// The camera last frame, valid while the extent stays the same.
	FMatrix4 PreviousWorldToClip;
	FExtent2D PreviousExtent{};
	FComputePipelineHandle LightClusterPipeline;
	FBufferHandle Lights;
	FBufferHandle LightClusters;
	// One mask with every light bit set, bound when there are no clusters.
	FBufferHandle AllLights;
	// Adaptation reads last frame's exposure and writes this frame's, alternating between the two.
	std::array<FTextureHandle, 2> ExposureStates;
	FComputePipelineHandle ExposurePipeline;
	std::size_t ExposureStateIndex = 0;
	bool bExposureAdapted = false;
	// Rayleigh and Mie scales, planet radius, and atmosphere height the table was built from.
	std::array<float, 4> SkyMultipleScatteringKey{};
	FTextureHandle ShadowAtlas;
	FTextureHandle White;
	FTextureHandle EnvironmentDiffuse;
	FTextureHandle EnvironmentSpecular;
	bool bHdrEnvironment = false;
	FTextureHandle SmaaArea;
	FTextureHandle LtcMatrix;
	FTextureHandle LtcAmplitude;
	FTextureHandle SpecularAlbedo;
	FTextureHandle SmaaSearch;
	FBufferHandle FullscreenVertices;
	FBufferHandle FullscreenIndices;
	// The tone mapping and SMAA neighborhood slots stay empty; those passes write the display target and live in DisplayPipelines.
	std::array<FGraphicsPipelineHandle, 8> VisualPipelines;
	FDisplayShaders DisplayShaders;
	// SDR, then HDR, which is built the first time HDR output is requested.
	std::array<FDisplayPipelines, 2> DisplayPipelines;
	FGraphicsPipelineHandle ShadowPipeline;
	FGraphicsPipelineHandle InstancedShadowPipeline;
	FGraphicsPipelineHandle ShadowClearPipeline;
	FBufferHandle ShadowClearVertices;
	FVisualUniforms Uniforms;
	std::vector<FShadowView> Shadows;
	FGraphicsPipelineHandle Pipeline;
	FGraphicsPipelineHandle InstancedPipeline;
	FGraphicsPipelineHandle DepthPrepassPipeline;
	FGraphicsPipelineHandle InstancedDepthPrepassPipeline;
	std::unordered_map<std::string, std::array<FGraphicsPipelineHandle, 2>> MaterialPipelines;
	std::uint64_t MaterialShaderGeneration = 0;
	FBufferHandle Instances;
	std::vector<FMeshInstance> InstanceData;
	std::vector<FInstanceBatch> InstanceBatches;
	std::vector<std::vector<std::size_t>> BatchModels;
	// The camera first, then each shadow view.
	std::vector<FViewFrustum> ViewFrustums;
	std::vector<FModelBounds> ModelBounds;
	// Each view's instances are culled and stored back to back in Instances, in the order of ViewFrustums, then the visible selection.
	std::vector<std::vector<FInstanceDraw>> ViewDraws;
	std::vector<bool> SelectedModels;
	std::unordered_map<FInstanceKey, std::size_t, FInstanceKeyHash> InstanceBatchIndices;
	FBufferHandle GridVertices;
	FBufferHandle GridIndices;
	std::array<FBufferHandle, 2> DebugVertices;
	std::array<FBufferHandle, 2> DebugIndices;
	std::array<std::vector<FColoredClipVertex>, 2> DebugBatches;
	std::size_t LastDrawCount = 0;

	// The inputs each cached result was last rendered from. A pass whose inputs match skips its work and keeps the result.
	std::vector<std::byte> SkyViewKey;
	std::vector<std::byte> ToneMapLutKey;
	std::vector<std::byte> LightsKey;
	std::vector<std::byte> LightClustersKey;
	// One key per shadow view: its projection, tile, and every caster it saw.
	std::vector<std::vector<std::byte>> ShadowKeys;
	std::vector<std::byte> ShadowKeyScratch;
	std::uint64_t FrameIndex = 0;
};

std::span<const FCookedMaterial> FRenderMesh::GetMaterials() const noexcept
{
	return Materials;
}

void FRenderMesh::UpdateBounds(const std::span<const FCookedVertex> NewVertices) const noexcept
{
	constexpr float Infinity = std::numeric_limits<float>::infinity();
	BoundsMinimum = {Infinity, Infinity, Infinity};
	BoundsMaximum = {-Infinity, -Infinity, -Infinity};
	for (const FCookedVertex& Vertex : NewVertices)
	{
		for (std::size_t Axis = 0; Axis < 3; ++Axis)
		{
			BoundsMinimum[Axis] = std::min(BoundsMinimum[Axis], Vertex.Position[Axis]);
			BoundsMaximum[Axis] = std::max(BoundsMaximum[Axis], Vertex.Position[Axis]);
		}
	}
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
	// Without a prepass, hidden surfaces drawn first run the full lighting shader before nearer ones cover them; measured at six shaded samples per pixel.
	// The prepass reuses the PBR vertex shaders so both passes produce identical depth, and the masked shadow fragment shader keeps alpha cutouts.
	const bool bDepthPrepass = !VisualShaders.ShadowFragment.Bytecode.empty();
	if (bDepthPrepass)
	{
		auto DepthPrepass = Device.CreateGraphicsPipeline({.Name = "Depth prepass", .VertexShader = VertexShader, .FragmentShader = VisualShaders.ShadowFragment, .TextureCount = MeshTextureCount, .UniformBufferSize = VisualUniformBytes, .bDepthOnly = true, .BufferCount = MeshBufferCount});
		if (!DepthPrepass)
		{
			return std::unexpected(DepthPrepass.error());
		}

		State->DepthPrepassPipeline = std::move(*DepthPrepass);
		if (!InstancedVertexShader.Bytecode.empty())
		{
			auto InstancedDepthPrepass = Device.CreateGraphicsPipeline({.Name = "Instanced depth prepass", .VertexShader = InstancedVertexShader, .FragmentShader = VisualShaders.ShadowFragment, .bInstanced = true, .TextureCount = MeshTextureCount, .UniformBufferSize = VisualUniformBytes, .bDepthOnly = true, .BufferCount = MeshBufferCount});
			if (!InstancedDepthPrepass)
			{
				return std::unexpected(InstancedDepthPrepass.error());
			}

			State->InstancedDepthPrepassPipeline = std::move(*InstancedDepthPrepass);
			if (!VisualShaders.DepthVelocityFragment.Bytecode.empty() && !VisualShaders.MotionVectorsCompute.Bytecode.empty())
			{
				auto VelocityPrepass = Device.CreateGraphicsPipeline({.Name = "Instanced velocity prepass", .VertexShader = InstancedVertexShader, .FragmentShader = std::move(VisualShaders.DepthVelocityFragment), .ColorFormat = ETextureFormat::Rg16Float, .bInstanced = true, .TextureCount = MeshTextureCount, .UniformBufferSize = VisualUniformBytes, .BufferCount = MeshBufferCount});
				auto MotionVectors = Device.CreateComputePipeline({.Name = "Motion vectors", .Shader = std::move(VisualShaders.MotionVectorsCompute), .TextureCount = 1, .StorageTextureCount = 1, .UniformBufferSize = VisualUniformBytes});
				if (!VelocityPrepass || !MotionVectors)
				{
					return std::unexpected(!VelocityPrepass ? VelocityPrepass.error() : MotionVectors.error());
				}

				State->InstancedVelocityPrepassPipeline = std::move(*VelocityPrepass);
				State->MotionVectorPipeline = std::move(*MotionVectors);
			}
		}
	}

	if (!InstancedVertexShader.Bytecode.empty())
	{
		auto InstancedPipeline = Device.CreateGraphicsPipeline({.Name = "Instanced PBR reversed-Z", .VertexShader = std::move(InstancedVertexShader), .FragmentShader = FragmentShader, .ColorFormat = MeshColorFormat, .bInstanced = true, .TextureCount = MeshTextureCount, .UniformBufferSize = VisualUniformBytes, .bDepthWrite = !bDepthPrepass, .BufferCount = MeshBufferCount});
		if (!InstancedPipeline)
		{
			return std::unexpected(InstancedPipeline.error());
		}

		State->InstancedPipeline = std::move(*InstancedPipeline);
	}

	auto Pipeline = Device.CreateGraphicsPipeline({.Name = "PBR reversed-Z", .VertexShader = std::move(VertexShader), .FragmentShader = std::move(FragmentShader), .ColorFormat = MeshColorFormat, .TextureCount = MeshTextureCount, .UniformBufferSize = VisualUniformBytes, .bDepthWrite = !bDepthPrepass, .BufferCount = MeshBufferCount});
	if (!Pipeline)
	{
		return std::unexpected(Pipeline.error());
	}

	State->Pipeline = std::move(*Pipeline);
	if (!GridVertexShader.Bytecode.empty())
	{
		auto GridVertices = Device.CreateBuffer({.Name = "World grid vertices", .Size = 4 * sizeof(FColoredClipVertex), .Usage = EBufferUsage::Vertex, .VertexFormat = EGraphicsVertexFormat::ColoredClipPosition});
		auto GridIndexBuffer = Device.CreateBuffer({.Name = "World grid indices", .Size = sizeof(GridIndices), .Usage = EBufferUsage::Index});
		if (!GridVertices || !GridIndexBuffer)
		{
			return std::unexpected(!GridVertices ? GridVertices.error() : GridIndexBuffer.error());
		}

		State->GridVertices = std::move(*GridVertices);
		State->GridIndices = std::move(*GridIndexBuffer);
	}

	State->DisplayShaders = {
	    .FullscreenVertex = VisualShaders.FullscreenVertex,
	    .ToneMap = std::move(VisualShaders.ToneMapFragment),
	    .SmaaNeighborhood = std::move(VisualShaders.SmaaNeighborhood),
	    .Outline = std::move(VisualShaders.SelectionOutline),
	    .GridVertex = std::move(GridVertexShader),
	    .GridFragment = std::move(GridFragmentShader),
	    .DebugVertex = std::move(DebugVertexShader),
	    .DebugFragment = std::move(DebugFragmentShader),
	};

	auto DisplayPipelines = CreateDisplayPipelines(Device, State->DisplayShaders, ETextureFormat::Rgba8Srgb);
	if (!DisplayPipelines)
	{
		return std::unexpected(DisplayPipelines.error());
	}

	State->DisplayPipelines[0] = std::move(*DisplayPipelines);

	auto White = Device.CreateTexture({.Name = "Default material map", .Extent = {1, 1}, .Format = ETextureFormat::Rgba8});
	auto Lights = Device.CreateBuffer({.Name = "Lights", .Size = MaximumRenderLights * sizeof(FVisualLightUniform), .Usage = EBufferUsage::Storage});
	auto AllLights = Device.CreateBuffer({.Name = "Unculled lights", .Size = LightMaskWords * sizeof(std::uint32_t), .Usage = EBufferUsage::Storage});
	if (!Lights || !AllLights)
	{
		return std::unexpected(!Lights ? Lights.error() : AllLights.error());
	}

	State->Lights = std::move(*Lights);
	State->AllLights = std::move(*AllLights);
	auto ShadowAtlas = Device.CreateTexture({.Name = "Shadow atlas", .Extent = GetShadowAtlasExtent(1.f), .Format = ETextureFormat::Depth32, .bRenderTarget = true});
	auto LtcMatrix = Device.CreateTexture({.Name = "Rect light LTC matrix", .Extent = {64, 64}, .Format = ETextureFormat::Rgba32Float});
	auto LtcAmplitude = Device.CreateTexture({.Name = "Rect light LTC amplitude", .Extent = {64, 64}, .Format = ETextureFormat::Rgba32Float});
	// Half floats filter at full rate, and the lighting pass samples this for every pixel.
	auto SpecularAlbedo = Device.CreateTexture({.Name = "Specular albedo", .Extent = {SpecularAlbedoTableSize, SpecularAlbedoTableSize}, .Format = ETextureFormat::Rgba16Float});
	if (!SpecularAlbedo)
	{
		return std::unexpected(SpecularAlbedo.error());
	}

	State->SpecularAlbedo = std::move(*SpecularAlbedo);
	if (!White || !ShadowAtlas || !LtcMatrix || !LtcAmplitude)
	{
		return std::unexpected(!White ? White.error() : !ShadowAtlas ? ShadowAtlas.error()
		                                            : !LtcMatrix     ? LtcMatrix.error()
		                                                             : LtcAmplitude.error());
	}

	State->LtcMatrix = std::move(*LtcMatrix);
	State->LtcAmplitude = std::move(*LtcAmplitude);

	State->White = std::move(*White);
	State->ShadowAtlas = std::move(*ShadowAtlas);
	State->EnvironmentDiffuse = State->White;
	State->EnvironmentSpecular = State->White;
	std::vector<std::byte> AreaPixels;
	std::vector<std::byte> SearchPixels;
	if (!VisualShaders.FullscreenVertex.Bytecode.empty())
	{
		std::array<FShaderAsset, 8> Fragments{std::move(VisualShaders.SkyFragment), std::move(VisualShaders.FogFragment), FShaderAsset{}, std::move(VisualShaders.SmaaEdges), std::move(VisualShaders.SmaaWeights), FShaderAsset{}, std::move(VisualShaders.SkyViewFragment), std::move(VisualShaders.ToneMapLutFragment)};
		constexpr std::array<std::uint32_t, 8> TextureCounts{2, 3, 5, 1, 3, 2, 1, 1};
		constexpr std::array<std::string_view, 8> Names{"Sky", "Volumetric fog", "Fog, exposure and tone mapping", "SMAA edges", "SMAA weights", "SMAA neighborhood", "Sky view", "Tone map LUT"};
		constexpr std::array Formats{ETextureFormat::Rgba16Float, ETextureFormat::Rgba16Float, ETextureFormat::Rgba8Srgb, ETextureFormat::Rgba8, ETextureFormat::Rgba8, ETextureFormat::Rgba8Srgb, ETextureFormat::Rgba16Float, ETextureFormat::Rgba16Float};
		for (std::size_t Index = 0; Index < Fragments.size(); ++Index)
		{
			// Tone mapping and SMAA neighborhood write the display target and are built with DisplayPipelines.
			if (Index == 2 || Index == 5)
			{
				continue;
			}

			auto VisualPipeline = Device.CreateGraphicsPipeline({.Name = std::string(Names[Index]), .VertexShader = VisualShaders.FullscreenVertex, .FragmentShader = std::move(Fragments[Index]), .ColorFormat = Formats[Index], .VertexFormat = EGraphicsVertexFormat::ColoredClipPosition, .bDepthTest = false, .TextureCount = TextureCounts[Index], .UniformBufferSize = VisualUniformBytes, .bDepthWrite = false, .CullMode = EGraphicsCullMode::None, .bClampSampler = true, .BufferCount = Index == 1 ? MeshBufferCount : 0u});
			if (!VisualPipeline)
			{
				return std::unexpected(VisualPipeline.error());
			}

			State->VisualPipelines[Index] = std::move(*VisualPipeline);
		}

		// Clears one atlas tile to the far plane, so tiles whose casters did not change keep last frame's depth.
		auto ShadowClearPipeline = Device.CreateGraphicsPipeline({.Name = "Shadow tile clear", .VertexShader = VisualShaders.ShadowVertex, .VertexFormat = EGraphicsVertexFormat::ShadowMesh, .TextureCount = 1, .UniformBufferSize = VisualUniformBytes, .bDepthOnly = true, .bDepthOverwrite = true, .CullMode = EGraphicsCullMode::None});
		auto ShadowClearVertices = Device.CreateBuffer({.Name = "Shadow tile clear quad", .Size = 4 * sizeof(FMeshVertex), .Usage = EBufferUsage::Vertex});
		if (!ShadowClearPipeline || !ShadowClearVertices)
		{
			return std::unexpected(!ShadowClearPipeline ? ShadowClearPipeline.error() : ShadowClearVertices.error());
		}

		State->ShadowClearPipeline = std::move(*ShadowClearPipeline);
		State->ShadowClearVertices = std::move(*ShadowClearVertices);
		auto ShadowPipeline = Device.CreateGraphicsPipeline({.Name = "Masked shadow atlas", .VertexShader = std::move(VisualShaders.ShadowVertex), .FragmentShader = VisualShaders.ShadowFragment, .VertexFormat = EGraphicsVertexFormat::ShadowMesh, .TextureCount = 1, .UniformBufferSize = VisualUniformBytes, .bDepthOnly = true});
		auto InstancedShadowPipeline = Device.CreateGraphicsPipeline({.Name = "Instanced masked shadow atlas", .VertexShader = std::move(VisualShaders.ShadowInstancedVertex), .FragmentShader = std::move(VisualShaders.ShadowFragment), .VertexFormat = EGraphicsVertexFormat::ShadowMesh, .bInstanced = true, .TextureCount = 1, .UniformBufferSize = VisualUniformBytes, .bDepthOnly = true});
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
		if (!VisualShaders.LightClustersCompute.Bytecode.empty())
		{
			auto Clusters = Device.CreateComputePipeline({.Name = "Light clusters", .Shader = std::move(VisualShaders.LightClustersCompute), .StorageBufferCount = 1, .UniformBufferSize = VisualUniformBytes, .BufferCount = MeshBufferCount});
			if (!Clusters)
			{
				return std::unexpected(Clusters.error());
			}

			State->LightClusterPipeline = std::move(*Clusters);
		}

		// Culling feeds the instanced depth prepass, which builds the hierarchy the late phase tests against.
		if (State->InstancedDepthPrepassPipeline && !VisualShaders.DepthHierarchyCompute.Bytecode.empty() && !VisualShaders.OcclusionCullCompute.Bytecode.empty())
		{
			const std::array<FComputePipelineDescriptor, 2> Descriptors{{
			    {.Name = "Depth hierarchy", .Shader = std::move(VisualShaders.DepthHierarchyCompute), .TextureCount = 1, .StorageTextureCount = 1 + DepthHierarchyLevelsPerDispatch, .UniformBufferSize = sizeof(FDepthHierarchyUniforms)},
			    {.Name = "Occlusion culling", .Shader = std::move(VisualShaders.OcclusionCullCompute), .TextureCount = 1, .StorageBufferCount = 3, .UniformBufferSize = sizeof(FOcclusionCullUniforms), .BufferCount = 2},
			}};

			for (std::size_t Index = 0; Index < Descriptors.size(); ++Index)
			{
				auto OcclusionPipeline = Device.CreateComputePipeline(Descriptors[Index]);
				if (!OcclusionPipeline)
				{
					return std::unexpected(OcclusionPipeline.error());
				}

				State->OcclusionPipelines[Index] = std::move(*OcclusionPipeline);
			}
		}

		if (!VisualShaders.SmaaEdgeListCompute.Bytecode.empty() && !VisualShaders.SmaaEdgeWeightsCompute.Bytecode.empty())
		{
			auto EdgeList = Device.CreateComputePipeline({.Name = "SMAA edges", .Shader = std::move(VisualShaders.SmaaEdgeListCompute), .TextureCount = 1, .StorageTextureCount = 1, .StorageBufferCount = 2, .UniformBufferSize = VisualUniformBytes, .bLinearSampler = true});
			auto EdgeWeights = Device.CreateComputePipeline({.Name = "SMAA weights", .Shader = std::move(VisualShaders.SmaaEdgeWeightsCompute), .TextureCount = 3, .StorageTextureCount = 1, .StorageBufferCount = 2, .UniformBufferSize = VisualUniformBytes, .bLinearSampler = true});
			auto Arguments = Device.CreateBuffer({.Name = "SMAA weight arguments", .Size = 4 * sizeof(std::uint32_t), .Usage = EBufferUsage::Storage});
			if (!EdgeList || !EdgeWeights || !Arguments)
			{
				return std::unexpected(!EdgeList ? EdgeList.error() : !EdgeWeights ? EdgeWeights.error()
				                                                                   : Arguments.error());
			}

			State->SmaaPipelines = {std::move(*EdgeList), std::move(*EdgeWeights)};
			State->SmaaArguments = std::move(*Arguments);
		}

		// GTAO reads the prepass depth, so it runs only when there is one.
		if (bDepthPrepass && !VisualShaders.GtaoCompute.Bytecode.empty())
		{
			const std::array<FComputePipelineDescriptor, 3> Descriptors{{
			    {.Name = "GTAO depth", .Shader = std::move(VisualShaders.GtaoDepthCompute), .TextureCount = 1, .StorageTextureCount = GtaoDepthMips, .UniformBufferSize = sizeof(FGtaoUniforms)},
			    {.Name = "GTAO", .Shader = std::move(VisualShaders.GtaoCompute), .TextureCount = 1, .StorageTextureCount = 1, .UniformBufferSize = sizeof(FGtaoUniforms)},
			    {.Name = "GTAO denoise", .Shader = std::move(VisualShaders.GtaoDenoiseCompute), .TextureCount = 2, .StorageTextureCount = 1, .UniformBufferSize = sizeof(FGtaoUniforms)},
			}};

			for (std::size_t Index = 0; Index < Descriptors.size(); ++Index)
			{
				auto GtaoPipeline = Device.CreateComputePipeline(Descriptors[Index]);
				if (!GtaoPipeline)
				{
					return std::unexpected(GtaoPipeline.error());
				}

				State->GtaoPipelines[Index] = std::move(*GtaoPipeline);
			}
		}

		auto SkyView = Device.CreateTexture({.Name = "Sky view", .Extent = {256, 128}, .Format = ETextureFormat::Rgba16Float, .bRenderTarget = true});
		if (!SkyView)
		{
			return std::unexpected(SkyView.error());
		}

		State->SkyView = std::move(*SkyView);
		if (!VisualShaders.ExposureCompute.Bytecode.empty())
		{
			auto Exposure = Device.CreateComputePipeline({.Name = "Exposure", .Shader = std::move(VisualShaders.ExposureCompute), .TextureCount = 3, .StorageTextureCount = 1, .UniformBufferSize = VisualUniformBytes});
			if (!Exposure)
			{
				return std::unexpected(Exposure.error());
			}

			State->ExposurePipeline = std::move(*Exposure);
		}

		// Matches ToneMapLutSize in VisualShared.slangh.
		auto ToneMapLut = Device.CreateTexture({.Name = "Tone map LUT", .Extent = {64 * 64, 64}, .Format = ETextureFormat::Rgba16Float, .bRenderTarget = true});
		if (!ToneMapLut)
		{
			return std::unexpected(ToneMapLut.error());
		}

		State->ToneMapLut = std::move(*ToneMapLut);
		for (FTextureHandle& ExposureState : State->ExposureStates)
		{
			auto Texture = Device.CreateTexture({.Name = "Adapted exposure", .Extent = {1, 1}, .Format = ETextureFormat::Rgba32Float, .bStorage = true});
			if (!Texture)
			{
				return std::unexpected(Texture.error());
			}

			ExposureState = std::move(*Texture);
		}
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
		constexpr std::array<std::uint32_t, LightMaskWords> AllBits = [] consteval
		{
			std::array<std::uint32_t, LightMaskWords> Bits{};
			Bits.fill(~0u);
			return Bits;
		}();

		Result = Device.WriteBuffer(State->AllLights, std::as_bytes(std::span{AllBits}));
	}

	if (Result)
	{
		Result = Device.WriteTexture(State->LtcMatrix, 0, std::as_bytes(std::span{Ltc::InverseMatrix}));
	}

	if (Result)
	{
		Result = Device.WriteTexture(State->LtcAmplitude, 0, std::as_bytes(std::span{Ltc::Amplitude}));
	}

	if (Result)
	{
		std::vector<std::uint16_t> Halves;
		for (const auto& Texel : GetSpecularAlbedoTable())
		{
			for (const float Value : Texel)
			{
				Halves.push_back(ToHalf(Value));
			}
		}

		Result = Device.WriteTexture(State->SpecularAlbedo, 0, std::as_bytes(std::span{Halves}));
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

		// Clip space at the reversed-Z far plane; the identity transform keeps it there.
		if (Result)
		{
			constexpr std::array<FMeshVertex, 4> ClearQuad{{{.Position = {-1, -1, 0}}, {.Position = {1, -1, 0}}, {.Position = {1, 1, 0}}, {.Position = {-1, 1, 0}}}};
			Result = Device.WriteBuffer(State->ShadowClearVertices, std::as_bytes(std::span{ClearQuad}));
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

const FTextureHandle& FMeshRenderer::GetMotionVectors() const noexcept
{
	static const FTextureHandle None;
	return Implementation->bMotionVectors ? Implementation->MotionVectors : None;
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
	for (const FTextureHandle* Target : {&State.Color, &State.Depth, &State.HdrColor, &State.FogColor, &State.ToneColor, &State.Edges, &State.Weights, &State.SelectionDepth, &State.SkyView, &State.ShadowAtlas, &State.GtaoViewDepth, &State.GtaoNoisy, &State.GtaoOcclusion})
	{
		if (*Target)
		{
			const auto& Descriptor = (*Target)->GetDescriptor();
			for (std::uint32_t Mip = 0; Mip < Descriptor.MipLevels; ++Mip)
			{
				const FExtent2D MipExtent = GetMipExtent(Descriptor.Extent, Mip);
				Bytes += std::size_t{MipExtent.Width} * MipExtent.Height * GetTextureTexelBytes(Descriptor.Format);
			}
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
					if (Binding.Binding >= MeshTextureCount)
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
				case EShaderBindingType::Buffer:
					if (Binding.Binding < ReadBufferBinding || Binding.Binding >= ReadBufferBinding + MeshBufferCount)
					{
						return false;
					}

					break;
				case EShaderBindingType::ConstantBuffer:
					if (Binding.Binding != 64 || Binding.ByteSize != VisualUniformBytes || bVisualUniforms)
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
	auto Pipeline = State.Device->CreateGraphicsPipeline({.Name = std::format("Material {}", Key), .VertexShader = std::move(Vertex), .FragmentShader = Fragment, .ColorFormat = Format, .TextureCount = MeshTextureCount, .UniformBufferSize = VisualUniformBytes, .BufferCount = MeshBufferCount});
	auto Instanced = State.Device->CreateGraphicsPipeline({.Name = std::format("Instanced material {}", Key), .VertexShader = std::move(InstancedVertex), .FragmentShader = std::move(Fragment), .ColorFormat = Format, .bInstanced = true, .TextureCount = MeshTextureCount, .UniformBufferSize = VisualUniformBytes, .BufferCount = MeshBufferCount});
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
	++State.FrameIndex;
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
	// A new extent restarts motion, as there is no previous frame at that size to compare with.
	const FMatrix4 PreviousWorldToClip = State.PreviousExtent == Extent ? State.PreviousWorldToClip : WorldToClip;
	State.PreviousWorldToClip = WorldToClip;
	State.PreviousExtent = Extent;
	State.bMotionVectors = State.MotionVectorPipeline && View.Visuals.bMotionVectors;
	State.Uniforms.ClipToPreviousClip = (PreviousWorldToClip * FMatrix4(State.Uniforms.ViewToWorld) * FMatrix4(State.Uniforms.ClipToView)).Data();
	const bool bPreviousModels = View.PreviousModels.size() == View.Models.size();
	// Thumbnail renderers without the visual passes always produce sRGB.
	const bool bHdrDisplay = View.Visuals.HdrDisplay.has_value() && State.VisualPipelines[0];
	if (!bHdrDisplay)
	{
		State.Uniforms.Display = {};
	}

	if (bHdrDisplay && !State.DisplayPipelines[1].ToneMap)
	{
		auto Created = CreateDisplayPipelines(Device, State.DisplayShaders, ETextureFormat::Rgba16Float);
		if (!Created)
		{
			return std::unexpected(Created.error());
		}

		State.DisplayPipelines[1] = std::move(*Created);
	}

	const FDisplayPipelines& Display = State.DisplayPipelines[bHdrDisplay ? 1 : 0];
	const ETextureFormat DisplayFormat = bHdrDisplay ? ETextureFormat::Rgba16Float : ETextureFormat::Rgba8Srgb;
	State.Uniforms.Sky[2] = static_cast<float>(State.EnvironmentSpecular->GetDescriptor().MipLevels - 1);
	State.Uniforms.Sky[3] = State.VisualPipelines[0] ? 1.f : 0.f;
	State.Uniforms.AtmosphereGeometry[2] = State.bHdrEnvironment ? 1.f : 0.f;

	if (View.Visuals.bStudioPreview && View.Lights.empty() && !View.Visuals.Atmosphere && State.VisualPipelines[0])
	{
		// Unlit new levels retain a studio preview until authored lighting exists.
		State.Uniforms.Sky[3] = 2.f;
		State.Uniforms.Controls[2] = 1.f;
		State.Uniforms.Exposure[0] = 0.f;
	}

	// Turning automatic exposure back on starts from the current scene instead of easing from a stale value.
	const bool bAutoExposure = State.ExposurePipeline && State.Uniforms.Exposure[0] > 0.5f;
	State.Uniforms.Exposure[0] = bAutoExposure ? 1.f : 0.f;
	State.Uniforms.Exposure[3] = bAutoExposure && !State.bExposureAdapted ? 1.f : 0.f;
	State.bExposureAdapted = bAutoExposure;

	State.Shadows = State.ShadowPipeline && View.Visuals.ShadowQuality != EShadowQuality::Off ? BuildShadowViews(State.Uniforms, View.Lights, View.Projection, View.Visuals.ShadowResolution) : std::vector<FShadowView>{};
	if (const FExtent2D ShadowExtent = GetShadowAtlasExtent(View.Visuals.ShadowResolution); !State.Shadows.empty() && State.ShadowAtlas->GetDescriptor().Extent != ShadowExtent)
	{
		auto ShadowAtlas = Device.CreateTexture({.Name = "Shadow atlas", .Extent = ShadowExtent, .Format = ETextureFormat::Depth32, .bRenderTarget = true});
		if (!ShadowAtlas)
		{
			return std::unexpected(ShadowAtlas.error());
		}

		State.ShadowAtlas = std::move(*ShadowAtlas);
	}

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

	for (const FRenderMeshVertexUpdate& Update : View.VertexUpdates)
	{
		if (!Update.Mesh || Update.Vertices.size_bytes() != Update.Mesh->Vertices->GetDescriptor().Size)
		{
			return std::unexpected(FPresentationError{.Code = EPresentationErrorCode::InvalidDescriptor, .Message = "A mesh vertex update must replace every vertex of an existing mesh"});
		}
	}

	for (const FRenderMeshVertexUpdate& Update : View.VertexUpdates)
	{
		Update.Mesh->UpdateBounds(Update.Vertices);
	}

	if (View.bDrawGrid && !Display.Grid)
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

		if (!Display.Debug[Index])
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

	State.ViewFrustums.clear();
	State.ViewFrustums.push_back(MakeViewFrustum(WorldToClip));
	for (const FShadowView& Shadow : State.Shadows)
	{
		State.ViewFrustums.push_back(MakeViewFrustum(Shadow.WorldToClip, Shadow.LightPosition, Shadow.LightRange));
	}

	State.ModelBounds.resize(View.Models.size());
	for (std::size_t Model = 0; Model < View.Models.size(); ++Model)
	{
		const FRenderMesh* Mesh = View.Meshes[Model];
		if (!Mesh)
		{
			continue;
		}

		const FMatrix4& Transform = View.Models[Model];
		const FVector3 LocalCenter = (Mesh->BoundsMinimum + Mesh->BoundsMaximum) * 0.5f;
		const FVector3 LocalExtent = (Mesh->BoundsMaximum - Mesh->BoundsMinimum) * 0.5f;
		FImplementation::FModelBounds& Bounds = State.ModelBounds[Model];
		for (std::size_t Axis = 0; Axis < 3; ++Axis)
		{
			Bounds.Center[Axis] = Transform(Axis, 0) * LocalCenter.X + Transform(Axis, 1) * LocalCenter.Y + Transform(Axis, 2) * LocalCenter.Z + Transform(Axis, 3);
			Bounds.Extent[Axis] = std::abs(Transform(Axis, 0)) * LocalExtent.X + std::abs(Transform(Axis, 1)) * LocalExtent.Y + std::abs(Transform(Axis, 2)) * LocalExtent.Z;
		}
	}

	const auto IsVisible = [&](const std::size_t ViewIndex, const std::size_t Model)
	{
		const FImplementation::FModelBounds& Bounds = State.ModelBounds[Model];
		return IsBoxInFrustum(State.ViewFrustums[ViewIndex], Bounds.Center, Bounds.Extent);
	};

	if (State.InstancedPipeline)
	{
		State.InstanceBatches.clear();
		State.InstanceBatchIndices.clear();
		for (std::vector<std::size_t>& Models : State.BatchModels)
		{
			Models.clear();
		}

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
				if (State.BatchModels.size() < State.InstanceBatches.size())
				{
					State.BatchModels.emplace_back();
				}
			}

			State.BatchModels[Entry->second].push_back(Model);
		}

		State.SelectedModels.assign(View.Models.size(), false);
		for (const std::size_t Index : View.Selected)
		{
			if (Index < State.SelectedModels.size())
			{
				State.SelectedModels[Index] = true;
			}
		}

		// The selection outline rasterizes the selected models seen by the camera, one instanced draw per batch.
		const std::size_t SelectionView = State.ViewFrustums.size();
		State.InstanceData.clear();
		State.CandidateData.clear();
		State.OcclusionDrawCount = 0;
		State.ViewDraws.resize(SelectionView + 1);
		for (std::size_t ViewIndex = 0; ViewIndex <= SelectionView; ++ViewIndex)
		{
			std::vector<FImplementation::FInstanceDraw>& Draws = State.ViewDraws[ViewIndex];
			Draws.clear();
			if (ViewIndex == SelectionView && View.Selected.empty())
			{
				continue;
			}

			for (std::size_t Batch = 0; Batch < State.InstanceBatches.size(); ++Batch)
			{
				const std::size_t FirstInstance = State.InstanceData.size();
				const auto SectionCount = static_cast<std::uint32_t>(State.InstanceBatches[Batch].Mesh->Sections.size());
				for (const std::size_t Model : State.BatchModels[Batch])
				{
					if (ViewIndex == SelectionView ? State.SelectedModels[Model] && IsVisible(0, Model) : IsVisible(ViewIndex, Model))
					{
						const FMatrix4 ObjectToClip = WorldToClip * View.Models[Model];
						const FMatrix4 PreviousObjectToClip = State.bMotionVectors ? PreviousWorldToClip * (bPreviousModels ? View.PreviousModels[Model] : View.Models[Model]) : ObjectToClip;
						State.InstanceData.push_back({.WorldToClip = ObjectToClip.Data(), .ObjectToView = (View.View * View.Models[Model]).Data(), .PreviousWorldToClip = PreviousObjectToClip.Data()});
						if (ViewIndex == 0)
						{
							const FImplementation::FModelBounds& Bounds = State.ModelBounds[Model];
							State.CandidateData.push_back({.Center = {Bounds.Center.X, Bounds.Center.Y, Bounds.Center.Z}, .Model = static_cast<std::uint32_t>(Model), .Extent = {Bounds.Extent.X, Bounds.Extent.Y, Bounds.Extent.Z}, .FirstDraw = State.OcclusionDrawCount, .SectionCount = SectionCount});
						}
					}
				}

				if (State.InstanceData.size() > FirstInstance)
				{
					Draws.push_back({.Batch = Batch, .FirstInstance = static_cast<std::uint32_t>(FirstInstance), .InstanceCount = static_cast<std::uint32_t>(State.InstanceData.size() - FirstInstance)});
					State.OcclusionDrawCount += ViewIndex == 0 ? SectionCount : 0;
				}
			}
		}

		if (State.InstanceData.size() > MaximumUploadBytesPerRecording / sizeof(FMeshInstance))
		{
			return std::unexpected(FPresentationError{.Code = EPresentationErrorCode::InvalidDescriptor, .Message = "Mesh instances must fit the 64 MiB recording upload budget"});
		}

		// Culling changes the instance count every time the camera moves, so the buffer grows by powers of two instead of matching it.
		const std::size_t InstanceBytes = State.InstanceData.size() * sizeof(FMeshInstance);
		if (InstanceBytes > 0 && (!State.Instances || State.Instances->GetDescriptor().Size < InstanceBytes))
		{
			// Storage, so occlusion culling can read the camera's instances.
			auto Instances = Device.CreateBuffer({.Name = "Mesh instances", .Size = std::bit_ceil(State.InstanceData.size()) * sizeof(FMeshInstance), .Usage = EBufferUsage::Storage, .VertexFormat = EGraphicsVertexFormat::MeshInstance});
			if (!Instances)
			{
				return std::unexpected(Instances.error());
			}

			State.Instances = std::move(*Instances);
		}
	}

	if (State.bMotionVectors && (!State.MotionVectors || State.MotionVectors->GetDescriptor().Extent != Extent))
	{
		auto MotionVectors = Device.CreateTexture({.Name = "Motion vectors", .Extent = Extent, .Format = ETextureFormat::Rg16Float, .bRenderTarget = true, .bStorage = true});
		if (!MotionVectors)
		{
			return std::unexpected(MotionVectors.error());
		}

		State.MotionVectors = std::move(*MotionVectors);
	}

	// The hierarchy's first level is the largest power of two within half the view, so it needs at least 2x2 pixels.
	State.bOcclusionCulling = State.OcclusionPipelines[0] && View.Visuals.bOcclusionCulling && !State.CandidateData.empty() && Extent.Width >= 2 && Extent.Height >= 2;
	if (State.bOcclusionCulling)
	{
		const auto CandidateCount = static_cast<std::uint32_t>(State.CandidateData.size());
		State.OcclusionArgumentData.clear();
		for (std::uint32_t List = 0; List < OcclusionListCount; ++List)
		{
			for (const FImplementation::FInstanceDraw& Draw : State.ViewDraws[0])
			{
				for (const FRenderMesh::FSection& Section : State.InstanceBatches[Draw.Batch].Mesh->Sections)
				{
					State.OcclusionArgumentData.insert(State.OcclusionArgumentData.end(), {Section.IndexCount, 0u, Section.FirstIndex, 0u, List * CandidateCount + Draw.FirstInstance});
				}
			}
		}

		// Grows by powers of two in elements, so instance buffers keep holding whole instances.
		const auto Grow = [&](FBufferHandle& Buffer, const std::string_view Name, const std::size_t Count, const std::size_t ElementBytes, const EGraphicsVertexFormat Format = EGraphicsVertexFormat::Mesh) -> std::expected<bool, FPresentationError>
		{
			if (Buffer && Buffer->GetDescriptor().Size >= Count * ElementBytes)
			{
				return false;
			}

			auto Created = Device.CreateBuffer({.Name = std::string(Name), .Size = std::bit_ceil(Count) * ElementBytes, .Usage = EBufferUsage::Storage, .VertexFormat = Format});
			if (!Created)
			{
				return std::unexpected(Created.error());
			}

			Buffer = std::move(*Created);
			return true;
		};

		auto Grown = Grow(State.OcclusionCandidates, "Occlusion candidates", State.CandidateData.size(), sizeof(FOcclusionCandidate));
		if (Grown)
		{
			Grown = Grow(State.CulledInstances, "Culled mesh instances", OcclusionListCount * State.CandidateData.size(), sizeof(FMeshInstance), EGraphicsVertexFormat::MeshInstance);
		}

		if (Grown)
		{
			Grown = Grow(State.OcclusionArguments, "Occlusion draw arguments", State.OcclusionArgumentData.size(), sizeof(std::uint32_t));
		}

		if (Grown)
		{
			Grown = Grow(State.OcclusionHistory, "Occlusion history", View.Models.size(), sizeof(std::uint32_t));
			State.bResetOcclusionHistory = State.bResetOcclusionHistory || (Grown && *Grown);
		}

		if (!Grown)
		{
			return std::unexpected(Grown.error());
		}

		// Power-of-two levels halve exactly, so each coarser texel covers exactly four finer ones.
		const FExtent2D HierarchyExtent{std::bit_floor(Extent.Width / 2), std::bit_floor(Extent.Height / 2)};
		if (!State.DepthHierarchy || State.DepthHierarchy->GetDescriptor().Extent != HierarchyExtent)
		{
			auto Hierarchy = Device.CreateTexture({.Name = "Depth hierarchy", .Extent = HierarchyExtent, .Format = ETextureFormat::R32Float, .MipLevels = GetMipLevelCount(HierarchyExtent), .bStorage = true});
			if (!Hierarchy)
			{
				return std::unexpected(Hierarchy.error());
			}

			State.DepthHierarchy = std::move(*Hierarchy);
		}
	}

	if (!State.Color || State.Color->GetDescriptor().Extent != Extent || State.Color->GetDescriptor().Format != DisplayFormat)
	{
		auto Color = Device.CreateTexture({.Name = "Scene color", .Extent = Extent, .Format = DisplayFormat, .bRenderTarget = true});
		auto Depth = Device.CreateTexture({.Name = "Scene reversed-Z depth", .Extent = Extent, .Format = ETextureFormat::Depth32, .bRenderTarget = true});
		if (!Color || !Depth)
		{
			return std::unexpected(!Color ? Color.error() : Depth.error());
		}

		State.Color = std::move(*Color);
		State.Depth = std::move(*Depth);
		if (State.VisualPipelines[0])
		{
			const auto Target = [&](const char* Name, const FExtent2D Size, const ETextureFormat Format, FTextureHandle& Destination, const bool bStorage = false) -> std::expected<void, FPresentationError>
			{
				auto Texture = Device.CreateTexture({.Name = Name, .Extent = Size, .Format = Format, .bRenderTarget = true, .bStorage = bStorage});
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
				Created = Target("Tone-mapped scene", Extent, DisplayFormat, State.ToneColor);
			}

			if (Created)
			{
				Created = Target("SMAA edges", Extent, ETextureFormat::Rgba8, State.Edges, static_cast<bool>(State.SmaaPipelines[0]));
			}

			if (Created)
			{
				Created = Target("SMAA weights", Extent, ETextureFormat::Rgba8, State.Weights, static_cast<bool>(State.SmaaPipelines[0]));
			}

			// Every pixel can be an edge pixel.
			if (Created && State.SmaaPipelines[0])
			{
				auto EdgePixels = Device.CreateBuffer({.Name = "SMAA edge pixels", .Size = std::size_t{Extent.Width} * Extent.Height * sizeof(std::uint32_t), .Usage = EBufferUsage::Storage});
				if (!EdgePixels)
				{
					return std::unexpected(EdgePixels.error());
				}

				State.SmaaEdgePixels = std::move(*EdgePixels);
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

	const float FogResolution = std::isfinite(View.Visuals.FogResolution) ? std::clamp(View.Visuals.FogResolution, 0.0625f, 1.f) : 0.25f;
	const float FogScale = std::min({FogResolution, 2048.f * FogResolution / static_cast<float>(Extent.Width), 2048.f * FogResolution / static_cast<float>(Extent.Height)});
	const FExtent2D FogExtent{std::max(1u, static_cast<std::uint32_t>(std::ceil(static_cast<float>(Extent.Width) * FogScale))), std::max(1u, static_cast<std::uint32_t>(std::ceil(static_cast<float>(Extent.Height) * FogScale)))};
	if (State.VisualPipelines[0] && (!State.FogColor || State.FogColor->GetDescriptor().Extent != FogExtent))
	{
		auto Fog = Device.CreateTexture({.Name = "Fog volume", .Extent = FogExtent, .Format = ETextureFormat::Rgba16Float, .bRenderTarget = true});
		if (!Fog)
		{
			return std::unexpected(Fog.error());
		}

		State.FogColor = std::move(*Fog);
	}

	// The depth chain needs every one of its mips, so tiny targets skip occlusion.
	const bool bScreenOcclusion = State.GtaoPipelines[0] && View.Visuals.AmbientOcclusion != EAmbientOcclusion::Off && GetMipLevelCount(Extent) >= GtaoDepthMips;
	if (bScreenOcclusion && (!State.GtaoOcclusion || State.GtaoOcclusion->GetDescriptor().Extent != Extent))
	{
		auto ViewDepth = Device.CreateTexture({.Name = "GTAO view depth", .Extent = Extent, .Format = ETextureFormat::R32Float, .MipLevels = GtaoDepthMips, .bStorage = true});
		auto Noisy = Device.CreateTexture({.Name = "GTAO noisy occlusion", .Extent = GetMipExtent(Extent, 1), .Format = ETextureFormat::Rgba8, .bStorage = true});
		auto Occlusion = Device.CreateTexture({.Name = "GTAO occlusion", .Extent = Extent, .Format = ETextureFormat::Rgba8, .bStorage = true});
		if (!ViewDepth || !Noisy || !Occlusion)
		{
			return std::unexpected(!ViewDepth ? ViewDepth.error() : !Noisy ? Noisy.error()
			                                                               : Occlusion.error());
		}

		State.GtaoViewDepth = std::move(*ViewDepth);
		State.GtaoNoisy = std::move(*Noisy);
		State.GtaoOcclusion = std::move(*Occlusion);
	}

	State.Uniforms.Occlusion[0] = bScreenOcclusion ? 1.f : 0.f;
	const FExtent2D LightTiles{(Extent.Width + LightClusterTileSize - 1) / LightClusterTileSize, (Extent.Height + LightClusterTileSize - 1) / LightClusterTileSize};
	const std::size_t ClusterBytes = std::size_t{LightTiles.Width} * LightTiles.Height * LightClusterSlices * LightMaskWords * sizeof(std::uint32_t);
	if (State.LightClusterPipeline && (!State.LightClusters || State.LightClusters->GetDescriptor().Size != ClusterBytes))
	{
		auto Clusters = Device.CreateBuffer({.Name = "Light clusters", .Size = ClusterBytes, .Usage = EBufferUsage::Storage});
		if (!Clusters)
		{
			return std::unexpected(Clusters.error());
		}

		State.LightClusters = std::move(*Clusters);
	}

	const std::array LightBuffers{State.Lights, State.LightClusterPipeline ? State.LightClusters : State.AllLights};
	if (State.LightClusterPipeline)
	{
		constexpr float NearSlice = 0.1f;
		constexpr float FarSlice = 500.f;
		const float SliceScale = static_cast<float>(LightClusterSlices) / std::log2(FarSlice / NearSlice);
		State.Uniforms.LightGrid = {static_cast<float>(LightClusterTileSize), SliceScale, -std::log2(NearSlice) * SliceScale, static_cast<float>(LightTiles.Width)};
	}

	const FTextureHandle MeshColor = State.HdrColor ? State.HdrColor : State.Color;
	const auto MaterialTextures = [&](const FRenderMesh& Mesh, const FRenderMesh::FSection& Section, const std::span<const FRenderMaterial* const> Overrides)
	{
		std::array<FTextureHandle, MeshTextureCount> Textures;
		std::ranges::fill(Textures, State.White);
		const FRenderMaterial* Override = Section.Material < Overrides.size() ? Overrides[Section.Material] : nullptr;
		const FCookedMaterial& Imported = Mesh.Materials[Section.Material];
		const FMaterialParameters& Material = Override ? Override->Parameters : Imported.Parameters;
		State.Uniforms.Material = {.BaseColor = Material.BaseColor, .Surface = {Material.Metallic, Material.Roughness, Material.NormalStrength, Material.OcclusionStrength}, .Emissive = {Material.Emissive[0], Material.Emissive[1], Material.Emissive[2], Material.EmissiveIntensity}, .UV = {Material.UVScale[0], Material.UVScale[1], Material.UVOffset[0], Material.UVOffset[1]}, .Channels1 = {0, 0, static_cast<float>(Material.BlendMode), Material.AlphaCutoff}, .Specular = {Material.SpecularColor[0], Material.SpecularColor[1], Material.SpecularColor[2], Material.DiffuseRoughness}};
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
		Textures[9] = State.LtcMatrix;
		Textures[10] = State.LtcAmplitude;
		// Only the PBR pass reads screen occlusion; earlier passes run before it is written.
		Textures[11] = State.White;
		Textures[12] = State.SpecularAlbedo;
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
		constexpr std::array Names{"Sky", "Volumetric fog", "Tone mapping", "SMAA edges", "SMAA weights", "SMAA neighborhood", "Sky view", "Tone map LUT"};
		const auto Timer = TimedPass(Names[Pipeline]);
		// Tone mapping and SMAA neighborhood write the display target, so their pipelines follow its format.
		const FGraphicsPipelineHandle& Selected = Pipeline == 2 ? Display.ToneMap : Pipeline == 5 ? Display.SmaaNeighborhood
		                                                                                          : State.VisualPipelines[Pipeline];
		// Only volumetric fog reads lights.
		return Device.DrawIndexed({.Pipeline = Selected, .Vertices = State.FullscreenVertices, .Indices = State.FullscreenIndices, .ColorTarget = Destination, .IndexCount = 6, .Textures = Sources, .Uniforms = GetVisualUniformBytes(State.Uniforms), .Buffers = Pipeline == 1 ? std::span<const FBufferHandle>(LightBuffers) : std::span<const FBufferHandle>{}, .bCoversColorTarget = true});
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
	if (!View.VertexUpdates.empty())
	{
		Graph.AddPass("Mesh vertex updates", {{.Resource = Geometry, .Access = ERenderGraphAccess::Write}}, [&]() -> std::expected<void, FRenderGraphError>
		{
			for (const FRenderMeshVertexUpdate& Update : View.VertexUpdates)
			{
				if (const auto Written = Device.WriteBuffer(Update.Mesh->Vertices, std::as_bytes(Update.Vertices)); !Written)
				{
					return GraphResult(Written);
				}
			}

			return {};
		});
	}

	if (State.InstancedPipeline && !State.InstanceData.empty())
	{
		Graph.AddPass("Mesh instances", {{.Resource = Geometry, .Access = ERenderGraphAccess::Write}}, [&]
		{
			auto Result = Device.WriteBuffer(State.Instances, std::as_bytes(std::span{State.InstanceData}));
			if (Result && State.bOcclusionCulling)
			{
				Result = Device.WriteBuffer(State.OcclusionCandidates, std::as_bytes(std::span{State.CandidateData}));
			}

			if (Result && State.bOcclusionCulling)
			{
				Result = Device.WriteBuffer(State.OcclusionArguments, std::as_bytes(std::span{State.OcclusionArgumentData}));
			}

			// A new history starts with everything visible, so the first frame draws it all early.
			if (Result && State.bOcclusionCulling && State.bResetOcclusionHistory)
			{
				const std::vector<std::uint32_t> Visible(State.OcclusionHistory->GetDescriptor().Size / sizeof(std::uint32_t), 1u);
				Result = Device.WriteBuffer(State.OcclusionHistory, std::as_bytes(std::span{Visible}));
				State.bResetOcclusionHistory = !Result;
			}

			return GraphResult(Result);
		});
	}

	Graph.AddPass("Shadow atlas", {{.Resource = Shadow, .Access = ERenderGraphAccess::Write}, {.Resource = Geometry, .Access = ERenderGraphAccess::Read}, {.Resource = Texture, .Access = ERenderGraphAccess::Read}}, [&]() -> std::expected<void, FRenderGraphError>
	{
		if (State.Shadows.empty())
		{
			return {};
		}

		// A tile keeps last frame's depth while its projection and every caster it saw stay the same.
		// Casters are keyed by world transform, mesh, and the material inputs the masked shadow shader reads.
		State.ShadowKeys.resize(State.Shadows.size());
		std::vector<bool> Stale(State.Shadows.size(), false);
		std::vector<std::byte>& Key = State.ShadowKeyScratch;
		const auto Append = [&Key](const std::span<const std::byte> Bytes)
		{
			Key.insert(Key.end(), Bytes.begin(), Bytes.end());
		};

		for (std::size_t ShadowIndex = 0; ShadowIndex < State.Shadows.size(); ++ShadowIndex)
		{
			const FShadowView& ShadowView = State.Shadows[ShadowIndex];
			Key.clear();
			Append(KeyBytes(ShadowView.WorldToClip));
			Append(KeyBytes(ShadowView.Viewport));
			Append(KeyBytes(State.ShadowAtlas.get()));
			for (std::size_t Model = 0; Model < View.Models.size(); ++Model)
			{
				const FRenderMesh* Mesh = View.Meshes[Model];
				if (!Mesh || !IsVisible(ShadowIndex + 1, Model))
				{
					continue;
				}

				Append(KeyBytes(Mesh));
				Append(KeyBytes(View.Models[Model]));
				const auto Overrides = View.Materials.empty() ? std::span<const FRenderMaterial* const>{} : View.Materials[Model];
				for (const FRenderMesh::FSection& Section : Mesh->Sections)
				{
					const FRenderMaterial* Override = Section.Material < Overrides.size() ? Overrides[Section.Material] : nullptr;
					const FMaterialParameters& Material = Override ? Override->Parameters : Mesh->Materials[Section.Material].Parameters;
					if (Material.BlendMode != EMaterialBlendMode::Opaque)
					{
						Append(KeyBytes(Material.BlendMode));
						Append(KeyBytes(Material.AlphaCutoff));
						Append(KeyBytes(Material.BaseColor[3]));
						Append(KeyBytes(Material.UVScale));
						Append(KeyBytes(Material.UVOffset));
						Append(KeyBytes(MaterialTextures(*Mesh, Section, Overrides)[0].get()));
					}
				}

				// Deforming meshes change without their transform changing.
				if (std::ranges::any_of(View.VertexUpdates, [Mesh](const FRenderMeshVertexUpdate& Update)
				{
					return Update.Mesh == Mesh;
				}))
				{
					Append(KeyBytes(State.FrameIndex));
				}
			}

			if (Key != State.ShadowKeys[ShadowIndex])
			{
				Stale[ShadowIndex] = true;
				State.ShadowKeys[ShadowIndex].swap(Key);
			}
		}

		const auto StaleCount = std::ranges::count(Stale, true);
		if (StaleCount == 0)
		{
			return {};
		}

		const auto Timer = TimedPass("Shadows");
		if (StaleCount == std::ssize(Stale))
		{
			if (const auto Cleared = Device.ClearTargets({}, State.ShadowAtlas, {}); !Cleared)
			{
				return GraphResult(Cleared);
			}
		}

		for (std::size_t ShadowIndex = 0; ShadowIndex < State.Shadows.size(); ++ShadowIndex)
		{
			const FShadowView& ShadowView = State.Shadows[ShadowIndex];
			if (!Stale[ShadowIndex])
			{
				continue;
			}

			if (StaleCount != std::ssize(Stale))
			{
				constexpr std::array<float, 16> Identity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
				if (const auto Cleared = Device.DrawIndexed({.Pipeline = State.ShadowClearPipeline, .Vertices = State.ShadowClearVertices, .Indices = State.FullscreenIndices, .Texture = State.White, .DepthTarget = State.ShadowAtlas, .WorldToClip = Identity, .IndexCount = 6, .Uniforms = GetVisualUniformBytes(State.Uniforms), .Viewport = ShadowView.Viewport}); !Cleared)
				{
					return GraphResult(Cleared);
				}
			}

			if (State.InstancedShadowPipeline && State.InstancedPipeline)
			{
				const auto Transform = (ShadowView.WorldToClip * FMatrix4(State.Uniforms.ViewToWorld)).Data();
				for (const FImplementation::FInstanceDraw& Instances : State.ViewDraws[ShadowIndex + 1])
				{
					const FImplementation::FInstanceBatch& Batch = State.InstanceBatches[Instances.Batch];
					const FRenderMesh& Mesh = *Batch.Mesh;
					for (const FRenderMesh::FSection& Section : Mesh.Sections)
					{
						const auto Textures = MaterialTextures(Mesh, Section, Batch.Materials);
						const auto Draw = Device.DrawIndexed({.Pipeline = State.InstancedShadowPipeline, .Vertices = Mesh.Vertices, .Indices = Mesh.Indices, .Texture = Textures[0], .DepthTarget = State.ShadowAtlas, .WorldToClip = Transform, .IndexCount = Section.IndexCount, .FirstIndex = Section.FirstIndex, .Instances = State.Instances, .InstanceCount = Instances.InstanceCount, .FirstInstance = Instances.FirstInstance, .Uniforms = GetVisualUniformBytes(State.Uniforms), .Viewport = ShadowView.Viewport});
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
					if (!View.Meshes[Model] || !IsVisible(ShadowIndex + 1, Model))
					{
						continue;
					}

					const FRenderMesh& Mesh = *View.Meshes[Model];
					const auto Overrides = View.Materials.empty() ? std::span<const FRenderMaterial* const>{} : View.Materials[Model];
					for (const FRenderMesh::FSection& Section : Mesh.Sections)
					{
						const auto Textures = MaterialTextures(Mesh, Section, Overrides);
						const auto Draw = Device.DrawIndexed({.Pipeline = State.ShadowPipeline, .Vertices = Mesh.Vertices, .Indices = Mesh.Indices, .Texture = Textures[0], .DepthTarget = State.ShadowAtlas, .WorldToClip = (ShadowView.WorldToClip * View.Models[Model]).Data(), .IndexCount = Section.IndexCount, .FirstIndex = Section.FirstIndex, .Uniforms = GetVisualUniformBytes(State.Uniforms), .Viewport = ShadowView.Viewport});
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

	const auto LightGrid = Graph.ImportResource("Light clusters");
	const auto LightCount = static_cast<std::size_t>(State.Uniforms.Controls[0]);
	if (LightCount != 0)
	{
		Graph.AddPass("Lights", {{.Resource = LightGrid, .Access = ERenderGraphAccess::Write}}, [&]() -> std::expected<void, FRenderGraphError>
		{
			const auto Lights = std::as_bytes(std::span(State.Uniforms.Lights).first(LightCount));
			if (!UpdateKey(State.LightsKey, {Lights, KeyBytes(State.Lights.get())}))
			{
				return {};
			}

			return GraphResult(Device.WriteBuffer(State.Lights, Lights));
		});
	}

	if (State.LightClusterPipeline)
	{
		Graph.AddPass("Light clusters", {{.Resource = LightGrid, .Access = ERenderGraphAccess::ReadWrite}}, [&]() -> std::expected<void, FRenderGraphError>
		{
			// The clusters depend only on the lights, the camera, and the tile grid, so a still camera over still lights keeps them.
			const FVisualUniforms& Uniforms = State.Uniforms;
			if (!UpdateKey(State.LightClustersKey, {std::as_bytes(std::span(Uniforms.Lights).first(LightCount)), KeyBytes(Uniforms.ViewToWorld), KeyBytes(Uniforms.ClipToView), KeyBytes(Uniforms.Viewport), KeyBytes(Uniforms.LightGrid), KeyBytes(Uniforms.Controls[0]), KeyBytes(State.LightClusters.get())}))
			{
				return {};
			}

			const auto Timer = TimedPass("Light clusters");
			const std::array Clusters{State.LightClusters};
			const std::array Sources{State.Lights, State.AllLights};
			return GraphResult(Device.Dispatch({.Pipeline = State.LightClusterPipeline, .StorageBuffers = Clusters, .Uniforms = GetVisualUniformBytes(State.Uniforms), .GroupCount = {(LightTiles.Width + 3) / 4, (LightTiles.Height + 3) / 4, LightClusterSlices / 4}, .Buffers = Sources}));
		});
	}

	Graph.AddPass("Clear", {{.Resource = Color, .Access = ERenderGraphAccess::Write}, {.Resource = Depth, .Access = ERenderGraphAccess::Write}}, [&]
	{
		// The sky pass covers the whole color target, so only depth needs clearing under it.
		auto Result = Device.ClearTargets(State.VisualPipelines[0] ? FTextureHandle{} : MeshColor, FrameDepth, {0.035f, 0.035f, 0.035f, 1.f});
		// The table follows only the atmosphere and the sun, not the camera.
		const FTextureHandle& MultipleScattering = State.SkyMultipleScattering ? State.SkyMultipleScattering : State.White;
		const FVisualUniforms& Uniforms = State.Uniforms;
		if (Result && State.VisualPipelines[0] && UpdateKey(State.SkyViewKey, {KeyBytes(Uniforms.Atmosphere), KeyBytes(Uniforms.AtmosphereGeometry), KeyBytes(Uniforms.SunDirection), KeyBytes(Uniforms.SunIlluminance), KeyBytes(Uniforms.SunRadiance), KeyBytes(MultipleScattering.get())}))
		{
			const std::array SkySources{MultipleScattering};
			Result = Fullscreen(6, State.SkyView, SkySources);
		}

		if (Result && State.VisualPipelines[0])
		{
			const std::array Sources{State.EnvironmentSpecular, State.SkyView};
			Result = Fullscreen(0, MeshColor, Sources);
		}

		return GraphResult(Result);
	});

	// The depth prepass and the PBR pass draw the same visible sections with the same vertex shaders, so the PBR pass can shade only the depth the prepass kept.
	// With occlusion culling, instanced draws read their instance counts from List's indirect arguments.
	const auto DrawScene = [&](const bool bDepthPrepass, const EOcclusionList List) -> std::expected<void, FRenderGraphError>
	{
		const FTextureHandle ColorTarget = bDepthPrepass ? FTextureHandle{} : MeshColor;
		if (State.InstancedPipeline)
		{
			const bool bVelocity = bDepthPrepass && State.bMotionVectors;
			const FGraphicsPipelineHandle& Prepass = bVelocity ? State.InstancedVelocityPrepassPipeline : State.InstancedDepthPrepassPipeline;
			const auto CandidateCount = static_cast<std::uint32_t>(State.CandidateData.size());
			std::uint32_t DrawIndex = 0;
			for (const FImplementation::FInstanceDraw& Instances : State.ViewDraws[0])
			{
				const FImplementation::FInstanceBatch& Batch = State.InstanceBatches[Instances.Batch];
				const FRenderMesh& Mesh = *Batch.Mesh;
				for (const FRenderMesh::FSection& Section : Mesh.Sections)
				{
					const std::uint32_t IndirectOffset = ((static_cast<std::uint32_t>(List) * State.OcclusionDrawCount) + DrawIndex++) * IndirectDrawWords * sizeof(std::uint32_t);
					const FGraphicsPipelineHandle Shading = MaterialPipeline(Section, Batch.Materials, true);
					// Custom material shaders may move vertices, so they skip the prepass and write their own depth.
					if (bDepthPrepass && Shading != State.InstancedPipeline)
					{
						continue;
					}

					auto Textures = MaterialTextures(Mesh, Section, Batch.Materials);
					if (bScreenOcclusion && !bDepthPrepass)
					{
						Textures[11] = State.GtaoOcclusion;
					}

					FIndexedDraw Draw{.Pipeline = bDepthPrepass ? Prepass : Shading, .Vertices = Mesh.Vertices, .Indices = Mesh.Indices, .ColorTarget = bVelocity ? State.MotionVectors : ColorTarget, .DepthTarget = FrameDepth, .IndexCount = Section.IndexCount, .FirstIndex = Section.FirstIndex, .Instances = State.Instances, .InstanceCount = Instances.InstanceCount, .FirstInstance = Instances.FirstInstance, .Textures = Textures, .Uniforms = GetVisualUniformBytes(State.Uniforms), .Buffers = LightBuffers};
					if (State.bOcclusionCulling)
					{
						Draw.Instances = State.CulledInstances;
						Draw.FirstInstance += static_cast<std::uint32_t>(List) * CandidateCount;
						Draw.IndirectArguments = State.OcclusionArguments;
						Draw.IndirectOffset = IndirectOffset;
					}

					const auto Result = Device.DrawIndexed(Draw);
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
			if (View.Meshes[Index] == nullptr || !IsVisible(0, Index))
			{
				continue;
			}

			const FRenderMesh& Mesh = *View.Meshes[Index];
			const std::span<const FRenderMaterial* const> Overrides = View.Materials.empty() ? std::span<const FRenderMaterial* const>{} : View.Materials[Index];
			const auto Transform = (WorldToClip * View.Models[Index]).Data();
			const auto ObjectToView = (View.View * View.Models[Index]).Data();
			for (const FRenderMesh::FSection& Section : Mesh.Sections)
			{
				const FGraphicsPipelineHandle Shading = MaterialPipeline(Section, Overrides, false);
				if (bDepthPrepass && Shading != State.Pipeline)
				{
					continue;
				}

				auto Textures = MaterialTextures(Mesh, Section, Overrides);
				if (bScreenOcclusion && !bDepthPrepass)
				{
					Textures[11] = State.GtaoOcclusion;
				}

				const auto Result = Device.DrawIndexed({.Pipeline = bDepthPrepass ? State.DepthPrepassPipeline : Shading, .Vertices = Mesh.Vertices, .Indices = Mesh.Indices, .ColorTarget = ColorTarget, .DepthTarget = FrameDepth, .WorldToClip = Transform, .IndexCount = Section.IndexCount, .FirstIndex = Section.FirstIndex, .ObjectToView = ObjectToView, .Textures = Textures, .Uniforms = GetVisualUniformBytes(State.Uniforms), .Buffers = LightBuffers});
				if (!Result)
				{
					return GraphResult(Result);
				}

				++State.LastDrawCount;
			}
		}

		return {};
	};

	const auto CullInstances = [&](const std::uint32_t Phase)
	{
		const FTextureDescriptor& HierarchyDescriptor = State.DepthHierarchy->GetDescriptor();
		const FOcclusionCullUniforms Uniforms{.WorldToClip = WorldToClip.Data(), .Counts = {static_cast<std::uint32_t>(State.CandidateData.size()), State.OcclusionDrawCount, Phase, HierarchyDescriptor.MipLevels}, .Hierarchy = {static_cast<float>(HierarchyDescriptor.Extent.Width), static_cast<float>(HierarchyDescriptor.Extent.Height), 0, 0}};
		// The early phase reads only the history, and the hierarchy is unwritten before the first late phase.
		const std::array Hierarchy{Phase == 0 ? State.White : State.DepthHierarchy};
		const std::array Outputs{State.CulledInstances, State.OcclusionArguments, State.OcclusionHistory};
		const std::array Inputs{State.OcclusionCandidates, State.Instances};
		return Device.Dispatch({.Pipeline = State.OcclusionPipelines[1], .Textures = Hierarchy, .StorageBuffers = Outputs, .Uniforms = std::as_bytes(std::span{&Uniforms, 1}), .GroupCount = {(Uniforms.Counts[0] + 63) / 64, 1, 1}, .Buffers = Inputs});
	};

	if (State.DepthPrepassPipeline)
	{
		Graph.AddPass("Depth prepass", {{.Resource = Depth, .Access = ERenderGraphAccess::ReadWrite}, {.Resource = Geometry, .Access = ERenderGraphAccess::ReadWrite}, {.Resource = Texture, .Access = ERenderGraphAccess::Read}, {.Resource = LightGrid, .Access = ERenderGraphAccess::Read}}, [&]() -> std::expected<void, FRenderGraphError>
		{
			if (State.bMotionVectors)
			{
				constexpr std::array<float, 4> Unknown{65504.f, 0.f, 0.f, 0.f};
				if (const auto Cleared = Device.ClearTargets(State.MotionVectors, {}, Unknown); !Cleared)
				{
					return GraphResult(Cleared);
				}
			}

			if (State.bOcclusionCulling)
			{
				const auto Timer = TimedPass("Occlusion culling");
				if (const auto Culled = CullInstances(0); !Culled)
				{
					return GraphResult(Culled);
				}
			}

			const auto Timer = TimedPass("Depth prepass");
			return DrawScene(true, EOcclusionList::Early);
		});
	}

	if (State.DepthPrepassPipeline && State.bOcclusionCulling)
	{
		Graph.AddPass("Depth hierarchy", {{.Resource = Depth, .Access = ERenderGraphAccess::Read}, {.Resource = Geometry, .Access = ERenderGraphAccess::ReadWrite}}, [&]() -> std::expected<void, FRenderGraphError>
		{
			const std::array SceneDepth{FrameDepth};
			const FExtent2D HierarchyExtent = State.DepthHierarchy->GetDescriptor().Extent;
			const std::uint32_t Levels = State.DepthHierarchy->GetDescriptor().MipLevels;
			std::expected<void, FPresentationError> Result;
			auto Timer = TimedPass("Depth hierarchy");
			for (std::uint32_t First = 0; Result && First < Levels; First += DepthHierarchyLevelsPerDispatch)
			{
				// Level 0 reads scene depth, so its source binding is unused; bindings past the last level repeat it and stay unwritten.
				std::array<FStorageTextureBinding, 1 + DepthHierarchyLevelsPerDispatch> Bindings;
				Bindings[0] = {.Texture = State.DepthHierarchy, .MipLevel = First == 0 ? 0 : First - 1};
				for (std::uint32_t Target = 0; Target < DepthHierarchyLevelsPerDispatch; ++Target)
				{
					Bindings[1 + Target] = {.Texture = State.DepthHierarchy, .MipLevel = std::min(First + Target, Levels - 1)};
				}

				const FDepthHierarchyUniforms Uniforms{.Settings = {First == 0 ? 1u : 0u, std::min(DepthHierarchyLevelsPerDispatch, Levels - First), Extent.Width, Extent.Height}};
				const FExtent2D FirstExtent = GetMipExtent(HierarchyExtent, First);
				Result = Device.Dispatch({.Pipeline = State.OcclusionPipelines[0], .Textures = SceneDepth, .StorageTextures = Bindings, .Uniforms = std::as_bytes(std::span{&Uniforms, 1}), .GroupCount = {(FirstExtent.Width + 15) / 16, (FirstExtent.Height + 15) / 16, 1}});
			}

			Timer.reset();
			if (Result)
			{
				const auto CullTimer = TimedPass("Late occlusion culling");
				Result = CullInstances(1);
			}

			return GraphResult(Result);
		});

		Graph.AddPass("Late depth prepass", {{.Resource = Depth, .Access = ERenderGraphAccess::ReadWrite}, {.Resource = Geometry, .Access = ERenderGraphAccess::Read}, {.Resource = Texture, .Access = ERenderGraphAccess::Read}, {.Resource = LightGrid, .Access = ERenderGraphAccess::Read}}, [&]() -> std::expected<void, FRenderGraphError>
		{
			const auto Timer = TimedPass("Late depth prepass");
			return DrawScene(true, EOcclusionList::Late);
		});
	}

	const auto ScreenOcclusion = Graph.ImportResource("Screen occlusion");
	if (bScreenOcclusion)
	{
		Graph.AddPass("Ambient occlusion", {{.Resource = ScreenOcclusion, .Access = ERenderGraphAccess::Write}, {.Resource = Depth, .Access = ERenderGraphAccess::Read}}, [&]() -> std::expected<void, FRenderGraphError>
		{
			const FGtaoUniforms Uniforms = BuildGtaoUniforms(View.Projection, FMatrix4(State.Uniforms.ClipToView), Extent, View.Visuals.AmbientOcclusion);
			const auto UniformBytes = std::as_bytes(std::span{&Uniforms, 1});
			const auto Groups = [](const FExtent2D Target, const std::uint32_t Size)
			{
				return std::array<std::uint32_t, 3>{(Target.Width + Size - 1) / Size, (Target.Height + Size - 1) / Size, 1};
			};

			std::array<FStorageTextureBinding, GtaoDepthMips> DepthMips;
			for (std::uint32_t Mip = 0; Mip < GtaoDepthMips; ++Mip)
			{
				DepthMips[Mip] = {.Texture = State.GtaoViewDepth, .MipLevel = Mip};
			}

			const std::array SceneDepth{FrameDepth};
			const std::array ViewDepth{State.GtaoViewDepth};
			const std::array DenoiseSources{State.GtaoNoisy, State.GtaoViewDepth};
			const std::array Noisy{FStorageTextureBinding{.Texture = State.GtaoNoisy}};
			const std::array Occlusion{FStorageTextureBinding{.Texture = State.GtaoOcclusion}};
			std::expected<void, FPresentationError> Result;
			{
				const auto Timer = TimedPass("GTAO depth");
				Result = Device.Dispatch({.Pipeline = State.GtaoPipelines[0], .Textures = SceneDepth, .StorageTextures = DepthMips, .Uniforms = UniformBytes, .GroupCount = Groups(Extent, 16)});
			}

			if (Result)
			{
				const auto Timer = TimedPass("GTAO");
				Result = Device.Dispatch({.Pipeline = State.GtaoPipelines[1], .Textures = ViewDepth, .StorageTextures = Noisy, .Uniforms = UniformBytes, .GroupCount = Groups(GetMipExtent(Extent, 1), 8)});
			}

			if (Result)
			{
				const auto Timer = TimedPass("GTAO denoise");
				Result = Device.Dispatch({.Pipeline = State.GtaoPipelines[2], .Textures = DenoiseSources, .StorageTextures = Occlusion, .Uniforms = UniformBytes, .GroupCount = Groups(Extent, 8)});
			}

			return GraphResult(Result);
		});
	}

	Graph.AddPass("PBR meshes", {{.Resource = Color, .Access = ERenderGraphAccess::ReadWrite}, {.Resource = Depth, .Access = ERenderGraphAccess::ReadWrite}, {.Resource = Geometry, .Access = ERenderGraphAccess::Read}, {.Resource = Texture, .Access = ERenderGraphAccess::Read}, {.Resource = Shadow, .Access = ERenderGraphAccess::Read}, {.Resource = ScreenOcclusion, .Access = ERenderGraphAccess::Read}, {.Resource = LightGrid, .Access = ERenderGraphAccess::Read}}, [&]() -> std::expected<void, FRenderGraphError>
	{
		const auto Timer = TimedPass("PBR meshes");
		return DrawScene(false, EOcclusionList::Final);
	});

	// After shading, so custom-material surfaces, which write their own depth, reproject from it too.
	const auto MotionVectors = Graph.ImportResource("Motion vectors");
	if (State.bMotionVectors && State.DepthPrepassPipeline)
	{
		Graph.AddPass("Motion vectors", {{.Resource = MotionVectors, .Access = ERenderGraphAccess::Write}, {.Resource = Depth, .Access = ERenderGraphAccess::Read}}, [&]() -> std::expected<void, FRenderGraphError>
		{
			const auto Timer = TimedPass("Motion vectors");
			const std::array SceneDepth{FrameDepth};
			const std::array Target{FStorageTextureBinding{.Texture = State.MotionVectors}};
			return GraphResult(Device.Dispatch({.Pipeline = State.MotionVectorPipeline, .Textures = SceneDepth, .StorageTextures = Target, .Uniforms = GetVisualUniformBytes(State.Uniforms), .GroupCount = {(Extent.Width + 7) / 8, (Extent.Height + 7) / 8, 1}}));
		});
	}

	if (State.VisualPipelines[0])
	{
		Graph.AddPass("Fog, tone mapping and SMAA", {{.Resource = Color, .Access = ERenderGraphAccess::ReadWrite}, {.Resource = Depth, .Access = ERenderGraphAccess::Read}, {.Resource = Shadow, .Access = ERenderGraphAccess::Read}}, [&]() -> std::expected<void, FRenderGraphError>
		{
			// Without fog the volume is never written, so the shaders, which skip it, bind a placeholder.
			FTextureHandle FogSource = State.White;
			if (State.Uniforms.Fog[0] > 0.f)
			{
				const std::array FogSources{State.Depth, State.ShadowAtlas, State.EnvironmentDiffuse};
				if (const auto Result = Fullscreen(1, State.FogColor, FogSources); !Result)
				{
					return GraphResult(Result);
				}

				FogSource = State.FogColor;
			}

			FTextureHandle ExposureSource = State.White;
			if (State.Uniforms.Exposure[0] > 0.5f)
			{
				const std::size_t Next = State.ExposureStateIndex ^ 1;
				// A restart ignores the previous value, which may not have been written yet.
				const bool bRestart = State.Uniforms.Exposure[3] > 0.5f;
				const std::array Sources{State.HdrColor, FogSource, bRestart ? State.White : State.ExposureStates[State.ExposureStateIndex]};
				const std::array Adapted{FStorageTextureBinding{.Texture = State.ExposureStates[Next]}};
				const auto Timer = TimedPass("Exposure");
				if (const auto Result = Device.Dispatch({.Pipeline = State.ExposurePipeline, .Textures = Sources, .StorageTextures = Adapted, .Uniforms = GetVisualUniformBytes(State.Uniforms), .GroupCount = {1, 1, 1}}); !Result)
				{
					return GraphResult(Result);
				}

				State.ExposureStateIndex = Next;
				ExposureSource = State.ExposureStates[Next];
			}

			const bool bSmaa = View.Visuals.AntiAliasing != EAntiAliasing::Off;
			// The curve depends only on the display's paper white and peak.
			if (UpdateKey(State.ToneMapLutKey, {KeyBytes(State.Uniforms.Display)}))
			{
				const std::array LutSources{State.White};
				if (const auto Result = Fullscreen(7, State.ToneMapLut, LutSources); !Result)
				{
					return GraphResult(Result);
				}
			}

			const std::array ToneSources{State.HdrColor, ExposureSource, State.ToneMapLut, FogSource, State.Depth};
			if (const auto Result = Fullscreen(2, bSmaa ? State.ToneColor : State.Color, ToneSources); !Result)
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
				const std::array EdgeSources{State.ToneColor};
				const std::array WeightSources{State.Edges, State.SmaaArea, State.SmaaSearch};
				if (State.SmaaPipelines[0])
				{
					const auto Uniforms = GetVisualUniformBytes(State.Uniforms);
					const std::array Buffers{State.SmaaEdgePixels, State.SmaaArguments};
					const std::array Edges{FStorageTextureBinding{.Texture = State.Edges}};
					const std::array Weights{FStorageTextureBinding{.Texture = State.Weights}};
					constexpr std::array<std::uint32_t, 4> ResetArguments{0, 1, 1, 0};
					std::expected<void, FPresentationError> Result;
					{
						const auto Timer = TimedPass("SMAA edges");
						Result = Device.WriteBuffer(State.SmaaArguments, std::as_bytes(std::span{ResetArguments}));
						if (Result)
						{
							Result = Device.Dispatch({.Pipeline = State.SmaaPipelines[0], .Textures = EdgeSources, .StorageTextures = Edges, .StorageBuffers = Buffers, .Uniforms = Uniforms, .GroupCount = {(Extent.Width + 7) / 8, (Extent.Height + 7) / 8, 1}});
						}
					}

					if (Result)
					{
						const auto Timer = TimedPass("SMAA weights");
						Result = Device.ClearTargets(State.Weights, {}, {});
						if (Result)
						{
							Result = Device.Dispatch({.Pipeline = State.SmaaPipelines[1], .Textures = WeightSources, .StorageTextures = Weights, .StorageBuffers = Buffers, .Uniforms = Uniforms, .IndirectArguments = State.SmaaArguments});
						}
					}

					if (!Result)
					{
						return GraphResult(Result);
					}
				}
				else
				{
					if (const auto Result = Device.ClearTargets(State.Edges, {}, {}); !Result)
					{
						return GraphResult(Result);
					}

					if (const auto Result = Fullscreen(3, State.Edges, EdgeSources); !Result)
					{
						return GraphResult(Result);
					}

					if (const auto Result = Fullscreen(4, State.Weights, WeightSources); !Result)
					{
						return GraphResult(Result);
					}
				}

				const std::array NeighborSources{State.ToneColor, State.Weights};
				if (const auto Result = Fullscreen(5, State.Color, NeighborSources); !Result)
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
				Result = Device.DrawIndexed({.Pipeline = Display.Grid, .Vertices = State.GridVertices, .Indices = State.GridIndices, .Texture = {}, .ColorTarget = State.Color, .DepthTarget = FrameDepth, .WorldToClip = FMatrix4::Identity().Data(), .IndexCount = static_cast<std::uint32_t>(GridIndices.size())});
				if (Result)
				{
					++State.LastDrawCount;
				}
			}

			return GraphResult(Result);
		});
	}

	if (Display.Outline && State.ShadowPipeline && !View.Selected.empty())
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
			if (State.InstancedShadowPipeline && State.InstancedPipeline)
			{
				const auto Transform = (WorldToClip * FMatrix4(State.Uniforms.ViewToWorld)).Data();
				for (const FImplementation::FInstanceDraw& Instances : State.ViewDraws.back())
				{
					const FImplementation::FInstanceBatch& Batch = State.InstanceBatches[Instances.Batch];
					const FRenderMesh& Mesh = *Batch.Mesh;
					for (const FRenderMesh::FSection& Section : Mesh.Sections)
					{
						const auto Textures = MaterialTextures(Mesh, Section, Batch.Materials);
						const auto Draw = Device.DrawIndexed({.Pipeline = State.InstancedShadowPipeline, .Vertices = Mesh.Vertices, .Indices = Mesh.Indices, .Texture = Textures[0], .DepthTarget = State.SelectionDepth, .WorldToClip = Transform, .IndexCount = Section.IndexCount, .FirstIndex = Section.FirstIndex, .Instances = State.Instances, .InstanceCount = Instances.InstanceCount, .FirstInstance = Instances.FirstInstance, .Uniforms = GetVisualUniformBytes(State.Uniforms)});
						if (!Draw)
						{
							return GraphResult(Draw);
						}

						++State.LastDrawCount;
					}
				}
			}

			for (const std::size_t Index : State.InstancedShadowPipeline&& State.InstancedPipeline ? std::span<const std::size_t>{} : View.Selected)
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
					const auto Draw = Device.DrawIndexed({.Pipeline = State.ShadowPipeline, .Vertices = Mesh.Vertices, .Indices = Mesh.Indices, .Texture = Textures[0], .DepthTarget = State.SelectionDepth, .WorldToClip = Transform, .IndexCount = Section.IndexCount, .FirstIndex = Section.FirstIndex, .Uniforms = GetVisualUniformBytes(State.Uniforms)});
					if (!Draw)
					{
						return GraphResult(Draw);
					}

					++State.LastDrawCount;
				}
			}

			// The outline shader works in pixel coordinates, so shading only the selection's screen bounds skips the rest of the target.
			std::uint32_t Left = Extent.Width;
			std::uint32_t Top = Extent.Height;
			std::uint32_t Right = 0;
			std::uint32_t Bottom = 0;
			for (const std::size_t Index : View.Selected)
			{
				if (Index >= State.ModelBounds.size() || View.Meshes[Index] == nullptr)
				{
					continue;
				}

				const FImplementation::FModelBounds& Bounds = State.ModelBounds[Index];
				const FRenderViewport Screen = GetBoxScreenBounds(WorldToClip, Extent, Bounds.Center, Bounds.Extent);
				if (Screen.Width > 0 && Screen.Height > 0)
				{
					Left = std::min(Left, Screen.X);
					Top = std::min(Top, Screen.Y);
					Right = std::max(Right, Screen.X + Screen.Width);
					Bottom = std::max(Bottom, Screen.Y + Screen.Height);
				}
			}

			if (Right <= Left || Bottom <= Top)
			{
				return {};
			}

			// The outline extends two pixels past the silhouette.
			constexpr std::uint32_t OutlinePadding = 3;
			Left -= std::min(Left, OutlinePadding);
			Top -= std::min(Top, OutlinePadding);
			Right = std::min(Extent.Width, Right + OutlinePadding);
			Bottom = std::min(Extent.Height, Bottom + OutlinePadding);
			const std::array Sources{State.SelectionDepth, State.Depth};
			return GraphResult(Device.DrawIndexed({.Pipeline = Display.Outline, .Vertices = State.FullscreenVertices, .Indices = State.FullscreenIndices, .ColorTarget = State.Color, .IndexCount = 6, .Textures = Sources, .Uniforms = GetVisualUniformBytes(State.Uniforms), .Viewport = {.X = Left, .Y = Top, .Width = Right - Left, .Height = Bottom - Top}}));
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
				Result = Device.DrawIndexed({.Pipeline = Display.Debug[Index], .Vertices = State.DebugVertices[Index], .Indices = State.DebugIndices[Index], .Texture = {}, .ColorTarget = State.Color, .DepthTarget = Index == 0 ? FrameDepth : FTextureHandle{}, .WorldToClip = FMatrix4::Identity().Data(), .IndexCount = static_cast<std::uint32_t>(Indices.size())});
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

	// A recording that never reached the GPU may have refreshed a cached result, so nothing cached can be trusted afterwards.
	const auto ResetCaches = [&State]
	{
		for (std::vector<std::byte>* Key : {&State.SkyViewKey, &State.ToneMapLutKey, &State.LightsKey, &State.LightClustersKey})
		{
			Key->clear();
		}

		State.ShadowKeys.clear();
	};

	auto Execution = Graph.Execute();
	if (!Execution)
	{
		Device.CancelCommands();
		ResetCaches();
		return std::unexpected(FPresentationError{.Code = EPresentationErrorCode::CommandSubmissionFailed, .Message = Execution.error().Message});
	}

	auto Submission = Device.SubmitCommands();
	if (!Submission)
	{
		ResetCaches();
		return std::unexpected(Submission.error());
	}

	return {};
}
}
