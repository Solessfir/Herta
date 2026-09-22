#include "RendererSmoke.h"

#include "Herta/Math/Matrix.h"

#include <algorithm>
#include <array>
#include <memory>
#include <numbers>

namespace Herta
{
namespace
{
[[nodiscard]] std::unexpected<FPresentationError> Failure(const char* Message)
{
	return std::unexpected(FPresentationError{EPresentationErrorCode::InvalidState, Message});
}

[[nodiscard]] std::expected<void, FPresentationError> CheckInvalidCommands(IGraphicsDevice& Device)
{
	if (Device.SubmitCommands())
	{
		return Failure("Submitting without an open graphics frame unexpectedly succeeded");
	}

	if (Device.CreateBuffer({"Invalid usage", 4, static_cast<EBufferUsage>(255)}))
	{
		return Failure("Invalid buffer usage was accepted");
	}

	if (Device.CreateTexture({"Invalid format", {1, 1}, static_cast<ETextureFormat>(255), false}))
	{
		return Failure("Invalid texture format was accepted");
	}

	auto Buffer = Device.CreateBuffer({"Upload validation buffer", 4, EBufferUsage::Index});
	if (!Buffer)
	{
		return std::unexpected(Buffer.error());
	}

	auto Texture = Device.CreateTexture({"Upload validation texture", {1, 1}, ETextureFormat::Rgba8, false});
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

	if (Device.WriteBuffer(*Buffer, {}) || Device.WriteTexture(*Texture, {}))
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
		auto Buffer = Device.CreateBuffer({"Lifetime test buffer", sizeof(Indices), EBufferUsage::Index});
		if (!Buffer)
		{
			return std::unexpected(Buffer.error());
		}

		auto Texture = Device.CreateTexture({"Lifetime test texture", {1, 1}, ETextureFormat::Rgba8, false});
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
			Result = Device.WriteTexture(*Texture, std::as_bytes(std::span(Texels)));
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
}

std::expected<void, FPresentationError> RunRendererSmoke(IGraphicsDevice& Device, const FShaderAsset& VertexShader, const FShaderAsset& FragmentShader)
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
	constexpr std::array<FExtent2D, 8> Extents{{{128, 128}, {64, 192}, {0, 0}, {320, 180}, {1, 1}, {192, 64}, {0, 128}, {128, 128}}};
	for (std::size_t Frame = 0; Frame < 32; ++Frame)
	{
		auto Result = (*Renderer)->Render(Extents[Frame % Extents.size()]);
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

	// Submit the near red quad before the far green quad so a disabled depth test cannot pass.
	constexpr std::array<FMeshVertex, 4> NearVertices{{{{-1, -1, 0}, {0.25f, 0.5f}}, {{-1, 1, 0}, {0.25f, 0.5f}}, {{1, 1, 0}, {0.25f, 0.5f}}, {{1, -1, 0}, {0.25f, 0.5f}}}};
	auto FarVertices = NearVertices;
	for (FMeshVertex& Vertex : FarVertices)
	{
		Vertex.UV[0] = 0.75f;
	}
	constexpr std::array<std::uint32_t, 6> Indices{0, 1, 2, 0, 2, 3};
	constexpr std::array<std::uint8_t, 8> Texels{255, 0, 0, 255, 0, 255, 0, 255};
	auto Near = Device.CreateBuffer({"Depth test near", sizeof(NearVertices), EBufferUsage::Vertex});
	auto Far = Device.CreateBuffer({"Depth test far", sizeof(FarVertices), EBufferUsage::Vertex});
	auto Index = Device.CreateBuffer({"Depth test indices", sizeof(Indices), EBufferUsage::Index});
	auto Texture = Device.CreateTexture({"Depth test colors", {2, 1}, ETextureFormat::Rgba8Srgb, false});
	auto Color = Device.CreateTexture({"Depth test color", {64, 64}, ETextureFormat::Rgba8Srgb, true});
	auto Depth = Device.CreateTexture({"Depth test depth", {64, 64}, ETextureFormat::Depth32, true});
	auto Pipeline = Device.CreateGraphicsPipeline({"Depth regression", VertexShader, FragmentShader, ETextureFormat::Rgba8Srgb});
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
		Result = Device.WriteTexture(*Texture, std::as_bytes(std::span(Texels)));
	}

	if (Result)
	{
		Result = Device.ClearTargets(*Color, *Depth, {0, 0, 0, 1});
	}

	const FMatrix4 Projection = FMatrix4::PerspectiveReversedInfinite(std::numbers::pi_v<float> / 2.0f, 1, 0.1f);
	if (Result && Device.DrawIndexed({*Pipeline, *Near, *Index, *Texture, *Color, *Depth, (Projection * FMatrix4::Translation({0, 0, 2})).Data(), 7}))
	{
		Device.CancelCommands();
		return Failure("Indexed draw exceeding the index buffer unexpectedly succeeded");
	}

	if (Result)
	{
		Result = Device.DrawIndexed({*Pipeline, *Near, *Index, *Texture, *Color, *Depth, (Projection * FMatrix4::Translation({0, 0, 2})).Data(), 6});
	}

	if (Result)
	{
		Result = Device.DrawIndexed({*Pipeline, *Far, *Index, *Texture, *Color, *Depth, (Projection * FMatrix4::Translation({0, 0, 4})).Data(), 6});
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
	if (std::to_integer<unsigned>((*DepthPixels)[Center]) < 250 || std::to_integer<unsigned>((*DepthPixels)[Center + 1]) > 5)
	{
		return Failure("Reversed-Z regression: far geometry overwrote the near red quad");
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
