#include "PathTracer.h"

#include <cmath>

namespace Herta
{
namespace
{
constexpr std::uint32_t MaximumBounces = 8;

template <typename T>
void AppendKey(std::vector<std::byte>& Key, const T& Value)
{
	const auto Bytes = std::as_bytes(std::span{&Value, 1});
	Key.insert(Key.end(), Bytes.begin(), Bytes.end());
}
}

std::expected<std::unique_ptr<FPathTracer>, FPresentationError> FPathTracer::Create(IGraphicsDevice& Device, FShaderAsset Shader)
{
	if (!Device.SupportsRayQueries() || Shader.Bytecode.empty())
	{
		return std::unexpected(FPresentationError{.Code = EPresentationErrorCode::InvalidDescriptor, .Message = "The path tracer needs ray queries and its shader"});
	}

	auto Pipeline = Device.CreateComputePipeline({.Name = "Path tracing", .Shader = std::move(Shader), .TextureCount = 3, .StorageTextureCount = 2, .UniformBufferSize = VisualUniformBytes, .bLinearSampler = true, .bRepeatSampler = true, .BufferCount = 3, .bRayQuery = true, .bResourceTable = true});
	if (!Pipeline)
	{
		return std::unexpected(Pipeline.error());
	}

	return std::unique_ptr<FPathTracer>(new FPathTracer(Device, std::move(*Pipeline)));
}

FPathTracer::FPathTracer(IGraphicsDevice& InDevice, FComputePipelineHandle InPipeline)
    : Device(InDevice)
    , Pipeline(std::move(InPipeline))
{
}

std::uint32_t FPathTracer::GetSampleCount() const noexcept
{
	return SampleCount;
}

void FPathTracer::Release() noexcept
{
	Accumulation.reset();
	Key.clear();
	SampleCount = 0;
}

std::expected<void, FPresentationError> FPathTracer::Render(const FPathTraceInputs& Inputs)
{
	const FVisualUniforms& Uniforms = *Inputs.Uniforms;
	const FRayTracingScene& Scene = *Inputs.Scene;

	// The view, scene, lights, and environment; exposure and display settings only change how the result is shown.
	NextKey.clear();
	AppendKey(NextKey, Inputs.Extent);
	AppendKey(NextKey, Scene.GetGeneration());
	AppendKey(NextKey, Uniforms.ViewToWorld);
	AppendKey(NextKey, Uniforms.ClipToView);
	AppendKey(NextKey, Uniforms.Sky);
	AppendKey(NextKey, Uniforms.SkyColor);
	AppendKey(NextKey, Uniforms.Atmosphere);
	AppendKey(NextKey, Uniforms.AtmosphereGeometry);
	AppendKey(NextKey, Uniforms.Controls);
	AppendKey(NextKey, Uniforms.SunIlluminance);
	AppendKey(NextKey, Uniforms.SunDirection);
	AppendKey(NextKey, Uniforms.SunRadiance);
	AppendKey(NextKey, Uniforms.Fog);
	AppendKey(NextKey, Uniforms.FogColor);
	AppendKey(NextKey, Inputs.EnvironmentIdentity);
	const std::size_t LightCount = std::min<std::size_t>(static_cast<std::size_t>(Uniforms.Controls[0]), MaximumRenderLights);
	const auto LightBytes = std::as_bytes(std::span{Uniforms.Lights}.first(LightCount));
	NextKey.insert(NextKey.end(), LightBytes.begin(), LightBytes.end());

	const bool bChanged = NextKey != Key || !Accumulation || Accumulation->GetDescriptor().Extent != Inputs.Extent;
	std::swap(Key, NextKey);
	if (!bChanged && SampleCount >= PathTracingSampleLimit)
	{
		return {};
	}

	if (bChanged)
	{
		SampleCount = 0;
		if (!Accumulation || Accumulation->GetDescriptor().Extent != Inputs.Extent)
		{
			auto Created = Device.CreateTexture({.Name = "Path tracing accumulation", .Extent = Inputs.Extent, .Format = ETextureFormat::Rgba32Float, .bStorage = true});
			if (!Created)
			{
				return std::unexpected(Created.error());
			}

			Accumulation = std::move(*Created);
		}
	}

	FVisualUniforms Dispatch = Uniforms;
	// The angle one pixel subtends at the image centre seeds each ray cone.
	const float FocalScale = std::abs(Inputs.View->Projection.Data()[5]);
	Dispatch.Material.BaseColor = {static_cast<float>(SampleCount), static_cast<float>(MaximumBounces), std::atan(2.f / (FocalScale * static_cast<float>(Inputs.Extent.Height))), 0.f};
	const std::array Sources{Inputs.Environment, Inputs.SkyView, Inputs.SpecularAlbedo};
	const std::array Targets{FStorageTextureBinding{.Texture = Accumulation}, FStorageTextureBinding{.Texture = Inputs.Output}};
	const std::array Buffers{Inputs.Lights, Scene.GetSections(), Scene.GetMaterials()};
	auto Traced = Device.Dispatch({.Pipeline = Pipeline, .Textures = Sources, .StorageTextures = Targets, .Uniforms = GetVisualUniformBytes(Dispatch), .GroupCount = {(Inputs.Extent.Width + 7) / 8, (Inputs.Extent.Height + 7) / 8, 1}, .Buffers = Buffers, .Scene = Scene.GetTopLevel(), .ResourceTable = Scene.GetTable()});
	if (!Traced)
	{
		Key.clear();
		return Traced;
	}

	++SampleCount;
	return {};
}
}
