#include "RendererSmoke.h"

#include "Herta/Math/Matrix.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <numbers>

namespace Herta
{
namespace
{
[[nodiscard]] std::unexpected<FPresentationError> Failure(const char* Message)
{
	return std::unexpected(FPresentationError{.Code = EPresentationErrorCode::InvalidState, .Message = Message});
}

// The renderer owns no content. The checks draw a white 1 m cube, scaled to 2 m where they measure coverage and occlusion.
[[nodiscard]] std::expected<std::shared_ptr<const FRenderMesh>, FPresentationError> CreateSmokeCube(IGraphicsDevice& Device)
{
	FCookedTexture White{.ColorSpace = ETextureColorSpace::Srgb, .Mips = {{.Width = 1, .Height = 1, .Pixels = std::vector<std::byte>(4, std::byte{255})}}};
	return FRenderMesh::Create(Device, CreateTexturedCubeModel(std::move(White)), "Smoke cube");
}

[[nodiscard]] std::expected<void, FPresentationError> CheckInvalidCommands(IGraphicsDevice& Device)
{
	if (Device.SubmitCommands())
	{
		return Failure("Submitting without an open graphics frame unexpectedly succeeded");
	}

	if (Device.CreateBuffer({.Name = "Invalid usage", .Size = 4, .Usage = static_cast<EBufferUsage>(255)}))
	{
		return Failure("Invalid buffer usage was accepted");
	}

	if (Device.CreateTexture({.Name = "Invalid format", .Extent = {.Width = 1, .Height = 1}, .Format = static_cast<ETextureFormat>(255), .bRenderTarget = false}))
	{
		return Failure("Invalid texture format was accepted");
	}

	auto Buffer = Device.CreateBuffer({.Name = "Upload validation buffer", .Size = 4, .Usage = EBufferUsage::Index});
	if (!Buffer)
	{
		return std::unexpected(Buffer.error());
	}

	auto Texture = Device.CreateTexture({.Name = "Upload validation texture", .Extent = {.Width = 1, .Height = 1}, .Format = ETextureFormat::Rgba8, .bRenderTarget = false});
	if (!Texture)
	{
		return std::unexpected(Texture.error());
	}

	if (Device.ReadbackTexture(*Texture))
	{
		return Failure("Readback of an uninitialized texture unexpectedly succeeded");
	}

	const auto Begin = Device.BeginCommands();
	if (!Begin)
	{
		return Begin;
	}

	if (Device.BeginCommands())
	{
		Device.CancelCommands();
		return Failure("Opening a second graphics frame unexpectedly succeeded");
	}

	if (Device.WriteBuffer(*Buffer, {}) || Device.WriteTexture(*Texture, 0, {}))
	{
		Device.CancelCommands();
		return Failure("Incomplete resource upload unexpectedly succeeded");
	}

	Device.CancelCommands();
	return {};
}

[[nodiscard]] std::expected<void, FPresentationError> CheckResourceLifetimes(IGraphicsDevice& Device)
{
	constexpr std::array<std::uint32_t, 1> Indices{0};
	constexpr std::array<std::uint8_t, 4> Texels{255, 255, 255, 255};
	for (const bool bSubmit : {false, true})
	{
		auto Buffer = Device.CreateBuffer({.Name = "Lifetime test buffer", .Size = sizeof(Indices), .Usage = EBufferUsage::Index});
		if (!Buffer)
		{
			return std::unexpected(Buffer.error());
		}

		auto Texture = Device.CreateTexture({.Name = "Lifetime test texture", .Extent = {.Width = 1, .Height = 1}, .Format = ETextureFormat::Rgba8, .bRenderTarget = false});
		if (!Texture)
		{
			return std::unexpected(Texture.error());
		}

		auto Result = Device.BeginCommands();
		if (!Result)
		{
			return Result;
		}

		Result = Device.WriteBuffer(*Buffer, std::as_bytes(std::span(Indices)));
		if (Result)
		{
			Result = Device.WriteTexture(*Texture, 0, std::as_bytes(std::span(Texels)));
		}

		if (!Result)
		{
			Device.CancelCommands();
			return Result;
		}

		const std::weak_ptr<IRhiBuffer> BufferLifetime = *Buffer;
		const std::weak_ptr<IRhiTexture> TextureLifetime = *Texture;
		Buffer->reset();
		Texture->reset();
		if (BufferLifetime.expired() || TextureLifetime.expired())
		{
			Device.CancelCommands();
			return Failure("Recorded upload commands did not retain their resources");
		}

		if (bSubmit)
		{
			const auto Submission = Device.SubmitCommands();
			if (!Submission)
			{
				Device.CancelCommands();
				return std::unexpected(Submission.error());
			}

			// A completed GPU submission may retire immediately; unfinished work must retain ownership.
			if (Device.GetStatistics().CompletedSerial < *Submission && (BufferLifetime.expired() || TextureLifetime.expired()))
			{
				return Failure("Submitted upload resources were released before GPU completion");
			}

			Result = Device.WaitForIdle();
			if (!Result)
			{
				return Result;
			}
		}
		else
		{
			Device.CancelCommands();
		}

		if (!BufferLifetime.expired() || !TextureLifetime.expired())
		{
			return Failure(bSubmit ? "Completed upload resources were not retired" : "Cancelled upload resources were not released");
		}
	}

	return {};
}

[[nodiscard]] std::expected<void, FPresentationError> CheckWorldGrid(IGraphicsDevice& Device, FMeshRenderer& Renderer)
{
	constexpr FExtent2D Extent{.Width = 128, .Height = 128};
	const auto Cube = CreateSmokeCube(Device);
	if (!Cube)
	{
		return std::unexpected(Cube.error());
	}

	const FRenderMesh* const CubeMesh = Cube->get();
	const FMatrix4 Model = FMatrix4::Scale({2, 2, 2});
	const FMatrix4 Projection = FMatrix4::PerspectiveReversedInfinite(std::numbers::pi_v<float> / 2.f, 1.f, 0.1f);
	const float Pitch = std::atan2(3.f, 5.f);
	for (const bool bBelowPlane : {false, true})
	{
		const float Height = bBelowPlane ? -3.f : 3.f;
		FMeshRenderView View{
		    .View = FMatrix4::Rotation(FQuaternion::FromAxisAngle({1, 0, 0}, bBelowPlane ? Pitch : -Pitch)) * FMatrix4::Translation({0, -Height, 5}),
		    .Projection = Projection,
		    .Models = std::span{&Model, 1},
		    .GridCenter = {0, 0, 0},
		    .Meshes = std::span{&CubeMesh, 1},
		};

		auto Result = Renderer.Render(Extent, View);
		if (!Result)
		{
			return Result;
		}

		const auto WithoutGrid = Device.ReadbackTexture(Renderer.GetColorTarget());
		if (!WithoutGrid)
		{
			return std::unexpected(WithoutGrid.error());
		}

		View.bDrawGrid = true;
		Result = Renderer.Render(Extent, View);
		if (!Result)
		{
			return Result;
		}

		const auto WithGrid = Device.ReadbackTexture(Renderer.GetColorTarget());
		if (!WithGrid)
		{
			return std::unexpected(WithGrid.error());
		}

		if (WithoutGrid->size() != Extent.Width * Extent.Height * 4 || WithGrid->size() != WithoutGrid->size())
		{
			return Failure("World grid readback has an incorrect byte count");
		}

		std::size_t ChangedPixels = 0;
		for (std::size_t Pixel = 0; Pixel < WithGrid->size(); Pixel += 4)
		{
			ChangedPixels += !std::equal(WithoutGrid->begin() + static_cast<std::ptrdiff_t>(Pixel), WithoutGrid->begin() + static_cast<std::ptrdiff_t>(Pixel + 3), WithGrid->begin() + static_cast<std::ptrdiff_t>(Pixel)) ? 1u : 0u;
		}

		if (ChangedPixels < 8)
		{
			return Failure(bBelowPlane ? "World grid was not visible from below the plane" : "World grid did not change the rendered scene");
		}

		for (std::size_t Y = 60; Y < 68; ++Y)
		{
			for (std::size_t X = 60; X < 68; ++X)
			{
				const std::size_t Pixel = (Y * Extent.Width + X) * 4;
				if (!std::equal(WithoutGrid->begin() + static_cast<std::ptrdiff_t>(Pixel), WithoutGrid->begin() + static_cast<std::ptrdiff_t>(Pixel + 4), WithGrid->begin() + static_cast<std::ptrdiff_t>(Pixel)))
				{
					return Failure("World grid rendered through the nearer cube");
				}
			}
		}
	}

	return {};
}

[[nodiscard]] std::expected<void, FPresentationError> CheckMultipleModels(IGraphicsDevice& Device, FMeshRenderer& Renderer)
{
	constexpr FExtent2D Extent{.Width = 128, .Height = 128};
	const std::array Models{FMatrix4::Translation({2, 0, 0}) * FMatrix4::Scale({2, 2, 2}), FMatrix4::Translation({-2, 0, 0}) * FMatrix4::Scale({1.5f, 1, 2})};
	const auto Cube = CreateSmokeCube(Device);
	if (!Cube)
	{
		return std::unexpected(Cube.error());
	}

	const std::array<const FRenderMesh*, 2> Meshes{Cube->get(), Cube->get()};
	FMeshRenderView View{.View = FMatrix4::Translation({0, 0, 5}), .Projection = FMatrix4::PerspectiveReversedInfinite(std::numbers::pi_v<float> / 2.f, 1.f, 0.1f), .Models = {}};
	auto Result = Renderer.Render(Extent, View);
	if (!Result)
	{
		return Result;
	}

	const auto Background = Device.ReadbackTexture(Renderer.GetColorTarget());
	if (!Background)
	{
		return std::unexpected(Background.error());
	}

	std::array<std::vector<std::byte>, 2> Isolated;
	for (std::size_t Index = 0; Index < Models.size(); ++Index)
	{
		View.Models = std::span{Models}.subspan(Index, 1);
		View.Meshes = std::span{Meshes}.subspan(Index, 1);
		Result = Renderer.Render(Extent, View);
		if (!Result)
		{
			return Result;
		}

		auto Pixels = Device.ReadbackTexture(Renderer.GetColorTarget());
		if (!Pixels)
		{
			return std::unexpected(Pixels.error());
		}

		Isolated[Index] = std::move(*Pixels);
	}

	View.Models = Models;
	View.Meshes = Meshes;
	Result = Renderer.Render(Extent, View);
	if (!Result)
	{
		return Result;
	}

	const auto Combined = Device.ReadbackTexture(Renderer.GetColorTarget());
	if (!Combined)
	{
		return std::unexpected(Combined.error());
	}

	if (Combined->size() != Extent.Width * Extent.Height * 4 || Background->size() != Combined->size() || Isolated[0].size() != Combined->size() || Isolated[1].size() != Combined->size())
	{
		return Failure("Multiple-model readback has an incorrect byte count");
	}

	std::array<std::size_t, 2> Coverage{};
	for (std::size_t Pixel = 0; Pixel < Combined->size(); Pixel += 4)
	{
		const auto Offset = static_cast<std::ptrdiff_t>(Pixel);
		const bool bFirstModel = !std::equal(Isolated[0].begin() + Offset, Isolated[0].begin() + Offset + 4, Background->begin() + Offset);
		const bool bSecondModel = !std::equal(Isolated[1].begin() + Offset, Isolated[1].begin() + Offset + 4, Background->begin() + Offset);
		Coverage[0] += bFirstModel ? 1u : 0u;
		Coverage[1] += bSecondModel ? 1u : 0u;
		const auto& Expected = bFirstModel ? Isolated[0] : Isolated[1];
		if ((bFirstModel && bSecondModel) || !std::equal(Expected.begin() + Offset, Expected.begin() + Offset + 4, Combined->begin() + Offset))
		{
			return Failure("Multiple-model rendering did not preserve each draw's transform");
		}
	}

	if (Coverage[0] < 100 || Coverage[1] < 100)
	{
		return Failure("Multiple-model rendering missed a transformed mesh");
	}

	return {};
}

// Two sections of one mesh draw with their own textures. The right-hand section samples a mip chain uploaded level by level.
[[nodiscard]] std::expected<void, FPresentationError> CheckRenderMeshSections(IGraphicsDevice& Device, FMeshRenderer& Renderer)
{
	const auto SolidTexture = [](const std::uint32_t Size, const std::array<std::uint8_t, 4>& Color)
	{
		FCookedTexture Texture{.ColorSpace = ETextureColorSpace::Srgb, .Mips = {}};
		for (std::uint32_t Level = Size; Level > 0; Level /= 2)
		{
			FCookedTextureMip Mip{.Width = Level, .Height = Level, .Pixels = std::vector<std::byte>(std::size_t{Level} * Level * 4)};
			for (std::size_t Byte = 0; Byte < Mip.Pixels.size(); ++Byte)
			{
				Mip.Pixels[Byte] = static_cast<std::byte>(Color[Byte % 4]);
			}

			Texture.Mips.push_back(std::move(Mip));
		}

		return Texture;
	};

	// +X is left in Herta, so the negative-X quad appears on the right half of the image.
	FCookedModel Model;
	Model.Vertices = {{.Position = {-3, -2, 0}, .UV = {0, 1}}, {.Position = {-3, 2, 0}, .UV = {0, 0}}, {.Position = {-0.5f, 2, 0}, .UV = {1, 0}}, {.Position = {-0.5f, -2, 0}, .UV = {1, 1}}, {.Position = {0.5f, -2, 0}, .UV = {0, 1}}, {.Position = {0.5f, 2, 0}, .UV = {0, 0}}, {.Position = {3, 2, 0}, .UV = {1, 0}}, {.Position = {3, -2, 0}, .UV = {1, 1}}};
	Model.Indices = {0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7};
	Model.Sections = {{.FirstIndex = 0, .IndexCount = 6, .Material = 0}, {.FirstIndex = 6, .IndexCount = 6, .Material = 1}};
	Model.Materials = {{.Name = "Red", .BaseColorTexture = 0}, {.Name = "Green", .BaseColorTexture = 1}};
	Model.Textures = {SolidTexture(1, {255, 0, 0, 255}), SolidTexture(8, {0, 255, 0, 255})};
	auto Mesh = FRenderMesh::Create(Device, Model, "Section smoke");
	if (!Mesh)
	{
		return std::unexpected(Mesh.error());
	}

	constexpr FExtent2D Extent{.Width = 128, .Height = 128};
	const FMatrix4 Identity;
	const FRenderMesh* const MeshPointer = Mesh->get();
	FMeshRenderView View{.View = FMatrix4::Translation({0, 0, 5}), .Projection = FMatrix4::PerspectiveReversedInfinite(std::numbers::pi_v<float> / 2.f, 1.f, 0.1f), .Models = std::span{&Identity, 1}};
	View.Meshes = std::span{&MeshPointer, 1};
	if (auto Result = Renderer.Render(Extent, View); !Result)
	{
		return Result;
	}

	const auto Pixels = Device.ReadbackTexture(Renderer.GetColorTarget());
	if (!Pixels)
	{
		return std::unexpected(Pixels.error());
	}

	std::size_t RedOnRight = 0;
	std::size_t GreenOnLeft = 0;
	for (std::size_t Pixel = 0; Pixel < Pixels->size() / 4; ++Pixel)
	{
		const auto Channel = [&](const std::size_t Index)
		{
			return static_cast<std::uint8_t>((*Pixels)[Pixel * 4 + Index]);
		};

		const bool bRight = Pixel % Extent.Width >= Extent.Width / 2;
		RedOnRight += bRight && Channel(0) > 200 && Channel(1) < 50 ? 1u : 0u;
		GreenOnLeft += !bRight && Channel(1) > 200 && Channel(0) < 50 ? 1u : 0u;
	}

	if (RedOnRight < 100 || GreenOnLeft < 100)
	{
		return Failure("Render mesh sections did not draw with their own index ranges and textures");
	}

	return {};
}

[[nodiscard]] std::expected<void, FPresentationError> CheckDebugDraw(IGraphicsDevice& Device, const FShaderAsset& VertexShader, const FShaderAsset& FragmentShader, const FShaderAsset& DebugVertexShader, const FShaderAsset& DebugFragmentShader, const FShaderAsset& GridVertexShader, const FShaderAsset& GridFragmentShader)
{
	auto Renderer = FMeshRenderer::Create(Device, VertexShader, FragmentShader, DebugVertexShader, DebugFragmentShader, GridVertexShader, GridFragmentShader);
	if (!Renderer)
	{
		return std::unexpected(Renderer.error());
	}

	const auto Cube = CreateSmokeCube(Device);
	if (!Cube)
	{
		return std::unexpected(Cube.error());
	}

	const FRenderMesh* const CubeMesh = Cube->get();
	const FMatrix4 Model = FMatrix4::Scale({2, 2, 2});
	FMeshRenderView View{.View = FMatrix4::Translation({0, 0, 5}), .Projection = FMatrix4::PerspectiveReversedInfinite(std::numbers::pi_v<float> / 2.f, 1, 0.1f), .Models = std::span{&Model, 1}};
	View.Meshes = std::span{&CubeMesh, 1};
	auto Result = (*Renderer)->Render({.Width = 128, .Height = 128}, View);
	if (!Result)
	{
		return Result;
	}

	const auto Baseline = Device.ReadbackTexture((*Renderer)->GetColorTarget());
	if (!Baseline)
	{
		return std::unexpected(Baseline.error());
	}

	const std::array<FDebugDrawVertex, 1> Points{{{.Position = {0, 0, 1.5f}, .Size = 12, .Color = {1, 0, 0, 1}}}};
	const std::array<FDebugDrawVertex, 2> Lines{{{.Position = {-0.3f, -0.8f, 1.5f}, .Size = 4, .Color = {0, 1, 0, 1}}, {.Position = {0.3f, -0.8f, 1.5f}, .Size = 4, .Color = {0, 1, 0, 1}}}};
	const std::array<FDebugDrawVertex, 3> Triangles{{{.Position = {-0.3f, 0.8f, 1.5f}, .Size = 1, .Color = {0, 0, 1, 1}}, {.Position = {0.3f, 0.8f, 1.5f}, .Size = 1, .Color = {0, 0, 1, 1}}, {.Position = {0, 1.1f, 1.5f}, .Size = 1, .Color = {0, 0, 1, 1}}}};
	std::array Lists{FDebugDrawList{.Primitive = EDebugPrimitive::Points, .Vertices = Points}, FDebugDrawList{.Primitive = EDebugPrimitive::Lines, .Vertices = Lines}, FDebugDrawList{.Primitive = EDebugPrimitive::Triangles, .Vertices = Triangles}};
	Result = (*Renderer)->Render({.Width = 128, .Height = 128}, View, Lists);
	if (!Result)
	{
		return Result;
	}

	const auto Occluded = Device.ReadbackTexture((*Renderer)->GetColorTarget());
	if (!Occluded)
	{
		return std::unexpected(Occluded.error());
	}

	if (*Baseline != *Occluded)
	{
		return Failure("Depth-tested debug primitives overwrote the nearer cube");
	}

	for (FDebugDrawList& List : Lists)
	{
		List.bDepthTest = false;
	}

	Result = (*Renderer)->Render({.Width = 128, .Height = 128}, View, Lists);
	if (!Result)
	{
		return Result;
	}

	const auto Overlay = Device.ReadbackTexture((*Renderer)->GetColorTarget());
	if (!Overlay)
	{
		return std::unexpected(Overlay.error());
	}

	std::array<std::size_t, 3> Coverage{};
	for (std::size_t Pixel = 0; Pixel < Overlay->size(); Pixel += 4)
	{
		for (std::size_t Channel = 0; Channel < 3; ++Channel)
		{
			if (std::to_integer<unsigned>((*Overlay)[Pixel + Channel]) > 250 && std::to_integer<unsigned>((*Overlay)[Pixel + (Channel + 1) % 3]) < 5 && std::to_integer<unsigned>((*Overlay)[Pixel + (Channel + 2) % 3]) < 5)
			{
				++Coverage[Channel];
			}
		}
	}

	if (Coverage[0] < 130 || Coverage[0] > 160 || Coverage[1] < 10 || Coverage[2] < 3)
	{
		return Failure("Debug point, line, or triangle overlay has incorrect pixel coverage");
	}

	return CheckWorldGrid(Device, **Renderer);
}
}

std::expected<void, FPresentationError> RunRendererSmoke(IGraphicsDevice& Device, const FShaderAsset& VertexShader, const FShaderAsset& FragmentShader, const FShaderAsset& DebugVertexShader, const FShaderAsset& DebugFragmentShader, const FShaderAsset& GridVertexShader, const FShaderAsset& GridFragmentShader)
{
	if (const auto InvalidCommands = CheckInvalidCommands(Device); !InvalidCommands)
	{
		return InvalidCommands;
	}

	auto Renderer = FMeshRenderer::Create(Device, VertexShader, FragmentShader);
	if (!Renderer)
	{
		return std::unexpected(Renderer.error());
	}

	const auto Cube = CreateSmokeCube(Device);
	if (!Cube)
	{
		return std::unexpected(Cube.error());
	}

	const FRenderMesh* const CubeMesh = Cube->get();
	const FMatrix4 Model = FMatrix4::Rotation(FQuaternion::FromAxisAngle({0, 1, 0}, 0.4f) * FQuaternion::FromAxisAngle({1, 0, 0}, -0.25f)) * FMatrix4::Scale({2, 2, 2});
	constexpr std::array<FExtent2D, 8> Extents{{{.Width = 128, .Height = 128}, {.Width = 64, .Height = 192}, {.Width = 0, .Height = 0}, {.Width = 320, .Height = 180}, {.Width = 1, .Height = 1}, {.Width = 192, .Height = 64}, {.Width = 0, .Height = 128}, {.Width = 128, .Height = 128}}};
	for (std::size_t Frame = 0; Frame < 32; ++Frame)
	{
		const FExtent2D Extent = Extents[Frame % Extents.size()];
		const float Aspect = Extent.IsEmpty() ? 1.f : static_cast<float>(Extent.Width) / static_cast<float>(Extent.Height);
		FMeshRenderView View{.View = FMatrix4::Translation({0, 0, 5}), .Projection = FMatrix4::PerspectiveReversedInfinite(std::numbers::pi_v<float> / 3.f, Aspect, 0.1f), .Models = std::span{&Model, 1}};
		View.Meshes = std::span{&CubeMesh, 1};
		auto Result = (*Renderer)->Render(Extent, View);
		if (!Result)
		{
			return Result;
		}
	}

	auto Pixels = Device.ReadbackTexture((*Renderer)->GetColorTarget());
	if (!Pixels)
	{
		return std::unexpected(Pixels.error());
	}

	if (Pixels->size() != 128 * 128 * 4)
	{
		return Failure("Textured mesh readback has an incorrect byte count");
	}

	std::size_t MeshPixels = 0;
	for (std::size_t Index = 0; Index < Pixels->size(); Index += 4)
	{
		MeshPixels += std::to_integer<unsigned>((*Pixels)[Index + 2]) > 120 ? 1u : 0u;
	}

	if (MeshPixels < 500 || MeshPixels > 128 * 128 / 2)
	{
		return Failure("Textured mesh readback has incorrect coverage (projection, winding, or shader failure)");
	}

	if (const auto Sections = CheckRenderMeshSections(Device, **Renderer); !Sections)
	{
		return Sections;
	}

	if (const auto MultipleModels = CheckMultipleModels(Device, **Renderer); !MultipleModels)
	{
		return MultipleModels;
	}

	// Submit the near red quad before the far green quad so a disabled depth test cannot pass.
	constexpr std::array<FMeshVertex, 4> NearVertices{{{.Position = {-1, -1, 0}, .UV = {0.25f, 0.5f}}, {.Position = {-1, 1, 0}, .UV = {0.25f, 0.5f}}, {.Position = {1, 1, 0}, .UV = {0.25f, 0.5f}}, {.Position = {1, -1, 0}, .UV = {0.25f, 0.5f}}}};
	auto FarVertices = NearVertices;
	for (FMeshVertex& Vertex : FarVertices)
	{
		Vertex.UV[0] = 0.75f;
	}

	constexpr std::array<std::uint32_t, 6> Indices{0, 1, 2, 0, 2, 3};
	constexpr std::array<std::uint8_t, 8> Texels{255, 0, 0, 255, 0, 255, 0, 255};
	auto Near = Device.CreateBuffer({.Name = "Depth test near", .Size = sizeof(NearVertices), .Usage = EBufferUsage::Vertex});
	auto Far = Device.CreateBuffer({.Name = "Depth test far", .Size = sizeof(FarVertices), .Usage = EBufferUsage::Vertex});
	auto Index = Device.CreateBuffer({.Name = "Depth test indices", .Size = sizeof(Indices), .Usage = EBufferUsage::Index});
	auto Texture = Device.CreateTexture({.Name = "Depth test colors", .Extent = {.Width = 2, .Height = 1}, .Format = ETextureFormat::Rgba8Srgb, .bRenderTarget = false});
	auto Color = Device.CreateTexture({.Name = "Depth test color", .Extent = {.Width = 64, .Height = 64}, .Format = ETextureFormat::Rgba8Srgb, .bRenderTarget = true});
	auto Depth = Device.CreateTexture({.Name = "Depth test depth", .Extent = {.Width = 64, .Height = 64}, .Format = ETextureFormat::Depth32, .bRenderTarget = true});
	auto Pipeline = Device.CreateGraphicsPipeline({.Name = "Depth regression", .VertexShader = VertexShader, .FragmentShader = FragmentShader, .ColorFormat = ETextureFormat::Rgba8Srgb});
	if (!Near)
	{
		return std::unexpected(Near.error());
	}

	if (!Far)
	{
		return std::unexpected(Far.error());
	}

	if (!Index)
	{
		return std::unexpected(Index.error());
	}

	if (!Texture)
	{
		return std::unexpected(Texture.error());
	}

	if (!Color)
	{
		return std::unexpected(Color.error());
	}

	if (!Depth)
	{
		return std::unexpected(Depth.error());
	}

	if (!Pipeline)
	{
		return std::unexpected(Pipeline.error());
	}

	auto Result = Device.BeginCommands();
	if (!Result)
	{
		return Result;
	}

	Result = Device.WriteBuffer(*Near, std::as_bytes(std::span(NearVertices)));
	if (Result)
	{
		Result = Device.WriteBuffer(*Far, std::as_bytes(std::span(FarVertices)));
	}

	if (Result)
	{
		Result = Device.WriteBuffer(*Index, std::as_bytes(std::span(Indices)));
	}

	if (Result)
	{
		Result = Device.WriteTexture(*Texture, 0, std::as_bytes(std::span(Texels)));
	}

	if (Result)
	{
		Result = Device.ClearTargets(*Color, *Depth, {0, 0, 0, 1});
	}

	const FMatrix4 Projection = FMatrix4::PerspectiveReversedInfinite(std::numbers::pi_v<float> / 2.f, 1, 0.1f);
	if (Result && Device.DrawIndexed({.Pipeline = *Pipeline, .Vertices = *Near, .Indices = *Index, .Texture = *Texture, .ColorTarget = *Color, .DepthTarget = *Depth, .WorldToClip = (Projection * FMatrix4::Translation({0, 0, 2})).Data(), .IndexCount = 7, .FirstIndex = 0, .ObjectToView = FMatrix4::Translation({0, 0, 2}).Data()}))
	{
		Device.CancelCommands();
		return Failure("Indexed draw exceeding the index buffer unexpectedly succeeded");
	}

	if (Result)
	{
		Result = Device.DrawIndexed({.Pipeline = *Pipeline, .Vertices = *Near, .Indices = *Index, .Texture = *Texture, .ColorTarget = *Color, .DepthTarget = *Depth, .WorldToClip = (Projection * FMatrix4::Translation({0, 0, 2})).Data(), .IndexCount = 6, .FirstIndex = 0, .ObjectToView = FMatrix4::Translation({0, 0, 2}).Data()});
	}

	if (Result)
	{
		Result = Device.DrawIndexed({.Pipeline = *Pipeline, .Vertices = *Far, .Indices = *Index, .Texture = *Texture, .ColorTarget = *Color, .DepthTarget = *Depth, .WorldToClip = (Projection * FMatrix4::Translation({0, 0, 4})).Data(), .IndexCount = 6, .FirstIndex = 0, .ObjectToView = FMatrix4::Translation({0, 0, 4}).Data()});
	}

	if (!Result)
	{
		Device.CancelCommands();
		return Result;
	}

	auto Submit = Device.SubmitCommands();
	if (!Submit)
	{
		Device.CancelCommands();
		return std::unexpected(Submit.error());
	}

	auto DepthPixels = Device.ReadbackTexture(*Color);
	if (!DepthPixels)
	{
		return std::unexpected(DepthPixels.error());
	}

	if (DepthPixels->size() != 64 * 64 * 4)
	{
		return Failure("Depth regression readback has an incorrect byte count");
	}

	constexpr std::size_t Center = (32 * 64 + 32) * 4;
	// The studio light shades even camera-facing quads slightly below full brightness.
	if (std::to_integer<unsigned>((*DepthPixels)[Center]) < 200 || std::to_integer<unsigned>((*DepthPixels)[Center + 1]) > 5)
	{
		return Failure("Reversed-Z regression: far geometry overwrote the near red quad");
	}

	Result = CheckDebugDraw(Device, VertexShader, FragmentShader, DebugVertexShader, DebugFragmentShader, GridVertexShader, GridFragmentShader);
	if (!Result)
	{
		return Result;
	}

	Result = CheckResourceLifetimes(Device);
	if (!Result)
	{
		return Result;
	}

	if (Device.GetStatistics().InFlightFrames != 0)
	{
		return Failure("Frame resources were not retired after GPU completion");
	}

	return {};
}
}
