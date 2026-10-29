#include "RendererSmoke.h"

#include "Herta/Math/Matrix.h"
#include "Herta/Renderer/EnvironmentLighting.h"
#include "Herta/Renderer/Visuals.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <limits>
#include <memory>
#include <numbers>
#include <print>

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

// Runs the GTAO depth prefilter on a cleared depth target: every texel is sky, so mip 0 must read back the clamped far depth.
[[nodiscard]] std::expected<void, FPresentationError> CheckCompute(IGraphicsDevice& Device, const FVisualShaderSet& Shaders)
{
	constexpr FExtent2D Extent{.Width = 40, .Height = 24};
	auto Pipeline = Device.CreateComputePipeline({.Name = "Compute regression", .Shader = Shaders.GtaoDepthCompute, .TextureCount = 1, .StorageTextureCount = GtaoDepthMips, .UniformBufferSize = sizeof(FGtaoUniforms)});
	auto Depth = Device.CreateTexture({.Name = "Compute regression depth", .Extent = Extent, .Format = ETextureFormat::Depth32, .bRenderTarget = true});
	auto ViewDepth = Device.CreateTexture({.Name = "Compute regression view depth", .Extent = Extent, .Format = ETextureFormat::R32Float, .MipLevels = GtaoDepthMips, .bStorage = true});
	if (!Pipeline || !Depth || !ViewDepth)
	{
		return std::unexpected(!Pipeline ? Pipeline.error() : !Depth ? Depth.error()
		                                                             : ViewDepth.error());
	}

	if (Device.CreateComputePipeline({.Name = "Undersized storage", .Shader = Shaders.GtaoDepthCompute, .TextureCount = 1, .StorageTextureCount = 1, .UniformBufferSize = sizeof(FGtaoUniforms)}))
	{
		return Failure("Compute pipeline accepted fewer storage bindings than its shader declares");
	}

	const FGtaoUniforms Uniforms{.DepthToView = {0, 1, 0, 1}, .Extent = {40, 24, 1.f / 40.f, 1.f / 24.f}, .Settings = {1, 0.3f, 1, 1}};
	std::array<FStorageTextureBinding, GtaoDepthMips> Mips;
	for (std::uint32_t Mip = 0; Mip < GtaoDepthMips; ++Mip)
	{
		Mips[Mip] = {.Texture = *ViewDepth, .MipLevel = Mip};
	}

	const std::array Sources{*Depth};
	const std::array SelfSources{*ViewDepth};
	FComputeDispatch Dispatch{.Pipeline = *Pipeline, .Textures = Sources, .StorageTextures = Mips, .Uniforms = std::as_bytes(std::span{&Uniforms, 1}), .GroupCount = {3, 2, 1}};
	if (const auto Begin = Device.BeginCommands(); !Begin)
	{
		return Begin;
	}

	if (Device.Dispatch(Dispatch))
	{
		Device.CancelCommands();
		return Failure("Dispatch accepted an uninitialized source texture");
	}

	auto Result = Device.ClearTargets({}, *Depth, {});
	if (Result)
	{
		FComputeDispatch Invalid = Dispatch;
		Invalid.GroupCount = {0, 1, 1};
		const bool bEmptyAccepted = Device.Dispatch(Invalid).has_value();
		Invalid = Dispatch;
		Invalid.Textures = SelfSources;
		const bool bAliasAccepted = Device.Dispatch(Invalid).has_value();
		Invalid = Dispatch;
		Mips[4].MipLevel = GtaoDepthMips;
		const bool bMipAccepted = Device.Dispatch(Invalid).has_value();
		Mips[4].MipLevel = 4;
		if (bEmptyAccepted || bAliasAccepted || bMipAccepted)
		{
			Device.CancelCommands();
			return Failure("Dispatch accepted an empty group count, a texture both sampled and written, or a missing mip level");
		}

		Result = Device.Dispatch(Dispatch);
	}

	if (!Result)
	{
		Device.CancelCommands();
		return Result;
	}

	if (const auto Submitted = Device.SubmitCommands(); !Submitted)
	{
		return std::unexpected(Submitted.error());
	}

	const auto Pixels = Device.ReadbackTexture(*ViewDepth);
	if (!Pixels)
	{
		return std::unexpected(Pixels.error());
	}

	if (Pixels->size() != std::size_t{Extent.Width} * Extent.Height * sizeof(float))
	{
		return Failure("Compute readback has an incorrect byte count");
	}

	for (std::size_t Offset = 0; Offset < Pixels->size(); Offset += sizeof(float))
	{
		float Value;
		std::memcpy(&Value, Pixels->data() + Offset, sizeof(Value));
		if (Value != 100000.f)
		{
			return Failure("Compute depth prefilter wrote an unexpected view depth for the sky");
		}
	}

	return {};
}

[[nodiscard]] std::expected<void, FPresentationError> CheckHdrResources(IGraphicsDevice& Device)
{
	for (const ETextureFormat Format : {ETextureFormat::Rgba16Float, ETextureFormat::Rgba32Float})
	{
		const auto Texture = Device.CreateTexture({.Name = "HDR clear regression", .Extent = {.Width = 4, .Height = 2}, .Format = Format, .bRenderTarget = true});
		if (!Texture)
		{
			return std::unexpected(Texture.error());
		}

		if (const auto Begin = Device.BeginCommands(); !Begin)
		{
			return Begin;
		}

		if (Device.WriteTexture(*Texture, 0, std::span<const std::byte>{}))
		{
			Device.CancelCommands();
			return Failure("HDR texture accepted a partial upload");
		}

		Device.BeginGpuTiming("HDR clear");
		const auto Clear = Device.ClearTargets(*Texture, {}, {4.f, 2.f, 0.5f, 1.f});
		Device.EndGpuTiming();
		if (!Clear)
		{
			Device.CancelCommands();
			return Clear;
		}

		if (const auto Submit = Device.SubmitCommands(); !Submit)
		{
			Device.CancelCommands();
			return std::unexpected(Submit.error());
		}

		const auto Pixels = Device.ReadbackTexture(*Texture);
		if (!Pixels)
		{
			return std::unexpected(Pixels.error());
		}

		if (Pixels->size() != 8 * GetTextureTexelBytes(Format))
		{
			return Failure("HDR readback has an incorrect texel stride");
		}

		for (const FGpuPassTiming& Timing : Device.GetGpuTimings())
		{
			if (Timing.Name != "HDR clear" || !std::isfinite(Timing.Milliseconds) || Timing.Milliseconds < 0)
			{
				return Failure("Completed GPU timing reported an invalid sample");
			}
		}

		if (Format == ETextureFormat::Rgba32Float)
		{
			std::array<float, 4> Color;
			std::memcpy(Color.data(), Pixels->data(), sizeof(Color));
			if (Color != std::array<float, 4>{4.f, 2.f, 0.5f, 1.f})
			{
				return Failure("HDR clear clamped or changed radiance");
			}
		}
		else
		{
			std::array<std::uint16_t, 4> Color;
			std::memcpy(Color.data(), Pixels->data(), sizeof(Color));
			if (Color != std::array<std::uint16_t, 4>{0x4400, 0x4000, 0x3800, 0x3c00})
			{
				return Failure("Half-float HDR clear changed representable radiance");
			}
		}
	}

	const auto Hdr = Device.CreateTexture({.Name = "HDR upload regression", .Extent = {.Width = 1, .Height = 1}, .Format = ETextureFormat::Rgba32Float});
	const auto Depth = Device.CreateTexture({.Name = "Depth-only clear regression", .Extent = {.Width = 1, .Height = 1}, .Format = ETextureFormat::Depth32, .bRenderTarget = true});
	if (!Hdr || !Depth)
	{
		return std::unexpected(!Hdr ? Hdr.error() : Depth.error());
	}

	constexpr std::array<float, 4> Radiance{8.f, 3.f, 0.25f, 1.f};
	for (const bool bSubmit : {false, true})
	{
		if (const auto Begin = Device.BeginCommands(); !Begin)
		{
			return Begin;
		}

		auto Result = Device.WriteTexture(*Hdr, 0, std::as_bytes(std::span(Radiance)));
		if (Result)
		{
			Result = Device.ClearTargets({}, *Depth, {});
		}

		if (!Result)
		{
			Device.CancelCommands();
			return Result;
		}

		if (!bSubmit)
		{
			Device.CancelCommands();
			if (Device.ReadbackTexture(*Hdr))
			{
				return Failure("Cancelled HDR upload initialized its texture");
			}
		}
		else if (const auto Submit = Device.SubmitCommands(); !Submit)
		{
			Device.CancelCommands();
			return std::unexpected(Submit.error());
		}
	}

	const auto Pixels = Device.ReadbackTexture(*Hdr);
	if (!Pixels)
	{
		return std::unexpected(Pixels.error());
	}

	if (*Pixels != std::vector<std::byte>(std::as_bytes(std::span(Radiance)).begin(), std::as_bytes(std::span(Radiance)).end()))
	{
		return Failure("HDR upload did not preserve radiance bytes");
	}

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
	for (FCookedVertex& Vertex : Model.Vertices)
	{
		Vertex.Normal = {0, 0, -1};
	}

	Model.Indices = {0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7};
	Model.Sections = {{.FirstIndex = 0, .IndexCount = 6, .Material = 0}, {.FirstIndex = 6, .IndexCount = 6, .Material = 1}};
	Model.Materials = {{.Name = "Red"}, {.Name = "Green"}};
	Model.Materials[0].Textures[0] = 0;
	Model.Materials[1].Textures[0] = 1;
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
	std::uint8_t MaximumRed = 0;
	std::uint8_t MaximumGreen = 0;
	for (std::size_t Pixel = 0; Pixel < Pixels->size() / 4; ++Pixel)
	{
		const auto Channel = [&](const std::size_t Index)
		{
			return static_cast<std::uint8_t>((*Pixels)[Pixel * 4 + Index]);
		};

		const bool bRight = Pixel % Extent.Width >= Extent.Width / 2;
		MaximumRed = std::max(MaximumRed, Channel(0));
		MaximumGreen = std::max(MaximumGreen, Channel(1));
		RedOnRight += bRight && Channel(0) > 150 && Channel(1) < 50 ? 1u : 0u;
		GreenOnLeft += !bRight && Channel(1) > 150 && Channel(0) < 50 ? 1u : 0u;
	}

	if (RedOnRight < 100 || GreenOnLeft < 100)
	{
		return std::unexpected(FPresentationError{.Code = EPresentationErrorCode::InvalidState, .Message = std::format("Render mesh sections did not draw with their own index ranges and textures: red {}, green {}, maxima {}/{}", RedOnRight, GreenOnLeft, MaximumRed, MaximumGreen)});
	}

	return {};
}

[[nodiscard]] std::expected<void, FPresentationError> CheckInstancing(IGraphicsDevice& Device, FMeshRenderer& Reference, const FShaderAsset& VertexShader, const FShaderAsset& FragmentShader, const FShaderAsset& InstancedVertexShader)
{
	auto Renderer = FMeshRenderer::Create(Device, VertexShader, FragmentShader, {}, {}, {}, {}, InstancedVertexShader);
	const auto First = CreateSmokeCube(Device);
	const auto Second = CreateSmokeCube(Device);
	if (!Renderer || !First || !Second)
	{
		return std::unexpected(!Renderer ? Renderer.error() : !First ? First.error()
		                                                             : Second.error());
	}

	constexpr FExtent2D Extent{.Width = 256, .Height = 128};
	std::array Models{
	    FMatrix4::Translation({-2, 1, 0}) * FMatrix4::Rotation(FQuaternion::FromAxisAngle({0, 1, 0}, 0.45f)) * FMatrix4::Scale({1.5f, 0.8f, 1.f}),
	    FMatrix4::Translation({2, -1, 0.5f}) * FMatrix4::Rotation(FQuaternion::FromAxisAngle({1, 0, 0}, -0.3f)) * FMatrix4::Scale({1.f, 1.5f, 0.8f}),
	    FMatrix4::Translation({2, 1, 0}) * FMatrix4::Rotation(FQuaternion::FromAxisAngle({0, 0, 1}, 0.35f)) * FMatrix4::Scale({-1.2f, 1.f, 1.f}),
	    FMatrix4::Translation({-2, -1, -0.5f}) * FMatrix4::Scale({0.7f, 1.3f, 1.1f}),
	};

	const std::array Meshes{First->get(), Second->get(), First->get(), Second->get()};
	const FMeshRenderView View{.View = FMatrix4::Rotation(FQuaternion::FromAxisAngle({0, 1, 0}, 0.12f) * FQuaternion::FromAxisAngle({1, 0, 0}, -0.08f)) * FMatrix4::Translation({0.2f, -0.3f, 8}), .Projection = FMatrix4::PerspectiveReversedInfinite(std::numbers::pi_v<float> / 3.f, 2.f, 0.1f), .Models = Models, .Meshes = Meshes};
	for (std::size_t Frame = 0; Frame < 2; ++Frame)
	{
		if (const auto Result = Reference.Render(Extent, View); !Result)
		{
			return Result;
		}

		const auto Baseline = Device.ReadbackTexture(Reference.GetColorTarget());
		if (!Baseline)
		{
			return std::unexpected(Baseline.error());
		}

		if (const auto Result = (*Renderer)->Render(Extent, View); !Result)
		{
			return Result;
		}

		const auto Instanced = Device.ReadbackTexture((*Renderer)->GetColorTarget());
		if (!Instanced)
		{
			return std::unexpected(Instanced.error());
		}

		if (Baseline->size() != Instanced->size() || Reference.GetLastDrawCount() != 4 || (*Renderer)->GetLastDrawCount() != 2)
		{
			return Failure("Shared meshes did not reduce to one instanced draw per mesh section");
		}

		std::size_t DifferentPixels = 0;
		std::size_t MeshPixels = 0;
		int MaximumDifference = 0;
		for (std::size_t Pixel = 0; Pixel < Baseline->size(); Pixel += 4)
		{
			bool bDifferent = false;
			for (std::size_t Channel = 0; Channel < 3; ++Channel)
			{
				const int Difference = std::to_integer<int>((*Baseline)[Pixel + Channel]) - std::to_integer<int>((*Instanced)[Pixel + Channel]);
				MaximumDifference = std::max(MaximumDifference, std::abs(Difference));
				bDifferent |= std::abs(Difference) > 8;
			}

			DifferentPixels += bDifferent ? 1u : 0u;
			MeshPixels += std::to_integer<unsigned>((*Baseline)[Pixel]) > 120 ? 1u : 0u;
		}

		std::println("Instancing readback frame {}: {} mesh pixels, {} differing pixels, maximum channel difference {}", Frame, MeshPixels, DifferentPixels, MaximumDifference);

		// Separate matrix-column arithmetic can shift edge samples slightly without changing the image.
		if (MeshPixels < 250 || DifferentPixels > Baseline->size() / 4 / 100)
		{
			return std::unexpected(FPresentationError{.Code = EPresentationErrorCode::InvalidState, .Message = std::format("Instanced transforms, view-space lighting, or winding differ from individual mesh draws: frame {}, {} mesh pixels, {} differing pixels, maximum channel difference {}", Frame, MeshPixels, DifferentPixels, MaximumDifference)});
		}

		Models[0] = FMatrix4::Translation({0.4f, -0.2f, 0.3f}) * Models[0];
	}

	if (const auto Sections = CheckRenderMeshSections(Device, **Renderer); !Sections)
	{
		return Sections;
	}

	return CheckMultipleModels(Device, **Renderer);
}

[[nodiscard]] std::expected<void, FPresentationError> CheckDebugDraw(IGraphicsDevice& Device, const FShaderAsset& VertexShader, const FShaderAsset& FragmentShader, const FShaderAsset& DebugVertexShader, const FShaderAsset& DebugFragmentShader, const FShaderAsset& GridVertexShader, const FShaderAsset& GridFragmentShader, const FShaderAsset& InstancedVertexShader)
{
	auto Renderer = FMeshRenderer::Create(Device, VertexShader, FragmentShader, DebugVertexShader, DebugFragmentShader, GridVertexShader, GridFragmentShader, InstancedVertexShader);
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

[[nodiscard]] std::expected<void, FPresentationError> CheckVisualPipeline(IGraphicsDevice& Device, const FShaderAsset& VertexShader, const FShaderAsset& FragmentShader, const FShaderAsset& InstancedVertexShader, const FVisualShaderSet& Shaders)
{
	if (Shaders.FullscreenVertex.Bytecode.empty())
	{
		return Failure("Visual smoke requires the cooked HDR, shadows, fog and SMAA shaders");
	}

	const auto Renderer = FMeshRenderer::Create(Device, VertexShader, FragmentShader, {}, {}, {}, {}, InstancedVertexShader, Shaders);
	const auto Cube = CreateSmokeCube(Device);
	if (!Renderer || !Cube)
	{
		return std::unexpected(!Renderer ? Renderer.error() : Cube.error());
	}

	constexpr FExtent2D Extent{192, 128};
	const std::array Models{
	    FMatrix4::Rotation(FQuaternion::FromAxisAngle({0, 1, 0}, 0.35f)) * FMatrix4::Scale({1.5f, 1.5f, 1.5f}),
	    FMatrix4::Translation({0, -0.85f, 1}) * FMatrix4::Scale({8, 0.2f, 8}),
	};

	const std::array<const FRenderMesh*, 2> Meshes{Cube->get(), Cube->get()};
	FMeshRenderView View{
	    .View = FMatrix4::Rotation(FQuaternion::FromAxisAngle({1, 0, 0}, -0.25f)) * FMatrix4::Translation({0, -2, 7}),
	    .Projection = FMatrix4::PerspectiveReversedInfinite(std::numbers::pi_v<float> / 3.f, 1.5f, 0.1f),
	    .Models = Models,
	    .Meshes = Meshes,
	    .Visuals = {.ExposureEV100 = 7.737f, .AntiAliasing = EAntiAliasing::Off},
	};

	View.Visuals.bStudioPreview = false;
	const auto Capture = [&]() -> std::expected<std::vector<std::byte>, FPresentationError>
	{
		if (const auto Rendered = (*Renderer)->Render(Extent, View); !Rendered)
		{
			return std::unexpected(Rendered.error());
		}

		auto Image = Device.ReadbackTexture((*Renderer)->GetColorTarget());
		if (!Image)
		{
			return std::unexpected(Image.error());
		}

		if (Image->size() != static_cast<std::size_t>(Extent.Width) * Extent.Height * 4)
		{
			return Failure("Visual pipeline returned an invalid LDR image size");
		}

		return Image;
	};

	const auto Difference = [](const std::vector<std::byte>& A, const std::vector<std::byte>& B)
	{
		std::size_t Count = 0;
		for (std::size_t Pixel = 0; Pixel < A.size(); Pixel += 4)
		{
			bool bChanged = false;
			for (std::size_t Channel = 0; Channel < 3; ++Channel)
			{
				bChanged |= std::abs(std::to_integer<int>(A[Pixel + Channel]) - std::to_integer<int>(B[Pixel + Channel])) > 2;
			}

			Count += bChanged ? 1u : 0u;
		}

		return Count;
	};

	const auto Brightness = [](const std::vector<std::byte>& Image)
	{
		std::uint64_t Sum = 0;
		for (std::size_t Pixel = 0; Pixel < Image.size(); Pixel += 4)
		{
			Sum += std::to_integer<unsigned>(Image[Pixel]) + std::to_integer<unsigned>(Image[Pixel + 1]) + std::to_integer<unsigned>(Image[Pixel + 2]);
		}

		return Sum;
	};

	const auto Dark = Capture();
	if (!Dark)
	{
		return std::unexpected(Dark.error());
	}

	std::array<std::size_t, 4> LitPixels{};
	const FMatrix4 LightTransform = FMatrix4::Translation({2, 4, -3}) * FMatrix4::Rotation(FQuaternion::FromAxisAngle({0, 1, 0}, -0.35f) * FQuaternion::FromAxisAngle({1, 0, 0}, 0.7f));
	std::array<FRenderLight, 2> Lights{
	    FRenderLight{.Settings = {.Type = ELightType::Directional, .Intensity = 3000.f, .bCastShadows = false, .Range = 40.f}, .Transform = LightTransform},
	    FRenderLight{.Settings = {.Type = ELightType::Sky, .Intensity = 10.f, .bCastShadows = false, .bEnvironmentVisible = false}, .Transform = FMatrix4{}},
	};

	constexpr std::array Types{ELightType::Directional, ELightType::Point, ELightType::Spot, ELightType::Rect};
	for (std::size_t Index = 0; Index < Types.size(); ++Index)
	{
		Lights[0].Settings.Type = Types[Index];
		Lights[0].Settings.Intensity = Types[Index] == ELightType::Directional ? 3000.f : 100000.f;
		View.Lights = std::span(Lights).first(1);
		const auto Lit = Capture();
		if (!Lit)
		{
			return std::unexpected(Lit.error());
		}

		LitPixels[Index] = Difference(*Dark, *Lit);
		if (LitPixels[Index] < 10 || Brightness(*Lit) <= Brightness(*Dark))
		{
			return std::unexpected(FPresentationError{.Code = EPresentationErrorCode::InvalidState, .Message = std::format("Authored light {} failed to illuminate PBR geometry: {} changed pixels, brightness dark {} lit {}", static_cast<unsigned>(Types[Index]), LitPixels[Index], Brightness(*Dark), Brightness(*Lit))});
		}
	}

	Lights[0].Settings.Type = ELightType::Directional;
	Lights[0].Settings.Intensity = 3000.f;
	View.Lights = Lights;
	const auto Unshadowed = Capture();
	Lights[0].Settings.bCastShadows = true;
	const auto Shadowed = Capture();
	if (!Unshadowed || !Shadowed)
	{
		return std::unexpected(!Unshadowed ? Unshadowed.error() : Shadowed.error());
	}

	const std::size_t ShadowPixels = Difference(*Unshadowed, *Shadowed);
	if (ShadowPixels < 10 || Brightness(*Shadowed) >= Brightness(*Unshadowed))
	{
		return std::unexpected(FPresentationError{.Code = EPresentationErrorCode::InvalidState, .Message = std::format("Directional atlas shadows did not darken visible receiver geometry: {} changed pixels", ShadowPixels)});
	}

	View.Visuals.ExposureEV100 = 6.737f;
	const auto Exposed = Capture();
	if (!Exposed)
	{
		return std::unexpected(Exposed.error());
	}

	if (Brightness(*Exposed) <= Brightness(*Shadowed))
	{
		return Failure("HDR exposure did not increase tone-mapped output brightness");
	}

	View.Visuals.ExposureEV100 = 7.737f;
	View.Visuals.Atmosphere = FSkyAtmosphereComponent{};
	Lights[1].Settings.bEnvironmentVisible = true;
	const auto Sky = Capture();
	if (!Sky)
	{
		return std::unexpected(Sky.error());
	}

	const std::size_t SkyPixels = Difference(*Shadowed, *Sky);
	if (SkyPixels < 100)
	{
		return Failure("Procedural atmosphere and visible sky did not change the background");
	}

	View.Visuals.Fog = FHeightFogComponent{.Density = 0.05f, .MaxDistance = 30.f};
	const auto Fogged = Capture();
	if (!Fogged)
	{
		return std::unexpected(Fogged.error());
	}

	const std::size_t FogPixels = Difference(*Sky, *Fogged);
	if (FogPixels < 100)
	{
		return Failure("Volumetric fog and depth-aware composite did not change visible output");
	}

	View.Visuals.AntiAliasing = EAntiAliasing::SmaaHigh;
	const auto Antialiased = Capture();
	if (!Antialiased)
	{
		return std::unexpected(Antialiased.error());
	}

	const std::size_t SmaaPixels = Difference(*Fogged, *Antialiased);
	if (SmaaPixels == 0)
	{
		return Failure("SMAA edges, weights and neighborhood passes did not modify any edge pixels");
	}

	// The compute SMAA path must match the full-screen reference passes.
	if (!Shaders.SmaaEdgeListCompute.Bytecode.empty())
	{
		FVisualShaderSet ReferenceShaders = Shaders;
		ReferenceShaders.SmaaEdgeListCompute = {};
		const auto Reference = FMeshRenderer::Create(Device, VertexShader, FragmentShader, {}, {}, {}, {}, InstancedVertexShader, ReferenceShaders);
		if (!Reference)
		{
			return std::unexpected(Reference.error());
		}

		// Sun cascades fit the depth read back two renders earlier, so the fresh renderer needs three completed frames to match the one under test.
		for (int Frame = 0; Frame < 3; ++Frame)
		{
			if (const auto Rendered = (*Reference)->Render(Extent, View); !Rendered)
			{
				return Rendered;
			}

			if (const auto Idle = Device.WaitForIdle(); !Idle)
			{
				return Idle;
			}
		}

		const auto ReferenceImage = Device.ReadbackTexture((*Reference)->GetColorTarget());
		if (!ReferenceImage)
		{
			return std::unexpected(ReferenceImage.error());
		}

		int MaximumDifference = 0;
		for (std::size_t Index = 0; Index < ReferenceImage->size(); ++Index)
		{
			MaximumDifference = std::max(MaximumDifference, std::abs(std::to_integer<int>((*ReferenceImage)[Index]) - std::to_integer<int>((*Antialiased)[Index])));
		}

		if (MaximumDifference > 1)
		{
			return Failure("Compute SMAA differs from the full-screen reference passes");
		}
	}

	bool bPbrTiming = false;
	for (const FGpuPassTiming& Timing : Device.GetGpuTimings())
	{
		if (!std::isfinite(Timing.Milliseconds) || Timing.Milliseconds < 0)
		{
			return Failure("Visual passes reported invalid GPU timing samples");
		}

		bPbrTiming |= Timing.Name == "PBR meshes";
		std::println("Visual GPU: {} {:.3f} ms", Timing.Name, Timing.Milliseconds);
	}

	if (!bPbrTiming)
	{
		return Failure("Native visual rendering did not publish completed PBR GPU timings");
	}

	View.Visuals.Fog.reset();
	View.Visuals.Atmosphere.reset();
	View.Visuals.AntiAliasing = EAntiAliasing::Off;
	const auto BeforeEnvironment = Capture();
	if (!BeforeEnvironment)
	{
		return std::unexpected(BeforeEnvironment.error());
	}

	constexpr std::array<float, 4> Radiance{8, 4, 2, 1};
	FCookedTexture Environment{.ColorSpace = ETextureColorSpace::Linear, .PixelFormat = ETexturePixelFormat::Rgba32Float, .Mips = {{.Width = 1, .Height = 1, .Pixels = std::vector<std::byte>(sizeof(Radiance))}}};
	std::memcpy(Environment.Mips[0].Pixels.data(), Radiance.data(), sizeof(Radiance));
	const auto Snapshot = BuildVisualUniforms(View.View, View.Projection, Extent, View.Lights, View.Visuals);
	if (!Snapshot)
	{
		return std::unexpected(Snapshot.error());
	}

	const auto Start = std::chrono::steady_clock::now();
	const auto Filtered = BuildEnvironmentLighting(&Environment, *Snapshot);
	const double FilterMilliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - Start).count();
	if (!Filtered)
	{
		return std::unexpected(FPresentationError{.Code = EPresentationErrorCode::InvalidState, .Message = Filtered.error().Message});
	}

	if (const auto Uploaded = (*Renderer)->SetEnvironmentLighting(*Filtered); !Uploaded)
	{
		return Uploaded;
	}

	if (const auto Idle = Device.WaitForIdle(); !Idle)
	{
		return Idle;
	}

	for (const FGpuPassTiming& Timing : Device.GetGpuTimings())
	{
		std::println("Environment GPU: {} {:.3f} ms", Timing.Name, Timing.Milliseconds);
	}

	const auto WithEnvironment = Capture();
	if (!WithEnvironment)
	{
		return std::unexpected(WithEnvironment.error());
	}

	const std::size_t EnvironmentPixels = Difference(*BeforeEnvironment, *WithEnvironment);
	if (EnvironmentPixels < 100 || !Filtered->bFromHdr)
	{
		return Failure("Filtered HDR environment upload did not change visible sky or PBR lighting");
	}

	FMaterialAsset Material{.Name = "Smoke emissive"};
	Material.Parameters.BaseColor = {1, 0, 0, 1};
	Material.Parameters.Emissive = {1, 0, 0};
	Material.Parameters.EmissiveIntensity = 1000.f;
	const auto Override = FRenderMaterial::Create(Device, Material, {}, "Smoke material override");
	if (!Override)
	{
		return std::unexpected(Override.error());
	}

	const std::array<const FRenderMaterial*, 1> FirstSlot{Override->get()};
	const std::array<std::span<const FRenderMaterial* const>, 2> Overrides{FirstSlot, {}};
	View.Materials = Overrides;
	const auto WithMaterial = Capture();
	if (!WithMaterial)
	{
		return std::unexpected(WithMaterial.error());
	}

	const std::size_t MaterialPixels = Difference(*WithEnvironment, *WithMaterial);
	if (MaterialPixels < 10)
	{
		return Failure("PBR material-slot override did not change visible mesh output");
	}

	std::println("Environment CPU: GGX and irradiance {:.3f} ms, changed {} pixels; material override {} pixels", FilterMilliseconds, EnvironmentPixels, MaterialPixels);
	std::println("Visual smoke: light pixels {}/{}/{}/{}, shadow {}, sky {}, fog {}, SMAA {}", LitPixels[0], LitPixels[1], LitPixels[2], LitPixels[3], ShadowPixels, SkyPixels, FogPixels, SmaaPixels);
	return {};
}

[[nodiscard]] float DecodeHalf(const std::byte Low, const std::byte High) noexcept
{
	const unsigned Bits = std::to_integer<unsigned>(Low) | (std::to_integer<unsigned>(High) << 8);
	const int Exponent = static_cast<int>((Bits >> 10) & 0x1f);
	const auto Mantissa = static_cast<float>(Bits & 0x3ff);
	const float Magnitude = Exponent == 0 ? std::ldexp(Mantissa, -24) : std::ldexp(Mantissa + 1024.f, Exponent - 25);
	return (Bits & 0x8000) != 0 ? -Magnitude : Magnitude;
}

// Locks the physical camera chain: an 18% grey card under 100,000 lx of noon sun at "sunny 16" EV100 15 must display as photographic middle grey.
// White furnace: under light of the same radiance from every direction, a white surface that conserves energy reflects exactly that radiance, whatever its roughness or metalness.
// Lambert, rough EON diffuse, and a rough white metal therefore match, first under 240 directional lights spread over the sphere and then under a uniform environment. Single-scattering GGX alone leaves the metal about a fifth darker.
// Grey Lambert and grey rough EON match too, because EON inverts the authored color into the albedo that reflects as much on average.
// With ray queries the path tracer must pass the environment furnace as well, and match raster shading under a single sun, where no surface the cubes' front faces can see bounces light back.
[[nodiscard]] std::expected<void, FPresentationError> CheckWhiteFurnace(IGraphicsDevice& Device, const FShaderAsset& VertexShader, const FShaderAsset& FragmentShader, const FShaderAsset& InstancedVertexShader, const FVisualShaderSet& Shaders)
{
	const auto Renderer = FMeshRenderer::Create(Device, VertexShader, FragmentShader, {}, {}, {}, {}, InstancedVertexShader, Shaders);
	const auto Cube = CreateSmokeCube(Device);
	if (!Renderer || !Cube)
	{
		return std::unexpected(!Renderer ? Renderer.error() : Cube.error());
	}

	const auto MakeMaterial = [&Device](const float Metallic, const float DiffuseRoughness, const float Color = 1.f)
	{
		FMaterialAsset Material{.Name = "Furnace"};
		Material.Parameters.BaseColor = {Color, Color, Color, 1.f};
		Material.Parameters.Metallic = Metallic;
		Material.Parameters.Roughness = 1.f;
		Material.Parameters.DiffuseRoughness = DiffuseRoughness;
		return FRenderMaterial::Create(Device, Material, {}, "Furnace material");
	};

	const std::array Materials{MakeMaterial(0.f, 0.f), MakeMaterial(0.f, 1.f), MakeMaterial(1.f, 0.f), MakeMaterial(0.f, 0.f, 0.5f), MakeMaterial(0.f, 1.f, 0.5f)};
	for (const auto& Material : Materials)
	{
		if (!Material)
		{
			return std::unexpected(Material.error());
		}
	}

	constexpr std::array<const char*, 3> Names{"Lambert", "rough EON", "rough metal"};
	const std::array<const FRenderMaterial*, 1> Lambert{Materials[0]->get()};
	const std::array<const FRenderMaterial*, 1> Rough{Materials[1]->get()};
	const std::array<const FRenderMaterial*, 1> Metal{Materials[2]->get()};
	const std::array<std::span<const FRenderMaterial* const>, 3> Overrides{Lambert, Rough, Metal};
	const std::array<const FRenderMaterial*, 1> GreyLambert{Materials[3]->get()};
	const std::array<const FRenderMaterial*, 1> GreyRough{Materials[4]->get()};
	const std::array<std::span<const FRenderMaterial* const>, 3> GreyOverrides{GreyLambert, GreyRough, Metal};
	constexpr std::array<float, 3> Offsets{-1.5f, 0.f, 1.5f};
	const std::array Models{FMatrix4::Translation({Offsets[0], 0, 0}), FMatrix4::Translation({Offsets[1], 0, 0}), FMatrix4::Translation({Offsets[2], 0, 0})};
	const std::array<const FRenderMesh*, 3> Meshes{Cube->get(), Cube->get(), Cube->get()};
	constexpr FExtent2D Extent{240, 96};
	// Unit radiance lands on mid grey: the tone mapper divides by 1.2 * 2^EV100.
	const float ExposureEV100 = std::log2(1.f / (1.2f * 0.18f));
	FMeshRenderView View{
	    .View = FMatrix4::Translation({0, 0, 4}),
	    .Projection = FMatrix4::PerspectiveReversedInfinite(std::numbers::pi_v<float> / 4.f, 2.5f, 0.1f),
	    .Models = Models,
	    .Meshes = Meshes,
	    .Materials = Overrides,
	    .Visuals = {.ExposureEV100 = ExposureEV100, .AntiAliasing = EAntiAliasing::Off, .ShadowQuality = EShadowQuality::Off, .AmbientOcclusion = EAmbientOcclusion::Off, .bStudioPreview = false},
	};

	// The path tracer converges over frames, so it renders until this many samples per pixel.
	constexpr std::uint32_t PathTracedSamples = 256;
	const auto MeasureLevels = [&](const bool bGrey) -> std::expected<std::array<float, 3>, FPresentationError>
	{
		View.Materials = bGrey ? GreyOverrides : Overrides;
		for (std::uint32_t Sample = 0; Sample < (View.Visuals.bPathTracing ? PathTracedSamples : 1u); ++Sample)
		{
			if (const auto Rendered = (*Renderer)->Render(Extent, View); !Rendered)
			{
				return std::unexpected(Rendered.error());
			}
		}

		const auto Image = Device.ReadbackTexture((*Renderer)->GetColorTarget());
		if (!Image)
		{
			return std::unexpected(Image.error());
		}

		// Average the middle of each cube's front face, which looks straight at the camera.
		std::array<float, 3> Levels{};
		const FMatrix4 WorldToClip = View.Projection * View.View;
		for (std::size_t Index = 0; Index < Levels.size(); ++Index)
		{
			const FVector4 Clip = WorldToClip * FVector4{Offsets[Index], 0, -0.5f, 1};
			const auto CenterX = static_cast<int>((Clip.X / Clip.W * 0.5f + 0.5f) * static_cast<float>(Extent.Width));
			const auto CenterY = static_cast<int>((0.5f - Clip.Y / Clip.W * 0.5f) * static_cast<float>(Extent.Height));
			for (int Y = CenterY - 2; Y <= CenterY + 2; ++Y)
			{
				for (int X = CenterX - 2; X <= CenterX + 2; ++X)
				{
					Levels[Index] += static_cast<float>(std::to_integer<int>((*Image)[(static_cast<std::size_t>(Y) * Extent.Width + static_cast<std::size_t>(X)) * 4 + 1])) / 25.f;
				}
			}
		}

		return Levels;
	};

	// Grey rounds compare only the two diffuse cubes; the metal stays white.
	const auto Measure = [&](const std::string_view Lighting, const bool bGrey = false) -> std::expected<void, FPresentationError>
	{
		const auto Measured = MeasureLevels(bGrey);
		if (!Measured)
		{
			return std::unexpected(Measured.error());
		}

		const std::array<float, 3>& Levels = *Measured;
		std::println("{} furnace, {}: Lambert {:.1f}, rough EON {:.1f}, rough metal {:.1f}", bGrey ? "Grey" : "White", Lighting, Levels[0], Levels[1], Levels[2]);
		for (std::size_t Index = 0; Index < (bGrey ? 2u : Levels.size()); ++Index)
		{
			if (Levels[Index] < 40.f || Levels[Index] > 200.f || std::abs(Levels[Index] - Levels[0]) > 4.f)
			{
				return std::unexpected(FPresentationError{.Code = EPresentationErrorCode::InvalidState, .Message = std::format("A {} {} surface under the {} furnace read {:.1f}, not Lambert's {:.1f}", bGrey ? "grey" : "white", Names[Index], Lighting, Levels[Index], Levels[0])});
			}
		}

		return {};
	};

	// Each light stands for an equal share of the sphere, so it delivers that share of unit radiance as illuminance.
	constexpr std::size_t LightCount = 240;
	std::vector<FRenderLight> Lights(LightCount);
	for (std::size_t Index = 0; Index < LightCount; ++Index)
	{
		const float Z = 1.f - (2.f * static_cast<float>(Index) + 1.f) / static_cast<float>(LightCount);
		const float Radius = std::sqrt(1.f - Z * Z);
		const float Angle = static_cast<float>(Index) * std::numbers::pi_v<float> * (3.f - std::sqrt(5.f));
		FMatrix4 Transform;
		Transform(0, 2) = Radius * std::cos(Angle);
		Transform(1, 2) = Radius * std::sin(Angle);
		Transform(2, 2) = Z;
		Lights[Index] = {.Settings = {.Type = ELightType::Directional, .Intensity = 4.f * std::numbers::pi_v<float> / static_cast<float>(LightCount), .bCastShadows = false}, .Transform = Transform};
	}

	View.Lights = Lights;
	if (const auto Direct = Measure("directional"); !Direct)
	{
		return Direct;
	}

	if (const auto Direct = Measure("directional", true); !Direct)
	{
		return Direct;
	}

	constexpr std::array<float, 4> Radiance{1, 1, 1, 1};
	FCookedTexture Environment{.ColorSpace = ETextureColorSpace::Linear, .PixelFormat = ETexturePixelFormat::Rgba32Float, .Mips = {{.Width = 1, .Height = 1, .Pixels = std::vector<std::byte>(sizeof(Radiance))}}};
	std::memcpy(Environment.Mips[0].Pixels.data(), Radiance.data(), sizeof(Radiance));
	const std::array Sky{FRenderLight{.Settings = {.Type = ELightType::Sky, .Intensity = 1.f, .bCastShadows = false, .bEnvironmentVisible = false}, .Transform = FMatrix4{}}};
	View.Lights = Sky;
	const auto Snapshot = BuildVisualUniforms(View.View, View.Projection, Extent, View.Lights, View.Visuals);
	if (!Snapshot)
	{
		return std::unexpected(Snapshot.error());
	}

	const auto Filtered = BuildEnvironmentLighting(&Environment, *Snapshot);
	if (!Filtered)
	{
		return std::unexpected(FPresentationError{.Code = EPresentationErrorCode::InvalidState, .Message = Filtered.error().Message});
	}

	if (const auto Uploaded = (*Renderer)->SetEnvironmentLighting(*Filtered); !Uploaded)
	{
		return Uploaded;
	}

	if (const auto Environmental = Measure("environment"); !Environmental)
	{
		return Environmental;
	}

	if (const auto Environmental = Measure("environment", true); !Environmental || !(*Renderer)->SupportsPathTracing())
	{
		return Environmental;
	}

	View.Visuals.bPathTracing = true;
	if (const auto Traced = Measure("path traced environment"); !Traced)
	{
		return Traced;
	}

	// One sun from above the camera's left, without shadow maps on either side, so raster and path traced direct light compare exactly.
	FMatrix4 SunTransform;
	const FVector3 Toward = FVector3{0.4f, 0.5f, -1.f}.Normalized();
	SunTransform(0, 2) = -Toward.X;
	SunTransform(1, 2) = -Toward.Y;
	SunTransform(2, 2) = -Toward.Z;
	const std::array Sun{FRenderLight{.Settings = {.Type = ELightType::Directional, .Intensity = 2.f, .bCastShadows = false}, .Transform = SunTransform}};
	View.Lights = Sun;
	const auto Traced = MeasureLevels(false);
	View.Visuals.bPathTracing = false;
	const auto Raster = MeasureLevels(false);
	if (!Traced || !Raster)
	{
		return std::unexpected(!Traced ? Traced.error() : Raster.error());
	}

	std::println("Direct sun, raster against path traced: Lambert {:.1f}/{:.1f}, rough EON {:.1f}/{:.1f}, rough metal {:.1f}/{:.1f}", (*Raster)[0], (*Traced)[0], (*Raster)[1], (*Traced)[1], (*Raster)[2], (*Traced)[2]);
	for (std::size_t Index = 0; Index < Names.size(); ++Index)
	{
		if ((*Raster)[Index] < 20.f || std::abs((*Raster)[Index] - (*Traced)[Index]) > 3.f)
		{
			return std::unexpected(FPresentationError{.Code = EPresentationErrorCode::InvalidState, .Message = std::format("Under one sun, the path traced {} surface read {:.1f} against raster's {:.1f}", Names[Index], (*Traced)[Index], (*Raster)[Index])});
		}
	}

	return {};
}

[[nodiscard]] float DecodeHalf(const std::uint16_t Bits)
{
	const int Exponent = (Bits >> 10) & 31;
	const float Mantissa = static_cast<float>(Bits & 1023);
	const float Magnitude = Exponent == 0 ? std::ldexp(Mantissa, -24) : std::ldexp(1024.f + Mantissa, Exponent - 25);
	return (Bits & 0x8000) != 0 ? -Magnitude : Magnitude;
}

// A moving cube reports its own screen motion while the static sky reports none, and a turning camera moves the cube and the sky alike.
[[nodiscard]] std::expected<void, FPresentationError> CheckMotionVectors(IGraphicsDevice& Device, const FShaderAsset& VertexShader, const FShaderAsset& FragmentShader, const FShaderAsset& InstancedVertexShader, const FVisualShaderSet& Shaders)
{
	if (Shaders.MotionVectorsCompute.Bytecode.empty())
	{
		return {};
	}

	const auto Renderer = FMeshRenderer::Create(Device, VertexShader, FragmentShader, {}, {}, {}, {}, InstancedVertexShader, Shaders);
	const auto Cube = CreateSmokeCube(Device);
	if (!Renderer || !Cube)
	{
		return std::unexpected(!Renderer ? Renderer.error() : Cube.error());
	}

	constexpr FExtent2D Extent{192, 128};
	std::array Models{FMatrix4::Translation({0, 0, 0})};
	std::array PreviousModels = Models;
	const std::array<const FRenderMesh*, 1> Meshes{Cube->get()};
	FMeshRenderView View{
	    .View = FMatrix4::Translation({0, 0, 5}),
	    .Projection = FMatrix4::PerspectiveReversedInfinite(std::numbers::pi_v<float> / 3.f, 1.5f, 0.1f),
	    .Models = Models,
	    .PreviousModels = PreviousModels,
	    .Meshes = Meshes,
	    .Visuals = {.AntiAliasing = EAntiAliasing::Off, .bMotionVectors = true, .bStudioPreview = false},
	};

	const auto Capture = [&]() -> std::expected<std::vector<std::array<float, 2>>, FPresentationError>
	{
		if (const auto Rendered = (*Renderer)->Render(Extent, View); !Rendered)
		{
			return std::unexpected(Rendered.error());
		}

		if (!(*Renderer)->GetMotionVectors())
		{
			return Failure("Motion vectors were requested but not produced");
		}

		const auto Bytes = Device.ReadbackTexture((*Renderer)->GetMotionVectors());
		if (!Bytes)
		{
			return std::unexpected(Bytes.error());
		}

		std::vector<std::array<float, 2>> Motion(Bytes->size() / 4);
		for (std::size_t Pixel = 0; Pixel < Motion.size(); ++Pixel)
		{
			std::array<std::uint16_t, 2> Bits;
			std::memcpy(Bits.data(), Bytes->data() + Pixel * 4, sizeof(Bits));
			Motion[Pixel] = {DecodeHalf(Bits[0]), DecodeHalf(Bits[1])};
		}

		return Motion;
	};

	const auto ProjectUV = [&](const FMatrix4& WorldToClip, const FVector3& Point)
	{
		const FVector4 Clip = WorldToClip * FVector4{Point.X, Point.Y, Point.Z, 1.f};
		return std::array{Clip.X / Clip.W * 0.5f + 0.5f, 0.5f - Clip.Y / Clip.W * 0.5f};
	};

	const auto At = [&](const std::vector<std::array<float, 2>>& Motion, const std::array<float, 2>& UV)
	{
		const auto X = static_cast<std::size_t>(UV[0] * static_cast<float>(Extent.Width));
		const auto Y = static_cast<std::size_t>(UV[1] * static_cast<float>(Extent.Height));
		return Motion[Y * Extent.Width + X];
	};

	if (const auto First = Capture(); !First)
	{
		return std::unexpected(First.error());
	}

	Models[0] = FMatrix4::Translation({0.2f, 0.1f, 0});
	const auto Moved = Capture();
	if (!Moved)
	{
		return std::unexpected(Moved.error());
	}

	const FMatrix4 WorldToClip = View.Projection * View.View;
	// The visible point is on the cube's front face, half a meter nearer than its center.
	const auto Current = ProjectUV(WorldToClip, {0.2f, 0.1f, -0.5f});
	const auto Previous = ProjectUV(WorldToClip, {0, 0, -0.5f});
	const auto CubeMotion = At(*Moved, Current);
	const auto Sky = At(*Moved, {0.05f, 0.5f});
	if (std::abs(CubeMotion[0] - (Current[0] - Previous[0])) > 2e-3f || std::abs(CubeMotion[1] - (Current[1] - Previous[1])) > 2e-3f || std::abs(Sky[0]) > 1e-4f || std::abs(Sky[1]) > 1e-4f)
	{
		return std::unexpected(FPresentationError{.Code = EPresentationErrorCode::InvalidState, .Message = std::format("A moving cube must report its screen motion over a still sky (cube {} {}, expected {} {}, sky {} {})", CubeMotion[0], CubeMotion[1], Current[0] - Previous[0], Current[1] - Previous[1], Sky[0], Sky[1])});
	}

	PreviousModels = Models;
	View.View = FMatrix4::Rotation(FQuaternion::FromAxisAngle({0, 1, 0}, 0.02f)) * FMatrix4::Translation({0, 0, 5});
	const auto Turned = Capture();
	if (!Turned)
	{
		return std::unexpected(Turned.error());
	}

	// A small turn shifts pixels in the same column alike, whichever pass wrote them, so compare the cube with the sky above it.
	const auto CubeUV = ProjectUV(View.Projection * View.View, {0.2f, 0.1f, -0.5f});
	const auto TurnedCube = At(*Turned, CubeUV);
	const auto TurnedSky = At(*Turned, {CubeUV[0], 0.1f});
	if (std::abs(TurnedCube[0]) < 1e-3f || std::abs(TurnedCube[0] - TurnedSky[0]) > 0.1f * std::abs(TurnedCube[0]))
	{
		return std::unexpected(FPresentationError{.Code = EPresentationErrorCode::InvalidState, .Message = std::format("Turning the camera must move the cube and the sky alike (cube {}, sky {})", TurnedCube[0], TurnedSky[0])});
	}

	return {};
}

// Occlusion culling must never change pixels: a cube hidden behind a wall stays culled, and the frame the wall moves away it must already be drawn.
[[nodiscard]] std::expected<void, FPresentationError> CheckOcclusionCulling(IGraphicsDevice& Device, const FShaderAsset& VertexShader, const FShaderAsset& FragmentShader, const FShaderAsset& InstancedVertexShader, const FVisualShaderSet& Shaders)
{
	if (Shaders.OcclusionCullCompute.Bytecode.empty())
	{
		return {};
	}

	const auto Culled = FMeshRenderer::Create(Device, VertexShader, FragmentShader, {}, {}, {}, {}, InstancedVertexShader, Shaders);
	const auto Reference = FMeshRenderer::Create(Device, VertexShader, FragmentShader, {}, {}, {}, {}, InstancedVertexShader, Shaders);
	const auto Cube = CreateSmokeCube(Device);
	if (!Culled || !Reference || !Cube)
	{
		return std::unexpected(!Culled ? Culled.error() : !Reference ? Reference.error()
		                                                             : Cube.error());
	}

	constexpr FExtent2D Extent{192, 128};
	std::array Models{
	    FMatrix4::Translation({0, 1, -2}) * FMatrix4::Scale({8, 6, 0.2f}),
	    FMatrix4::Translation({0, 1, 2}),
	    FMatrix4::Translation({0, -0.1f, 0}) * FMatrix4::Scale({12, 0.2f, 12}),
	};

	const std::array<const FRenderMesh*, 3> Meshes{Cube->get(), Cube->get(), Cube->get()};
	const std::array Lights{
	    FRenderLight{.Settings = {.Type = ELightType::Directional, .Intensity = 3000.f, .bCastShadows = false, .Range = 40.f}, .Transform = FMatrix4::Rotation(FQuaternion::FromAxisAngle({1, 0, 0}, 0.7f))},
	    FRenderLight{.Settings = {.Type = ELightType::Sky, .Intensity = 10.f, .bCastShadows = false, .bEnvironmentVisible = false}, .Transform = FMatrix4{}},
	};

	FMeshRenderView View{
	    .View = FMatrix4::Translation({0, -1, 6}),
	    .Projection = FMatrix4::PerspectiveReversedInfinite(std::numbers::pi_v<float> / 3.f, 1.5f, 0.1f),
	    .Models = Models,
	    .Meshes = Meshes,
	    .Lights = Lights,
	    .Visuals = {.ExposureEV100 = 7.737f, .AntiAliasing = EAntiAliasing::Off, .bStudioPreview = false},
	};

	const auto Capture = [&](FMeshRenderer& Renderer, const bool bCulling) -> std::expected<std::vector<std::byte>, FPresentationError>
	{
		View.Visuals.bOcclusionCulling = bCulling;
		if (const auto Rendered = Renderer.Render(Extent, View); !Rendered)
		{
			return std::unexpected(Rendered.error());
		}

		return Device.ReadbackTexture(Renderer.GetColorTarget());
	};

	const auto Matches = [](const std::vector<std::byte>& A, const std::vector<std::byte>& B)
	{
		for (std::size_t Index = 0; Index < A.size(); ++Index)
		{
			if (std::abs(std::to_integer<int>(A[Index]) - std::to_integer<int>(B[Index])) > 1)
			{
				return false;
			}
		}

		return A.size() == B.size();
	};

	std::expected<std::vector<std::byte>, FPresentationError> Hidden;
	for (int Frame = 0; Frame < 3 && (Frame == 0 || Hidden); ++Frame)
	{
		Hidden = Capture(**Culled, true);
	}

	const auto HiddenReference = Capture(**Reference, false);
	if (!Hidden || !HiddenReference)
	{
		return std::unexpected(!Hidden ? Hidden.error() : HiddenReference.error());
	}

	if (!Matches(*Hidden, *HiddenReference))
	{
		return Failure("Occlusion culling changed the image of a scene with a hidden cube");
	}

	Models[0] = FMatrix4::Translation({30, 1, -2}) * FMatrix4::Scale({8, 6, 0.2f});
	const auto Revealed = Capture(**Culled, true);
	const auto RevealedReference = Capture(**Reference, false);
	if (!Revealed || !RevealedReference)
	{
		return std::unexpected(!Revealed ? Revealed.error() : RevealedReference.error());
	}

	if (Matches(*Hidden, *Revealed))
	{
		return Failure("Moving the occluding wall did not reveal the cube behind it");
	}

	if (!Matches(*Revealed, *RevealedReference))
	{
		return Failure("Occlusion culling drew a newly revealed cube a frame late");
	}

	return {};
}

[[nodiscard]] std::expected<void, FPresentationError> CheckExposureCalibration(IGraphicsDevice& Device, const FShaderAsset& VertexShader, const FShaderAsset& FragmentShader, const FShaderAsset& InstancedVertexShader, const FVisualShaderSet& Shaders)
{
	const auto Renderer = FMeshRenderer::Create(Device, VertexShader, FragmentShader, {}, {}, {}, {}, InstancedVertexShader, Shaders);
	const auto Cube = CreateSmokeCube(Device);
	FMaterialAsset GreyCard{.Name = "Grey card"};
	GreyCard.Parameters.BaseColor = {0.18f, 0.18f, 0.18f, 1.f};
	GreyCard.Parameters.Metallic = 0.f;
	GreyCard.Parameters.Roughness = 1.f;
	const auto Material = FRenderMaterial::Create(Device, GreyCard, {}, "Grey card");
	if (!Renderer || !Cube || !Material)
	{
		return std::unexpected(!Renderer ? Renderer.error() : !Cube ? Cube.error()
		                                                            : Material.error());
	}

	// No sky light or atmosphere, so the card receives only the overhead sun; the camera looks down at 45 degrees, away from the specular peak.
	constexpr FExtent2D Extent{64, 64};
	const FMatrix4 Card = FMatrix4::Scale({40.f, 0.1f, 40.f});
	const FRenderMesh* const Mesh = Cube->get();
	const std::array<const FRenderMaterial*, 1> Slots{Material->get()};
	const std::array<std::span<const FRenderMaterial* const>, 1> Overrides{Slots};
	const std::array Lights{FRenderLight{.Settings = {.Type = ELightType::Directional, .Intensity = 100'000.f, .bCastShadows = false, .Range = 40.f}, .Transform = FMatrix4::Rotation(FQuaternion::FromAxisAngle({1.f, 0.f, 0.f}, std::numbers::pi_v<float> / 2.f))}};
	FMeshRenderView View{
	    .View = FMatrix4::Rotation(FQuaternion::FromAxisAngle({1.f, 0.f, 0.f}, -std::numbers::pi_v<float> / 4.f)) * FMatrix4::Translation({0.f, -3.f, 3.f}),
	    .Projection = FMatrix4::PerspectiveReversedInfinite(std::numbers::pi_v<float> / 3.f, 1.f, 0.1f),
	    .Models = std::span{&Card, 1},
	    .Meshes = std::span{&Mesh, 1},
	    .Materials = Overrides,
	    .Lights = Lights,
	    .Visuals = {.ExposureEV100 = 15.f, .AntiAliasing = EAntiAliasing::Off},
	};
	View.Visuals.bStudioPreview = false;
	if (const auto Rendered = (*Renderer)->Render(Extent, View); !Rendered)
	{
		return Rendered;
	}

	const auto Image = Device.ReadbackTexture((*Renderer)->GetColorTarget());
	if (!Image || Image->size() != std::size_t{Extent.Width} * Extent.Height * 4)
	{
		return Image ? Failure("Exposure calibration returned an invalid image size") : std::unexpected(Image.error());
	}

	const std::size_t Center = (std::size_t{Extent.Height / 2} * Extent.Width + Extent.Width / 2) * 4;
	const int Red = std::to_integer<int>((*Image)[Center]);
	const int Green = std::to_integer<int>((*Image)[Center + 1]);
	const int Blue = std::to_integer<int>((*Image)[Center + 2]);
	std::println("Exposure calibration: 18% grey under 100000 lx at EV100 15 displays as {}/{}/{}", Red, Green, Blue);

	// GT7 keeps midtones nearly linear, so the camera's 14.6% exposed grey lands near 105 in sRGB; a little rough specular may lift it a few values.
	if (Green < 98 || Green > 116 || std::abs(Red - Green) > 3 || std::abs(Blue - Green) > 3)
	{
		return std::unexpected(FPresentationError{.Code = EPresentationErrorCode::CommandSubmissionFailed, .Message = std::format("An 18% grey card at sunny 16 displayed as {}/{}/{}, not neutral middle grey near 105", Red, Green, Blue).c_str()});
	}

	// HDR output keeps midtones where SDR puts them, relative to paper white, and compresses highlights to the display peak instead of clipping at white.
	constexpr FHdrDisplaySettings Hdr{.PaperWhite = 250.f, .PeakLuminance = 1000.f};
	// Brightening the exposure rather than the sun keeps scene color inside half-float range.
	const auto RenderHdr = [&](const float ExposureEV100, const bool bCalibrationPattern) -> std::expected<std::vector<std::byte>, FPresentationError>
	{
		FMeshRenderView HdrView = View;
		HdrView.Visuals.ExposureEV100 = ExposureEV100;
		HdrView.Visuals.HdrDisplay = FHdrDisplaySettings{.PaperWhite = Hdr.PaperWhite, .PeakLuminance = Hdr.PeakLuminance, .bCalibrationPattern = bCalibrationPattern};
		if (const auto Rendered = (*Renderer)->Render(Extent, HdrView); !Rendered)
		{
			return std::unexpected(Rendered.error());
		}

		auto Pixels = Device.ReadbackTexture((*Renderer)->GetColorTarget());
		if (Pixels && Pixels->size() != std::size_t{Extent.Width} * Extent.Height * 8)
		{
			return Failure("HDR display target is not half-float RGBA");
		}

		return Pixels;
	};

	const auto Channel = [&](const std::vector<std::byte>& Pixels, const std::uint32_t X, const std::uint32_t Y, const std::size_t Index)
	{
		const std::size_t Offset = (std::size_t{Y} * Extent.Width + X) * 8 + Index * 2;
		return DecodeHalf(Pixels[Offset], Pixels[Offset + 1]);
	};

	const auto Midtone = RenderHdr(15.f, false);
	const auto Highlight = RenderHdr(7.f, false);
	const auto Pattern = RenderHdr(15.f, true);
	if (!Midtone || !Highlight || !Pattern)
	{
		return std::unexpected(!Midtone ? Midtone.error() : !Highlight ? Highlight.error()
		                                                               : Pattern.error());
	}

	const std::uint32_t CenterX = Extent.Width / 2;
	const std::uint32_t CenterY = Extent.Height / 2;
	const float SdrGrey = std::pow((static_cast<float>(Green) / 255.f + 0.055f) / 1.055f, 2.4f);
	const float HdrGrey = Channel(*Midtone, CenterX, CenterY, 1);
	const float HdrHighlight = Channel(*Highlight, CenterX, CenterY, 1);
	const float PatternInner = Channel(*Pattern, CenterX, CenterY, 1);
	const float PatternOuter = Channel(*Pattern, CenterX + 10, CenterY, 1);
	const float PatternBackground = Channel(*Pattern, 0, 0, 1);
	std::println("HDR calibration: grey {:.4f} (SDR {:.4f}), 8-stop highlight {:.3f}, pattern {:.1f}/{:.2f}/{:.2f} of paper white", HdrGrey, SdrGrey, HdrHighlight, PatternInner, PatternOuter, PatternBackground);

	const float Peak = Hdr.PeakLuminance / Hdr.PaperWhite;
	if (std::abs(HdrGrey - SdrGrey) > 0.01f || std::abs(Channel(*Midtone, CenterX, CenterY, 0) - HdrGrey) > 0.005f || std::abs(Channel(*Midtone, CenterX, CenterY, 2) - HdrGrey) > 0.005f)
	{
		return std::unexpected(FPresentationError{.Code = EPresentationErrorCode::CommandSubmissionFailed, .Message = std::format("HDR grey card read {:.4f}, not the SDR value {:.4f} relative to paper white", HdrGrey, SdrGrey).c_str()});
	}

	if (HdrHighlight < Peak * 0.97f || HdrHighlight > Peak * 1.001f)
	{
		return std::unexpected(FPresentationError{.Code = EPresentationErrorCode::CommandSubmissionFailed, .Message = std::format("A highlight 8 stops over noon read {:.3f} of paper white, not the {:.1f} peak", HdrHighlight, Peak).c_str()});
	}

	if (std::abs(PatternInner - 10000.f / Hdr.PaperWhite) > 0.05f || std::abs(PatternOuter - Peak) > 0.01f || PatternBackground != 0.f)
	{
		return Failure("HDR calibration pattern does not show the peak around a 10000 cd/m2 square on black");
	}

	return {};
}
}

std::expected<void, FPresentationError> RunRendererSmoke(IGraphicsDevice& Device, const FShaderAsset& VertexShader, const FShaderAsset& FragmentShader, const FShaderAsset& DebugVertexShader, const FShaderAsset& DebugFragmentShader, const FShaderAsset& GridVertexShader, const FShaderAsset& GridFragmentShader, const FShaderAsset& InstancedVertexShader, const FVisualShaderSet& VisualShaders)
{
	if (const auto Hdr = CheckHdrResources(Device); !Hdr)
	{
		return Hdr;
	}

	if (const auto InvalidCommands = CheckInvalidCommands(Device); !InvalidCommands)
	{
		return InvalidCommands;
	}

	if (const auto Compute = CheckCompute(Device, VisualShaders); !Compute)
	{
		return Compute;
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

	if (const auto Instancing = CheckInstancing(Device, **Renderer, VertexShader, FragmentShader, InstancedVertexShader); !Instancing)
	{
		return Instancing;
	}

	// Submit the near red quad before the far green quad so a disabled depth test cannot pass.
	constexpr std::array<FMeshVertex, 4> NearVertices{{{.Position = {-1, -1, 0}, .UV = {0.25f, 0.5f}, .Normal = {0, 0, -1}}, {.Position = {-1, 1, 0}, .UV = {0.25f, 0.5f}, .Normal = {0, 0, -1}}, {.Position = {1, 1, 0}, .UV = {0.25f, 0.5f}, .Normal = {0, 0, -1}}, {.Position = {1, -1, 0}, .UV = {0.25f, 0.5f}, .Normal = {0, 0, -1}}}};
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
	auto Pipeline = Device.CreateGraphicsPipeline({.Name = "Depth regression", .VertexShader = VertexShader, .FragmentShader = FragmentShader, .ColorFormat = ETextureFormat::Rgba8Srgb, .TextureCount = 15, .UniformBufferSize = VisualUniformBytes, .BufferCount = 2});
	auto InstancedPipeline = Device.CreateGraphicsPipeline({.Name = "Instance validation regression", .VertexShader = InstancedVertexShader, .FragmentShader = FragmentShader, .ColorFormat = ETextureFormat::Rgba8Srgb, .bInstanced = true, .TextureCount = 15, .UniformBufferSize = VisualUniformBytes, .BufferCount = 2});
	const auto ShadowDepth = Device.CreateTexture({.Name = "Depth-only sampled atlas regression", .Extent = {.Width = 64, .Height = 64}, .Format = ETextureFormat::Depth32, .bRenderTarget = true});
	const auto ShadowPipeline = Device.CreateGraphicsPipeline({.Name = "Depth-only viewport regression", .VertexShader = VertexShader, .TextureCount = 15, .UniformBufferSize = VisualUniformBytes, .bDepthOnly = true, .BufferCount = 2});
	auto Instances = Device.CreateBuffer({.Name = "Instance validation transforms", .Size = 2 * sizeof(FMeshInstance), .Usage = EBufferUsage::Vertex, .VertexFormat = EGraphicsVertexFormat::MeshInstance});
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

	if (!InstancedPipeline || !Instances)
	{
		return std::unexpected(!InstancedPipeline ? InstancedPipeline.error() : Instances.error());
	}

	if (!ShadowDepth || !ShadowPipeline)
	{
		return std::unexpected(!ShadowDepth ? ShadowDepth.error() : ShadowPipeline.error());
	}

	if (Device.CreateGraphicsPipeline({.Name = "Undersized reflected uniforms", .VertexShader = VertexShader, .FragmentShader = FragmentShader, .TextureCount = 15, .UniformBufferSize = 16, .BufferCount = 2}))
	{
		return Failure("Pipeline accepted a uniform block smaller than its shader reflection");
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

	// The mesh shaders bind lights and light cluster masks; with no lights neither is read.
	auto Lights = Device.CreateBuffer({.Name = "Depth test lights", .Size = sizeof(FVisualLightUniform), .Usage = EBufferUsage::Storage});
	auto LightMasks = Device.CreateBuffer({.Name = "Depth test light masks", .Size = LightMaskWords * sizeof(std::uint32_t), .Usage = EBufferUsage::Storage});
	if (!Lights || !LightMasks)
	{
		Device.CancelCommands();
		return std::unexpected(!Lights ? Lights.error() : LightMasks.error());
	}

	const std::array Buffers{*Lights, *LightMasks};
	if (Result)
	{
		Result = Device.ClearTargets(*Color, *Depth, {0, 0, 0, 1});
	}

	const FMatrix4 Projection = FMatrix4::PerspectiveReversedInfinite(std::numbers::pi_v<float> / 2.f, 1, 0.1f);
	std::array<FTextureHandle, 15> Textures;
	std::ranges::fill(Textures, *Texture);
	FVisualUniforms Uniforms;
	Uniforms.ViewToWorld = FMatrix4{}.Data();
	Uniforms.Material.TextureFlags[0] = 1.f;
	Uniforms.Sky[3] = 2.f;
	const auto UniformBytes = GetVisualUniformBytes(Uniforms);
	if (Result)
	{
		Result = Device.ClearTargets({}, *ShadowDepth, {});
	}

	if (Result)
	{
		Result = Device.DrawIndexed({.Pipeline = *ShadowPipeline, .Vertices = *Near, .Indices = *Index, .DepthTarget = *ShadowDepth, .WorldToClip = (Projection * FMatrix4::Translation({0, 0, 2})).Data(), .IndexCount = 6, .ObjectToView = FMatrix4::Translation({0, 0, 2}).Data(), .Textures = Textures, .Uniforms = UniformBytes, .Viewport = {.X = 8, .Y = 8, .Width = 16, .Height = 16}, .Buffers = Buffers});
	}

	if (Result)
	{
		Textures[8] = *ShadowDepth;
	}

	if (Result)
	{
		FIndexedDraw InvalidDraw{.Pipeline = *Pipeline, .Vertices = *Near, .Indices = *Index, .Texture = *Texture, .ColorTarget = *Color, .DepthTarget = *Depth, .IndexCount = 6, .Textures = Textures, .Uniforms = UniformBytes, .Buffers = Buffers};
		InvalidDraw.Uniforms = UniformBytes.first(16);
		if (Device.DrawIndexed(InvalidDraw))
		{
			Device.CancelCommands();
			return Failure("Draw accepted an incomplete uniform block");
		}

		InvalidDraw.Uniforms = UniformBytes;
		InvalidDraw.Textures = std::span(Textures).first(8);
		if (Device.DrawIndexed(InvalidDraw))
		{
			Device.CancelCommands();
			return Failure("Draw accepted incomplete texture bindings");
		}

		InvalidDraw.Textures = Textures;
		InvalidDraw.Viewport = {.X = 60, .Y = 0, .Width = 8, .Height = 8};
		if (Device.DrawIndexed(InvalidDraw))
		{
			Device.CancelCommands();
			return Failure("Draw accepted an out-of-range viewport");
		}

		InvalidDraw.Viewport = {};
		Textures[8] = *Depth;
		if (Device.DrawIndexed(InvalidDraw))
		{
			Device.CancelCommands();
			return Failure("Draw sampled its active depth attachment");
		}

		Textures[8] = *ShadowDepth;
	}

	if (Result)
	{
		FIndexedDraw InstanceDraw{.Pipeline = *InstancedPipeline, .Vertices = *Near, .Indices = *Index, .Texture = *Texture, .ColorTarget = *Color, .DepthTarget = *Depth, .IndexCount = 6, .Instances = *Instances, .InstanceCount = 2, .Textures = Textures, .Uniforms = UniformBytes, .Buffers = Buffers};
		if (Device.DrawIndexed(InstanceDraw))
		{
			Device.CancelCommands();
			return Failure("Indexed instancing accepted an uninitialized instance buffer");
		}

		std::array InstanceData{FMeshInstance{.WorldToClip = (Projection * FMatrix4::Translation({0, 0, 2})).Data(), .ObjectToView = FMatrix4::Translation({0, 0, 2}).Data()}, FMeshInstance{.WorldToClip = (Projection * FMatrix4::Translation({0, 0, 4})).Data(), .ObjectToView = FMatrix4::Translation({0, 0, 4}).Data()}};
		InstanceData[0].WorldToClip[0] = std::numeric_limits<float>::quiet_NaN();
		if (Device.WriteBuffer(*Instances, std::as_bytes(std::span{InstanceData})))
		{
			Device.CancelCommands();
			return Failure("Mesh instances accepted a nonfinite transform");
		}

		InstanceData[0].WorldToClip = (Projection * FMatrix4::Translation({0, 0, 2})).Data();
		Result = Device.WriteBuffer(*Instances, std::as_bytes(std::span{InstanceData}));
		if (Result)
		{
			for (const std::uint32_t Count : {0u, 3u})
			{
				InstanceDraw.InstanceCount = Count;
				if (Device.DrawIndexed(InstanceDraw))
				{
					Device.CancelCommands();
					return Failure("Indexed instancing accepted an empty or out-of-range instance count");
				}
			}

			InstanceDraw.InstanceCount = 2;
			InstanceDraw.FirstInstance = 1;
			if (Device.DrawIndexed(InstanceDraw))
			{
				Device.CancelCommands();
				return Failure("Indexed instancing accepted a range beyond its instance buffer");
			}

			InstanceDraw.FirstInstance = 0;
			InstanceDraw.Instances = *Near;
			if (Device.DrawIndexed(InstanceDraw))
			{
				Device.CancelCommands();
				return Failure("Indexed instancing accepted an ordinary vertex buffer as instance data");
			}

			InstanceDraw.Instances = *Instances;
			InstanceDraw.Pipeline = *Pipeline;
			if (Device.DrawIndexed(InstanceDraw))
			{
				Device.CancelCommands();
				return Failure("A non-instanced pipeline accepted instance data");
			}
		}
	}

	if (Result && Device.DrawIndexed({.Pipeline = *Pipeline, .Vertices = *Near, .Indices = *Index, .Texture = *Texture, .ColorTarget = *Color, .DepthTarget = *Depth, .WorldToClip = (Projection * FMatrix4::Translation({0, 0, 2})).Data(), .IndexCount = 7, .FirstIndex = 0, .ObjectToView = FMatrix4::Translation({0, 0, 2}).Data(), .Textures = Textures, .Uniforms = UniformBytes, .Buffers = Buffers}))
	{
		Device.CancelCommands();
		return Failure("Indexed draw exceeding the index buffer unexpectedly succeeded");
	}

	if (Result)
	{
		Result = Device.DrawIndexed({.Pipeline = *Pipeline, .Vertices = *Near, .Indices = *Index, .Texture = *Texture, .ColorTarget = *Color, .DepthTarget = *Depth, .WorldToClip = (Projection * FMatrix4::Translation({0, 0, 2})).Data(), .IndexCount = 6, .FirstIndex = 0, .ObjectToView = FMatrix4::Translation({0, 0, 2}).Data(), .Textures = Textures, .Uniforms = UniformBytes, .Buffers = Buffers});
	}

	if (Result)
	{
		Result = Device.DrawIndexed({.Pipeline = *Pipeline, .Vertices = *Far, .Indices = *Index, .Texture = *Texture, .ColorTarget = *Color, .DepthTarget = *Depth, .WorldToClip = (Projection * FMatrix4::Translation({0, 0, 4})).Data(), .IndexCount = 6, .FirstIndex = 0, .ObjectToView = FMatrix4::Translation({0, 0, 4}).Data(), .Textures = Textures, .Uniforms = UniformBytes, .Buffers = Buffers});
	}

	// Mutating caller-owned inputs after recording must not change the submitted draws.
	Uniforms.Material.BaseColor = {0, 1, 0, 1};
	std::ranges::fill(Textures, FTextureHandle{});

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

	Result = CheckDebugDraw(Device, VertexShader, FragmentShader, DebugVertexShader, DebugFragmentShader, GridVertexShader, GridFragmentShader, InstancedVertexShader);
	if (!Result)
	{
		return Result;
	}

	Result = CheckVisualPipeline(Device, VertexShader, FragmentShader, InstancedVertexShader, VisualShaders);
	if (!Result)
	{
		return Result;
	}

	Result = CheckOcclusionCulling(Device, VertexShader, FragmentShader, InstancedVertexShader, VisualShaders);
	if (!Result)
	{
		return Result;
	}

	Result = CheckMotionVectors(Device, VertexShader, FragmentShader, InstancedVertexShader, VisualShaders);
	if (!Result)
	{
		return Result;
	}

	Result = CheckWhiteFurnace(Device, VertexShader, FragmentShader, InstancedVertexShader, VisualShaders);
	if (!Result)
	{
		return Result;
	}

	Result = CheckExposureCalibration(Device, VertexShader, FragmentShader, InstancedVertexShader, VisualShaders);
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
